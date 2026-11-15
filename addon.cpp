#include <windows.h>
#include <d3d12.h>
#include <imgui.h>
#include <include/reshade.hpp>
#include <utils/hash.hpp>
#include <MinHook.h>
#include "dlaa.hpp"
#include "motion-id.inc"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace {
using namespace reshade::api;
using Event = reshade::addon_event;
constexpr uint32_t kTaaHash = 0xF35783D3;
constexpr uint32_t kCompositeHash = 0x41177900;
constexpr uint32_t kMotionPixelHash = 0x9FA3C08F;
constexpr uint32_t kMaxRootParameters = 64;
constexpr uint64_t kBatchBytes = 144;
constexpr uint64_t kViewBytes = 3248;

struct Binding {
    uint32_t param = kMaxRootParameters;
    uint32_t offset = 0;
};
enum Input {
    kBatch, kView, kDepth, kColor, kMotion, kHistory0, kHistory1,
    kComposite0, kComposite1, kInstances, kBindingCount,
};
constexpr uint32_t kTaaMask = (1u << kComposite0) - 1;
struct Layout {
    std::array<Binding, kBindingCount> bindings{};
    uint32_t parameter_count = 0;
    uint32_t found = 0;
    bool motion_draw_constants = false;
};

struct Slot {
    descriptor_type type{};
    resource image{};
    resource_view_desc view{};
    uint64_t generation = 0;
    buffer_range cbv{};
};
struct ConstantCopy {
    resource source{}, destination{};
    uint64_t source_offset = 0, destination_offset = 0, size = 0;
};
struct TaaConstants {
    buffer_range batch{}, view{};
    uint32_t phase = 0;
    std::shared_ptr<dirt5::Frame> frame;
    size_t transition_count = 0;
    uint32_t groups_x = 0, groups_y = 0;
};
struct BatchSample {
    dirt5::Rect history, render;
    std::array<float, 2> sample_offset{}, center_offset{};
    uint32_t phase = UINT32_MAX;
};
struct UploadSnapshot {
    BatchSample batch{};
    std::array<float, 2> projection_offset{};
    std::array<float, 4> camera_position{}, camera_forward{};
    uint64_t size = 0, generation = 0;
};
struct ReadyFrame {
    std::shared_ptr<dirt5::Frame> frame;
    float jitter_x = 0, jitter_y = 0;
    uint32_t phase = 0;
    std::array<float, 4> camera_position{}, camera_forward{};
    bool depth_readable = false;
    uint64_t submission_serial = 0;
    dirt5::Rect render, history;
    int preset = 0;
    bool instances_readable = false;
};
struct QueueTrace {
    std::unordered_map<uint64_t, std::unordered_map<uint64_t, UploadSnapshot>> uploads;
    unsigned matches = 0;
    std::unique_ptr<dirt5::DlaaQueue> renderer;
    ReadyFrame ready, last;
    unsigned evaluated = 0, fallbacks = 0, resets = 0, depth_recoveries = 0;
    bool reset = true;
    std::chrono::steady_clock::time_point last_evaluation{};
};
struct __declspec(uuid("0BBF4C4A-891B-4F45-98CC-0F78BC13C5CB")) DeviceData {
    std::recursive_mutex mutex;
    std::unordered_map<uint64_t, std::vector<Slot>> heaps;
    std::unordered_map<uint64_t, uint64_t> generations;
    std::unordered_map<uint64_t, Layout> layouts;
    std::unordered_set<uint64_t> taa_pipelines;
    std::unordered_set<uint64_t> composite_pipelines;
    std::unordered_map<command_queue*, QueueTrace> trace_queues;
    std::vector<std::shared_ptr<dirt5::Staging>> staging_pool;
    command_queue* dlaa_queue = nullptr;
    uint64_t next_generation = 0;
    unsigned reports = 0;
    unsigned matches = 0;
};
struct Transition {
    resource image{};
    resource_usage before{};
    resource_usage after{};
    D3D12_RESOURCE_BARRIER_FLAGS flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    bool whole_resource = false;
};
struct __declspec(uuid("44C798D7-523C-4C7D-BAB9-2307F91738B1")) ListData {
    pipeline_layout layout{};
    std::array<descriptor_table, kMaxRootParameters> tables{};
    std::array<buffer_range, kMaxRootParameters> root_cbvs{};
    pipeline_layout graphics_layout{};
    std::array<descriptor_table, kMaxRootParameters> graphics_tables{};
    bool taa = false;
    bool composite = false;
    std::array<Transition, 32> barriers{};
    uint32_t barrier_count = 0;
    std::vector<std::variant<ConstantCopy, TaaConstants>> trace_commands;
    std::vector<Transition> native_transitions;
    resource final_history{};
    size_t composite_transition_count = 0;
    bool incomplete_transitions = false;
};

bool pass_through = false;
bool trace_jitter = false;
bool enable_dlaa = false;
std::atomic_int sharpening{0};
std::atomic_uint motion_view_range{0};
std::atomic_int motion_blur{0};

constexpr char kSettingsSection[] = "DIRT5_DLAA";
constexpr char kSettingsTab[] = "DIRT 5 DLSS";
enum Mode { kStockTaa, kDlaa, kPassThrough };
struct Settings {
    int mode = kDlaa;
    bool trace_jitter = false;
};
Settings startup_settings, pending_settings;
std::atomic_int preset_index{0};

int ReadLegacySharpening(const wchar_t* path) {
    wchar_t value[32]{};
    if (GetPrivateProfileStringW(L"Dirt5DLAA", L"Sharpening", L"0", value,
                                 static_cast<DWORD>(std::size(value)), path) >= std::size(value) - 1) return 0;
    wchar_t* end = nullptr;
    const long percent = std::wcstol(value, &end, 10);
    if (end == value) return 0;
    while (std::iswspace(*end)) ++end;
    return *end == L'\0' ? static_cast<int>(std::clamp(percent, 0L, 100L)) : 0;
}

void LoadSettings(const std::wstring& directory) {
    pending_settings = {};
    int strength = 0;
    if (reshade::get_config_value(nullptr, kSettingsSection, "Mode", pending_settings.mode)) {
        if (pending_settings.mode < kStockTaa || pending_settings.mode > kPassThrough) pending_settings.mode = kDlaa;
        reshade::get_config_value(nullptr, kSettingsSection, "TraceJitter", pending_settings.trace_jitter);
        reshade::get_config_value(nullptr, kSettingsSection, "Sharpening", strength);
    } else {
        // Import older installations once. All subsequent reads use ReShade's config.
        const auto path = directory + L"\\Dirt5DLAA.ini";
        if (!directory.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            const auto read = [&](const wchar_t* key) {
                return GetPrivateProfileIntW(L"Dirt5DLAA", key, 0, path.c_str()) == 1;
            };
            pending_settings.mode = read(L"EnableDLAA") ? kDlaa : read(L"PassThrough") ? kPassThrough : kStockTaa;
            pending_settings.trace_jitter = read(L"TraceJitter");
            strength = ReadLegacySharpening(path.c_str());
        }
        reshade::set_config_value(nullptr, kSettingsSection, "Sharpening", strength);
        reshade::set_config_value(nullptr, kSettingsSection, "TraceJitter", pending_settings.trace_jitter);
        reshade::set_config_value(nullptr, kSettingsSection, "Mode", pending_settings.mode);
    }
    sharpening.store(std::clamp(strength, 0, 100), std::memory_order_relaxed);
    int preset = 0;
    reshade::get_config_value(nullptr, kSettingsSection, "ModelPreset", preset);
    if (preset < 0 || static_cast<size_t>(preset) >= dirt5::kPresets.size()) preset = 0;
    preset_index.store(preset, std::memory_order_relaxed);
    startup_settings = pending_settings;
    pass_through = startup_settings.mode == kPassThrough;
    trace_jitter = startup_settings.trace_jitter;
    enable_dlaa = startup_settings.mode == kDlaa;
}

