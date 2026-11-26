using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;

namespace LeonidaLauncher;

public class RunSessionItem
{
    public string Name { get; set; } = "";
    public DateTime Timestamp { get; set; }
    public string Status { get; set; } = "Unknown";
    public int DurationSec { get; set; }
    public int Presents { get; set; }
    public int ShadersCount { get; set; }
    public int CapturesCount { get; set; }
    public long LogSizeBytes { get; set; }
    public string LogFilePath { get; set; } = "";
    public string SummaryFilePath { get; set; } = "";
    public string DigestFilePath { get; set; } = "";
    public string CapDirPath { get; set; } = "";
}

public class SessionManager
{
    /// <summary>
    /// Scans search directories for past runs (e.g. run1 .. run78) and calculates the next logical run name (e.g. run79).
    /// </summary>
    public static string GetNextRunName(List<string> searchDirs)
    {
        int maxRun = 0;
        foreach (var dir in searchDirs)
        {
            if (!Directory.Exists(dir)) continue;

            try
            {
                var files = Directory.GetFiles(dir, "run*.*");
                foreach (var file in files)
                {
                    string filename = Path.GetFileNameWithoutExtension(file);
                    var match = Regex.Match(filename, @"^run(\d+)");
                    if (match.Success && int.TryParse(match.Groups[1].Value, out int num))
                    {
                        if (num > maxRun) maxRun = num;
                    }
                }

                // Also check directory names like cap_run78
                var dirs = Directory.GetDirectories(dir, "*run*");
                foreach (var d in dirs)
                {
                    string dirname = Path.GetFileName(d);
                    var match = Regex.Match(dirname, @"run(\d+)");
                    if (match.Success && int.TryParse(match.Groups[1].Value, out int num))
                    {
                        if (num > maxRun) maxRun = num;
                    }
                }
            }
            catch { }
        }

        return maxRun > 0 ? $"run{maxRun + 1}" : "run79";
    }

    public static List<RunSessionItem> DiscoverSessions(List<string> searchDirs)
    {
        var discovered = new Dictionary<string, RunSessionItem>();

        foreach (var dir in searchDirs)
        {
            if (!Directory.Exists(dir)) continue;

            try
            {
                var files = Directory.GetFiles(dir, "*.*");
                foreach (var file in files)
                {
                    string filename = Path.GetFileName(file);
                    var match = Regex.Match(filename, @"^(run[A-Za-z0-9_]+|boot-[0-9_-]+)");
                    if (!match.Success) continue;

                    string runName = match.Groups[1].Value;
                    if (runName.Contains('.')) runName = runName.Split('.')[0];

                    if (!discovered.ContainsKey(runName))
                    {
                        var item = new RunSessionItem { Name = runName };
                        string summaryPath = Path.Combine(dir, $"{runName}.summary.txt");
                        string digestPath = Path.Combine(dir, $"{runName}.digest.txt");
                        string logPath = Path.Combine(dir, $"{runName}.log");
                        if (!File.Exists(logPath))
                        {
                            string altErr = Path.Combine(dir, $"{runName}.err");
                            if (File.Exists(altErr)) logPath = altErr;
                        }
                        string capDir = Path.Combine(dir, $"cap_{runName}");

                        if (File.Exists(summaryPath)) item.SummaryFilePath = summaryPath;
                        if (File.Exists(digestPath)) item.DigestFilePath = digestPath;
                        if (File.Exists(logPath)) item.LogFilePath = logPath;
                        if (Directory.Exists(capDir)) item.CapDirPath = capDir;

                        // Parse Digest
                        if (File.Exists(digestPath))
                        {
                            try
                            {
                                string text = File.ReadAllText(digestPath);
                                var durM = Regex.Match(text, @"\((\d+)\s*s\)");
                                if (durM.Success) item.DurationSec = int.Parse(durM.Groups[1].Value);

                                if (text.Contains("stopped")) item.Status = "Stopped (Time Limit)";
                                else if (text.ToLower().Contains("fault")) item.Status = "Device Fault";
                                else item.Status = "Completed";

                                var presM = Regex.Match(text, @"PresentX ok\s+(\d+)");
                                if (presM.Success) item.Presents = int.Parse(presM.Groups[1].Value);

                                var shadM = Regex.Match(text, @"(\d+)\s+translated shaders");
                                if (shadM.Success) item.ShadersCount = int.Parse(shadM.Groups[1].Value);

                                var capM = Regex.Match(text, @"(\d+)\s+captured");
                                if (capM.Success) item.CapturesCount = int.Parse(capM.Groups[1].Value);
                            }
                            catch { }
                        }

                        if (Directory.Exists(capDir) && item.CapturesCount == 0)
                        {
                            try
                            {
                                item.CapturesCount = Directory.GetFiles(capDir, "*.ppm").Length
                                                   + Directory.GetFiles(capDir, "*.png").Length;
                            }
                            catch { }
                        }

                        if (File.Exists(logPath))
                        {
                            try
                            {
                                var fi = new FileInfo(logPath);
                                item.LogSizeBytes = fi.Length;
                                item.Timestamp = fi.LastWriteTime;
                            }
                            catch { }
                        }
                        else
                        {
                            item.Timestamp = File.GetLastWriteTime(file);
                        }

                        discovered[runName] = item;
                    }
                }
            }
            catch { }
        }

        return discovered.Values.OrderByDescending(r => r.Timestamp).ToList();
    }
}
