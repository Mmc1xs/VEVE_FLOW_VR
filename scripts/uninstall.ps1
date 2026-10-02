<#
.SYNOPSIS
    Removes the VEVE_FLOW_VR registrations from SteamVR (driver + dashboard helper).

.DESCRIPTION
    Leaves Desktop+ installed. With -RestoreSettings, steamvr.vrsettings and Desktop+ config.ini
    are restored from the *.bak-veve backups made by install.ps1. With -RemoveApk, the Flow app
    is uninstalled over ADB.
#>
param(
    [switch]$RestoreSettings,
    [switch]$RemoveApk
)

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$OpenVr = Get-Content (Join-Path $env:LOCALAPPDATA "openvr\openvrpaths.vrpath") -Raw | ConvertFrom-Json
$Runtime = @($OpenVr.runtime)[0]
$VrPathReg = Join-Path $Runtime "bin\win64\vrpathreg.exe"
$HelperExe = Join-Path $Root "pc\flow_dashboard_helper\build\dist\flow_dashboard_helper.exe"

function Stop-SteamVR {
    Get-Process vrmonitor, vrserver, vrcompositor, vrdashboard, vrwebhelper, steamtours, steamvr_media_player,
        DesktopPlus, DesktopPlusUI, flow_dashboard_helper -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Seconds 2
}

Stop-SteamVR

# Unregistering the helper needs a running SteamVR.
if (Test-Path $HelperExe) {
    Start-Process -FilePath (Join-Path $Runtime "bin\win64\vrmonitor.exe") | Out-Null
    $deadline = (Get-Date).AddSeconds(60)
    while (-not (Get-Process vrserver -ErrorAction SilentlyContinue) -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 1 }
    Start-Sleep -Seconds 8
    Start-Process -FilePath $HelperExe -ArgumentList "--uninstall" -Wait | Out-Null
    Stop-SteamVR
    Write-Host "helper unregistered"
}

foreach ($registered in @($OpenVr.external_drivers)) {
    if ($registered -and (Split-Path $registered -Leaf) -eq "flowvr") {
        & $VrPathReg removedriver $registered | Out-Null
        Write-Host "driver unregistered: $registered"
    }
}

if ($RestoreSettings) {
    $steamvr = Join-Path @($OpenVr.config)[0] "steamvr.vrsettings"
    if (Test-Path "$steamvr.bak-veve") { Copy-Item "$steamvr.bak-veve" $steamvr -Force; Write-Host "restored $steamvr" }
    $steam = (Get-ItemProperty "HKCU:\Software\Valve\Steam" -ErrorAction SilentlyContinue).SteamPath
    if ($steam) {
        Select-String -Path (Join-Path $steam "steamapps\libraryfolders.vdf") -Pattern '"path"\s+"([^"]+)"' | ForEach-Object {
            $config = Join-Path ($_.Matches[0].Groups[1].Value -replace '\\\\', '\') "steamapps\common\DesktopPlus\config.ini"
            if (Test-Path "$config.bak-veve") { Copy-Item "$config.bak-veve" $config -Force; Write-Host "restored $config" }
        }
    }
}

if ($RemoveApk) {
    adb uninstall com.htc.vr.samples.wvr_flow_probe
}
