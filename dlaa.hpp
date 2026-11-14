#pragma once

#include <wrl/client.h>
#include <nvsdk_ngx_helpers.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <deque>
#include <memory>
#include <string>
#include <utility>

#include "prepare.inc"
#include "pack.inc"

namespace dirt5 {
using Microsoft::WRL::ComPtr;

inline std::wstring addon_directory;
inline void LogResult(const char* action, unsigned result) {
    char line[160];
    std::snprintf(line, sizeof(line), "[Dirt5DLAA] %s (0x%08X)", action, result);
    reshade::log::message(reshade::log::level::info, line);
}

inline D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* image, D3D12_RESOURCE_STATES before,
                                         D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Transition = {image, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    return barrier;
}

struct Extent {
    UINT width = 0, height = 0;
    bool operator==(const Extent&) const = default;
};

struct Rect {
    UINT x = 0, y = 0;
    Extent size;
    bool operator==(const Rect&) const = default;

    bool Fits(Extent texture) const {
        return size.width && size.height && x < texture.width && y < texture.height &&
            size.width <= texture.width - x && size.height <= texture.height - y;
    }
};

inline Extent Size(ID3D12Resource* texture) {
    if (!texture) return {};
    const auto desc = texture->GetDesc();
    return {static_cast<UINT>(desc.Width), desc.Height};
}

inline void Dispatch(ID3D12GraphicsCommandList* list, Extent size) {
    list->Dispatch((size.width + 15) / 16, (size.height + 7) / 8, 1);
}

inline bool Texture(ID3D12Device* device, DXGI_FORMAT format, D3D12_RESOURCE_STATES state,
                    D3D12_RESOURCE_FLAGS flags, ID3D12Resource** result, Extent size) {
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                                     D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    const D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, size.width, size.height, 1, 1,
                                  format, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, flags};
    return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                                     IID_PPV_ARGS(result)));
}

struct Staging {
    ComPtr<ID3D12Resource> color, motion;
    Extent color_size, motion_size;
};
struct Frame {
    std::shared_ptr<Staging> staging;
    ComPtr<ID3D12Resource> depth, history, batch, view;
    UINT64 batch_offset = 0, view_offset = 0;
};

inline constexpr std::array kPresets{
    NVSDK_NGX_DLSS_Hint_Render_Preset_Default, NVSDK_NGX_DLSS_Hint_Render_Preset_J,
    NVSDK_NGX_DLSS_Hint_Render_Preset_K, NVSDK_NGX_DLSS_Hint_Render_Preset_L,
    NVSDK_NGX_DLSS_Hint_Render_Preset_M,
};

inline bool IsUpscale(Extent render, Extent history) {
    return render != history && render.width <= history.width && render.height <= history.height;
}

struct FeatureSize {
    Extent input, output;
    NVSDK_NGX_DLSS_Hint_Render_Preset preset = NVSDK_NGX_DLSS_Hint_Render_Preset_Default;
    bool operator==(const FeatureSize&) const = default;
};

