param(
    [Parameter(Mandatory)][string]$RenoDX,
    [Parameter(Mandatory)][string]$ReShade,
    [Parameter(Mandatory)][string]$NGX,
    [Parameter(Mandatory)][string]$MinHook,
    [string]$Dxc = 'dxc.exe',
    [string]$BuildDir = (Join-Path $PSScriptRoot 'build')
)
$ErrorActionPreference = 'Stop'
$renodxPath = (Resolve-Path -LiteralPath $RenoDX).Path
$reshadePath = (Resolve-Path -LiteralPath $ReShade).Path
$ngxPath = (Resolve-Path -LiteralPath $NGX).Path
$minHookPath = (Resolve-Path -LiteralPath $MinHook).Path
$imguiPath = Join-Path $reshadePath 'deps\imgui'
if (!(Test-Path -LiteralPath (Join-Path $imguiPath 'imgui.h'))) {
    throw 'Initialize the Dear ImGui submodule in the ReShade source tree.'
}
$objectDir = (New-Item -ItemType Directory -Force $BuildDir).FullName
$ngxLib = Join-Path $ngxPath 'lib\Windows_x86_64\x64\nvsdk_ngx_s.lib'
if (!(Test-Path -LiteralPath $ngxLib)) {
    $ngxLib = (Get-ChildItem (Join-Path $ngxPath 'lib\Windows_x86_64') -Recurse -Filter nvsdk_ngx_s.lib | Select-Object -First 1).FullName
}
if (!$ngxLib) { throw 'The NVIDIA NGX static x64 library is required.' }

foreach ($shader in @(@('prepare', 'kPrepareCS'), @('pack', 'kPackCS'))) {
    & $Dxc -T cs_6_0 -E main -WX -Fh "$objectDir\$($shader[0]).inc" -Vn $shader[1] (Join-Path $PSScriptRoot "$($shader[0]).hlsl")
    if ($LASTEXITCODE -ne 0) { throw "DXC failed for $($shader[0]).hlsl." }
}

$hookObjects = foreach ($source in @('buffer', 'hook', 'trampoline', 'hde\hde64')) {
    $object = Join-Path $objectDir "$(Split-Path -Leaf $source).obj"
    & cl.exe /nologo /TC /O2 /MT /DNDEBUG "/Fo:$object" /c "$minHookPath\src\$source.c" | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "MinHook build failed for $source.c." }
    $object
}
& cl.exe /nologo /std:c++20 /EHsc /O2 /MT /W4 /WX /DNDEBUG /DWIN32_LEAN_AND_MEAN /DNOMINMAX "/I$reshadePath" "/I$imguiPath" "/I$renodxPath\src" "/I$objectDir" "/I$minHookPath\include" "/external:I$ngxPath\include" /external:W0 "/Fo:$objectDir\addon.obj" /LD (Join-Path $PSScriptRoot 'addon.cpp') $hookObjects $ngxLib d3d12.lib advapi32.lib user32.lib /link /MACHINE:X64 "/OUT:$PSScriptRoot\Dirt5DLSS.addon64" "/IMPLIB:$objectDir\Dirt5DLSS.lib"
if ($LASTEXITCODE -ne 0) { throw "C++ build failed with exit code $LASTEXITCODE." }

