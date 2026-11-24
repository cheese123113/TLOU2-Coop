# Installs the co-op winmm.dll into the game folder.
# The first install moves the existing winmm.dll (e.g. Twelve Weeks) into tlou2-coop-backup\
# so uninstall.ps1 can put it back. Twelve Weeks' t2r-ext.dll is left alone: only its own
# winmm.dll loads it, so it is inert while the co-op build is installed.
param(
    [string]$GameDir = "C:\Program Files (x86)\Steam\steamapps\common\The Last of Us Part II",
    [string]$Config  = "TO1Debug"
)
$ErrorActionPreference = "Stop"

$built = Join-Path $PSScriptRoot "build-ninja\bin\winmm.dll"
if ($Config -eq "TO1Release") { $built = Join-Path $PSScriptRoot "build-ninja-release\bin\winmm.dll" }
if (-not (Test-Path $built)) { throw "No build at $built - build first." }
if (-not (Test-Path (Join-Path $GameDir "tlou-ii.exe"))) { throw "tlou-ii.exe not found in $GameDir" }
if (Get-Process tlou-ii -ErrorAction SilentlyContinue) { throw "The game is running - close it first." }

$target = Join-Path $GameDir "winmm.dll"
$backupDir = Join-Path $GameDir "tlou2-coop-backup"

function Test-IsCoopDll([string]$path) {
    if (-not (Test-Path $path)) { return $false }
    $bytes = [System.IO.File]::ReadAllBytes($path)
    $text = [System.Text.Encoding]::ASCII.GetString($bytes)
    return $text.Contains("tlou2-coop.log")
}

if ((Test-Path $target) -and -not (Test-IsCoopDll $target)) {
    New-Item -ItemType Directory -Force $backupDir | Out-Null
    $dest = Join-Path $backupDir "winmm.dll"
    if (Test-Path $dest) {
        $dest = Join-Path $backupDir ("winmm-" + (Get-Date -Format "yyyyMMdd-HHmmss") + ".dll")
    }
    Move-Item $target $dest
    Write-Host "Backed up the existing winmm.dll to $dest"
}

Copy-Item $built $target -Force
Write-Host "Installed co-op winmm.dll ($Config) to $target"
Write-Host "Log file: $(Join-Path $GameDir 'tlou2-coop.log')   Settings: $(Join-Path $GameDir 'tlou2-coop.ini')"
