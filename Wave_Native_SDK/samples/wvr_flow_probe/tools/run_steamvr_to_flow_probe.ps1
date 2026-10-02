param(
    [ValidateSet("x264", "nvenc")]
    [string]$Encoder = "nvenc",

    [int]$Frames = 0,

    [switch]$SkipInstall,

    [switch]$LaunchSample,

    [switch]$NoSample,

    [switch]$NoDashboard,

    [string]$SamplePath = "",

    [switch]$UseBridge
    ,

    [switch]$DirectDesktop
)

$ErrorActionPreference = "Stop"

# tools -> wvr_flow_probe -> samples -> Wave_Native_SDK -> project root
$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..\..")).Path
$ProbeRoot = Join-Path $ProjectRoot "Wave_Native_SDK\samples\wvr_flow_probe"
$ToolsRoot = Join-Path $ProbeRoot "tools"
$Bridge = Join-Path $ToolsRoot "steamvr_compositor_bridge.py"
$Apk = Join-Path $ProbeRoot "app\build\outputs\apk\bit64\debug\app-bit64-debug.apk"
$HelloVr = if ($SamplePath) { $SamplePath } else { Join-Path $ProjectRoot "pc\openvr\samples\bin\win32\hellovr_opengl.exe" }
$HelloVrWorkdir = Split-Path -Parent $HelloVr
$VrMonitor = "C:\Program Files (x86)\Steam\steamapps\common\SteamVR\bin\win64\vrmonitor.exe"
$VrCmd = "C:\Program Files (x86)\Steam\steamapps\common\SteamVR\bin\win64\vrcmd.exe"
$DesktopSender = Join-Path $ToolsRoot "live_h264_socket_sender.py"
$DesktopSenderStdout = Join-Path $ToolsRoot "desktop_h264_sender.stdout.log"
$DesktopSenderStderr = Join-Path $ToolsRoot "desktop_h264_sender.stderr.log"
$BridgeStdout = Join-Path $ToolsRoot "steamvr_compositor_bridge.stdout.log"
$BridgeStderr = Join-Path $ToolsRoot "steamvr_compositor_bridge.stderr.log"
$DriverTrace = Join-Path $ProjectRoot "pc\flow_steamvr_driver\build\dist\flowvr\logs\flow_virtual_display_trace.log"
$FlowPackage = "com.htc.vr.samples.wvr_flow_probe"
$FlowActivity = "$FlowPackage/.MainActivity"
$sampleProcess = $null
$bridgeProcess = $null
$desktopSenderProcess = $null

function Assert-AdbDevice {
    $devices = adb devices | Select-String -Pattern "`tdevice$"
    if (-not $devices) {
        throw "No Flow ADB device is connected. Connect Flow over ADB, keep it awake/worn, then run again."
    }
}

Write-Host "Stopping old SteamVR/sample processes..."
Get-Process vrserver,vrmonitor,vrcompositor,hellovr_opengl,ffmpeg -ErrorAction SilentlyContinue | Stop-Process -Force
Assert-AdbDevice
adb shell am force-stop $FlowPackage | Out-Null
Start-Sleep -Seconds 2

if (-not $SkipInstall) {
    if (-not (Test-Path -LiteralPath $Apk)) {
        throw "APK not found: $Apk"
    }
    Write-Host "Installing Flow probe APK..."
    adb install -r $Apk
}

Write-Host "Clearing Flow logs..."
adb logcat -c

if ($DirectDesktop) {
    Write-Host "Starting direct desktop H264 stream on TCP :8001. SteamVR/dashboard/input are not used."
    Remove-Item -LiteralPath $DesktopSenderStdout,$DesktopSenderStderr -ErrorAction SilentlyContinue
    $desktopArgs = @(
        $DesktopSender,
        "--source", "ddagrab",
        "--encoder", "nvenc",
        "--width", "1920",
        "--height", "1080",
        "--fps", "75",
        "--bitrate", "60M",
        "--frames", "0"
    )
    $desktopSenderProcess = Start-Process -FilePath "python" -ArgumentList $desktopArgs -WorkingDirectory $ProbeRoot -RedirectStandardOutput $DesktopSenderStdout -RedirectStandardError $DesktopSenderStderr -PassThru
} elseif ($UseBridge) {
    Write-Host "Starting SteamVR compositor bridge fallback..."
    $bridgeArgs = @($Bridge, "--encoder", $Encoder, "--frames", "$Frames")
    Remove-Item -LiteralPath $BridgeStdout,$BridgeStderr -ErrorAction SilentlyContinue
    $bridgeProcess = Start-Process -FilePath "python" -ArgumentList $bridgeArgs -WorkingDirectory $ProbeRoot -RedirectStandardOutput $BridgeStdout -RedirectStandardError $BridgeStderr -PassThru
} else {
    Write-Host "Using direct driver NVENC H264 stream on TCP :8001. No Python/FFmpeg bridge."
}