inline void CopyInput(ID3D12GraphicsCommandList* list, ID3D12Resource* source, ID3D12Resource* dest) {
    auto barrier = Transition(source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(1, &barrier);
    list->CopyResource(dest, source);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    list->ResourceBarrier(1, &barrier);
}

using ExecuteLists = void (STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

class DlaaQueue {
public:
    ~DlaaQueue() {
        if (fence_ && fence_->GetCompletedValue() < serial_) {
            const HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (done) {
                if (SUCCEEDED(fence_->SetEventOnCompletion(serial_, done))) WaitForSingleObject(done, INFINITE);
                CloseHandle(done);
            }
        }
        if (feature_) NVSDK_NGX_D3D12_ReleaseFeature(feature_);
        if (parameters_) NVSDK_NGX_D3D12_DestroyParameters(parameters_);
        if (capabilities_) NVSDK_NGX_D3D12_DestroyParameters(capabilities_);
        if (ngx_initialized_) NVSDK_NGX_D3D12_Shutdown1(device_.Get());
    }

    bool Track(ID3D12CommandQueue* queue, const std::shared_ptr<Frame>& frame) {
        if (!fence_) {
            if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device_))) ||
                FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) {
                disabled_ = true;
                in_flight_.emplace_back(serial_ + 1, frame);
                return false;
            }
        }
        Prune();
        // Keep the frame even if a device failure prevents the fence from being signaled.
        in_flight_.emplace_back(serial_ + 1, frame);
        if (FAILED(queue->Signal(fence_.Get(), serial_ + 1))) {
            disabled_ = true;
            return false;
        }
        ++serial_;
        return true;
    }

    bool Disabled() const { return disabled_; }

    void Prune() {
        if (!fence_) return;
        const UINT64 complete = fence_->GetCompletedValue();
        while (!in_flight_.empty() && in_flight_.front().first <= complete) in_flight_.pop_front();
    }

    bool Run(ID3D12CommandQueue* queue, ExecuteLists execute, const std::shared_ptr<Frame>& frame,
             Rect render, Rect history, float jitter_x, float jitter_y, bool reset, UINT sharpening,
             NVSDK_NGX_DLSS_Hint_Render_Preset preset) {
        if (disabled_ || !fence_) return false;
        Prune();
        if (!ngx_initialized_ && !Initialize()) {
            disabled_ = true;
            return false;
        }
        const bool upscale = IsUpscale(render.size, history.size);
        const FeatureSize requested{render.size, upscale ? history.size : render.size, preset};
        if (requested == rejected_) return false;
        if (!feature_ || requested != feature_size_) {
            if (fence_->GetCompletedValue() < feature_serial_) return false;
            if (!CreateFeature(queue, execute, requested)) {
                rejected_ = requested;
                return false;
            }
            reset = true;
        }

        EvalSlot* selected = nullptr;
        const auto completed = fence_->GetCompletedValue();
        for (auto& slot : slots_) {
            if (slot.serial <= completed) {
                selected = &slot;
                break;
            }
        }
        if (!selected) return false;
        auto& slot = *selected;
        if (FAILED(slot.allocator->Reset()) || FAILED(slot.list->Reset(slot.allocator.Get(), prepare_.Get()))) {
            disabled_ = true;
            return false;
        }
        PopulateDescriptors(slot, *frame);
        auto* list = slot.list.Get();
        auto* staged_color = frame->staging->color.Get();
        auto* staged_motion = frame->staging->motion.Get();
        constexpr auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        constexpr auto write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        D3D12_RESOURCE_BARRIER to_read[]{
            Transition(staged_color, D3D12_RESOURCE_STATE_COPY_DEST, read),
            Transition(staged_motion, D3D12_RESOURCE_STATE_COPY_DEST, read),
        };
        list->ResourceBarrier(2, to_read);
        ID3D12DescriptorHeap* heaps[]{slot.descriptors.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(prepare_root_.Get());
        list->SetComputeRootConstantBufferView(0, frame->batch->GetGPUVirtualAddress() + frame->batch_offset);
        list->SetComputeRootConstantBufferView(1, frame->view->GetGPUVirtualAddress() + frame->view_offset);
        list->SetComputeRootDescriptorTable(2, Gpu(slot, 0));
        const UINT render_rect[]{render.x, render.y, render.size.width, render.size.height};
        list->SetComputeRoot32BitConstants(3, 4, render_rect, 0);
        Dispatch(list, render.size);

        D3D12_RESOURCE_BARRIER prepare_done[]{
            Transition(staged_color, read, D3D12_RESOURCE_STATE_COPY_DEST),
            Transition(staged_motion, read, D3D12_RESOURCE_STATE_COPY_DEST),
            Transition(slot.motion.Get(), write, read),
            Transition(slot.depth.Get(), write, read),
            Transition(slot.color.Get(), write, read),
        };
        list->ResourceBarrier(5, prepare_done);

        NVSDK_NGX_D3D12_DLSS_Eval_Params eval{};
        eval.Feature.pInColor = slot.color.Get();
        eval.Feature.pInOutput = slot.output.Get();
        eval.pInDepth = slot.depth.Get();
        eval.pInMotionVectors = slot.motion.Get();
        eval.InRenderSubrectDimensions = {render.size.width, render.size.height};
        eval.InJitterOffsetX = jitter_x;
        eval.InJitterOffsetY = jitter_y;
        eval.InMVScaleX = 1;
        eval.InMVScaleY = 1;
        eval.InPreExposure = 1;
        eval.InExposureScale = 1;
        eval.InReset = reset ? 1 : 0;
        const auto result = NGX_D3D12_EVALUATE_DLSS_EXT(list, feature_, parameters_, &eval);
        if (NVSDK_NGX_FAILED(result)) {
            LogResult("DLSS evaluation failed. Using stock TAA for this configuration", result);
            list->Close();
            rejected_ = requested;
            return false;
        }

        D3D12_RESOURCE_BARRIER pack_barriers[]{
            Transition(slot.output.Get(), write, read),
            Transition(frame->history.Get(), read, write),
        };
        list->ResourceBarrier(2, pack_barriers);
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(pack_root_.Get());
        list->SetPipelineState(pack_.Get());
        list->SetComputeRootDescriptorTable(0, Gpu(slot, 6));
        const UINT pack_settings[]{sharpening, history.x, history.y, history.size.width, history.size.height};
        list->SetComputeRoot32BitConstants(1, 5, pack_settings, 0);
        Dispatch(list, history.size);
        D3D12_RESOURCE_BARRIER done[]{
            Transition(slot.output.Get(), read, write),
            Transition(frame->history.Get(), write, read),
            Transition(slot.motion.Get(), read, write),
            Transition(slot.depth.Get(), read, write),
            Transition(slot.color.Get(), read, write),
        };
        list->ResourceBarrier(5, done);
        if (FAILED(list->Close())) {
            disabled_ = true;
            return false;
        }
        ID3D12CommandList* lists[]{list};
        execute(queue, 1, lists);
        slot.serial = serial_ + 1;
        feature_serial_ = slot.serial;
        Track(queue, frame);
        return true;
    }

private:
    struct EvalSlot {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        ComPtr<ID3D12DescriptorHeap> descriptors;
        ComPtr<ID3D12Resource> motion, depth, color, output;
        UINT64 serial = 0;
    };
    std::array<EvalSlot, 3> slots_{};
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12Fence> fence_;
    ComPtr<ID3D12RootSignature> prepare_root_, pack_root_;
    ComPtr<ID3D12PipelineState> prepare_, pack_;
    NVSDK_NGX_Parameter* parameters_ = nullptr;
    NVSDK_NGX_Parameter* capabilities_ = nullptr;
    NVSDK_NGX_Handle* feature_ = nullptr;
    std::deque<std::pair<UINT64, std::shared_ptr<Frame>>> in_flight_;
    UINT64 serial_ = 0, feature_serial_ = 0;
    UINT increment_ = 0;
    bool ngx_initialized_ = false, disabled_ = false;
    FeatureSize feature_size_, rejected_;

    bool Pipeline(bool prepare, const void* code, size_t size, ID3D12RootSignature** signature,
                  ID3D12PipelineState** pipeline) {
        const UINT descriptors = prepare ? 3 : 1;
        const D3D12_DESCRIPTOR_RANGE ranges[]{
            {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, descriptors, 0, 0, 0},
            {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, descriptors, 0, 0, descriptors},
        };
        D3D12_ROOT_PARAMETER root[4]{};
        const UINT table = prepare ? 2 : 0;
        if (prepare) {
            root[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            root[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            root[1].Descriptor.ShaderRegister = 1;
            root[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            root[3].Constants = {2, 0, 4};
        } else {
            root[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            root[1].Constants.Num32BitValues = 5;
        }
        root[table].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        root[table].DescriptorTable = {2, ranges};
        const D3D12_ROOT_SIGNATURE_DESC desc{prepare ? 4u : 2u, root, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> blob;
        if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr)) ||
            FAILED(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(signature)))) {
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = *signature;
        pso.CS = {code, size};
        return SUCCEEDED(device_->CreateComputePipelineState(&pso, IID_PPV_ARGS(pipeline)));
    }

    bool Initialize() {
        const wchar_t* search_path = addon_directory.c_str();
        NVSDK_NGX_FeatureCommonInfo info{};
        info.PathListInfo = {&search_path, 1};
        auto result = NVSDK_NGX_D3D12_Init_with_ProjectID("6ebfc817-214f-42ac-89b5-080a633d88e0",
            NVSDK_NGX_ENGINE_TYPE_CUSTOM, "0.5", addon_directory.c_str(), device_.Get(), &info);
        if (NVSDK_NGX_FAILED(result)) {
            LogResult("NGX initialization failed", result);
            return false;
        }
        ngx_initialized_ = true;
        result = NVSDK_NGX_D3D12_GetCapabilityParameters(&capabilities_);
        int available = 0;
        if (capabilities_) capabilities_->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available);
        if (NVSDK_NGX_FAILED(result) || !available) {
            LogResult("NGX reports DLSS unavailable", result);
            return false;
        }
        result = NVSDK_NGX_D3D12_AllocateParameters(&parameters_);
        if (NVSDK_NGX_FAILED(result) ||
            !Pipeline(true, kPrepareCS, sizeof(kPrepareCS), prepare_root_.GetAddressOf(), prepare_.GetAddressOf()) ||
            !Pipeline(false, kPackCS, sizeof(kPackCS), pack_root_.GetAddressOf(), pack_.GetAddressOf())) {
            LogResult("DLAA pipeline setup failed", result);
            return false;
        }
        increment_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8,
                                            D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        for (auto& slot : slots_) {
            if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator))) ||
                FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(), nullptr,
                                                  IID_PPV_ARGS(&slot.list))) ||
                FAILED(slot.list->Close()) ||
                FAILED(device_->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&slot.descriptors)))) {
                reshade::log::message(reshade::log::level::info, "[Dirt5DLAA] Could not allocate DLAA resources.");
                return false;
            }
        }
        return true;
    }

    bool CreateFeature(ID3D12CommandQueue* queue, ExecuteLists execute, FeatureSize size) {
        using Quality = NVSDK_NGX_PerfQuality_Value;
        Quality quality = NVSDK_NGX_PerfQuality_Value_DLAA;
        const char* preset_parameter = NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA;
        Extent maximum = size.input;
        if (size.input != size.output) {
            struct Mode { Quality quality; const char* preset; };
            constexpr Mode modes[]{
                {NVSDK_NGX_PerfQuality_Value_MaxQuality, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality},
                {NVSDK_NGX_PerfQuality_Value_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced},
                {NVSDK_NGX_PerfQuality_Value_MaxPerf, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance},
                {NVSDK_NGX_PerfQuality_Value_UltraPerformance, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance},
            };
            double best = 1e30;
            for (const auto& mode : modes) {
                Extent optimal{}, high{}, low{};
                float sharpness = 0;
                const auto result = NGX_DLSS_GET_OPTIMAL_SETTINGS(capabilities_, size.output.width, size.output.height,
                    mode.quality, &optimal.width, &optimal.height, &high.width, &high.height,
                    &low.width, &low.height, &sharpness);
                if (NVSDK_NGX_FAILED(result) || !optimal.width || !optimal.height ||
                    size.input.width < low.width || size.input.height < low.height ||
                    size.input.width > high.width || size.input.height > high.height) continue;
                const double distance = std::abs(double(size.input.width) / optimal.width - 1) +
                    std::abs(double(size.input.height) / optimal.height - 1);
                if (distance >= best) continue;
                best = distance;
                quality = mode.quality;
                preset_parameter = mode.preset;
                maximum = high;
            }
            if (quality == NVSDK_NGX_PerfQuality_Value_DLAA) {
                char line[160];
                std::snprintf(line, sizeof(line), "[Dirt5DLAA] NGX does not support %ux%u -> %ux%u. Using stock TAA.",
                    size.input.width, size.input.height, size.output.width, size.output.height);
                reshade::log::message(reshade::log::level::info, line);
                return false;
            }
        }
        if (feature_) {
            NVSDK_NGX_D3D12_ReleaseFeature(feature_);
            feature_ = nullptr;
        }
        constexpr auto state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        constexpr auto flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        for (auto& slot : slots_) {
            if (!Texture(device_.Get(), DXGI_FORMAT_R16G16_FLOAT, state, flags, slot.motion.ReleaseAndGetAddressOf(), maximum) ||
                !Texture(device_.Get(), DXGI_FORMAT_R32_FLOAT, state, flags, slot.depth.ReleaseAndGetAddressOf(), maximum) ||
                !Texture(device_.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, state, flags, slot.color.ReleaseAndGetAddressOf(), maximum) ||
                !Texture(device_.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, state, flags, slot.output.ReleaseAndGetAddressOf(), size.output)) {
                reshade::log::message(reshade::log::level::info, "[Dirt5DLAA] Could not allocate DLSS textures.");
                return false;
            }
        }
        auto& slot = slots_[0];
        if (FAILED(slot.allocator->Reset()) || FAILED(slot.list->Reset(slot.allocator.Get(), nullptr))) return false;
        NVSDK_NGX_DLSS_Create_Params create{};
        create.Feature.InWidth = maximum.width;
        create.Feature.InHeight = maximum.height;
        create.Feature.InTargetWidth = size.output.width;
        create.Feature.InTargetHeight = size.output.height;
        create.Feature.InPerfQualityValue = quality;
        create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
            NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted |
            NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
        parameters_->Set(preset_parameter, static_cast<unsigned>(size.preset));
        const auto result = NGX_D3D12_CREATE_DLSS_EXT(slot.list.Get(), 1, 1, &feature_, parameters_, &create);
        if (FAILED(slot.list->Close()) || NVSDK_NGX_FAILED(result)) {
            LogResult("NGX DLSS feature creation failed", result);
            if (feature_) NVSDK_NGX_D3D12_ReleaseFeature(std::exchange(feature_, nullptr));
            return false;
        }
        ID3D12CommandList* lists[]{slot.list.Get()};
        execute(queue, 1, lists);
        slot.serial = serial_ + 1;
        if (FAILED(queue->Signal(fence_.Get(), slot.serial))) {
            disabled_ = true;
            return false;
        }
        ++serial_;
        feature_serial_ = serial_;
        feature_size_ = size;
        char line[200];
        std::snprintf(line, sizeof(line), "[Dirt5DLAA] NGX %s initialized %ux%u -> %ux%u, quality=%d, preset hint=%d.",
            quality == NVSDK_NGX_PerfQuality_Value_DLAA ? "DLAA" : "DLSS", size.input.width, size.input.height,
            size.output.width, size.output.height, int(quality), int(size.preset));
        reshade::log::message(reshade::log::level::info, line);
        return true;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(const EvalSlot& slot, UINT index) const {
        return {slot.descriptors->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(index) * increment_};
    }
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(const EvalSlot& slot, UINT index) const {
        return {slot.descriptors->GetGPUDescriptorHandleForHeapStart().ptr + UINT64(index) * increment_};
    }
    void PopulateDescriptors(const EvalSlot& slot, const Frame& frame) {
        auto srv = [&](UINT index, ID3D12Resource* image, DXGI_FORMAT format) {
            D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
            desc.Format = format;
            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            desc.Texture2D.MipLevels = 1;
            device_->CreateShaderResourceView(image, &desc, Cpu(slot, index));
        };
        auto uav = [&](UINT index, ID3D12Resource* image, DXGI_FORMAT format) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
            desc.Format = format;
            desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            device_->CreateUnorderedAccessView(image, nullptr, &desc, Cpu(slot, index));
        };
        srv(0, frame.depth.Get(), DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS);
        srv(1, frame.staging->motion.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
        srv(2, frame.staging->color.Get(), DXGI_FORMAT_R11G11B10_FLOAT);
        uav(3, slot.motion.Get(), DXGI_FORMAT_R16G16_FLOAT);
        uav(4, slot.depth.Get(), DXGI_FORMAT_R32_FLOAT);
        uav(5, slot.color.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
        srv(6, slot.output.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
        uav(7, frame.history.Get(), DXGI_FORMAT_R11G11B10_FLOAT);
    }
};
}  // namespace dirt5