void DrawSettings(effect_runtime*) {
    constexpr const char* modes[]{"Stock TAA", "DLAA / DLSS (automatic)", "Unfiltered RGB (diagnostic)"};
    ImGui::TextUnformatted("DIRT 5 DLSS v1.12");
    if (ImGui::Combo("Anti-aliasing", &pending_settings.mode, modes, static_cast<int>(std::size(modes)))) {
        reshade::set_config_value(nullptr, kSettingsSection, "Mode", pending_settings.mode);
    }
    ImGui::SetItemTooltip("Restart the game to change mode. Automatic uses the render and history sizes to choose DLAA or DLSS.");

    constexpr const char* presets[]{"NVIDIA default", "Preset J", "Preset K", "Preset L", "Preset M"};
    static_assert(std::size(presets) == dirt5::kPresets.size());
    int preset = preset_index.load(std::memory_order_relaxed);
    if (ImGui::Combo("NVIDIA model preset", &preset, presets, static_cast<int>(std::size(presets)))) {
        preset_index.store(preset, std::memory_order_relaxed);
        reshade::set_config_value(nullptr, kSettingsSection, "ModelPreset", preset);
    }
    ImGui::SetItemTooltip("Applies to DLAA and DLSS. Changing the model recreates the feature and resets its history.");

    int strength = sharpening.load(std::memory_order_relaxed);
    if (ImGui::SliderInt("Sharpening", &strength, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp)) {
        sharpening.store(strength, std::memory_order_relaxed);
        reshade::set_config_value(nullptr, kSettingsSection, "Sharpening", strength);
    }
    ImGui::SetItemTooltip("Applies immediately to DLAA and DLSS. 0%% disables sharpening.");

    constexpr const char* exposures[]{"Off", "DriveClub (1/150 s)", "Short (1/300 s)"};
    int blur = motion_blur.load(std::memory_order_relaxed);
    if (ImGui::Combo("Gameplay motion blur", &blur, exposures, static_cast<int>(std::size(exposures)))) {
        motion_blur.store(blur, std::memory_order_relaxed);
        reshade::set_config_value(nullptr, kSettingsSection, "MotionBlur", blur);
    }
    ImGui::SetItemTooltip("Motion blur with per-draw surface IDs. Applies live in automatic DLAA / DLSS mode.");

    static int motion_range = 16;
    bool show_motion = motion_view_range.load(std::memory_order_relaxed) != 0;
    if (ImGui::Checkbox("Show DLSS motion vectors", &show_motion)) {
        motion_view_range.store(show_motion ? static_cast<unsigned>(motion_range) : 0, std::memory_order_relaxed);
    }
    ImGui::SetItemTooltip("Replaces the scene with the vectors supplied to NGX while automatic DLAA / DLSS is active.");
    if (show_motion) {
        if (ImGui::SliderInt("Motion range", &motion_range, 1, 128, "%d px", ImGuiSliderFlags_AlwaysClamp)) {
            motion_view_range.store(static_cast<unsigned>(motion_range), std::memory_order_relaxed);
        }
        ImGui::TextWrapped("Neutral gray = zero. Red tracks horizontal motion and green tracks vertical motion. "
            "Brighter means right or down, darker means left or up. Blue increases with speed.");
    }

    if (ImGui::Checkbox("Trace jitter (diagnostic)", &pending_settings.trace_jitter)) {
        reshade::set_config_value(nullptr, kSettingsSection, "TraceJitter", pending_settings.trace_jitter);
    }
    ImGui::SetItemTooltip("Logs the jitter checks in stock TAA and unfiltered modes after restart. DLAA always verifies jitter.");
    if (pending_settings.mode != startup_settings.mode || pending_settings.trace_jitter != startup_settings.trace_jitter) {
        ImGui::TextWrapped("Restart the game to apply anti-aliasing or jitter tracing changes.");
    }
}

struct PendingSubmission {
    ID3D12CommandQueue* queue;
    ID3D12CommandList* list;
    DeviceData* device;
    command_queue* proxy;
    std::vector<std::shared_ptr<dirt5::Frame>> staged;
    ReadyFrame ready;
    resource final_history{};
    std::vector<resource> final_depth;
    bool taa = false;
};
thread_local std::vector<PendingSubmission> pending;
std::mutex hook_mutex;
struct SubmissionOrder {
    std::recursive_mutex mutex;
    uint64_t serial = 0;
};
std::mutex submission_orders_mutex;
std::unordered_map<ID3D12CommandQueue*, std::shared_ptr<SubmissionOrder>> submission_orders;
void* execute_target = nullptr;
void* barrier_target = nullptr;
std::atomic_bool native_queue_hook_ready{false};
std::atomic_bool native_barrier_hook_ready{false};
dirt5::ExecuteLists original_execute = nullptr;
using ResourceBarrier = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
ResourceBarrier original_barrier = nullptr;
struct ObservedBarriers {
    ID3D12GraphicsCommandList* list = nullptr;
    std::vector<D3D12_RESOURCE_BARRIER> barriers;
};
thread_local ObservedBarriers observed_barriers;

void STDMETHODCALLTYPE ExecuteHook(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists);
void STDMETHODCALLTYPE BarrierHook(ID3D12GraphicsCommandList* list, UINT count, const D3D12_RESOURCE_BARRIER* barriers) {
    original_barrier(list, count, barriers);
    observed_barriers.list = list;
    observed_barriers.barriers.clear();
    if (count) observed_barriers.barriers.assign(barriers, barriers + count);
}

bool InstallHook(void* target, void* detour, void** original, void** installed) {
    const std::scoped_lock lock(hook_mutex);
    if (*installed) return *installed == target;
    if (MH_CreateHook(target, detour, original) != MH_OK) return false;
    if (MH_EnableHook(target) != MH_OK) {
        MH_RemoveHook(target);
        return false;
    }
    *installed = target;
    return true;
}
template <class T, class H> T* Data(H* object) {
    return object->template get_private_data<T>();
}
template <class T, class H> void CreateData(H* object) {
    object->set_private_data(reinterpret_cast<const uint8_t*>(&__uuidof(T)), reinterpret_cast<uint64_t>(new T{}));
}
template <class T, class H> void DeleteData(H* object) {
    auto* data = Data<T>(object);
    object->set_private_data(reinterpret_cast<const uint8_t*>(&__uuidof(T)), 0);
    delete data;
}
template <class T> T* Native(resource object) {
    return reinterpret_cast<T*>(static_cast<uintptr_t>(object.handle));
}