try {
    Start-Sleep -Seconds 2

    if (-not $DirectDesktop) {
        Write-Host "Starting SteamVR..."
        if (Test-Path -LiteralPath $VrMonitor) {
            Start-Process -FilePath $VrMonitor | Out-Null
        }
        Start-Sleep -Seconds 10

        $shouldLaunchSample = $LaunchSample -or [bool]$SamplePath
        if ($NoSample) {
            $shouldLaunchSample = $false
        }

        if ($shouldLaunchSample) {
            if (-not (Test-Path -LiteralPath $HelloVr)) {
                throw "OpenVR sample/app not found: $HelloVr"
            }

            Write-Host "Starting OpenVR sample/app: $HelloVr"
            $sampleProcess = Start-Process -FilePath $HelloVr -WorkingDirectory $HelloVrWorkdir -PassThru
        } else {
            Write-Host "Skipping sample launch. Start any SteamVR app normally after Flow is connected."
        }
    } else {
        Write-Host "Skipping SteamVR. Direct desktop stream is waiting for Flow."
    }

    Start-Sleep -Seconds 2

    Write-Host "Starting Flow probe app. Keep wearing the Flow headset."
    adb shell am start -n $FlowActivity | Out-Host

    if (-not $DirectDesktop -and -not $NoDashboard -and (Test-Path -LiteralPath $VrCmd)) {
        Start-Sleep -Seconds 3
        Write-Host "Opening SteamVR dashboard for desktop streaming..."
        & $VrCmd --showdashboard | Out-Null
    }

    Write-Host ""
    if ($DirectDesktop) {
        Write-Host "Flow direct desktop streaming mode is running. Watch for these lines:"
    } else {
        Write-Host "Flow SteamVR desktop streaming mode is running. Watch for these lines:"
    }
    if ($DirectDesktop) {
        Write-Host "  socket decoder started ... size=1920x1080 fps=75"
        Write-Host "  screen placed center=..."
    } else {
        Write-Host "  socket decoder started ... size=1920x960 fps=75"
    }
    if (-not $DirectDesktop) {
        Write-Host "  Using existing HMD flowvr.VIVEFLOW-STEAMVR-001"
        Write-Host "  Flow pose UDP: seq=..."
    }
    Write-Host ""
    if ($DirectDesktop) {
        Write-Host "Desktop sender stdout: $DesktopSenderStdout"
        Write-Host "Desktop sender stderr: $DesktopSenderStderr"
    } elseif ($UseBridge) {
        Write-Host "Bridge stdout: $BridgeStdout"
        Write-Host "Bridge stderr: $BridgeStderr"
    } else {
        Write-Host "Driver trace: $DriverTrace"
    }
    Write-Host ""
    if ($DirectDesktop) {
        Write-Host "PC desktop should be visible in Flow without SteamVR dashboard or controllers."
    } else {
        Write-Host "SteamVR dashboard should be visible in Flow. Use SteamVR's Desktop view for the PC desktop."
    }
    Write-Host "Press Ctrl+C in this PowerShell window to stop."

    while ($true) {
        Write-Host ""
        Write-Host "[Flow]"
        adb logcat -d -v time -s FLOW_MIN_PROBE FlowProbe |
            Select-String -Pattern "socket decoder started|socket decoder completed|screen placed|recenter event|decoder video size|decoder output format|fallback|SocketTimeout|connect failed|EOF|IllegalState" |
            Select-Object -Last 10

        if ($DirectDesktop) {
            Write-Host "[Desktop Sender]"
            if (Test-Path -LiteralPath $DesktopSenderStdout) {
                Get-Content -Path $DesktopSenderStdout -Tail 10
            }
            if (Test-Path -LiteralPath $DesktopSenderStderr) {
                Get-Content -Path $DesktopSenderStderr -Tail 10
            }
        } else {
            Write-Host "[SteamVR]"
            Get-Content "C:\Program Files (x86)\Steam\logs\vrserver.txt" -Tail 180 |
                Select-String -Pattern "Using existing HMD flowvr.VIVEFLOW-STEAMVR-001|Flow pose UDP|Flow virtual display Present|Flow NVENC initialized|showdashboard|dashboard|desktop|has no configured binding" |
                Select-Object -Last 10
        }

        if ($UseBridge -and $Frames -gt 0 -and $bridgeProcess.HasExited) {
            Write-Host "Bridge exited after finite frame run."
            if (Test-Path -LiteralPath $BridgeStdout) {
                Get-Content -Path $BridgeStdout -Tail 40
            }
            if (Test-Path -LiteralPath $BridgeStderr) {
                Get-Content -Path $BridgeStderr -Tail 40
            }
            break
        }
        Start-Sleep -Seconds 5
    }
}
finally {
    Write-Host "Stopping launched processes..."
    if ($null -ne $sampleProcess -and -not $sampleProcess.HasExited) {
        Stop-Process -Id $sampleProcess.Id -Force
    }
    if ($null -ne $bridgeProcess -and -not $bridgeProcess.HasExited) {
        Stop-Process -Id $bridgeProcess.Id -Force
    }
    if ($null -ne $desktopSenderProcess -and -not $desktopSenderProcess.HasExited) {
        Stop-Process -Id $desktopSenderProcess.Id -Force
    }
}
