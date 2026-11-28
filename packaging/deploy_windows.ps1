<#
.SYNOPSIS
    Builds MAVIRA_Core.exe and MAVIRA.exe (launcher) in x64-release, deploys the
    root master-folder layout (MAVIRA.exe + runtime/), runs windeployqt, strips
    unused Qt binaries, cleans build/deploy, and verifies total footprint (< 95 MB).
#>
[CmdletBinding()]
param(
    [string]$Configuration = "Release",
    [string]$Preset = "x64-release",
    [switch]$SkipBuild,
    [double]$MaxDeployedSizeMB = 95.0
)

$ErrorActionPreference = "Stop"

# Ensure no running MAVIRA instance locks MAVIRA.exe or runtime\MAVIRA_Core.exe
Stop-Process -Name "MAVIRA", "MAVIRA_Core" -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 300

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$BuildDir = Join-Path $RepoRoot "build\$Preset"
$DeployRoot = Join-Path $RepoRoot "build\deploy"
$RootLauncherExe = Join-Path $RepoRoot "MAVIRA.exe"
$RuntimeDir = Join-Path $RepoRoot "runtime"

function Import-VsDevEnvironment {
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
        return
    }
    $vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vsPath = $null
    if (Test-Path $vsWhere) {
        $vsPath = & $vsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
    }
    if (-not $vsPath) {
        $candidates = @(
            "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community",
            "${env:ProgramFiles}\Microsoft Visual Studio\2022\Professional",
            "${env:ProgramFiles}\Microsoft Visual Studio\2022\Enterprise",
            "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools"
        )
        $vsPath = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    }
    if (-not $vsPath) {
        throw "Visual Studio 2022 C++ toolchain not found."
    }

    $vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
    if (-not (Test-Path $vcvars)) {
        throw "vcvars64.bat not found at $vcvars"
    }

    Write-Host "Loading MSVC x64 environment from $vcvars..." -ForegroundColor Cyan
    $envDump = cmd.exe /c "`"$vcvars`" >nul 2>&1 && set"
    foreach ($line in $envDump) {
        if ($line -match "^([^=]+)=(.*)$") {
            [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2], "Process")
        }
    }

    if (-not $env:VCPKG_ROOT) {
        $vsVcpkg = Join-Path $vsPath "VC\vcpkg"
        if (Test-Path $vsVcpkg) {
            $env:VCPKG_ROOT = $vsVcpkg
        }
    }
}

Import-VsDevEnvironment

$cmakeExe = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmakeExe) {
    throw "cmake.exe not found in PATH even after loading VS environment."
}

if (-not $SkipBuild) {
    Write-Host "Configuring CMake preset '$Preset'..." -ForegroundColor Cyan
    & $cmakeExe --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }

    Write-Host "Building MAVIRA_Core.exe and MAVIRA.exe ($Configuration)..." -ForegroundColor Cyan
    & $cmakeExe --build --preset $Preset --target MAVIRA MAVIRA_Launcher --parallel
    if ($LASTEXITCODE -ne 0) { throw "CMake build failed." }
}

$CoreExePath = Join-Path $BuildDir "MAVIRA_Core.exe"
$LauncherExePath = Join-Path $BuildDir "MAVIRA.exe"
if (-not (Test-Path $CoreExePath)) {
    throw "MAVIRA_Core.exe not found at $CoreExePath"
}
if (-not (Test-Path $LauncherExePath)) {
    throw "MAVIRA.exe (launcher) not found at $LauncherExePath"
}

# Remove obsolete build/deploy leftovers
if (Test-Path $DeployRoot) {
    Write-Host "Cleaning obsolete build/deploy directory..." -ForegroundColor Cyan
    Remove-Item -Path $DeployRoot -Recurse -Force
}

# Clean and recreate runtime/ subfolder
if (Test-Path $RuntimeDir) {
    Remove-Item -Path $RuntimeDir -Recurse -Force
}
New-Item -ItemType Directory -Path $RuntimeDir -Force | Out-Null

# Copy root launcher MAVIRA.exe to master folder
Copy-Item -Path $LauncherExePath -Destination $RootLauncherExe -Force

# Copy MAVIRA_Core.exe, runtime DLLs from build output, assets, and license notices into runtime/
Copy-Item -Path $CoreExePath -Destination $RuntimeDir -Force
Get-ChildItem -Path $BuildDir -Filter "*.dll" | ForEach-Object {
    # Skip GoogleTest or debug-only DLLs
    if ($_.Name -notmatch "^(gtest|gmock).*\.dll$") {
        Copy-Item -Path $_.FullName -Destination $RuntimeDir -Force
    }
}

$AssetsSrc = Join-Path $RepoRoot "assets"
if (Test-Path $AssetsSrc) {
    $AssetsDst = Join-Path $RuntimeDir "assets"
    New-Item -ItemType Directory -Path $AssetsDst -Force | Out-Null
    $ShadersSrc = Join-Path $AssetsSrc "shaders"
    if (Test-Path $ShadersSrc) {
        Copy-Item -Path $ShadersSrc -Destination $AssetsDst -Recurse -Force
    }
    foreach ($brandAsset in @("logo.svg", "app.ico")) {
        $assetFile = Join-Path $AssetsSrc $brandAsset
        if (Test-Path $assetFile) {
            Copy-Item -Path $assetFile -Destination $AssetsDst -Force
        }
    }
}
foreach ($docFile in @("LICENSE", "THIRD_PARTY_NOTICES.txt")) {
    $docPath = Join-Path $RepoRoot $docFile
    if (Test-Path $docPath) {
        Copy-Item -Path $docPath -Destination $RuntimeDir -Force
    }
}

# Locate windeployqt.exe
$windeployqt = Get-ChildItem -Path @(
    (Join-Path $BuildDir "vcpkg_installed\x64-windows\tools\Qt6\bin\windeployqt.exe"),
    (Join-Path $RepoRoot "vcpkg_installed\x64-windows\tools\Qt6\bin\windeployqt.exe")
) -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName

if (-not $windeployqt) {
    $cmd = Get-Command windeployqt -ErrorAction SilentlyContinue
    if ($cmd) { $windeployqt = $cmd.Source }
}
if (-not $windeployqt) {
    throw "windeployqt.exe not found."
}

Write-Host "Running windeployqt on runtime\MAVIRA_Core.exe: $windeployqt" -ForegroundColor Cyan
& $windeployqt `
    --release `
    --no-translations `
    --no-compiler-runtime `
    --no-system-d3d-compiler `
    --no-opengl-sw `
    (Join-Path $RuntimeDir "MAVIRA_Core.exe")
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed." }

# Strip unused Qt DLLs and plugins to keep deployed footprint under 95 MB
$UnusedDllPatterns = @(
    "Qt6Network*.dll",
    "Qt6Qml*.dll",
    "Qt6Quick*.dll",
    "Qt6Pdf*.dll",
    "Qt6Svg*.dll",
    "Qt6VirtualKeyboard*.dll",
    "Qt6Sql*.dll",
    "Qt6Xml*.dll",
    "Qt6Test*.dll",
    "d3dcompiler_*.dll",
    "dxcompiler.dll",
    "dxil.dll",
    "opengl32sw.dll"
)
foreach ($pattern in $UnusedDllPatterns) {
    Get-ChildItem -Path $RuntimeDir -Filter $pattern -ErrorAction SilentlyContinue | Remove-Item -Force
}

$UnusedPluginDirs = @("tls", "networkinformation", "sqldrivers", "generic", "iconengines")
foreach ($dir in $UnusedPluginDirs) {
    $p = Join-Path $RuntimeDir $dir
    if (Test-Path $p) {
        Remove-Item -Path $p -Recurse -Force
    }
}

# Remove unneeded imageformat plugins (MAVIRA uses libjpeg-turbo/libpng/libwebp directly)
$ImageFormatsDir = Join-Path $RuntimeDir "imageformats"
if (Test-Path $ImageFormatsDir) {
    Get-ChildItem -Path $ImageFormatsDir -Filter "*.dll" | Where-Object {
        $_.Name -notin @("qico.dll", "qjpeg.dll")
    } | Remove-Item -Force
}

# Remove any PDBs/debug symbols from runtime
Get-ChildItem -Path $RuntimeDir -Recurse -Include "*.pdb", "*.ilk", "*.lib" | Remove-Item -Force

# Measure total deployed size (root MAVIRA.exe + runtime/)
$LauncherBytes = (Get-Item $RootLauncherExe).Length
$RuntimeBytes = (Get-ChildItem -Path $RuntimeDir -Recurse -File | Measure-Object -Property Length -Sum).Sum
$TotalBytes = $LauncherBytes + $RuntimeBytes
$TotalMB = [math]::Round($TotalBytes / 1MB, 2)
Write-Host "Deployed footprint (MAVIRA.exe + runtime/): $TotalMB MB ($TotalBytes bytes)" -ForegroundColor Green

if ($TotalMB -ge $MaxDeployedSizeMB) {
    throw "Deployed footprint ($TotalMB MB) exceeds limit of $MaxDeployedSizeMB MB!"
}
