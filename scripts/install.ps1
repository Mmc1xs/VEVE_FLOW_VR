<#
.SYNOPSIS
    Installs / re-applies the VEVE_FLOW_VR setup on this PC (safe to run repeatedly).

.DESCRIPTION
    1. Registers the SteamVR driver (pc\flow_steamvr_driver\build\dist\flowvr) with vrpathreg.
    2. Sets the SteamVR settings this project relies on (overlay render quality, 10 min idle standby).
    3. Applies the Desktop+ overlay / input settings (Desktop+ must be installed and run once).
    4. Registers the dashboard helper with SteamVR (auto-launch) - starts SteamVR briefly.
    5. Installs the Flow APK over ADB if a Flow is connected.

    Settings that live outside this repository are defined in the block below; change them
    here and re-run the script. Driver settings live in
    pc\flow_steamvr_driver\flowvr\resources\settings\default.vrsettings (rebuild to apply).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\install.ps1
#>
param(
    [switch]$SkipDriver,
    [switch]$SkipSteamVRSettings,
    [switch]$SkipDesktopPlus,
    [switch]$SkipHelper,
    [switch]$SkipApk
)

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

# ---- Settings outside this repository ---------------------------------------------------------
# Desktop+ [Overlay0] (the desktop shown in the Flow). Width/OffsetUp are in cm.
# DisplayMode 3 = "Only in Desktop+ Tab" (the dashboard helper opens that tab).
$DesktopPlusOverlay = [ordered]@{ Width = 248; OffsetUp = -22; DisplayMode = 3 }
# Desktop+ [Input]: no gaze-click key; the keypad controller provides the trigger.
$DesktopPlusInput = [ordered]@{ LaserPointerHMDKeyCodeLeft = 0 }
# steamvr.vrsettings, by section.
#   steamvr.overlayRenderQuality_2: 0 Auto, 1 Low, 2 Medium, 3 High.
#   power.turnOffScreensTimeout: seconds without head movement before SteamVR goes to standby
#     (SteamVR default 5). scripts\dev-awake.ps1 raises it to 30 min while developing.
#   power.pauseCompositorOnStandby: SteamVR default.
$SteamVRSettings = [ordered]@{
    steamvr = [ordered]@{ overlayRenderQuality_2 = 3 }
    power   = [ordered]@{ turnOffScreensTimeout = 600.0; pauseCompositorOnStandby = $true }
}
$FlowPackage = "com.htc.vr.samples.wvr_flow_probe"
# -------------------------------------------------------------------------------------------------

$DriverDir = Join-Path $Root "pc\flow_steamvr_driver\build\dist\flowvr"
$HelperExe = Join-Path $Root "pc\flow_dashboard_helper\build\dist\flow_dashboard_helper.exe"
$Apk = Join-Path $Root "Wave_Native_SDK\samples\wvr_flow_probe\app\build\outputs\apk\bit64\debug\app-bit64-debug.apk"

function Step($text) { Write-Host ""; Write-Host "==> $text" -ForegroundColor Cyan }
function Note($text) { Write-Host "    $text" }
function Warn($text) { Write-Host "    WARNING: $text" -ForegroundColor Yellow }
function Fail($text) { Write-Host "ERROR: $text" -ForegroundColor Red; exit 1 }
$Utf8NoBom = New-Object System.Text.UTF8Encoding $false

# SteamVR runtime and config locations, as registered by Steam.
$VrPaths = Join-Path $env:LOCALAPPDATA "openvr\openvrpaths.vrpath"
if (-not (Test-Path $VrPaths)) { Fail "SteamVR is not installed (missing $VrPaths). Install SteamVR from Steam and run it once." }
$OpenVr = Get-Content $VrPaths -Raw | ConvertFrom-Json
$Runtime = @($OpenVr.runtime)[0]
$ConfigDir = @($OpenVr.config)[0]
$VrPathReg = Join-Path $Runtime "bin\win64\vrpathreg.exe"
$VrMonitor = Join-Path $Runtime "bin\win64\vrmonitor.exe"

