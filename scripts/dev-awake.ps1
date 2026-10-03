<#
.SYNOPSIS
    Development mode: keeps the VIVE Flow awake while it is not worn and gives SteamVR a 30 min
    idle timeout. Run with -Off to restore normal behaviour.

.DESCRIPTION
    1. Flow: the OEM service (vive.wave.vr.oem, MiaCProximityEventHandler) calls
       PowerManager.goToSleep ~5 s after the screen wakes whenever the proximity sensor says
       "not mounted". Android's own stay-awake settings cannot override that, and the OEM
       setting that disables it (miac_config/auto_shut_screen) needs a signature permission.

       Instead this script waits until the proximity sensor reports "mounted" (cover the sensor
       on the inner nose bridge with a finger, or put the Flow on), then puts Android's
       SensorService into RESTRICTED mode (`dumpsys sensorservice restrict`, a CTS hook the
       shell may use). No app receives sensor events any more, so the OEM service keeps
       believing the headset is worn. Head tracking is unaffected: it comes from the native SVR
       service, not from SensorManager. This state lives only in memory: -Off or a reboot of
       the Flow restores it.

    2. SteamVR: a headset lying still on the desk counts as idle, and after
       power.turnOffScreensTimeout SteamVR goes to standby (the picture turns black). Development
       mode sets it to 30 min; -Off sets it back to the normal 10 min that scripts\install.ps1
       applies. This one is saved in steamvr.vrsettings and survives reboots.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\dev-awake.ps1
    powershell -ExecutionPolicy Bypass -File scripts\dev-awake.ps1 -Off
#>
param(
    [switch]$Off,
    [int]$TimeoutSeconds = 60
)

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
# Any package name works; it only names the (non-existent) app still allowed to read sensors.
$WhitelistPackage = "veve.flow.devawake"
# SteamVR power.turnOffScreensTimeout in seconds; keep $NormalIdleTimeout in sync with install.ps1.
$DevIdleTimeout = 1800
$NormalIdleTimeout = 600
$HelperExe = Join-Path $Root "pc\flow_dashboard_helper\build\dist\flow_dashboard_helper.exe"

function Get-SensorMode { (adb shell dumpsys sensorservice | Select-String -Pattern '^Mode : ').Line }
function Get-Wakefulness { ((adb shell dumpsys power | Select-String -Pattern 'mWakefulness=').Line -split '=')[1].Trim() }

# SteamVR rewrites steamvr.vrsettings on exit, so while it runs the change goes through its
# settings API (the dashboard helper's --idle-timeout); otherwise the file is edited directly.
function Set-SteamVRIdleTimeout([int]$seconds) {
    if (Get-Process vrserver -ErrorAction SilentlyContinue) {
        if (-not (Test-Path $HelperExe)) { Write-Warning "Helper not built ($HelperExe); SteamVR idle timeout unchanged."; return }
        $process = Start-Process $HelperExe -ArgumentList "--idle-timeout", $seconds -Wait -PassThru
        if ($process.ExitCode -ne 0) { Write-Warning "Could not change the SteamVR idle timeout (see the helper log)."; return }
    }
    else {
        $vrPaths = Join-Path $env:LOCALAPPDATA "openvr\openvrpaths.vrpath"
        $settingsPath = Join-Path (@((Get-Content $vrPaths -Raw | ConvertFrom-Json).config)[0]) "steamvr.vrsettings"
        $settings = if (Test-Path $settingsPath) { [IO.File]::ReadAllText($settingsPath).TrimStart([char]0xFEFF) | ConvertFrom-Json } else { New-Object PSObject }
        if (-not $settings.power) { $settings | Add-Member -NotePropertyName power -NotePropertyValue (New-Object PSObject) }
        $settings.power | Add-Member -NotePropertyName turnOffScreensTimeout -NotePropertyValue ([double]$seconds) -Force
        [IO.File]::WriteAllText($settingsPath, ($settings | ConvertTo-Json -Depth 32), (New-Object System.Text.UTF8Encoding $false))
    }
    Write-Host "SteamVR idle timeout: $([int]($seconds / 60)) min"
}

$state = (adb get-state 2>$null)

if ($Off) {
    if ($state -eq "device") {
        adb shell dumpsys sensorservice enable | Out-Null
        Write-Host "Flow sensor service: $(Get-SensorMode) (sleeps again when not worn)"
    }
    else { Write-Warning "No Flow connected over ADB; its sensor state resets on reboot anyway." }
    Set-SteamVRIdleTimeout $NormalIdleTimeout
    return
}

Set-SteamVRIdleTimeout $DevIdleTimeout

if ($state -ne "device") { throw "No Flow connected over ADB." }
if ((Get-SensorMode) -match 'RESTRICTED') {
    Write-Host "Flow already kept awake: $(Get-SensorMode)"
    return
}

adb logcat -c
Write-Host "Cover the proximity sensor (inner nose bridge) or put the Flow on..."
$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
$mounted = $false
while ((Get-Date) -lt $deadline) {
    $log = adb logcat -d -s MiaCProximityEventHandler:D
    $last = $log | Select-String -Pattern 'HMD (not )?mounted' | Select-Object -Last 1
    if ($last -and $last.Line -notmatch 'not mounted') { $mounted = $true; break }
    Start-Sleep -Milliseconds 300
}
if (-not $mounted) { throw "Timed out waiting for the proximity sensor to report 'mounted'." }

adb shell dumpsys sensorservice restrict $WhitelistPackage | Out-Null
Write-Host "Flow sensor service: $(Get-SensorMode)"
Write-Host "You can uncover the sensor now. Wakefulness: $(Get-Wakefulness)"
Write-Host "Restore with: scripts\dev-awake.ps1 -Off (the Flow part also resets when the Flow reboots)."
