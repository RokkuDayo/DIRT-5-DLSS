# DIRT 5 DLSS v0.9

This x64 ReShade add-on uses the NVIDIA DLSS SDK to evaluate DLAA and DLSS at sizes taken from the game. This update uses the NVIDIA-signed 310.9.16 release runtime from the v0.6 package.

v0.6 addresses the roughly one-second AA pulse observed in the menu. When the composite lacks its usual depth-state evidence, the add-on can reuse the depth proof from the immediately preceding TAA submission. It requires a confirmed depth-stencil input, an unchanged depth state for the rest of the TAA list, and adjacent submissions on the same direct queue. The jitter and motion-vector conversion are unchanged. The log reports cumulative fallbacks, resets, and recovered depth frames.

## Install and configure

1. Close DIRT 5. Use ReShade 6.8.0 x64 with full add-on support.
2. Remove the previous `Dirt5DLAA.addon64` from the game folder and copy in `Dirt5DLSS.addon64`. Keep `nvngx_dlss.dll` beside it. For a fresh installation, use the NVIDIA runtime from the v0.6 package.
3. Launch the game, press Home (or your ReShade overlay key), and open **DIRT 5 DLSS**. If your saved layout opens it as a floating panel, drag its title onto the ReShade tab bar.
4. On first load, the add-on imports settings from the old `Dirt5DLAA.ini` if present. After that launch, you can remove the old file. ReShade saves changes from the panel automatically.

- **Anti-aliasing** selects automatic DLAA/DLSS, stock TAA, or the unfiltered RGB diagnostic. Restart the game after changing mode.
- **Sharpening** applies live while DLAA or DLSS runs. The range is 0–100%, and 0% disables it. The filter runs on scene color before the game adds its final overlays.
- **NVIDIA model preset** offers NVIDIA default and presets J, K, L and M. Changing it requests a new NGX feature with fresh temporal history. The initialization line reports the requested preset hint.
- **Trace jitter** enables the upload cross-check in the stock TAA and unfiltered modes after a restart. DLAA always verifies jitter.

## Resolution settings

- **Render equals History** uses native-resolution DLAA.
- **Render is smaller than History** uses DLSS Super Resolution for ratios supported by the NVIDIA runtime. The add-on chooses the supported DLSS quality mode whose recommended input is closest to the game’s Render Resolution.
- **Render is larger than History** uses DLAA at Render Resolution and area resampling into the game’s history buffer.
- **Final Resolution** controls presentation through the game’s composite and scaling. NGX outputs at the history size when upscaling.

For the first unequal-resolution test, use 1920 × 1080 Render and 2560 × 1440 History and Final if the game offers those choices. Leave Dynamic Resolution off during that test. The add-on can recreate NGX when settings change, which resets its history and can briefly fall back to stock TAA. Continuous Dynamic Resolution has yet to be validated. Unsupported ratios and unrecognized frame layouts use stock TAA.

The `NGX DLSS initialized` or `NGX DLAA initialized` log line includes the input and NGX output sizes. Repeated `DLSS submitted #` or `DLAA submitted #` lines confirm that evaluation continues.

## Implementation

The add-on stages the unfiltered scene color and three-component displacement immediately before the captured TAA dispatch. It runs stock TAA and the original downsample. Before the next D3D12 submission begins the final composite, it checks the live history bindings and split barriers and runs a separate command list on the same direct queue. The compute prepass reconstructs current-to-previous motion in pixels from depth, displacement, and the game camera constants. It also converts the color and reversed-Z depth to NGX inputs. A second compute shader packs DLAA output into the game’s RGB history texture and restores its state for the final composite.

The stock pass provides its auxiliary outputs and a fallback whenever the guards fail. The same-frame downsample runs before DLAA. The RGB history is shared with the following frame’s stock pass. The initial exposure settings are NGX auto-exposure and a pre-exposure multiplier of 1. Reset is driven by skipped evaluations, history phase, and changes in camera position and direction. Live jitter sign, motion quality, exposure and camera cuts remain to be checked in the game.

## Build and verification

- RenoDX source revision `43d58747ef7318392cf9385c95bb0ddfba0df09a`, used for shader CRC32.
- ReShade source tag `v6.8.0`, add-on API 20, with its pinned Dear ImGui submodule initialized.
- NVIDIA DLSS SDK `v310.9.16` and MinHook `v1.3.4`.
- MSVC x64 14.44.35207 and Windows SDK / DXC 10.0.26100.0.

From an x64 Visual Studio Developer PowerShell with DXC on PATH, run

```powershell
.\build.ps1 -RenoDX C:\src\renodx -ReShade C:\src\reshade -NGX C:\src\DLSS -MinHook C:\src\minhook
```

The add-on loaded through the installed ReShade 6.8.0 loader and recorded staging against the captured TAA shader and root signature with synthetic resources. The NVIDIA RTX 2070 test installed the native hooks, observed the split barriers, created a native-resolution NGX feature, completed two evaluations with capture-derived constants, and read back packed RGB. The shader hash and jitter checks passed with both captures. v0.5 ran in the game. The v0.6 regression test exercised the new depth path with adjacent submissions on the RTX 2070 and confirmed stock fallback after an intervening submission. The menu pulse fix was confirmed in-game. v0.7 sharpening was verified with synthetic GPU inputs and an RTX 2070 NGX run. v0.8 loaded with the installed ReShade build. A separate callback test verified tab registration, legacy import, control updates, and reloading saved settings. v0.9 verified 720p to 1080p and 1080p to 1440p DLSS, DLAA with downsampling, model hint changes, and unsupported-ratio fallback on the RTX 2070. Unequal-resolution game settings still need a live race test.

This software uses the NVIDIA DLSS SDK. The NVIDIA runtime is subject to the accompanying NVIDIA RTX SDK license.