void InitDevice(device* dev) {
    if (dev->get_api() == device_api::d3d12) CreateData<DeviceData>(dev);
}
void DestroyDevice(device* dev) { DeleteData<DeviceData>(dev); }
void InitList(command_list* cmd) {
    if (!Data<DeviceData>(cmd->get_device())) return;
    CreateData<ListData>(cmd);
    if (enable_dlaa && cmd->get_native()) {
        // ID3D12GraphicsCommandList::ResourceBarrier in the COM vtable.
        auto** table = *reinterpret_cast<void***>(static_cast<uintptr_t>(cmd->get_native()));
        if (InstallHook(table[26], reinterpret_cast<void*>(&BarrierHook),
                        reinterpret_cast<void**>(&original_barrier), &barrier_target) &&
            !native_barrier_hook_ready.exchange(true)) {
            reshade::log::message(reshade::log::level::info, "[Dirt5DLAA] Native D3D12 barrier hook ready.");
        }
    }
}
void DestroyList(command_list* cmd) { DeleteData<ListData>(cmd); }
void InitDlaaQueue(command_queue* queue) {
    if (!Data<DeviceData>(queue->get_device()) || !queue->get_native()) return;
    auto* native = reinterpret_cast<ID3D12CommandQueue*>(static_cast<uintptr_t>(queue->get_native()));
    if (native->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return;
    // ID3D12CommandQueue::ExecuteCommandLists in the COM vtable.
    auto** table = *reinterpret_cast<void***>(native);
    if (!InstallHook(table[10], reinterpret_cast<void*>(&ExecuteHook),
                     reinterpret_cast<void**>(&original_execute), &execute_target)) return;
    {
        const std::scoped_lock lock(submission_orders_mutex);
        submission_orders.try_emplace(native, std::make_shared<SubmissionOrder>());
    }
    if (!native_queue_hook_ready.exchange(true)) {
        reshade::log::message(reshade::log::level::info, "[Dirt5DLAA] Native D3D12 queue hook ready.");
    }
}
void ResetList(command_list* cmd) {
    if (auto* list = Data<ListData>(cmd)) *list = {};
}

void InitResource(device* dev, const resource_desc&, const subresource_data*, resource_usage, resource image) {
    if (auto* data = Data<DeviceData>(dev)) {
        const std::scoped_lock lock(data->mutex);
        data->generations[image.handle] = ++data->next_generation;
    }
}
void DestroyResource(device* dev, resource image) {
    if (auto* data = Data<DeviceData>(dev)) {
        const std::scoped_lock lock(data->mutex);
        data->generations.erase(image.handle);
        for (auto& [queue, trace] : data->trace_queues) trace.uploads.erase(image.handle);
    }
}

bool RecordConstantCopy(command_list* cmd, resource source, uint64_t source_offset,
                        resource destination, uint64_t destination_offset, uint64_t size) {
    if (auto* list = Data<ListData>(cmd); list && size != 0) {
        list->trace_commands.emplace_back(ConstantCopy{source, destination, source_offset, destination_offset, size});
    }
    return false;
}

UploadSnapshot ReadConstantCopy(const DeviceData& data, const ConstantCopy& copy) {
    const auto dest = data.generations.find(copy.destination.handle);
    if (dest == data.generations.end() || !data.generations.contains(copy.source.handle)) return {};
    auto* src = Native<ID3D12Resource>(copy.source);
    D3D12_HEAP_PROPERTIES heap{};
    if (FAILED(src->GetHeapProperties(&heap, nullptr))) return {};
    if (heap.Type != D3D12_HEAP_TYPE_UPLOAD &&
        !(heap.Type == D3D12_HEAP_TYPE_CUSTOM && heap.MemoryPoolPreference == D3D12_MEMORY_POOL_L0 &&
          (heap.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE ||
           heap.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_BACK))) return {};
    const auto desc = src->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || copy.source_offset > desc.Width ||
        copy.size > desc.Width - copy.source_offset) return {};
    const size_t offset = static_cast<size_t>(copy.source_offset);
    const D3D12_RANGE read{offset, offset + (copy.size == kBatchBytes ? 124u : 104u)};
    void* mapped = nullptr;
    if (FAILED(src->Map(0, &read, &mapped)) || !mapped) return {};
    const auto* bytes = static_cast<const uint8_t*>(mapped) + offset;
    UploadSnapshot snapshot;
    snapshot.size = copy.size;
    snapshot.generation = dest->second;
    if (copy.size == kBatchBytes) {
        // The TAA shader uses c0 to bound its history stores and c2 to clamp
        // its scene/depth taps. The render bounds are inclusive.
        static_assert(sizeof(dirt5::Rect) == 16);
        std::memcpy(&snapshot.batch.history, bytes, sizeof(dirt5::Rect));
        std::memcpy(snapshot.batch.sample_offset.data(), bytes + 24, sizeof(snapshot.batch.sample_offset));
        std::array<uint32_t, 4> bounds{};
        std::memcpy(bounds.data(), bytes + 32, sizeof(bounds));
        if (bounds[2] >= bounds[0] && bounds[3] >= bounds[1] &&
            bounds[2] < D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION && bounds[3] < D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
            snapshot.batch.render = {bounds[0], bounds[1], {bounds[2] - bounds[0] + 1, bounds[3] - bounds[1] + 1}};
        }
        std::memcpy(snapshot.batch.center_offset.data(), bytes + 64, sizeof(snapshot.batch.center_offset));
        std::memcpy(&snapshot.batch.phase, bytes + 120, sizeof(snapshot.batch.phase));
    } else {
        std::memcpy(snapshot.projection_offset.data(), bytes + 96, sizeof(snapshot.projection_offset));
        std::memcpy(snapshot.camera_forward.data(), bytes + 32, sizeof(snapshot.camera_forward));
        std::memcpy(snapshot.camera_position.data(), bytes + 48, sizeof(snapshot.camera_position));
    }
    const D3D12_RANGE written{0, 0};
    src->Unmap(0, &written);
    return snapshot;
}

const UploadSnapshot* FindUpload(const DeviceData& data, const QueueTrace& trace,
                                  buffer_range binding, uint64_t size) {
    const auto live = data.generations.find(binding.buffer.handle);
    const auto buffer = trace.uploads.find(binding.buffer.handle);
    if (live == data.generations.end() || buffer == trace.uploads.end()) return nullptr;
    const auto sample = buffer->second.find(binding.offset);
    if (sample == buffer->second.end() || sample->second.size != size ||
        sample->second.generation != live->second) return nullptr;
    return &sample->second;
}

const char* ValidateJitter(const UploadSnapshot* batch, const UploadSnapshot* view,
                           uint32_t phase, float* x, float* y) {
    if (!batch || !view) return !batch ? (!view ? "missing-both" : "missing-batch") : "missing-view";
    const auto render = batch->batch.render;
    if (!render.size.width || !render.size.height) return "render-extent";
    if (batch->batch.phase != phase) return "history-phase";
    *x = batch->batch.sample_offset[0] - batch->batch.center_offset[0];
    *y = batch->batch.sample_offset[1] - batch->batch.center_offset[1];
    if (!(std::abs(*x) <= 0.5f && std::abs(*y) <= 0.5f)) return "sample-offset";
    const float projected_x = view->projection_offset[0] * (-0.5f * render.size.width);
    const float projected_y = view->projection_offset[1] * (0.5f * render.size.height);
    return std::abs(*x - projected_x) <= 0.001f && std::abs(*y - projected_y) <= 0.001f
        ? "verified" : "projection-mismatch";
}

bool PreservedAfterTaa(const ListData& list, resource input, size_t first) {
    if (list.incomplete_transitions || first > list.native_transitions.size()) return false;
    for (size_t i = first; i < list.native_transitions.size(); ++i) {
        const auto& transition = list.native_transitions[i];
        if (!transition.image.handle || transition.image == input) return false;
    }
    return true;
}

void TraceSubmission(command_queue* queue, command_list* cmd) {
    auto* list = Data<ListData>(cmd);
    if (!list || (list->trace_commands.empty() && !list->final_history.handle)) return;
    auto* data = Data<DeviceData>(cmd->get_device());
    const std::scoped_lock lock(data->mutex);
    auto& trace = data->trace_queues[queue];
    PendingSubmission submission{
        reinterpret_cast<ID3D12CommandQueue*>(static_cast<uintptr_t>(queue->get_native())),
        reinterpret_cast<ID3D12CommandList*>(static_cast<uintptr_t>(cmd->get_native())), data, queue};
    // Keep copies by submission order. Recording runs on several worker threads.
    for (const auto& entry : list->trace_commands) {
        if (const auto* copy = std::get_if<ConstantCopy>(&entry)) {
            if (const auto buffer = trace.uploads.find(copy->destination.handle); buffer != trace.uploads.end()) {
                std::erase_if(buffer->second, [&](const auto& sample) {
                    const uint64_t start = sample.first;
                    const uint64_t write = copy->destination_offset;
                    return write <= start ? start - write < copy->size : write - start < sample.second.size;
                });
            }
            if (copy->size == kBatchBytes || copy->size == kViewBytes) {
                const auto sample = ReadConstantCopy(*data, *copy);
                if (sample.size) trace.uploads[copy->destination.handle][copy->destination_offset] = sample;
            }
            continue;
        }
        const auto& taa = std::get<TaaConstants>(entry);
        const auto* batch = FindUpload(*data, trace, taa.batch, kBatchBytes);
        const auto* view = FindUpload(*data, trace, taa.view, kViewBytes);
        float x = 0, y = 0;
        const char* status = ValidateJitter(batch, view, taa.phase, &x, &y);
        if (taa.frame && std::strcmp(status, "verified") == 0) {
            const auto render = batch->batch.render;
            const auto history = batch->batch.history;
            if (!render.Fits(taa.frame->staging->color_size) || !render.Fits(taa.frame->staging->motion_size) ||
                !render.Fits(dirt5::Size(taa.frame->depth.Get())) || !history.Fits(dirt5::Size(taa.frame->history.Get())) ||
                taa.groups_x != (history.size.width + 15) / 16 || taa.groups_y != (history.size.height + 7) / 8) {
                status = "render-or-history-bounds";
            }
        }
        if (enable_dlaa) {
            submission.taa = true;
            submission.ready = {};
            if (taa.frame) {
                submission.staged.push_back(taa.frame);
                const auto history = reinterpret_cast<uintptr_t>(taa.frame->history.Get());
                for (auto it = list->native_transitions.rbegin(); it != list->native_transitions.rend(); ++it) {
                    if (it->image.handle != history) continue;
                    if (it->whole_resource && it->flags == D3D12_RESOURCE_BARRIER_FLAG_NONE &&
                        it->before == resource_usage::unordered_access &&
                        it->after == resource_usage::shader_resource_non_pixel && std::strcmp(status, "verified") == 0) {
                        submission.ready = {taa.frame, x, y, taa.phase, view->camera_position, view->camera_forward};
                        submission.ready.render = batch->batch.render;
                        submission.ready.history = batch->batch.history;
                        const resource depth{reinterpret_cast<uintptr_t>(taa.frame->depth.Get())};
                        submission.ready.depth_readable = PreservedAfterTaa(*list, depth, taa.transition_count);
                        if (taa.frame->instances) {
                            const resource instances{reinterpret_cast<uintptr_t>(taa.frame->instances.Get())};
                            submission.ready.instances_readable = PreservedAfterTaa(*list, instances, taa.transition_count);
                        }
                    }
                    break;
                }
            }
        }
        const auto match = ++trace.matches;
        if (match <= 24 || match % 600 == 0) {
            char line[300];
            const auto render = batch ? batch->batch.render : dirt5::Rect{};
            const auto history = batch ? batch->batch.history : dirt5::Rect{};
            std::snprintf(line, sizeof(line), "[Dirt5DLAA] Jitter submit #%u phase=%u status=%s offset=(%.6f,%.6f) batch=%s view=%s render=%ux%u history=%ux%u",
                          match, taa.phase, status, x, y, batch ? "found" : "missing", view ? "found" : "missing",
                          render.size.width, render.size.height, history.size.width, history.size.height);
            reshade::log::message(reshade::log::level::info, line);
        }
        trace.uploads.clear();
    }
    if (!enable_dlaa || !native_queue_hook_ready.load() || (!submission.taa && !list->final_history.handle)) return;
    if (list->final_history.handle) {
        submission.final_history = list->final_history;
        constexpr auto depth_read = resource_usage::depth_stencil_read | resource_usage::shader_resource;
        for (size_t i = list->composite_transition_count; i < list->native_transitions.size(); ++i) {
            const auto& transition = list->native_transitions[i];
            if (!transition.whole_resource || transition.flags != D3D12_RESOURCE_BARRIER_FLAG_NONE ||
                transition.before != depth_read || transition.after != resource_usage::depth_stencil_write) continue;
            bool earlier = false;
            for (size_t j = 0; j < i; ++j) earlier |= list->native_transitions[j].image == transition.image;
            if (!earlier) submission.final_depth.push_back(transition.image);
        }
    }
    pending.push_back(std::move(submission));
}

