param([switch]$Test, [switch]$Run)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $projectRoot
try {
    New-Item -ItemType Directory -Force -Path 'build' | Out-Null
    & gcc -std=c11 -O3 -Wall -Wextra -Wpedantic -Werror -Iinclude src/main.c src/parallel.c src/raytrace.c -o build/raytrace.exe -lgdi32 -luser32 -lm
    if ($LASTEXITCODE -ne 0) { throw 'GCC build failed.' }
    Write-Host 'Built build/raytrace.exe'
    if ($Test) {
        & .\build\raytrace.exe --self-test
        if ($LASTEXITCODE -ne 0) { throw 'Renderer self-test failed.' }
    }
    if ($Run) {
        & .\build\raytrace.exe
        if ($LASTEXITCODE -ne 0) { throw 'Demo exited with an error.' }
    }
} finally {
    Pop-Location
}
