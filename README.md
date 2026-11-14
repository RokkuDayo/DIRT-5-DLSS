# DIRT 5 DLSS v1.0

A ReShade add-on that adds NVIDIA DLSS Super Resolution and DLAA to the Windows DirectX 12 version of DIRT 5. It requires an NVIDIA GeForce RTX GPU.

## Install

1. Close DIRT 5. Install ReShade 6.8.0 with **full add-on support** for `DIRT5.exe`. In the installer, select **DirectX 10/11/12**. [ReShade downloads](https://reshade.me/)
2. Place the v1.0 `Dirt5DLSS.addon64` beside `DIRT5.exe`. If you downloaded the source archive, build the add-on using the instructions below. If you are upgrading from an older build named `Dirt5DLAA.addon64`, remove that copy from the game folder.
3. **Download NVIDIA’s `nvngx_dlss.dll` separately and manually copy it into the DIRT 5 game folder beside `DIRT5.exe` and `Dirt5DLSS.addon64`.** The release packages omit NVIDIA’s DLL. The tested version is **310.9.16**, from `lib/Windows_x86_64/rel/` in NVIDIA’s SDK. Use **Download raw file** on the linked page to get the Windows x64 release DLL. [NVIDIA DLSS runtime 310.9.16](https://github.com/NVIDIA/DLSS/blob/v310.9.16/lib/Windows_x86_64/rel/nvngx_dlss.dll)
4. Launch the game, press **Home** to open ReShade, and select **DIRT 5 DLSS**. Depending on your ReShade layout, it may first appear as a floating panel that you can dock to the tab bar. ReShade saves your choices automatically.

The game folder should contain these files:

```text
DIRT5.exe
Dirt5DLSS.addon64
nvngx_dlss.dll
```

## Configure

- **Anti-aliasing** selects automatic DLAA / DLSS, stock TAA, or the unfiltered RGB diagnostic. Restart the game after changing it.
- **NVIDIA model preset** offers NVIDIA default and presets J, K, L, and M. Changing the preset applies in-game and resets temporal history.
- **Sharpening** applies live. The range is 0–100%, and 0% disables it.
- **Trace jitter** enables diagnostic logging in stock TAA and unfiltered modes after a restart. The automatic mode always verifies jitter.

| In-game resolution settings | Automatic behavior |
| --- | --- |
| Render and History match | DLAA at the selected resolution |
| Render is smaller than History | DLSS Super Resolution for supported ratios |
| Render is larger than History | DLAA at Render Resolution, then downsampling into the history buffer |
| Final differs from History | DIRT 5 handles final scaling and UI composition |

For a first run, set **Render, History, and Final** to **1920 × 1080** and turn off **Dynamic Resolution**. To try upscaling, set **Render** to **1920 × 1080** and **History** and **Final** to **2560 × 1440** if the game offers those options. The 1080p native path was verified in-game. Unequal resolutions were exercised in standalone RTX 2070 tests, and continuous Dynamic Resolution still needs in-game validation.

Check `ReShade.log` beside the game executable. The `NGX DLAA initialized` or `NGX DLSS initialized` line reports the sizes and preset hint. Recurring `DLAA submitted` or `DLSS submitted` lines confirm that the feature is running. Unsupported ratios and unrecognized frame layouts fall back to stock TAA. If NGX fails to initialize, check the NVIDIA DLL filename and location from step 3.

## Build from source

Use an x64 Visual Studio Developer PowerShell with MSVC and DXC. The build uses ReShade `v6.8.0` with its Dear ImGui submodule initialized, RenoDX revision `43d58747ef7318392cf9385c95bb0ddfba0df09a`, NVIDIA DLSS SDK `v310.9.16`, and MinHook `v1.3.4`.

```powershell
.\build.ps1 -RenoDX C:\src\renodx -ReShade C:\src\reshade -NGX C:\src\DLSS -MinHook C:\src\minhook
```

The script writes `Dirt5DLSS.addon64` to the source folder. Install it using the steps above, including the separate NVIDIA DLL.

Third-party notices are in [licenses/](licenses/).