void DestroyTraceQueue(command_queue* queue) {
    if (enable_dlaa) {
        const std::scoped_lock lock(submission_orders_mutex);
        submission_orders.erase(reinterpret_cast<ID3D12CommandQueue*>(static_cast<uintptr_t>(queue->get_native())));
    }
    if (auto* data = Data<DeviceData>(queue->get_device())) {
        const std::scoped_lock lock(data->mutex);
        if (data->dlaa_queue == queue) data->dlaa_queue = nullptr;
        data->trace_queues.erase(queue);
    }
}

bool CameraCut(const ReadyFrame& previous, const ReadyFrame& current) {
    float distance_squared = 0, direction_dot = 0;
    for (size_t i = 0; i < 3; ++i) {
        const float delta = current.camera_position[i] - previous.camera_position[i];
        distance_squared += delta * delta;
        direction_dot += current.camera_forward[i] * previous.camera_forward[i];
    }
    return !(distance_squared <= 9.0f && direction_dot >= 0.94f) || current.phase == previous.phase;
}

void STDMETHODCALLTYPE ExecuteHook(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) {
    std::shared_ptr<SubmissionOrder> order;
    {
        const std::scoped_lock lock(submission_orders_mutex);
        if (const auto found = submission_orders.find(queue); found != submission_orders.end()) order = found->second;
    }
    std::unique_lock<std::recursive_mutex> queue_lock;
    uint64_t serial = 0;
    if (order) {
        queue_lock = std::unique_lock(order->mutex);
        if (count) ++order->serial;
        serial = order->serial;
    }
    std::vector<PendingSubmission> batch;
    if (!pending.empty()) {
        for (UINT i = 0; i < count; ++i) {
            const auto found = std::find_if(pending.begin(), pending.end(), [&](const PendingSubmission& entry) {
                return entry.queue == queue && entry.list == lists[i];
            });
            if (found == pending.end()) continue;
            batch.push_back(std::move(*found));
            pending.erase(found);
        }
    }
    if (batch.empty()) {
        original_execute(queue, count, lists);
        return;
    }
    auto& data = *batch.front().device;
    const std::scoped_lock lock(data.mutex);
    auto* proxy = batch.front().proxy;
    auto& trace = data.trace_queues[proxy];
    const bool contains_taa = std::any_of(batch.begin(), batch.end(), [](const auto& entry) { return entry.taa; });
    if (!data.dlaa_queue && contains_taa && queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) data.dlaa_queue = proxy;
    if (data.dlaa_queue != proxy) {
        original_execute(queue, count, lists);
        for (const auto& entry : batch) {
            for (const auto& frame : entry.staged) {
                if (!trace.renderer) trace.renderer = std::make_unique<dirt5::DlaaQueue>();
                trace.renderer->Track(queue, frame);
            }
        }
        return;
    }
    if (!trace.renderer) trace.renderer = std::make_unique<dirt5::DlaaQueue>();
    bool consumed = false;
    for (const auto& entry : batch) {
        if (!entry.final_history.handle || consumed) continue;
        consumed = true;
        auto& ready = trace.ready;
        const char* failure = nullptr;
        bool recovered_depth = false;
        if (entry.list != lists[0]) failure = "composite is not first in its submission";
        else if (contains_taa) failure = "TAA and composite in one submission";
        else if (!ready.frame) failure = "no staged frame with verified jitter";
        else if (reinterpret_cast<uintptr_t>(ready.frame->history.Get()) != entry.final_history.handle) failure = "history mismatch";
        else {
            const resource depth{reinterpret_cast<uintptr_t>(ready.frame->depth.Get())};
            const bool composite_depth = std::find(entry.final_depth.begin(), entry.final_depth.end(), depth) != entry.final_depth.end();
            const bool preceding_taa_depth = ready.depth_readable && ready.submission_serial && serial == ready.submission_serial + 1;
            if (!composite_depth && !preceding_taa_depth) failure = "depth state";
            recovered_depth = !composite_depth && preceding_taa_depth;
        }
        if (!failure) {
            ready.preset = preset_index.load(std::memory_order_relaxed);
            const bool reset = trace.reset || trace.last.render != ready.render || trace.last.history != ready.history ||
                trace.last.preset != ready.preset || CameraCut(trace.last, ready);
            const auto now = std::chrono::steady_clock::now();
            const float frame_seconds = std::chrono::duration<float>(now - trace.last_evaluation).count();
            const int exposure = motion_blur.load(std::memory_order_relaxed);
            float blur_scale = 0.0f;
            if (!reset && exposure && frame_seconds > 0.0f && frame_seconds <= 0.1f) {
                const float shutter_seconds = exposure == 1 ? 1.0f / 150.0f : 1.0f / 300.0f;
                blur_scale = std::min(1.0f, shutter_seconds / frame_seconds);
            }
            if (trace.renderer->Run(queue, original_execute, ready.frame, ready.render, ready.history,
                                    ready.jitter_x, ready.jitter_y, reset,
                                    static_cast<UINT>(sharpening.load(std::memory_order_relaxed)),
                                    dirt5::kPresets[ready.preset],
                                    motion_view_range.load(std::memory_order_relaxed), blur_scale,
                                    ready.instances_readable && serial == ready.submission_serial + 1)) {
                trace.last_evaluation = now;
                ++trace.evaluated;
                if (reset) ++trace.resets;
                if (recovered_depth && ++trace.depth_recoveries <= 4) {
                    reshade::log::message(reshade::log::level::info,
                        "[Dirt5DLAA] DLAA used depth verified by the preceding TAA submission.");
                }
                if (trace.evaluated <= 10 || trace.evaluated % 600 == 0) {
                    char line[200];
                    std::snprintf(line, sizeof(line), "[Dirt5DLAA] %s submitted #%u phase=%u jitter=(%.6f,%.6f) reset=%d fallbacks=%u resets=%u depthRecovered=%u",
                                  dirt5::IsUpscale(ready.render.size, ready.history.size) ? "DLSS" : "DLAA",
                                  trace.evaluated, ready.phase, ready.jitter_x, ready.jitter_y, int(reset),
                                  trace.fallbacks, trace.resets, trace.depth_recoveries);
                    reshade::log::message(reshade::log::level::info, line);
                }
                trace.last = ready;
                trace.last.frame.reset();
                trace.reset = false;
            } else {
                failure = "NGX or evaluation slot unavailable";
            }
        }
        if (failure) {
            trace.reset = true;
            if (++trace.fallbacks <= 10) {
                char line[180];
                std::snprintf(line, sizeof(line), "[Dirt5DLAA] Stock TAA: %s.", failure);
                reshade::log::message(reshade::log::level::info, line);
            }
        }
        ready = {};
    }
    original_execute(queue, count, lists);
    if (contains_taa) {
        if (trace.ready.frame) {
            trace.reset = true;
            if (++trace.fallbacks <= 10) {
                reshade::log::message(reshade::log::level::info,
                    "[Dirt5DLAA] Stock TAA: no matching composite submission.");
            }
        }
        for (const auto& entry : batch) {
            if (!entry.taa) continue;
            trace.ready = entry.list == lists[count - 1] ? entry.ready : ReadyFrame{};
            trace.ready.submission_serial = serial;
            for (const auto& frame : entry.staged) {
                if (!trace.renderer->Track(queue, frame)) trace.ready = {};
            }
        }
    }
}

