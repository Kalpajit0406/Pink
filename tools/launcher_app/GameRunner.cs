using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;

namespace LeonidaLauncher;

public class TelemetryData
{
    public double Fps { get; set; }
    public double FrametimeMs { get; set; }
    public double P50FrametimeMs { get; set; }
    public double P95FrametimeMs { get; set; }
    public double P99FrametimeMs { get; set; }
    public int PresentCount { get; set; }
    public int VramUsedMb { get; set; }
    public int HostRamUsedMb { get; set; }
    public double DstorageMbS { get; set; }
    public int AudioUnderruns { get; set; }
    public string Status { get; set; } = "Ready";
    public double ElapsedSeconds { get; set; }
    public string ActiveRunName { get; set; } = "";
}

public class GameRunner
{
    public event EventHandler<string>? LogReceived;
    public event EventHandler<TelemetryData>? TelemetryUpdated;
    public event EventHandler<int?>? ProcessExited;

    private Process? _process;
    private readonly object _lock = new();
    private readonly List<double> _recentFrametimes = new();
    private DateTime _lastPresentTime = DateTime.MinValue;
    private DateTime _startTime = DateTime.MinValue;
    private readonly TelemetryData _telemetry = new();
    private System.Threading.Timer? _telemetryTimer;

    public bool IsRunning
    {
        get
        {
            lock (_lock)
            {
                return _process != null && !_process.HasExited;
            }
        }
    }

    public int ProcessId
    {
        get
        {
            lock (_lock)
            {
                return _process?.Id ?? 0;
            }
        }
    }

    public string CurrentRunName { get; private set; } = "";

    public bool Launch(
        LauncherConfig config,
        string runName,
        Dictionary<string, string> effectiveEnv,
        string repoRoot,
        out string message
    )
    {
        lock (_lock)
        {
            if (IsRunning)
            {
                message = $"Game is already running (PID {ProcessId})";
                return false;
            }

            CurrentRunName = string.IsNullOrWhiteSpace(runName) ? "run79" : runName.Trim();
            _recentFrametimes.Clear();
            _lastPresentTime = DateTime.MinValue;
            _startTime = DateTime.UtcNow;

            _telemetry.Fps = 0;
            _telemetry.FrametimeMs = 0;
            _telemetry.P50FrametimeMs = 0;
            _telemetry.P95FrametimeMs = 0;
            _telemetry.P99FrametimeMs = 0;
            _telemetry.PresentCount = 0;
            _telemetry.VramUsedMb = 0;
            _telemetry.HostRamUsedMb = 0;
            _telemetry.DstorageMbS = 0;
            _telemetry.AudioUnderruns = 0;
            _telemetry.Status = "Running";
            _telemetry.ElapsedSeconds = 0;
            _telemetry.ActiveRunName = CurrentRunName;

            string bootScript = Path.Combine(repoRoot, @"scripts\windows-boot\boot.ps1");

            var psi = new ProcessStartInfo
            {
                FileName = "powershell.exe",
                Arguments = $"-NoProfile -ExecutionPolicy Bypass -File \"{bootScript}\" " +
                            $"-Name \"{CurrentRunName}\" " +
                            $"-Seconds {config.Settings.SecondsLimit} " +
                            $"-Runtime \"{config.Paths.RuntimePath}\" " +
                            $"-GameDir \"{config.Paths.GameDir}\" " +
                            $"-Exe \"{config.Paths.ExePath}\" " +
                            $"-Shaders \"{config.Paths.ShadersDir}\" " +
                            $"-OutDir \"{config.Paths.RunsDir}\"",
                UseShellExecute = false,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                CreateNoWindow = true,
                WorkingDirectory = repoRoot
            };

            if (config.Settings.EnableInputScript && !string.IsNullOrWhiteSpace(config.Settings.InputScript))
            {
                psi.Arguments += $" -InputScript \"{config.Settings.InputScript}\"";
            }
            else
            {
                psi.Arguments += " -NoInput";
            }

            foreach (var kv in effectiveEnv)
            {
                psi.EnvironmentVariables[kv.Key] = kv.Value;
                psi.Arguments += $" \"{kv.Key}={kv.Value}\"";
            }

            if (config.Settings.HighPrecisionTimer)
            {
                NativeMethods.TimeBeginPeriod(1);
            }

            try
            {
                _process = Process.Start(psi);
                if (_process == null)
                {
                    NativeMethods.TimeEndPeriod(1);
                    message = "Failed to create PowerShell harness process";
                    return false;
                }

                _telemetry.Status = "Running";
            }
            catch (Exception ex)
            {
                NativeMethods.TimeEndPeriod(1);
                message = $"Error starting process: {ex.Message}";
                return false;
            }

            // Priority elevation: default to high (realtime starves input/audio/DWM)
            string priorityStr = effectiveEnv.TryGetValue("LEONIDA_PRIORITY", out var pr) ? pr : "high";
            ApplyPriority(_process.Id, priorityStr);

            // Read output asynchronously with clean timestamps
            _process.OutputDataReceived += (s, e) =>
            {
                if (e.Data != null) HandleLogLine(e.Data, "stdout");
            };
            _process.ErrorDataReceived += (s, e) =>
            {
                if (e.Data != null) HandleLogLine(e.Data, "stderr");
            };

            _process.BeginOutputReadLine();
            _process.BeginErrorReadLine();

            // Periodic telemetry ticker
            _telemetryTimer = new System.Threading.Timer(OnTelemetryTick, null, 250, 250);

            // Background monitor for child gta6rt.exe priority elevation and exit wait
            Task.Run(() => MonitorProcess(priorityStr, repoRoot, config.Paths.RunsDir, CurrentRunName));

            message = $"GTA VI launched successfully: Session {CurrentRunName} (PID {_process.Id})";
            return true;
        }
    }

