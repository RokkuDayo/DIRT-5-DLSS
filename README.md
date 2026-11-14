# DIRT 5 DLSS v1.3

A ReShade add-on for replacing the game's TAA with NVIDIA DLSS/DLAA, based on RenoDX.

An option for enabling a port of the motion blur shader from Driveclub during gameplay is also available.

An NVIDIA GeForce RTX GPU is required.

## Install

1. Install ReShade 6.8.0 with **full add-on support** for `DIRT5.exe`. In the installer, select **DirectX 10/11/12**. [ReShade downloads](https://reshade.me/)
2. Place the v1.1 `Dirt5DLSS.addon64` beside `DIRT5.exe`. You can find it in the Releases page.
3. **Download NVIDIA’s `nvngx_dlss.dll` separately and manually copy it into the DIRT 5 game folder beside `DIRT5.exe` and `Dirt5DLSS.addon64`.** The tested version is **310.9.16**, from `lib/Windows_x86_64/rel/` in NVIDIA’s SDK. Use **Download raw file** on the linked page to get the Windows x64 release DLL. [NVIDIA DLSS runtime 310.9.16](https://github.com/NVIDIA/DLSS/blob/v310.9.16/lib/Windows_x86_64/rel/nvngx_dlss.dll)
4. Launch the game, press **Home** to open ReShade, and select **DIRT 5 DLSS** to confirm it is fully functional. Depending on your ReShade layout, it may first appear as a floating panel that you can dock to the tab bar. ReShade saves your choices automatically.

## Configuration

- **NVIDIA model preset** offers NVIDIA default and presets J, K, L, and M. Changing the preset applies in-game and resets temporal history.
- **Sharpening** applies live. The range is 0–100%, and 0% disables it.
- **Trace jitter** enables diagnostic logging in stock TAA and unfiltered modes after a restart. The automatic mode always verifies jitter.

| In-game resolution settings | Automatic behavior |
| --- | --- |
| Render and History match | DLAA at the selected resolution |
| Render is smaller than History | DLSS Super Resolution for supported ratios |
| Render is larger than History | DLAA at Render Resolution, then downsampling into the history buffer |
| Final differs from History | DIRT 5 handles final scaling and UI composition |

For a first run on a 1080p monitor, set **Render, History, and Final** to **1920 × 1080** and turn off **Dynamic Resolution**. To try upscaling, set **Render** to **1920 × 1080** and **History** and **Final** to **2560 × 1440** if the game offers those options

Check `ReShade.log` beside the game executable. The `NGX DLAA initialized` or `NGX DLSS initialized` line reports the sizes and preset hint. Recurring `DLAA submitted` or `DLSS submitted` lines confirm that the feature is running. Unsupported ratios and unrecognized frame layouts fall back to stock TAA. If NGX fails to initialize, check the NVIDIA DLL filename and location from step 3.

## Gameplay motion blur

In the **DIRT 5 DLSS** tab, choose **DriveClub 30 FPS exposure** or **Short 60 FPS exposure** under **Gameplay motion blur**. The setting applies live in automatic DLAA / DLSS mode and defaults to **Off**. The motion-vector debug view takes priority when enabled.

This implementation is based on the one found in the [Driveclub Recomp](https://github.com/OverkillLabs3/driveclub-recomp/pull/216/files).

## Inspect motion vectors

In the **DIRT 5 DLSS** tab, enable **Show DLSS motion vectors** while automatic DLAA / DLSS is active. The view shows the current-to-previous displacement supplied to NGX in render pixels. Gray means zero. Brighter red and green mean right and down, respectively, while darker red and green mean left and up. Blue increases with magnitude. Reduce **Motion range** to see small movements or increase it when the colors saturate. The view passes through the game's compositor and turns off on restart.

## Build from source

Use an x64 Visual Studio Developer PowerShell with MSVC and DXC. The build uses ReShade `v6.8.0` with its Dear ImGui submodule initialized, RenoDX revision `43d58747ef7318392cf9385c95bb0ddfba0df09a`, NVIDIA DLSS SDK `v310.9.16`, and MinHook `v1.3.4`.

```powershell
.\build.ps1 -RenoDX C:\src\renodx -ReShade C:\src\reshade -NGX C:\src\DLSS -MinHook C:\src\minhook
```

The script writes `Dirt5DLSS.addon64` to the source folder. Install it using the steps above, including the separate NVIDIA DLL.

Third-party notices are in [licenses/](licenses/).

# AI Usage Disclosure
Codex was used in this project to create the addon.