Slot ReadSlot(device* dev, const DeviceData& data, descriptor_table table, uint32_t binding) {
    if (!table.handle) return {};
    descriptor_heap heap{};
    uint32_t index = 0;
    dev->get_descriptor_heap_offset(table, binding, 0, &heap, &index);
    const auto found = data.heaps.find(heap.handle);
    if (found == data.heaps.end() || index >= found->second.size()) return {};
    const auto slot = found->second[index];
    if (slot.image.handle) {
        const auto live = data.generations.find(slot.image.handle);
        if (live == data.generations.end() || live->second != slot.generation) return {};
    }
    return slot;
}

bool UpdateDescriptors(device* dev, uint32_t count, const descriptor_table_update* updates) {
    auto* data = Data<DeviceData>(dev);
    if (!data) return false;
    const std::scoped_lock lock(data->mutex);
    for (uint32_t i = 0; i < count; ++i) {
        const auto& update = updates[i];
        if (!update.table.handle || update.array_offset || !update.count) continue;
        descriptor_heap heap{};
        uint32_t first = 0;
        dev->get_descriptor_heap_offset(update.table, update.binding, 0, &heap, &first);
        if (!heap.handle) continue;
        auto& slots = data->heaps[heap.handle];
        if (slots.size() < size_t(first) + update.count) slots.resize(size_t(first) + update.count);
        for (uint32_t n = 0; n < update.count; ++n) {
            auto& slot = slots[first + n];
            slot = {};
            slot.type = update.type;
            if (update.type == descriptor_type::constant_buffer) {
                slot.cbv = static_cast<const buffer_range*>(update.descriptors)[n];
            } else if (update.type == descriptor_type::buffer_shader_resource_view ||
                       update.type == descriptor_type::texture_shader_resource_view ||
                       update.type == descriptor_type::texture_unordered_access_view) {
                const auto view = static_cast<const resource_view*>(update.descriptors)[n];
                if (!view.handle) continue;
                slot.image = dev->get_resource_from_view(view);
                if (const auto live = data->generations.find(slot.image.handle); live != data->generations.end()) {
                    slot.generation = live->second;
                    slot.view = dev->get_resource_view_desc(view);
                }
            }
        }
    }
    return false;
}

bool CopyDescriptors(device* dev, uint32_t count, const descriptor_table_copy* copies) {
    auto* data = Data<DeviceData>(dev);
    if (!data) return false;
    const std::scoped_lock lock(data->mutex);
    for (uint32_t i = 0; i < count; ++i) {
        const auto& copy = copies[i];
        if (!copy.source_table.handle || !copy.dest_table.handle || !copy.count ||
            copy.source_array_offset || copy.dest_array_offset) continue;
        descriptor_heap source{}, dest{};
        uint32_t src_index = 0, dst_index = 0;
        dev->get_descriptor_heap_offset(copy.source_table, copy.source_binding, 0, &source, &src_index);
        dev->get_descriptor_heap_offset(copy.dest_table, copy.dest_binding, 0, &dest, &dst_index);
        if (!source.handle || !dest.handle) continue;
        auto& target = data->heaps[dest.handle];
        if (target.size() < size_t(dst_index) + copy.count) target.resize(size_t(dst_index) + copy.count);
        const auto found = data->heaps.find(source.handle);
        for (uint32_t n = 0; n < copy.count; ++n) {
            target[dst_index + n] = found != data->heaps.end() && src_index + n < found->second.size()
                ? found->second[src_index + n] : Slot{};
        }
    }
    return false;
}

Layout ResolveLayout(uint32_t count, const pipeline_layout_param* params) {
    Layout result;
    result.parameter_count = count;
    if (!params) return result;
    if (count && params[0].type == pipeline_layout_param_type::push_constants) {
        const auto& constants = params[0].push_constants;
        result.motion_draw_constants = constants.dx_register_index == 0 && constants.dx_register_space == 0 &&
            constants.count == 2 && (constants.visibility & shader_stage::pixel) != 0;
    }
    struct Register {
        descriptor_type type;
        uint32_t index;
        uint32_t space;
    };
    constexpr std::array<Register, kBindingCount> registers{{
        {descriptor_type::constant_buffer, 1, 0},
        {descriptor_type::constant_buffer, 0, 1},
        {descriptor_type::shader_resource_view, 0, 0},
        {descriptor_type::shader_resource_view, 6, 0},
        {descriptor_type::shader_resource_view, 7, 0},
        {descriptor_type::unordered_access_view, 0, 26},
        {descriptor_type::unordered_access_view, 1, 26},
        {descriptor_type::shader_resource_view, 4, 79},
        {descriptor_type::shader_resource_view, 5, 79},
        {descriptor_type::shader_resource_view, 31, 512},
    }};
    auto match = [&](uint32_t param, const descriptor_range& range, bool root_cbv) {
        for (uint32_t i = 0; i < registers.size(); ++i) {
            const auto& expected = registers[i];
            const auto stage = i == kComposite0 || i == kComposite1 ? shader_stage::pixel : shader_stage::compute;
            if ((range.visibility & stage) == 0 || (i == kBatch) != root_cbv || range.type != expected.type ||
                range.dx_register_space != expected.space || expected.index < range.dx_register_index) continue;
            const uint32_t delta = expected.index - range.dx_register_index;
            if (delta >= range.count || range.binding > UINT32_MAX - delta) continue;
            if (result.found & (1u << i)) {
                // Ambiguous bindings must never be used for a write.
                result.bindings[i] = {};
            } else {
                result.found |= 1u << i;
                result.bindings[i] = {param, range.binding + delta};
            }
        }
    };
    for (uint32_t i = 0; i < count && i < kMaxRootParameters; ++i) {
        const auto& param = params[i];
        if (param.type == pipeline_layout_param_type::push_descriptors) {
            match(i, param.push_descriptors, true);
        } else if (param.type == pipeline_layout_param_type::descriptor_table) {
            for (uint32_t n = 0; n < param.descriptor_table.count; ++n) {
                match(i, param.descriptor_table.ranges[n], false);
            }
        } else if (param.type == pipeline_layout_param_type::descriptor_table_with_flags) {
            const auto& table = param.descriptor_table_with_flags;
            for (uint32_t n = 0; n < table.count; ++n) match(i, table.ranges[n], false);
        } else if (param.type == pipeline_layout_param_type::push_descriptors_with_ranges_and_flags) {
            const auto& root = param.descriptor_table_with_flags;
            if (root.count == 1) match(i, root.ranges[0], true);
        }
    }
    result.found = 0;
    for (uint32_t i = 0; i < result.bindings.size(); ++i) {
        if (result.bindings[i].param != kMaxRootParameters) result.found |= 1u << i;
    }
    return result;
}

void InitLayout(device* dev, uint32_t count, const pipeline_layout_param* p, pipeline_layout layout) {
    auto* data = Data<DeviceData>(dev);
    if (!data) return;
    const std::scoped_lock lock(data->mutex);
    data->layouts.insert_or_assign(layout.handle, ResolveLayout(count, p));
}
void DestroyLayout(device* dev, pipeline_layout layout) {
    if (auto* data = Data<DeviceData>(dev)) {
        const std::scoped_lock lock(data->mutex);
        data->layouts.erase(layout.handle);
    }
}

