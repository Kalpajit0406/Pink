using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace LeonidaLauncher;

public class PathsConfig
{
    public string RuntimePath { get; set; } = @"E:\Leonida\gta6rt.exe";
    public string GameDir { get; set; } = @"E:\GTA VI_XBOX";
    public string ExePath { get; set; } = @"E:\GTA VI_XBOX\game_scarlett_master.exe";
    public string ShadersDir { get; set; } = @"E:\Leonida\work\shaders\spirv";
    public string RunsDir { get; set; } = @"E:\Leonida\runs";
}

public class ProfileConfig
{
    public string Id { get; set; } = "";
    public string Name { get; set; } = "";
    public string Badge { get; set; } = "";
    public string Description { get; set; } = "";
    public Dictionary<string, string> Env { get; set; } = new();
}

public class SettingsConfig
{
    public bool AutoPrecompileCheck { get; set; } = true;
    public string InputScript { get; set; } = "a@15,a@25,a@35,a@45,a@60,a@75,a@90";
    public bool EnableInputScript { get; set; } = false;
    public bool HighPrecisionTimer { get; set; } = true;
    public bool XapuAudioFix { get; set; } = true;
    public bool GamepadAutoMap { get; set; } = true;
    public int SecondsLimit { get; set; } = 300;

    // Sliders & Tuning
    public int ResolutionIndex { get; set; } = 0; // 0=720p, 1=900p, 2=1080p
    public int VramBudgetMb { get; set; } = 6144;
    public int HostRamLimitMb { get; set; } = 16384;
    public int TargetFpsCap { get; set; } = 30; // 30, 60, 120, 0(uncapped)
    public int FsrModeIndex { get; set; } = 0; // 0=Perf, 1=Quality, 2=Off
    public bool ConsoleHeapGpu { get; set; } = true;
    public bool GeometryAsync { get; set; } = true;
    public bool RefreshPerFrame { get; set; } = true;
    public bool Fullscreen { get; set; } = false;
    public bool LockCursor { get; set; } = false;
    public string PriorityClass { get; set; } = "realtime";
    public string CustomEnvVars { get; set; } = "";
    public string LastRunName { get; set; } = "run79";
}

