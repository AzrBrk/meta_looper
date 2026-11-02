param(
    [switch]$WithImGui,
    [switch]$NoDisplay,
    [switch]$CloseOnPaint,
    [string]$Compiler = 'G:/mingw64-16.2/bin/g++.exe',
    [string]$ImGuiRoot = 'G:/imgui-master'
)

$ErrorActionPreference = 'Stop'
if ($NoDisplay -and $CloseOnPaint) {
    throw 'Choose either -NoDisplay or -CloseOnPaint.'
}
$root = Split-Path $PSScriptRoot -Parent
$originalPath = $env:PATH
Push-Location $root
try {
    $env:PATH = (Split-Path $Compiler -Parent) + ';' + $originalPath
    New-Item -ItemType Directory -Force output | Out-Null
    $flags = @('-std=c++23', '-O2', '-Wall', '-Wextra', '-pedantic-errors',
               '-falign-functions=64', '-falign-loops=32', '-I.')
    & $Compiler @flags bench/winapi_ui_chain_bench.cpp -o output/winapi_ui_chain_bench.exe -luser32 -lgdi32
    if ($LASTEXITCODE -ne 0) { throw 'Main benchmark build failed.' }
    $programArguments = @()
    if ($NoDisplay) { $programArguments += '--no-display' }
    if ($CloseOnPaint) { $programArguments += '--close-on-paint' }
    & ./output/winapi_ui_chain_bench.exe @programArguments
    if ($LASTEXITCODE -ne 0) { throw 'Main benchmark failed.' }
    if ($WithImGui) {
        $sources = @('imgui.cpp', 'imgui_draw.cpp', 'imgui_tables.cpp', 'imgui_widgets.cpp',
                     'backends/imgui_impl_win32.cpp', 'backends/imgui_impl_dx11.cpp') |
            ForEach-Object { Join-Path $ImGuiRoot $_ }
        & $Compiler @flags "-I$ImGuiRoot" bench/imgui_frame_bench.cpp @sources -o output/imgui_frame_bench.exe -ld3d11 -ld3dcompiler -ldwmapi -lgdi32
        if ($LASTEXITCODE -ne 0) { throw 'ImGui benchmark build failed.' }
        & ./output/imgui_frame_bench.exe
        if ($LASTEXITCODE -ne 0) { throw 'ImGui benchmark failed.' }
    }
} finally {
    $env:PATH = $originalPath
    Pop-Location
}