bool CreatePipeline(device* dev, pipeline_layout layout, uint32_t count, const pipeline_subobject* objects) {
    shader_desc* pixel = nullptr;
    bool motion_target = false;
    for (uint32_t i = 0; i < count; ++i) {
        const auto& object = objects[i];
        if (object.count != 1 || !object.data) continue;
        if (object.type == pipeline_subobject_type::pixel_shader) {
            pixel = static_cast<shader_desc*>(object.data);
        } else if (object.type == pipeline_subobject_type::render_target_formats) {
            motion_target = *static_cast<const format*>(object.data) == format::r16g16b16a16_float;
        }
    }
    if (!motion_target || !pixel || !pixel->code || pixel->code_size != 5067 ||
        renodx::utils::hash::ComputeCRC32(static_cast<const uint8_t*>(pixel->code), pixel->code_size) != kMotionPixelHash) return false;
    auto* data = Data<DeviceData>(dev);
    if (!data) return false;
    const std::scoped_lock lock(data->mutex);
    const auto root = data->layouts.find(layout.handle);
    if (root == data->layouts.end() || !root->second.motion_draw_constants) return false;
    pixel->code = kMotionIdPS;
    pixel->code_size = sizeof(kMotionIdPS);
    static std::once_flag reported;
    std::call_once(reported, [] {
        reshade::log::message(reshade::log::level::info, "[Dirt5DLAA] Enabled motion surface IDs in MotionVectors_PS.");
    });
    return true;
}

void InitPipeline(device* dev, pipeline_layout, uint32_t count,
                  const pipeline_subobject* objects, pipeline pso) {
    auto* data = Data<DeviceData>(dev);
    if (!data) return;
    for (uint32_t i = 0; i < count; ++i) {
        if (objects[i].count != 1 || !objects[i].data ||
            (objects[i].type != pipeline_subobject_type::compute_shader &&
             (!enable_dlaa || objects[i].type != pipeline_subobject_type::pixel_shader))) continue;
        const auto& shader = *static_cast<const shader_desc*>(objects[i].data);
        if (objects[i].type == pipeline_subobject_type::pixel_shader) {
            if (shader.code && shader.code_size == 7795 &&
                renodx::utils::hash::ComputeCRC32(static_cast<const uint8_t*>(shader.code), shader.code_size) == kCompositeHash) {
                const std::scoped_lock lock(data->mutex);
                data->composite_pipelines.insert(pso.handle);
                reshade::log::message(reshade::log::level::info, "[Dirt5DLAA] Found HistoryToBackBufferPSMain 41177900.");
            }
            continue;
        }
        if (shader.code_size != 16828 || !shader.code ||
            renodx::utils::hash::ComputeCRC32(static_cast<const uint8_t*>(shader.code), shader.code_size) != kTaaHash) continue;
        const std::scoped_lock lock(data->mutex);
        data->taa_pipelines.insert(pso.handle);
        reshade::log::message(reshade::log::level::info, "[Dirt5DLAA] Found UpdateHistoryBufferCS F35783D3.");
    }
}

void DestroyPipeline(device* dev, pipeline pso) {
    if (auto* data = Data<DeviceData>(dev)) {
        const std::scoped_lock lock(data->mutex);
        data->taa_pipelines.erase(pso.handle);
        data->composite_pipelines.erase(pso.handle);
    }
}
void BindPipeline(command_list* cmd, pipeline_stage stages, pipeline pso) {
    auto* list = Data<ListData>(cmd);
    if (!list) return;
    auto* data = Data<DeviceData>(cmd->get_device());
    const std::scoped_lock lock(data->mutex);
    if ((stages & pipeline_stage::compute_shader) != 0) list->taa = data->taa_pipelines.contains(pso.handle);
    if (enable_dlaa && (stages & pipeline_stage::pixel_shader) != 0) {
        list->composite = data->composite_pipelines.contains(pso.handle);
    }
}

void SetLayout(ListData* list, pipeline_layout layout) {
    if (list->layout == layout) return;
    list->layout = layout;
    list->tables = {};
    list->root_cbvs = {};
}
void BindTables(command_list* cmd, shader_stage stages, pipeline_layout layout, uint32_t first,
                uint32_t count, const descriptor_table* tables, uint32_t dynamic_offset_count, const uint32_t*) {
    auto* list = Data<ListData>(cmd);
    if (!list) return;
    auto bind = [&](auto& slots) {
        for (uint32_t i = 0; first < slots.size() && i < count && i < slots.size() - first; ++i) {
            slots[first + i] = dynamic_offset_count == 0 ? tables[i] : descriptor_table{};
        }
    };
    if ((stages & shader_stage::compute) != 0) {
        SetLayout(list, layout);
        bind(list->tables);
    }
    if (enable_dlaa && (stages & shader_stage::pixel) != 0) {
        if (list->graphics_layout != layout) {
            list->graphics_layout = layout;
            list->graphics_tables = {};
        }
        bind(list->graphics_tables);
    }
}
void BindRootDescriptors(command_list* cmd, shader_stage stages, pipeline_layout layout,
                         uint32_t param, const descriptor_table_update& update) {
    auto* list = Data<ListData>(cmd);
    if (!list || (stages & shader_stage::compute) == 0) return;
    SetLayout(list, layout);
    if (param < list->root_cbvs.size()) {
        list->root_cbvs[param] = update.type == descriptor_type::constant_buffer && update.count == 1
            ? *static_cast<const buffer_range*>(update.descriptors) : buffer_range{};
    }
}

void Barriers(command_list* cmd, uint32_t count, const resource* images,
              const resource_usage* before, const resource_usage* after) {
    auto* list = Data<ListData>(cmd);
    if (!list) return;
    list->barrier_count = count <= list->barriers.size() ? count : 0;
    const bool observed = enable_dlaa && observed_barriers.barriers.size() == count &&
        observed_barriers.list == reinterpret_cast<ID3D12GraphicsCommandList*>(static_cast<uintptr_t>(cmd->get_native()));
    for (uint32_t i = 0; i < list->barrier_count; ++i) {
        list->barriers[i] = {images[i], before[i], after[i]};
    }
    if (!enable_dlaa) return;
    if (!observed) {
        if (count) list->incomplete_transitions = true;
        return;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const auto& native = observed_barriers.barriers[i];
        if (native.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
            // A wildcard aliasing barrier can affect any depth resource.
            const resource from{reinterpret_cast<uintptr_t>(native.Aliasing.pResourceBefore)};
            const resource to{reinterpret_cast<uintptr_t>(native.Aliasing.pResourceAfter)};
            list->native_transitions.push_back({from});
            list->native_transitions.push_back({to});
            continue;
        }
        if (native.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) continue;
        if (reinterpret_cast<uintptr_t>(native.Transition.pResource) != images[i].handle) {
            list->incomplete_transitions = true;
            continue;
        }
        const Transition transition{images[i], before[i], after[i], native.Flags,
            native.Transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES};
        if (i < list->barrier_count) list->barriers[i] = transition;
        list->native_transitions.push_back(transition);
    }
    observed_barriers.barriers.clear();
}

bool HasTransition(const ListData& list, resource image, resource_usage before, resource_usage after) {
    if (!image.handle) return false;
    for (uint32_t i = 0; i < list.barrier_count; ++i) {
        const auto& barrier = list.barriers[i];
        if (barrier.image == image && barrier.before == before && barrier.after == after) return true;
    }
    return false;
}

bool IsColor(const Slot& slot, descriptor_type type) {
    if (!slot.image.handle || slot.type != type || slot.view.type != resource_view_type::texture_2d ||
        slot.view.format != format::r11g11b10_float || slot.view.texture.first_level != 0 ||
        (slot.view.texture.levels != 1 && slot.view.texture.levels != UINT32_MAX)) return false;
    const auto desc = Native<ID3D12Resource>(slot.image)->GetDesc();
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.Format == DXGI_FORMAT_R11G11B10_FLOAT &&
        desc.DepthOrArraySize == 1 && desc.MipLevels == 1 && desc.SampleDesc.Count == 1;
}

bool IsTexture(ID3D12Resource* image, DXGI_FORMAT format) {
    if (!image) return false;
    const auto desc = image->GetDesc();
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.Format == format && desc.MipLevels == 1 &&
        desc.DepthOrArraySize == 1 && desc.SampleDesc.Count == 1;
}