public class LauncherConfig
{
    public PathsConfig Paths { get; set; } = new();
    public string ActiveProfile { get; set; } = "max_performance";
    public Dictionary<string, ProfileConfig> Profiles { get; set; } = new();
    public SettingsConfig Settings { get; set; } = new();

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        PropertyNameCaseInsensitive = true,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull
    };

    public static string GetConfigFilePath()
    {
        string baseDir = AppDomain.CurrentDomain.BaseDirectory;
        string repoCandidate = Path.GetFullPath(Path.Combine(baseDir, @"..\..\..\.."));
        if (File.Exists(Path.Combine(repoCandidate, "make_game.py")))
        {
            return Path.Combine(repoCandidate, "launcher_config.json");
        }
        return Path.Combine(baseDir, "launcher_config.json");
    }

    public static LauncherConfig LoadOrCreate()
    {
        string path = GetConfigFilePath();
        LauncherConfig? cfg = null;

        if (File.Exists(path))
        {
            try
            {
                string json = File.ReadAllText(path);
                cfg = JsonSerializer.Deserialize<LauncherConfig>(json, JsonOptions);
            }
            catch { }
        }

        cfg ??= new LauncherConfig();
        cfg.EnsureDefaults();
        return cfg;
    }

    public void Save()
    {
        string path = GetConfigFilePath();
        try
        {
            string dir = Path.GetDirectoryName(path)!;
            if (!Directory.Exists(dir)) Directory.CreateDirectory(dir);
            string json = JsonSerializer.Serialize(this, JsonOptions);
            File.WriteAllText(path, json);
        }
        catch { }
    }

    public void EnsureDefaults()
    {
        DetectSidePaths();

        if (Profiles.Count == 0)
        {
            Profiles["max_performance"] = new ProfileConfig
            {
                Id = "max_performance",
                Name = "Max Performance (720p FSR)",
                Badge = "Recommended",
                Description = "Locked 720p minimum with FSR, 6GB VRAM budget, Realtime process priority, 1ms timer, and GPU-side ConsoleHeap.",
                Env = new Dictionary<string, string>
                {
                    ["LEONIDA_D3D12"] = "vulkan",
                    ["LEONIDA_DEVICE_TYPE"] = "6",
                    ["LEONIDA_MIN_WIDTH"] = "1280",
                    ["LEONIDA_MIN_HEIGHT"] = "720",
                    ["LEONIDA_MAX_WIDTH"] = "1280",
                    ["LEONIDA_MAX_HEIGHT"] = "720",
                    ["LEONIDA_FSR"] = "1",
                    ["LEONIDA_MAX_VRAM_MB"] = "6144",
                    ["LEONIDA_TITLE_MEMORY_MB"] = "10240",
                    ["LEONIDA_MAX_HOST_RAM_MB"] = "16384",
                    ["LEONIDA_CONSOLE_HEAP_GPU"] = "1",
                    ["LEONIDA_GEOMETRY_ASYNC"] = "1",
                    ["LEONIDA_REFRESH_PER_FRAME"] = "1",
                    ["LEONIDA_POOL_ZERO_PAGES"] = "1",
                    ["LEONIDA_FAST_PIPELINES"] = "all",
                    ["LEONIDA_STUB_PIPELINES"] = "1",
                    ["LEONIDA_KEYBOARD_PAD"] = "1",
                    ["LEONIDA_PRIORITY"] = "realtime",
                    ["LEONIDA_CAPTURE"] = "0",
                    ["LEONIDA_PEEK_SECONDS"] = "5"
                }
            };

            Profiles["balanced"] = new ProfileConfig
            {
                Id = "balanced",
                Name = "Balanced (900p - 1080p FSR)",
                Badge = "Stable",
                Description = "Dynamic 720p-1080p with FSR upscaling, 8GB VRAM budget, and High process priority.",
                Env = new Dictionary<string, string>
                {
                    ["LEONIDA_D3D12"] = "vulkan",
                    ["LEONIDA_DEVICE_TYPE"] = "6",
                    ["LEONIDA_MIN_WIDTH"] = "1280",
                    ["LEONIDA_MIN_HEIGHT"] = "720",
                    ["LEONIDA_MAX_WIDTH"] = "1920",
                    ["LEONIDA_MAX_HEIGHT"] = "1080",
                    ["LEONIDA_FSR"] = "1",
                    ["LEONIDA_MAX_VRAM_MB"] = "8192",
                    ["LEONIDA_TITLE_MEMORY_MB"] = "10240",
                    ["LEONIDA_MAX_HOST_RAM_MB"] = "16384",
                    ["LEONIDA_CONSOLE_HEAP_GPU"] = "1",
                    ["LEONIDA_GEOMETRY_ASYNC"] = "1",
                    ["LEONIDA_REFRESH_PER_FRAME"] = "1",
                    ["LEONIDA_KEYBOARD_PAD"] = "1",
                    ["LEONIDA_PRIORITY"] = "high",
                    ["LEONIDA_CAPTURE"] = "0",
                    ["LEONIDA_PEEK_SECONDS"] = "5"
                }
            };

            Profiles["quality"] = new ProfileConfig
            {
                Id = "quality",
                Name = "Visual Quality (1080p Native)",
                Badge = "Fidelity",
                Description = "Native 1080p rendering (1920x1080) without FSR upscaling. Maximum image clarity.",
                Env = new Dictionary<string, string>
                {
                    ["LEONIDA_D3D12"] = "vulkan",
                    ["LEONIDA_DEVICE_TYPE"] = "6",
                    ["LEONIDA_MIN_WIDTH"] = "1920",
                    ["LEONIDA_MIN_HEIGHT"] = "1080",
                    ["LEONIDA_MAX_WIDTH"] = "1920",
                    ["LEONIDA_MAX_HEIGHT"] = "1080",
                    ["LEONIDA_FSR"] = "0",
                    ["LEONIDA_MAX_VRAM_MB"] = "8192",
                    ["LEONIDA_TITLE_MEMORY_MB"] = "10240",
                    ["LEONIDA_MAX_HOST_RAM_MB"] = "16384",
                    ["LEONIDA_CONSOLE_HEAP_GPU"] = "1",
                    ["LEONIDA_GEOMETRY_ASYNC"] = "1",
                    ["LEONIDA_REFRESH_PER_FRAME"] = "1",
                    ["LEONIDA_KEYBOARD_PAD"] = "1",
                    ["LEONIDA_PRIORITY"] = "high",
                    ["LEONIDA_CAPTURE"] = "0",
                    ["LEONIDA_PEEK_SECONDS"] = "5"
                }
            };

            Profiles["custom"] = new ProfileConfig
            {
                Id = "custom",
                Name = "Custom Configuration",
                Badge = "Unlocked",
                Description = "Fully user-configurable hardware sliders, memory budgets, and environment variable overrides.",
                Env = new Dictionary<string, string>()
            };
        }
    }

    private void DetectSidePaths()
    {
        try
        {
            string sideGame = @"E:\Leonida\game_path.txt";
            if (File.Exists(sideGame))
            {
                string line = File.ReadAllText(sideGame).Trim();
                if (!string.IsNullOrWhiteSpace(line))
                {
                    Paths.GameDir = line;
                    Paths.ExePath = Path.Combine(line, "game_scarlett_master.exe");
                }
            }

            string sideShaders = @"E:\Leonida\shader_path.txt";
            if (File.Exists(sideShaders))
            {
                string line = File.ReadAllText(sideShaders).Trim();
                if (!string.IsNullOrWhiteSpace(line)) Paths.ShadersDir = line;
            }
        }
        catch { }
    }
}