    private void ApplyPriority(int pid, string priorityStr)
    {
        try
        {
            uint desiredClass = NativeMethods.HIGH_PRIORITY_CLASS;
            if (priorityStr.Equals("realtime", StringComparison.OrdinalIgnoreCase))
                desiredClass = NativeMethods.REALTIME_PRIORITY_CLASS;
            else if (priorityStr.Equals("above_normal", StringComparison.OrdinalIgnoreCase))
                desiredClass = NativeMethods.ABOVE_NORMAL_PRIORITY_CLASS;

            IntPtr handle = NativeMethods.OpenProcess(0x0200 | 0x0400, false, pid);
            if (handle != IntPtr.Zero)
            {
                if (!NativeMethods.SetPriorityClass(handle, desiredClass) && desiredClass == NativeMethods.REALTIME_PRIORITY_CLASS)
                {
                    NativeMethods.SetPriorityClass(handle, NativeMethods.HIGH_PRIORITY_CLASS);
                }
                NativeMethods.CloseHandle(handle);
            }
        }
        catch { }
    }

    private void MonitorProcess(string priorityStr, string repoRoot, string runsDir, string runName)
    {
        bool elevatedChild = false;
        var proc = _process;
        if (proc == null) return;

        while (!proc.HasExited)
        {
            if (!elevatedChild)
            {
                try
                {
                    var gtaProcs = Process.GetProcessesByName("gta6rt");
                    if (gtaProcs.Length > 0)
                    {
                        ApplyPriority(gtaProcs[0].Id, priorityStr);
                        elevatedChild = true;
                    }
                }
                catch { }
            }
            Thread.Sleep(500);
        }

        int exitCode = proc.ExitCode;
        NativeMethods.TimeEndPeriod(1);
        _telemetryTimer?.Dispose();

        lock (_lock)
        {
            _telemetry.Status = exitCode == 0 ? "Terminated (Exit 0)" : $"Stopped (Exit {exitCode})";
        }

        // Post-run trace generation if missing
        string summaryPath = Path.Combine(runsDir, $"{runName}.summary.txt");
        string jsonlPath = Path.Combine(runsDir, $"{runName}.jsonl");
        string logPath = Path.Combine(runsDir, $"{runName}.log");

        if (!File.Exists(summaryPath) && File.Exists(jsonlPath))
        {
            string traceTool = Path.Combine(repoRoot, @"tools\boot_trace.py");
            if (File.Exists(traceTool))
            {
                try
                {
                    var psi = new ProcessStartInfo
                    {
                        FileName = "python",
                        Arguments = $"-I \"{traceTool}\" \"{jsonlPath}\" \"{logPath}\" --out \"{summaryPath}\"",
                        UseShellExecute = false,
                        CreateNoWindow = true
                    };
                    Process.Start(psi)?.WaitForExit(10000);
                }
                catch { }
            }
        }

        ProcessExited?.Invoke(this, exitCode);
    }