std::shared_ptr<dirt5::Frame> StageFrame(DeviceData& data, const ListData& list,
                                       ID3D12GraphicsCommandList* native, const Slot& color,
                                       const Slot& depth, const Slot& motion, const Slot& history,
                                       buffer_range batch, buffer_range view, const Slot& instances = {}) {
    auto has_input_transition = [&](resource image, resource_usage before, D3D12_RESOURCE_BARRIER_FLAGS flags) {
        for (uint32_t i = 0; i < list.barrier_count; ++i) {
            const auto& transition = list.barriers[i];
            if (transition.image == image && transition.before == before &&
                transition.after == resource_usage::shader_resource_non_pixel &&
                transition.whole_resource && transition.flags == flags) return true;
        }
        return false;
    };
    if (!has_input_transition(color.image, resource_usage::unordered_access, D3D12_RESOURCE_BARRIER_FLAG_NONE) ||
        !has_input_transition(motion.image, resource_usage::render_target, D3D12_RESOURCE_BARRIER_FLAG_END_ONLY) ||
        depth.type != descriptor_type::texture_shader_resource_view || depth.view.format != format::r32_float_x8_uint ||
        motion.type != descriptor_type::texture_shader_resource_view || motion.view.format != format::r16g16b16a16_float ||
        !IsTexture(Native<ID3D12Resource>(depth.image), DXGI_FORMAT_R32G8X24_TYPELESS) ||
        (Native<ID3D12Resource>(depth.image)->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) == 0 ||
        !IsTexture(Native<ID3D12Resource>(motion.image), DXGI_FORMAT_R16G16B16A16_FLOAT) ||
        !data.generations.contains(batch.buffer.handle) || !data.generations.contains(view.buffer.handle) ||
        ((batch.offset | view.offset) % D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT) != 0) return {};
    if (data.dlaa_queue) {
        auto& trace = data.trace_queues.at(data.dlaa_queue);
        if (trace.renderer) {
            if (trace.renderer->Disabled()) return {};
            trace.renderer->Prune();
        }
    }
    const auto size = dirt5::Size(Native<ID3D12Resource>(color.image));
    const auto motion_size = dirt5::Size(Native<ID3D12Resource>(motion.image));
    auto matching_size = [&](const auto& item) { return item->color_size == size && item->motion_size == motion_size; };
    std::erase_if(data.staging_pool, [&](const auto& item) { return item.use_count() == 1 && !matching_size(item); });
    std::shared_ptr<dirt5::Staging> staging;
    for (const auto& item : data.staging_pool) {
        if (item.use_count() == 1 && matching_size(item)) {
            staging = item;
            break;
        }
    }
    if (!staging) {
        if (data.staging_pool.size() >= 8) return {};
        dirt5::ComPtr<ID3D12Device> device;
        if (FAILED(native->GetDevice(IID_PPV_ARGS(&device)))) return {};
        staging = std::make_shared<dirt5::Staging>();
        staging->color_size = size;
        staging->motion_size = motion_size;
        constexpr auto copy = D3D12_RESOURCE_STATE_COPY_DEST;
        constexpr auto none = D3D12_RESOURCE_FLAG_NONE;
        if (!dirt5::Texture(device.Get(), DXGI_FORMAT_R11G11B10_FLOAT, copy, none, staging->color.GetAddressOf(), size) ||
            !dirt5::Texture(device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, copy, none, staging->motion.GetAddressOf(), motion_size)) return {};
        data.staging_pool.push_back(staging);
    }
    auto frame = std::make_shared<dirt5::Frame>();
    frame->staging = std::move(staging);
    frame->history = Native<ID3D12Resource>(history.image);
    frame->depth = Native<ID3D12Resource>(depth.image);
    frame->batch = Native<ID3D12Resource>(batch.buffer);
    frame->view = Native<ID3D12Resource>(view.buffer);
    frame->batch_offset = batch.offset;
    frame->view_offset = view.offset;
    if (instances.type == descriptor_type::buffer_shader_resource_view &&
        instances.view.type == resource_view_type::buffer && instances.view.format == format::unknown &&
        instances.view.buffer.offset == 0 && instances.view.buffer.structured.stride == dirt5::kInstanceStride &&
        instances.view.buffer.structured.count == 98304 && instances.image.handle) {
        auto* buffer = Native<ID3D12Resource>(instances.image);
        const auto desc = buffer->GetDesc();
        if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
            desc.Width == UINT64{98304} * dirt5::kInstanceStride) {
            frame->instances = buffer;
            frame->instance_count = 98304;
        }
    }
    dirt5::CopyInput(native, Native<ID3D12Resource>(color.image), frame->staging->color.Get());
    dirt5::CopyInput(native, Native<ID3D12Resource>(motion.image), frame->staging->motion.Get());
    return frame;
}

bool FindComposite(command_list* cmd, uint32_t vertices, uint32_t instances, uint32_t first, uint32_t first_instance) {
    auto* list = Data<ListData>(cmd);
    if (!list || !list->composite || vertices != 3 || instances != 1 || first != 0 || first_instance != 0) return false;
    auto* dev = cmd->get_device();
    auto* data = Data<DeviceData>(dev);
    const std::scoped_lock lock(data->mutex);
    const auto layout = data->layouts.find(list->graphics_layout.handle);
    if (layout == data->layouts.end()) return false;
    for (const auto input : {kComposite0, kComposite1}) {
        const auto binding = layout->second.bindings[input];
        if (binding.param == kMaxRootParameters) continue;
        const auto history = ReadSlot(dev, *data, list->graphics_tables[binding.param], binding.offset);
        if (!IsColor(history, descriptor_type::texture_shader_resource_view)) continue;
        // The history must enter the composite list in NON_PIXEL_SHADER_RESOURCE.
        bool started = false, finished = false;
        for (const auto& transition : list->native_transitions) {
            if (transition.image != history.image) continue;
            if (finished || !transition.whole_resource || transition.before != resource_usage::shader_resource_non_pixel ||
                transition.after != resource_usage::shader_resource_pixel) {
                finished = false;
                break;
            }
            if (!started && transition.flags == D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY) {
                started = true;
            } else if (transition.flags == (started ? D3D12_RESOURCE_BARRIER_FLAG_END_ONLY : D3D12_RESOURCE_BARRIER_FLAG_NONE)) {
                finished = true;
            } else {
                break;
            }
        }
        if (finished) {
            if (list->final_history.handle) {
                list->final_history = {};
                return false;
            }
            list->final_history = history.image;
            list->composite_transition_count = list->native_transitions.size();
        }
    }
    return false;
}

void RecordPassThrough(ID3D12GraphicsCommandList* cmd, ID3D12Resource* src,
                       ID3D12Resource* dst, uint32_t x, uint32_t y, uint32_t z) {
    // Record the stock dispatch exactly once, then replace only its RGB result.
    cmd->Dispatch(x, y, z);
    D3D12_RESOURCE_BARRIER transitions[2]{};
    transitions[0].Transition = {src, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE};
    transitions[1].Transition = {dst, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST};
    cmd->ResourceBarrier(2, transitions);
    cmd->CopyResource(dst, src);
    for (auto& barrier : transitions) {
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    }
    cmd->ResourceBarrier(2, transitions);
}

