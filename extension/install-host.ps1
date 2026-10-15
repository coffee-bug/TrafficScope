# TrafficScope Native Messaging host register (dev helper)
#
# Registers extension/host/com.trafficscope.site.json with Chrome and Edge so the
# browser is allowed to launch TrafficScopeHost.exe.
#
# Usage (normal user, HKCU - no admin needed):
#   powershell -ExecutionPolicy Bypass -File install-host.ps1
#
# Machine-wide (requires an elevated shell):
#   powershell -ExecutionPolicy Bypass -File install-host.ps1 -System
#
# Custom host exe location:
#   powershell -ExecutionPolicy Bypass -File install-host.ps1 -ExePath "D:\dev\TrafficScopeHost.exe"

param(
    [string]$ExePath = "",
    [switch]$System
)

$ErrorActionPreference = 'Stop'

$here = Split-Path -Parent $MyInvocation.MyCommand.Definition
$hostName = 'com.trafficscope.site'

# ---- 1. locate TrafficScopeHost.exe ----
if ($ExePath -eq "") {
    $candidates = @(
        (Join-Path (Split-Path -Parent $here) 'app\build\Release\TrafficScopeHost.exe'),
        (Join-Path $env:LOCALAPPDATA 'TrafficScope\TrafficScopeHost.exe'),
        'C:\Program Files\TrafficScope\TrafficScopeHost.exe'
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { $ExePath = $c; break }
    }
}
if ($ExePath -eq "" -or -not (Test-Path $ExePath)) {
    Write-Host "ERROR: TrafficScopeHost.exe not found. Pass -ExePath <full path>."
    exit 1
}
$ExePath = (Resolve-Path $ExePath).Path
Write-Host "host exe: $ExePath"

# ---- 2. write the manifest (path must be absolute; file must be on a local disk) ----
$outDir = Join-Path $env:LOCALAPPDATA 'TrafficScope\native-host'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$manifestPath = Join-Path $outDir "$hostName.json"

$manifest = @{
    name            = $hostName
    description     = 'TrafficScope site attribution bridge (browser extension -> local app)'
    path            = $ExePath
    type            = 'stdio'
    allowed_origins = @('chrome-extension://phdnbcbpkhpbodccdkncbpkfdlmakmcf/')
}
# ConvertTo-Json writes UTF-8 without BOM in PS7 - that is what Chrome expects.
$json = $manifest | ConvertTo-Json -Depth 4
[System.IO.File]::WriteAllText($manifestPath, $json, (New-Object System.Text.UTF8Encoding($false)))
Write-Host "manifest: $manifestPath"

# ---- 3. registry ----
$roots = @()
if ($System) {
    $roots += 'HKLM:\SOFTWARE\Google\Chrome\NativeMessagingHosts'
    $roots += 'HKLM:\SOFTWARE\Microsoft\Edge\NativeMessagingHosts'
} else {
    $roots += 'HKCU:\SOFTWARE\Google\Chrome\NativeMessagingHosts'
    $roots += 'HKCU:\SOFTWARE\Microsoft\Edge\NativeMessagingHosts'
}

foreach ($root in $roots) {
    $key = Join-Path $root $hostName
    New-Item -Path $key -Force | Out-Null
    New-ItemProperty -Path $key -Name '(Default)' -Value $manifestPath -PropertyType String -Force | Out-Null
    Write-Host "registered: $key"
}

Write-Host ""
Write-Host "DONE. Now load the extension itself:"
Write-Host "  chrome://extensions -> Developer mode -> Load unpacked -> select this folder"
Write-Host "  The ID must be phdnbcbpkhpbodccdkncbpkfdlmakmcf (pinned by manifest.json 'key')."
