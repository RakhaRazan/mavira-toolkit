<#
.SYNOPSIS
    Packages MAVIRA release artifacts (Portable ZIP, Inno Setup Installer, and SHA256SUMS.txt)
    with strict cleanroom verification.
#>
[CmdletBinding()]
param(
    [string]$Version = "1.1.0"
)

$ErrorActionPreference = "Stop"

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$ReleaseDir = Join-Path $RepoRoot "release"
$StageDir = Join-Path $ReleaseDir "_stage_portable"
$ZipPath = Join-Path $ReleaseDir "MAVIRA-v$Version-Windows-x64-Portable.zip"
$InstallerIss = Join-Path $PSScriptRoot "installer.iss"
$InstallerExe = Join-Path $ReleaseDir "MAVIRA-Setup-x64-v$Version.exe"
$ShaSumsPath = Join-Path $ReleaseDir "SHA256SUMS.txt"

$RootExe = Join-Path $RepoRoot "MAVIRA.exe"
$RuntimeDir = Join-Path $RepoRoot "runtime"
$CoreExe = Join-Path $RuntimeDir "MAVIRA_Core.exe"

if (-not (Test-Path $RootExe) -or -not (Test-Path $CoreExe)) {
    throw "Missing MAVIRA.exe or runtime\MAVIRA_Core.exe. Run packaging\deploy_windows.ps1 first."
}

New-Item -ItemType Directory -Path $ReleaseDir -Force | Out-Null

# Remove stale release artifacts for this version
Get-ChildItem -Path $ReleaseDir -Filter "MAVIRA-*v$Version*" -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force
if (Test-Path $StageDir) {
    Remove-Item -Path $StageDir -Recurse -Force
}

# 1. Stage Portable ZIP
Write-Host "Staging Portable ZIP in $StageDir..." -ForegroundColor Cyan
New-Item -ItemType Directory -Path $StageDir -Force | Out-Null
Copy-Item -Path $RootExe -Destination $StageDir -Force
Copy-Item -Path $RuntimeDir -Destination (Join-Path $StageDir "runtime") -Recurse -Force
foreach ($doc in @("LICENSE", "THIRD_PARTY_NOTICES.txt")) {
    $docPath = Join-Path $RepoRoot $doc
    if (Test-Path $docPath) {
        Copy-Item -Path $docPath -Destination $StageDir -Force
    }
}

# Strict cleanroom guard on staged folder
$ForbiddenItems = Get-ChildItem -Path $StageDir -Recurse -Force | Where-Object {
    $_.Name -in @("data", "logs", "V", "#", "settings.json", "Rar.exe") -or
    $_.Extension -in @(".db", ".db-wal", ".db-shm", ".pack", ".pdb")
}
if ($ForbiddenItems) {
    $names = ($ForbiddenItems | ForEach-Object { $_.FullName }) -join "; "
    Remove-Item -Path $StageDir -Recurse -Force
    throw "CLEANROOM GUARD ABORTED: Forbidden files/folders found in stage: $names"
}

Write-Host "Compressing Portable ZIP: $ZipPath..." -ForegroundColor Cyan
Compress-Archive -Path (Join-Path $StageDir "*") -DestinationPath $ZipPath -CompressionLevel Optimal -Force
Remove-Item -Path $StageDir -Recurse -Force

# 2. Verify Portable ZIP entries via ZipFile::OpenRead
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::OpenRead($ZipPath)
try {
    $entryNames = @($zip.Entries | ForEach-Object { $_.FullName.Replace('\', '/') })
    foreach ($req in @("MAVIRA.exe", "runtime/MAVIRA_Core.exe", "LICENSE", "THIRD_PARTY_NOTICES.txt")) {
        if ($req -notin $entryNames) {
            throw "ZIP VERIFICATION FAILED: Missing required entry '$req'"
        }
    }
    $badEntries = $entryNames | Where-Object {
        $_ -match '(^|/)(data|logs|V|#)(/|$)' -or
        $_ -match '(^|/)settings\.json$' -or
        $_ -match '\.(db|db-wal|db-shm|pack|pdb)$'
    }
    if ($badEntries) {
        throw "ZIP VERIFICATION FAILED: Forbidden entries found: $($badEntries -join ', ')"
    }
    Write-Host "ZIP Cleanroom Verification PASSED ($($entryNames.Count) entries)." -ForegroundColor Green
} finally {
    $zip.Dispose()
}

# 3. Build Inno Setup Installer (if ISCC.exe is available or installable via winget)
function Find-Iscc {
    $cmd = Get-Command iscc -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    foreach ($p in @(
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "${env:ProgramFiles}\Inno Setup 6\ISCC.exe",
        "${env:LOCALAPPDATA}\Programs\Inno Setup 6\ISCC.exe"
    )) {
        if (Test-Path $p) { return $p }
    }
    return $null
}

$isccExe = Find-Iscc
if (-not $isccExe -and (Get-Command winget -ErrorAction SilentlyContinue)) {
    Write-Host "ISCC.exe not found; attempting silent install via winget..." -ForegroundColor Yellow
    & winget install --id JRSoftware.InnoSetup -e --accept-package-agreements --accept-source-agreements
    $isccExe = Find-Iscc
}

if ($isccExe) {
    Write-Host "Running Inno Setup compiler: $isccExe" -ForegroundColor Cyan
    & $isccExe $InstallerIss
    if ($LASTEXITCODE -ne 0) {
        throw "ISCC.exe failed with exit code $LASTEXITCODE"
    }
    if (-not (Test-Path $InstallerExe)) {
        throw "Expected installer not found at $InstallerExe"
    }
    $instMB = [math]::Round((Get-Item $InstallerExe).Length / 1MB, 2)
    if ($instMB -ge 50.0) {
        throw "Installer size ($instMB MB) exceeds 50 MB limit!"
    }
    Write-Host "Installer built: $InstallerExe ($instMB MB)" -ForegroundColor Green
} else {
    Write-Warning "ISCC.exe unavailable; skipping Inno Setup installer build."
}

# 4. Generate SHA256SUMS.txt for all v$Version .zip and .exe artifacts in release/
$artifacts = Get-ChildItem -Path $ReleaseDir -File | Where-Object {
    $_.Name -like "*v$Version*" -and $_.Extension -in @(".zip", ".exe")
} | Sort-Object Name

$sumLines = foreach ($f in $artifacts) {
    $hash = (Get-FileHash -Path $f.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $($f.Name)"
}
$sumLines | Set-Content -Path $ShaSumsPath -Encoding ASCII

Write-Host "`nGenerated Release Artifacts:" -ForegroundColor Cyan
foreach ($f in $artifacts) {
    $mb = [math]::Round($f.Length / 1MB, 2)
    Write-Host ("  {0} : {1} MB ({2} bytes)" -f $f.Name, $mb, $f.Length)
}
Write-Host "`nSHA256SUMS.txt:" -ForegroundColor Cyan
Get-Content $ShaSumsPath | ForEach-Object { Write-Host "  $_" }
