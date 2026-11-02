param(
    [string]$Compiler = 'G:/mingw64-16.2/bin/g++.exe',
    [ValidateRange(1, 20)][int]$Repeats = 3
)

$ErrorActionPreference = 'Stop'
$originalPath = $env:PATH
Push-Location (Split-Path $PSScriptRoot -Parent)
try {
    $env:PATH = (Split-Path $Compiler -Parent) + ';' + $originalPath
    New-Item -ItemType Directory -Force output | Out-Null
    $flags = @('-std=c++23', '-O2', '-Wall', '-Wextra', '-Werror', '-pedantic-errors',
               '-falign-functions=64', '-falign-loops=32', '-I.')
    & $Compiler @flags bench/static_ui_compare_bench.cpp -o output/static_ui_compare_bench.exe -luser32 -lgdi32
    if ($LASTEXITCODE -ne 0) { throw 'Comparison build failed.' }
    for ($repeat = 1; $repeat -le $Repeats; ++$repeat) {
        Write-Output "Run $repeat/$Repeats"
        & ./output/static_ui_compare_bench.exe |
            Tee-Object -FilePath "output/static_ui_compare_$repeat.txt"
        if ($LASTEXITCODE -ne 0) { throw "Comparison run $repeat failed." }
    }
} finally {
    $env:PATH = $originalPath
    Pop-Location
}