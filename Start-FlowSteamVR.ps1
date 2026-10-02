param(
    [switch]$Install,
    [switch]$LaunchSample,
    [switch]$NoDashboard,
    [switch]$Dashboard,
    [switch]$DirectDesktop
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$Launcher = Join-Path $ProjectRoot "Wave_Native_SDK\samples\wvr_flow_probe\tools\run_steamvr_to_flow_probe.ps1"

$argsForLauncher = @()
if (-not $Install) {
    $argsForLauncher += "-SkipInstall"
}
if ($LaunchSample) {
    $argsForLauncher += "-LaunchSample"
}
if ($NoDashboard -or -not $Dashboard) {
    $argsForLauncher += "-NoDashboard"
}
if ($DirectDesktop) {
    $argsForLauncher += "-DirectDesktop"
}

& powershell -ExecutionPolicy Bypass -File $Launcher @argsForLauncher
