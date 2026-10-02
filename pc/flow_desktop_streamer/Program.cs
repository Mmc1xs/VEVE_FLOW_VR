using System.Diagnostics;

static string Quote(string value)
{
    return "\"" + value.Replace("\"", "\\\"") + "\"";
}

static string? FindProjectRoot(string start)
{
    var dir = new DirectoryInfo(start);
    while (dir != null)
    {
        var sender = Path.Combine(
            dir.FullName,
            "Wave_Native_SDK",
            "samples",
            "wvr_flow_probe",
            "tools",
            "live_h264_socket_sender.py");
        if (File.Exists(sender))
        {
            return dir.FullName;
        }
        dir = dir.Parent;
    }
    return null;
}

var projectRoot = FindProjectRoot(AppContext.BaseDirectory)
                  ?? FindProjectRoot(Environment.CurrentDirectory);
if (projectRoot == null)
{
    Console.Error.WriteLine("Cannot find the project root (run from inside the VEVE_FLOW_VR checkout).");
    return 2;
}

var sender = Path.Combine(projectRoot, "Wave_Native_SDK", "samples", "wvr_flow_probe", "tools", "live_h264_socket_sender.py");
if (!File.Exists(sender))
{
    Console.Error.WriteLine("Cannot find sender script: " + sender);
    return 2;
}

var senderWorkDir = Path.GetDirectoryName(sender)!;
var senderArgs = string.Join(" ", new[]
{
    Quote(sender),
    "--source", "ddagrab",
    "--encoder", "nvenc",
    "--width", "1920",
    "--height", "1080",
    "--fps", "75",
    "--bitrate", "60M",
    "--frames", "0"
});

Console.WriteLine("Flow desktop streamer");
Console.WriteLine("PC sender: 1920x1080 @ 75 fps, NVENC, LAN discovery enabled");
Console.WriteLine("Open the Flow app from the headset. No ADB or SteamVR dashboard is required.");
Console.WriteLine("Press Ctrl+C to stop.");
Console.WriteLine();

using var process = new Process();
process.StartInfo = new ProcessStartInfo
{
    FileName = "python",
    Arguments = senderArgs,
    WorkingDirectory = senderWorkDir,
    UseShellExecute = false,
    RedirectStandardOutput = true,
    RedirectStandardError = true,
    CreateNoWindow = false,
};

process.OutputDataReceived += (_, e) =>
{
    if (e.Data != null)
    {
        Console.WriteLine(e.Data);
    }
};
process.ErrorDataReceived += (_, e) =>
{
    if (e.Data != null)
    {
        Console.Error.WriteLine(e.Data);
    }
};

Console.CancelKeyPress += (_, e) =>
{
    e.Cancel = true;
    try
    {
        if (!process.HasExited)
        {
            process.Kill(entireProcessTree: true);
        }
    }
    catch
    {
    }
};

process.Start();
process.BeginOutputReadLine();
process.BeginErrorReadLine();
process.WaitForExit();
return process.ExitCode;