bool Dispatch(command_list* cmd, uint32_t x, uint32_t y, uint32_t z) {
    auto* list = Data<ListData>(cmd);
    if (!list) return false;
    if (!list->taa) {
        list->barrier_count = 0;
        return false;
    }
    auto* dev = cmd->get_device();
    auto* data = Data<DeviceData>(dev);
    const std::scoped_lock lock(data->mutex);
    ++data->matches;
    const auto known_layout = data->layouts.find(list->layout.handle);
    const Layout empty_layout;
    const auto& layout = known_layout == data->layouts.end() ? empty_layout : known_layout->second;
    const bool layout_ok = (layout.found & kTaaMask) == kTaaMask;
    auto resolve = [&](Input input) {
        const auto& binding = layout.bindings[input];
        return binding.param == kMaxRootParameters ? Slot{} :
            ReadSlot(dev, *data, list->tables[binding.param], binding.offset);
    };
    const auto source = resolve(kColor);
    const auto depth = resolve(kDepth);
    const auto motion = resolve(kMotion);
    const auto view = resolve(kView);
    const std::array history{resolve(kHistory0), resolve(kHistory1)};
    const bool to_uav_0 = HasTransition(*list, history[0].image, resource_usage::shader_resource, resource_usage::unordered_access);
    const bool to_uav_1 = HasTransition(*list, history[1].image, resource_usage::shader_resource, resource_usage::unordered_access);
    const int phase = to_uav_0 == to_uav_1 ? -1 : (to_uav_0 ? 1 : 0);
    const auto batch_param = layout.bindings[kBatch].param;
    const auto batch_binding = list->root_cbvs[batch_param == kMaxRootParameters ? 0 : batch_param];
    const auto target = phase < 0 ? Slot{} : history[1 - phase];
    auto* native = reinterpret_cast<ID3D12GraphicsCommandList*>(static_cast<uintptr_t>(cmd->get_native()));
    const char* guard = nullptr;
    if (!layout_ok) {
        guard = "layout";
    } else if (!x || !y || z != 1) {
        guard = "dispatch-size";
    } else if (!native || native->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        guard = "command-list-type";
    } else if (phase < 0) {
        guard = "history-transition";
    } else if (!IsColor(source, descriptor_type::texture_shader_resource_view) ||
               !IsColor(target, descriptor_type::texture_unordered_access_view) || source.image == target.image) {
        guard = "color-textures";
    } else if (!HasTransition(*list, source.image, resource_usage::unordered_access, resource_usage::shader_resource_non_pixel)) {
        guard = "scene-color-transition";
    } else if (pass_through && dirt5::Size(Native<ID3D12Resource>(source.image)) != dirt5::Size(Native<ID3D12Resource>(target.image))) {
        guard = "pass-through-size";
    }
    const bool compatible = guard == nullptr;
    bool staged = false;
    if ((trace_jitter || enable_dlaa) && compatible && batch_binding.buffer.handle && view.cbv.buffer.handle) {
        auto frame = enable_dlaa && native_queue_hook_ready.load() && native_barrier_hook_ready.load() ?
            StageFrame(*data, *list, native, source, depth, motion, target, batch_binding, view.cbv,
                       resolve(kInstances)) : nullptr;
        staged = bool(frame);
        list->trace_commands.emplace_back(TaaConstants{batch_binding, view.cbv, static_cast<uint32_t>(phase),
            std::move(frame), list->native_transitions.size(), x, y});
    }
    list->barrier_count = 0;

    if (data->reports < 10 || data->matches % 600 == 0) {
        char line[640];
        std::snprintf(line, sizeof(line),
            "[Dirt5DLAA] TAA #%u groups=%ux%ux%u layout=%s root=%llX params=%u bindings=%02X "
            "phase=%d color=%llX depth=%llX motion=%llX "
            "history=%llX historyCandidates=%llX/%llX viewCB=%llX+%llu batchCB=%llX+%llu copy=%s guard=%s staged=%d",
            data->matches, x, y, z, layout_ok ? "ok" : known_layout == data->layouts.end() ? "missing" : "partial",
            static_cast<unsigned long long>(list->layout.handle), layout.parameter_count, layout.found,
            phase,
            static_cast<unsigned long long>(source.image.handle), static_cast<unsigned long long>(depth.image.handle),
            static_cast<unsigned long long>(motion.image.handle), static_cast<unsigned long long>(target.image.handle),
            static_cast<unsigned long long>(history[0].image.handle), static_cast<unsigned long long>(history[1].image.handle),
            static_cast<unsigned long long>(view.cbv.buffer.handle), static_cast<unsigned long long>(view.cbv.offset),
            static_cast<unsigned long long>(batch_binding.buffer.handle), static_cast<unsigned long long>(batch_binding.offset),
            compatible ? (pass_through ? "recorded" : "ready") : "guarded", guard ? guard : "none", int(staged));
        reshade::log::message(reshade::log::level::info, line);
        ++data->reports;
    }
    if (!pass_through || !compatible) return false;

    RecordPassThrough(native, Native<ID3D12Resource>(source.image), Native<ID3D12Resource>(target.image), x, y, z);
    return true;
}

template <bool Register> void Events() {
#define DIRT5_EVENT(event, callback) \
    if constexpr (Register) reshade::register_event<Event::event>(callback); \
    else reshade::unregister_event<Event::event>(callback)
    DIRT5_EVENT(init_device, InitDevice);
    DIRT5_EVENT(destroy_device, DestroyDevice);
    DIRT5_EVENT(init_command_list, InitList);
    DIRT5_EVENT(destroy_command_list, DestroyList);
    DIRT5_EVENT(reset_command_list, ResetList);
    DIRT5_EVENT(init_resource, InitResource);
    DIRT5_EVENT(destroy_resource, DestroyResource);
    DIRT5_EVENT(update_descriptor_tables, UpdateDescriptors);
    DIRT5_EVENT(copy_descriptor_tables, CopyDescriptors);
    DIRT5_EVENT(init_pipeline_layout, InitLayout);
    DIRT5_EVENT(destroy_pipeline_layout, DestroyLayout);
    DIRT5_EVENT(init_pipeline, InitPipeline);
    DIRT5_EVENT(destroy_pipeline, DestroyPipeline);
    DIRT5_EVENT(bind_pipeline, BindPipeline);
    DIRT5_EVENT(bind_descriptor_tables, BindTables);
    DIRT5_EVENT(push_descriptors, BindRootDescriptors);
    DIRT5_EVENT(barrier, Barriers);
    DIRT5_EVENT(dispatch, Dispatch);
    if (enable_dlaa) {
        DIRT5_EVENT(create_pipeline, CreatePipeline);
        DIRT5_EVENT(init_command_queue, InitDlaaQueue);
        DIRT5_EVENT(draw, FindComposite);
    }
    if (trace_jitter || enable_dlaa) {
        DIRT5_EVENT(copy_buffer_region, RecordConstantCopy);
        DIRT5_EVENT(execute_command_list, TraceSubmission);
        DIRT5_EVENT(destroy_command_queue, DestroyTraceQueue);
    }
#undef DIRT5_EVENT
}
}  // namespace

extern "C" __declspec(dllexport) const char* NAME = "DIRT 5 DLSS";
extern "C" __declspec(dllexport) const char* DESCRIPTION = "Evaluates NVIDIA DLAA for the DIRT 5 final composite.";

extern "C" __declspec(dllexport) bool AddonInit(HMODULE addon, HMODULE reshade_module) {
    if (!reshade::register_addon(addon, reshade_module)) return false;
    dirt5::addon_directory.clear();
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(addon, path, MAX_PATH);
    if (length && length < MAX_PATH) {
        if (auto* slash = std::wcsrchr(path, L'\\')) {
            dirt5::addon_directory.assign(path, slash);
        }
    }
    LoadSettings(dirt5::addon_directory);
    motion_view_range.store(0, std::memory_order_relaxed);
    int exposure = 0;
    reshade::get_config_value(nullptr, kSettingsSection, "MotionBlur", exposure);
    motion_blur.store(exposure >= 0 && exposure <= 2 ? exposure : 0, std::memory_order_relaxed);
    if (enable_dlaa) {
        const auto status = MH_Initialize();
        if (status != MH_OK) {
            dirt5::LogResult("Could not initialize D3D12 hooks. Using stock TAA", status);
            enable_dlaa = false;
        }
    }
    Events<true>();
    reshade::register_overlay(kSettingsTab, DrawSettings);
    reshade::log::message(reshade::log::level::info,
        enable_dlaa ? "[Dirt5DLAA] Initialized v1.12 (automatic DLAA / DLSS)." :
        pass_through ? "[Dirt5DLAA] Initialized v1.12 (experimental color pass-through)." :
                       "[Dirt5DLAA] Initialized v1.12 (stock TAA).");
    if (enable_dlaa) {
        char line[96];
        std::snprintf(line, sizeof(line), "[Dirt5DLAA] Sharpening=%d (0-100).", sharpening.load(std::memory_order_relaxed));
        reshade::log::message(reshade::log::level::info, line);
    }
    if (trace_jitter) reshade::log::message(reshade::log::level::info, "[Dirt5DLAA] Submission-time jitter trace enabled.");
    return true;
}

extern "C" __declspec(dllexport) void AddonUninit(HMODULE addon, HMODULE reshade_module) {
    reshade::unregister_overlay(kSettingsTab, DrawSettings);
    Events<false>();
    if (enable_dlaa) {
        native_queue_hook_ready.store(false);
        native_barrier_hook_ready.store(false);
        MH_Uninitialize();
        execute_target = barrier_target = nullptr;
        pending.clear();
        const std::scoped_lock lock(submission_orders_mutex);
        submission_orders.clear();
    }
    reshade::unregister_addon(addon, reshade_module);
}