function Stop-SteamVR {
    Get-Process vrmonitor, vrserver, vrcompositor, vrdashboard, vrwebhelper, steamtours, steamvr_media_player,
        DesktopPlus, DesktopPlusUI, flow_dashboard_helper -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Seconds 2
}

function Set-IniValues([string[]]$lines, [string]$section, $values) {
    # Replaces existing "key=value" lines inside [section]; returns the new lines.
    $out = New-Object System.Collections.Generic.List[string]
    $inSection = $false
    $seen = @{}
    foreach ($line in $lines) {
        if ($line -match '^\s*\[(.+)\]\s*$') { $inSection = ($Matches[1] -eq $section) }
        elseif ($inSection -and $line -match '^([^=;]+)=') {
            $key = $Matches[1].Trim()
            if ($values.Contains($key)) { $line = "$key=$($values[$key])"; $seen[$key] = $true }
        }
        $out.Add($line)
    }
    foreach ($key in $values.Keys) {
        if (-not $seen[$key]) { throw "[$section] $key not found in Desktop+ config.ini" }
    }
    return $out.ToArray()
}

function Find-DesktopPlus {
    $libraries = @()
    $vdf = Join-Path ${env:ProgramFiles(x86)} "Steam\steamapps\libraryfolders.vdf"
    $steam = (Get-ItemProperty "HKCU:\Software\Valve\Steam" -ErrorAction SilentlyContinue).SteamPath
    if ($steam) { $vdf = Join-Path $steam "steamapps\libraryfolders.vdf" }
    if (Test-Path $vdf) {
        $libraries = Select-String -Path $vdf -Pattern '"path"\s+"([^"]+)"' |
            ForEach-Object { $_.Matches[0].Groups[1].Value -replace '\\\\', '\' }
    }
    foreach ($library in $libraries) {
        $dir = Join-Path $library "steamapps\common\DesktopPlus"
        if (Test-Path (Join-Path $dir "DesktopPlus.exe")) { return $dir }
    }
    return $null
}

Stop-SteamVR

if (-not $SkipDriver) {
    Step "SteamVR driver"
    if (-not (Test-Path (Join-Path $DriverDir "bin\win64\driver_flowvr.dll"))) { Fail "Driver not built. Run scripts\build.ps1 first." }
    # Drop registrations of this driver from other checkouts, then register this one.
    foreach ($registered in @($OpenVr.external_drivers)) {
        if ($registered -and (Split-Path $registered -Leaf) -eq "flowvr" -and $registered -ne $DriverDir) {
            & $VrPathReg removedriver $registered | Out-Null
            Note "removed old registration: $registered"
        }
    }
    & $VrPathReg adddriver $DriverDir | Out-Null
    Note "registered: $DriverDir"
}

if (-not $SkipSteamVRSettings) {
    Step "SteamVR settings"
    $settingsPath = Join-Path $ConfigDir "steamvr.vrsettings"
    $settings = if (Test-Path $settingsPath) { Get-Content $settingsPath -Raw | ConvertFrom-Json } else { New-Object PSObject }
    if (Test-Path $settingsPath) {
        $backup = "$settingsPath.bak-veve"
        if (-not (Test-Path $backup)) { Copy-Item $settingsPath $backup; Note "backup: $backup" }
    }
    foreach ($section in $SteamVRSettings.Keys) {
        if (-not $settings.$section) { $settings | Add-Member -NotePropertyName $section -NotePropertyValue (New-Object PSObject) }
        foreach ($key in $SteamVRSettings[$section].Keys) {
            $settings.$section | Add-Member -NotePropertyName $key -NotePropertyValue $SteamVRSettings[$section][$key] -Force
            Note "$section.$key = $($SteamVRSettings[$section][$key])"
        }
    }
    [IO.File]::WriteAllText($settingsPath, ($settings | ConvertTo-Json -Depth 32), $Utf8NoBom)
}

if (-not $SkipDesktopPlus) {
    Step "Desktop+"
    $desktopPlus = Find-DesktopPlus
    $config = if ($desktopPlus) { Join-Path $desktopPlus "config.ini" } else { $null }
    if (-not $desktopPlus) {
        Warn "Desktop+ is not installed. Install it from Steam (free, app 1494460), then re-run this script."
    }
    elseif (-not (Test-Path $config) -or -not (Select-String -Path $config -Pattern '^ConfigVersion=2' -Quiet)) {
        # Desktop+ writes config.ini (current format) when it exits; on its first launch it also
        # enables its own auto-launch with SteamVR.
        Warn "Desktop+ has not been run yet. Start it once from Steam (it starts SteamVR), quit SteamVR, then re-run this script."
    }
    else {
        $backup = "$config.bak-veve"
        if (-not (Test-Path $backup)) { Copy-Item $config $backup; Note "backup: $backup" }
        $lines = Get-Content $config -Encoding UTF8
        $lines = Set-IniValues $lines "Overlay0" $DesktopPlusOverlay
        $lines = Set-IniValues $lines "Input" $DesktopPlusInput
        [IO.File]::WriteAllLines($config, $lines, $Utf8NoBom)
        Note "[Overlay0] $(($DesktopPlusOverlay.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ' ')"
        Note "[Input] $(($DesktopPlusInput.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ' ')"
    }
}

if (-not $SkipHelper) {
    Step "Dashboard helper (auto-launch with SteamVR)"
    if (-not (Test-Path $HelperExe)) { Fail "Helper not built. Run scripts\build.ps1 first." }
    # Registering an app needs a running SteamVR.
    Start-Process -FilePath $VrMonitor | Out-Null
    $deadline = (Get-Date).AddSeconds(60)
    while (-not (Get-Process vrserver -ErrorAction SilentlyContinue) -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 1 }
    Start-Sleep -Seconds 8
    $install = Start-Process -FilePath $HelperExe -ArgumentList "--install" -Wait -PassThru
    $log = Join-Path (Split-Path $HelperExe) "flow_dashboard_helper.log"
    if ($install.ExitCode -ne 0) { Warn "helper --install failed (exit $($install.ExitCode)); see $log" }
    else { Note (Get-Content $log -Tail 1) }
    Stop-SteamVR
}

if (-not $SkipApk) {
    Step "Flow APK"
    $adb = (Get-Command adb -ErrorAction SilentlyContinue).Source
    if (-not $adb) {
        $sdk = @($env:ANDROID_SDK_ROOT, $env:ANDROID_HOME, (Join-Path $env:LOCALAPPDATA "Android\Sdk")) |
            Where-Object { $_ -and (Test-Path (Join-Path $_ "platform-tools\adb.exe")) } | Select-Object -First 1
        if ($sdk) { $adb = Join-Path $sdk "platform-tools\adb.exe" }
    }
    $device = if ($adb) { (& $adb devices) | Select-String "`tdevice$" | Select-Object -First 1 } else { $null }
    if (-not (Test-Path $Apk)) { Warn "APK not built (scripts\build.ps1)." }
    elseif (-not $adb) { Warn "adb not found; install the APK manually: $Apk" }
    elseif (-not $device) { Warn "No Flow connected over ADB; connect it (USB debugging) and re-run with -SkipDriver -SkipSteamVRSettings -SkipDesktopPlus -SkipHelper." }
    else {
        & $adb install -r $Apk | Select-Object -Last 1 | ForEach-Object { Note $_ }
    }
}

Step "Done"
Note "Daily use: start SteamVR from Steam, then open 'Flow Probe' on the Flow (same Wi-Fi as this PC)."
