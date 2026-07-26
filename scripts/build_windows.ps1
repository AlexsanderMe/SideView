param(
    [Parameter(Mandatory = $true)]
    [string]$WebView2SdkDir,
    [string]$CMakeExe = "cmake",
    [string]$BuildDir = "build\native-x64",
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"

if (-not (Test-Path $vswhere)) {
    throw "vswhere not found. Install Visual Studio Build Tools."
}

try {
    $resolvedCMake = (Get-Command -Name $CMakeExe -ErrorAction Stop).Source
}
catch {
    throw "CMake executable '$CMakeExe' was not found."
}

if (-not (Test-Path $WebView2SdkDir)) {
    throw "WebView2 SDK not found at $WebView2SdkDir."
}

$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) {
    throw "Visual Studio Build Tools with MSVC x86/x64 tools was not found."
}

$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) {
    throw "vcvars64.bat not found at $vcvars."
}

$configure = @(
    "set Path="
    "call `"$vcvars`""
    "`"$resolvedCMake`" -S native -B `"$BuildDir`" -G `"Visual Studio 17 2022`" -A x64 -DWEBVIEW2_SDK_DIR=`"$WebView2SdkDir`""
) -join " && "

$build = @(
    "set Path="
    "call `"$vcvars`""
    "`"$resolvedCMake`" --build `"$BuildDir`" --config `"$Configuration`""
) -join " && "

Push-Location $repoRoot
try {
    & $env:ComSpec /d /s /c $configure
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configure failed with exit code $LASTEXITCODE."
    }

    & $env:ComSpec /d /s /c $build
    if ($LASTEXITCODE -ne 0) {
        throw "CMake build failed with exit code $LASTEXITCODE."
    }

    $dll = Join-Path $BuildDir "$Configuration\sideview_native.dll"
    $destination = "src\sideview\sideview_native.dll"
    Copy-Item -LiteralPath $dll -Destination $destination -Force
    Write-Host "Built and copied $destination"
}
finally {
    Pop-Location
}
