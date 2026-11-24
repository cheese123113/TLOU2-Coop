# Removes the co-op winmm.dll and restores the one install.ps1 backed up (e.g. Twelve Weeks).
param(
    [string]$GameDir = "C:\Program Files (x86)\Steam\steamapps\common\The Last of Us Part II"
)
$ErrorActionPreference = "Stop"
if (Get-Process tlou-ii -ErrorAction SilentlyContinue) { throw "The game is running - close it first." }

$target = Join-Path $GameDir "winmm.dll"
$backup = Join-Path $GameDir "tlou2-coop-backup\winmm.dll"

if (Test-Path $target) {
    $text = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($target))
    if (-not $text.Contains("tlou2-coop.log")) { throw "The installed winmm.dll is not the co-op build - leaving it alone." }
    Remove-Item $target
    Write-Host "Removed the co-op winmm.dll"
}
if (Test-Path $backup) {
    Copy-Item $backup $target
    Write-Host "Restored $backup -> $target"
} else {
    Write-Host "No backup found; the game now runs without a winmm.dll mod."
}
