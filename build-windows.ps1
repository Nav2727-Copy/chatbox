param(
    [ValidateSet("x64", "x86")]
    [string]$Architecture = "x64",
    [ValidateSet("debug", "release")]
    [string]$Configuration = "debug"
)

$ErrorActionPreference = "Stop"
$repoRoot = $PSScriptRoot
$preset = "$Architecture-$Configuration"

if (-not $env:VCPKG_ROOT) {
    $defaultVcpkg = Join-Path $HOME "vcpkg"
    if (Test-Path (Join-Path $defaultVcpkg "scripts/buildsystems/vcpkg.cmake")) {
        $env:VCPKG_ROOT = $defaultVcpkg
    } else {
        throw "Set VCPKG_ROOT to a valid vcpkg checkout."
    }
}
if (-not (Test-Path (Join-Path $env:VCPKG_ROOT "scripts/buildsystems/vcpkg.cmake"))) {
    throw "VCPKG_ROOT does not contain scripts/buildsystems/vcpkg.cmake: $env:VCPKG_ROOT"
}
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw "CMake is required. Run this from a Visual Studio developer PowerShell with CMake and Ninja available."
}

cmake --preset $preset
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$buildDir = Join-Path $repoRoot "out/build/$preset"
cmake --build $buildDir --parallel
exit $LASTEXITCODE