    private void HandleLogLine(string rawLine, string streamTag)
    {
        string timestamp = DateTime.Now.ToString("HH:mm:ss.fff");
        string formattedLine = $"[{timestamp}] [{streamTag}] {rawLine}";
        LogReceived?.Invoke(this, formattedLine);

        DateTime now = DateTime.UtcNow;

        if (rawLine.Contains("PresentX ok") || rawLine.Contains("Present #") || rawLine.Contains("PresentX"))
        {
            lock (_lock)
            {
                _telemetry.PresentCount++;
                if (_lastPresentTime > DateTime.MinValue)
                {
                    double dtMs = (now - _lastPresentTime).TotalMilliseconds;
                    if (dtMs > 2.0 && dtMs < 500.0)
                    {
                        _recentFrametimes.Add(dtMs);
                        if (_recentFrametimes.Count > 100) _recentFrametimes.RemoveAt(0);

                        _telemetry.FrametimeMs = Math.Round(dtMs, 2);
                        _telemetry.Fps = Math.Round(1000.0 / dtMs, 1);

                        var sorted = _recentFrametimes.OrderBy(x => x).ToList();
                        int n = sorted.Count;
                        if (n >= 5)
                        {
                            _telemetry.P50FrametimeMs = Math.Round(sorted[(int)(n * 0.50)], 2);
                            _telemetry.P95FrametimeMs = Math.Round(sorted[Math.Min(n - 1, (int)(n * 0.95))], 2);
                            _telemetry.P99FrametimeMs = Math.Round(sorted[Math.Min(n - 1, (int)(n * 0.99))], 2);
                        }
                    }
                }
                _lastPresentTime = now;
            }
        }

        if (rawLine.Contains("decompressed") && rawLine.Contains("MB/s"))
        {
            var m = Regex.Match(rawLine, @"(\d+(\.\d+)?)\s*MB/s");
            if (m.Success)
            {
                lock (_lock) _telemetry.DstorageMbS = double.Parse(m.Groups[1].Value);
            }
        }

        if (rawLine.Contains("VRAM") || rawLine.Contains("title memory"))
        {
            var vm = Regex.Match(rawLine, @"VRAM[:\s]+(\d+)\s*MB", RegexOptions.IgnoreCase);
            if (vm.Success)
            {
                lock (_lock) _telemetry.VramUsedMb = int.Parse(vm.Groups[1].Value);
            }
            var rm = Regex.Match(rawLine, @"host[:\s]+(\d+)\s*MB", RegexOptions.IgnoreCase);
            if (rm.Success)
            {
                lock (_lock) _telemetry.HostRamUsedMb = int.Parse(rm.Groups[1].Value);
            }
        }

        if (rawLine.ToLower().Contains("audio underrun") || rawLine.ToLower().Contains("starvation"))
        {
            lock (_lock) _telemetry.AudioUnderruns++;
        }
    }

    private void OnTelemetryTick(object? state)
    {
        lock (_lock)
        {
            if (_startTime > DateTime.MinValue)
            {
                _telemetry.ElapsedSeconds = Math.Round((DateTime.UtcNow - _startTime).TotalSeconds, 1);
            }
            TelemetryUpdated?.Invoke(this, new TelemetryData
            {
                Fps = _telemetry.Fps,
                FrametimeMs = _telemetry.FrametimeMs,
                P50FrametimeMs = _telemetry.P50FrametimeMs,
                P95FrametimeMs = _telemetry.P95FrametimeMs,
                P99FrametimeMs = _telemetry.P99FrametimeMs,
                PresentCount = _telemetry.PresentCount,
                VramUsedMb = _telemetry.VramUsedMb,
                HostRamUsedMb = _telemetry.HostRamUsedMb,
                DstorageMbS = _telemetry.DstorageMbS,
                AudioUnderruns = _telemetry.AudioUnderruns,
                Status = _telemetry.Status,
                ElapsedSeconds = _telemetry.ElapsedSeconds,
                ActiveRunName = CurrentRunName
            });
        }
    }

    public void Stop()
    {
        lock (_lock)
        {
            if (_process == null || _process.HasExited) return;

            try
            {
                Process.Start(new ProcessStartInfo
                {
                    FileName = "taskkill",
                    Arguments = $"/F /T /PID {_process.Id}",
                    UseShellExecute = false,
                    CreateNoWindow = true
                })?.WaitForExit(3000);
            }
            catch { }

            try
            {
                Process.Start(new ProcessStartInfo
                {
                    FileName = "taskkill",
                    Arguments = "/F /IM gta6rt.exe",
                    UseShellExecute = false,
                    CreateNoWindow = true
                })?.WaitForExit(2000);
            }
            catch { }

            NativeMethods.TimeEndPeriod(1);
            _telemetry.Status = "Stopped by user";
        }
    }
}
