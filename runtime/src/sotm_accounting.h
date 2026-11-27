#pragma once
// Single-Owner Tiered Memory (SOTM) — Component C1: Memory Accounting
// Tags every allocation by category, monitors Windows commit limit/charge,
// tracks VRAM via VK_EXT_memory_budget, and provides periodic JSON telemetry.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

#include "memory_figures.h"
#include "memory_profile_loader.h"

namespace leonida::sotm {

enum class Category : uint8_t {
    GuestHeapCommitted = 0,
    GuestHeapReserved,
    TextureVram,
    RenderTargetVram,
    BufferVram,
    ShadowBuffers,
    HostImports,
    BvhVram,
    UploadRing,
    Staging,
    CommandAllocators,
    DescriptorMirrors,
    HashBuffers,
    PipelineCache,
    ShaderModules,
    WindowSurfaces,
    Other,
    Count
};

inline const char* category_name(Category cat) {
    switch (cat) {
    case Category::GuestHeapCommitted: return "guest_heap_committed";
    case Category::GuestHeapReserved:  return "guest_heap_reserved";
    case Category::TextureVram:        return "texture_vram";
    case Category::RenderTargetVram:   return "rendertarget_vram";
    case Category::BufferVram:         return "buffer_vram";
    case Category::ShadowBuffers:      return "shadow_buffers";
    case Category::HostImports:        return "host_imports";
    case Category::BvhVram:            return "bvh_vram";
    case Category::UploadRing:         return "upload_ring";
    case Category::Staging:            return "staging";
    case Category::CommandAllocators:  return "command_allocators";
    case Category::DescriptorMirrors:  return "descriptor_mirrors";
    case Category::HashBuffers:        return "hash_buffers";
    case Category::PipelineCache:      return "pipeline_cache";
    case Category::ShaderModules:      return "shader_modules";
    case Category::WindowSurfaces:     return "window_surfaces";
    case Category::Other:              return "other";
    default:                           return "unknown";
    }
}

class Accounting {
public:
    static Accounting& instance() {
        static Accounting inst;
        return inst;
    }

    void track_alloc(Category cat, uint64_t bytes) {
        if (static_cast<size_t>(cat) < static_cast<size_t>(Category::Count)) {
            categories_[static_cast<size_t>(cat)].fetch_add(bytes, std::memory_order_relaxed);
        }
    }

    void track_free(Category cat, uint64_t bytes) {
        if (static_cast<size_t>(cat) < static_cast<size_t>(Category::Count)) {
            auto& val = categories_[static_cast<size_t>(cat)];
            uint64_t current = val.load(std::memory_order_relaxed);
            while (current > 0) {
                uint64_t next = (bytes >= current) ? 0 : (current - bytes);
                if (val.compare_exchange_weak(current, next, std::memory_order_relaxed)) break;
            }
        }
    }

    // Tracks a VkDeviceMemory handle by exact category so deferred/async frees match allocations symmetrically.
    void track_vk_alloc(uint64_t handle, Category cat, uint64_t bytes) {
        if (!handle || !bytes) return;
        std::pair<Category, uint64_t> prev{};
        bool had_prev = false;
        {
            std::lock_guard lock(vk_allocs_mutex_);
            auto [it, inserted] = vk_allocs_.emplace(handle, std::make_pair(cat, bytes));
            if (!inserted) {
                prev = it->second;
                it->second = {cat, bytes};
                had_prev = true;
            }
        }
        if (had_prev) track_free(prev.first, prev.second);
        track_alloc(cat, bytes);
    }

    void track_vk_free(uint64_t handle) {
        if (!handle) return;
        std::pair<Category, uint64_t> found{};
        bool erased = false;
        {
            std::lock_guard lock(vk_allocs_mutex_);
            auto it = vk_allocs_.find(handle);
            if (it != vk_allocs_.end()) {
                found = it->second;
                vk_allocs_.erase(it);
                erased = true;
            }
        }
        if (erased) track_free(found.first, found.second);
    }

    void track_evict(uint64_t bytes) {
        evictions_.fetch_add(1, std::memory_order_relaxed);
        evicted_bytes_.fetch_add(bytes, std::memory_order_relaxed);
    }
    void track_decommit(uint64_t bytes) {
        decommits_.fetch_add(1, std::memory_order_relaxed);
        decommitted_bytes_.fetch_add(bytes, std::memory_order_relaxed);
    }
    void track_mip_cap(uint64_t saved_bytes) {
        mip_capped_.fetch_add(1, std::memory_order_relaxed);
        mip_saved_bytes_.fetch_add(saved_bytes, std::memory_order_relaxed);
    }

    void set_gauge(Category cat, uint64_t bytes) {
        if (static_cast<size_t>(cat) < static_cast<size_t>(Category::Count)) {
            categories_[static_cast<size_t>(cat)].store(bytes, std::memory_order_relaxed);
        }
    }

    uint64_t get_bytes(Category cat) const {
        if (static_cast<size_t>(cat) < static_cast<size_t>(Category::Count)) {
            return categories_[static_cast<size_t>(cat)].load(std::memory_order_relaxed);
        }
        return 0;
    }

    struct SystemCommit {
        uint64_t commit_total_bytes = 0;
        uint64_t commit_limit_bytes = 0;
        uint64_t commit_peak_bytes = 0;
        uint64_t physical_total_bytes = 0;
        uint64_t physical_avail_bytes = 0;
    };

    SystemCommit query_system_commit() const {
        SystemCommit sc;
#ifdef _WIN32
        PERFORMANCE_INFORMATION pi{};
        pi.cb = sizeof(pi);
        if (GetPerformanceInfo(&pi, sizeof(pi))) {
            const uint64_t ps = pi.PageSize;
            sc.commit_total_bytes = pi.CommitTotal * ps;
            sc.commit_limit_bytes = pi.CommitLimit * ps;
            sc.commit_peak_bytes = pi.CommitPeak * ps;
            sc.physical_total_bytes = pi.PhysicalTotal * ps;
            sc.physical_avail_bytes = pi.PhysicalAvailable * ps;
        }
#endif
        return sc;
    }

    std::string to_json(double timestamp_s, const VramBudget& vram) const {
        char buf[2048];
        const auto pm = memory_figures::process_memory();
        const auto sc = query_system_commit();

        int n = std::snprintf(buf, sizeof(buf),
            "{\"event\":\"sotm_memory\",\"time_s\":%.2f,"
            "\"process_ram\":{\"private_mb\":%llu,\"working_set_mb\":%llu},"
            "\"system_commit\":{\"commit_total_mb\":%llu,\"commit_limit_mb\":%llu,\"commit_peak_mb\":%llu,\"phys_avail_mb\":%llu},"
            "\"vram\":{\"available\":%s,\"local_mb\":%llu,\"budget_mb\":%llu,\"shared_mb\":%llu},"
            "\"categories_mb\":{",
            timestamp_s,
            (unsigned long long)(pm.private_usage >> 20),
            (unsigned long long)(pm.working_set >> 20),
            (unsigned long long)(sc.commit_total_bytes >> 20),
            (unsigned long long)(sc.commit_limit_bytes >> 20),
            (unsigned long long)(sc.commit_peak_bytes >> 20),
            (unsigned long long)(sc.physical_avail_bytes >> 20),
            vram.available ? "true" : "false",
            (unsigned long long)(vram.local_usage >> 20),
            (unsigned long long)(vram.local_budget >> 20),
            (unsigned long long)(vram.shared_usage >> 20)
        );

        for (size_t i = 0; i < static_cast<size_t>(Category::Count); ++i) {
            const auto cat = static_cast<Category>(i);
            const uint64_t mb = categories_[i].load(std::memory_order_relaxed) >> 20;
            n += std::snprintf(buf + n, sizeof(buf) - n, "%s\"%s\":%llu",
                (i > 0 ? "," : ""), category_name(cat), (unsigned long long)mb);
        }

        n += std::snprintf(buf + n, sizeof(buf) - n,
            "},\"sotm_ops\":{\"evictions\":%llu,\"evicted_mb\":%llu,\"decommits\":%llu,\"decommitted_mb\":%llu,\"mip_capped\":%llu,\"mip_saved_mb\":%llu}",
            (unsigned long long)evictions_.load(std::memory_order_relaxed),
            (unsigned long long)(evicted_bytes_.load(std::memory_order_relaxed) >> 20),
            (unsigned long long)decommits_.load(std::memory_order_relaxed),
            (unsigned long long)(decommitted_bytes_.load(std::memory_order_relaxed) >> 20),
            (unsigned long long)mip_capped_.load(std::memory_order_relaxed),
            (unsigned long long)(mip_saved_bytes_.load(std::memory_order_relaxed) >> 20));

        if (n < (int)sizeof(buf) - 1) {
            buf[n++] = '}';
            buf[n] = '\0';
        }

        return std::string(buf);
    }

    void dump_report(const char* filepath, const VramBudget& vram) const {
        if (!filepath) return;
        FILE* f = nullptr;
#ifdef _WIN32
        fopen_s(&f, filepath, "w");
#else
        f = std::fopen(filepath, "w");
#endif
        if (!f) return;
        const std::string json = to_json(0.0, vram);
        std::fwrite(json.data(), 1, json.size(), f);
        std::fputc('\n', f);
        std::fclose(f);
    }

    void dump_mem_map(FILE* out) const {
        if (!out) out = stderr;
        std::fprintf(out, "=== SOTM Memory Map (Category Breakdown) ===\n");
        for (size_t i = 0; i < static_cast<size_t>(Category::Count); ++i) {
            const auto cat = static_cast<Category>(i);
            const uint64_t b = categories_[i].load(std::memory_order_relaxed);
            std::fprintf(out, "  %-24s: %8llu MB (%llu bytes)\n",
                category_name(cat), (unsigned long long)(b >> 20), (unsigned long long)b);
        }
        const auto sc = query_system_commit();
        std::fprintf(out, "  System Commit: %llu / %llu MB (Peak: %llu MB)\n",
            (unsigned long long)(sc.commit_total_bytes >> 20),
            (unsigned long long)(sc.commit_limit_bytes >> 20),
            (unsigned long long)(sc.commit_peak_bytes >> 20));
        std::fprintf(out, "============================================\n");
        std::fflush(out);
    }

    void start_reporter(int interval_s, const std::string& log_path) {
        if (reporter_running_.exchange(true)) return;
        interval_s_ = interval_s > 0 ? interval_s : 5;
        log_path_ = log_path;
        start_time_ = std::chrono::steady_clock::now();
        reporter_thread_ = std::thread([this]() {
            while (reporter_running_.load()) {
                std::this_thread::sleep_for(std::chrono::seconds(interval_s_));
                if (!reporter_running_.load()) break;

                const auto now = std::chrono::steady_clock::now();
                const double elapsed = std::chrono::duration<double>(now - start_time_).count();
                const auto vram = memory_figures::latest_vram();
                const std::string json = to_json(elapsed, vram);

                if (!log_path_.empty()) {
                    FILE* f = nullptr;
#ifdef _WIN32
                    fopen_s(&f, log_path_.c_str(), "a");
#else
                    f = std::fopen(log_path_.c_str(), "a");
#endif
                    if (f) {
                        std::fwrite(json.data(), 1, json.size(), f);
                        std::fputc('\n', f);
                        std::fclose(f);
                    }
                }
                std::fprintf(stderr, "[sotm] %s\n", json.c_str());
                std::fflush(stderr);
            }
        });
    }

    void stop_reporter() {
        if (reporter_running_.exchange(false)) {
            if (reporter_thread_.joinable()) {
                reporter_thread_.join();
            }
        }
    }

private:
    Accounting() {
        for (auto& cat : categories_) cat.store(0, std::memory_order_relaxed);
        leonida::offline::MemoryProfileLoader::instance().open("memory_profile.bin");
    }
    ~Accounting() {
        stop_reporter();
    }

    std::atomic<uint64_t> categories_[static_cast<size_t>(Category::Count)];
    std::mutex vk_allocs_mutex_;
    std::unordered_map<uint64_t, std::pair<Category, uint64_t>> vk_allocs_;
    std::atomic<uint64_t> evictions_{0}, evicted_bytes_{0};
    std::atomic<uint64_t> decommits_{0}, decommitted_bytes_{0};
    std::atomic<uint64_t> mip_capped_{0}, mip_saved_bytes_{0};
    std::atomic<bool> reporter_running_{false};
    int interval_s_{5};
    std::string log_path_;
    std::chrono::steady_clock::time_point start_time_;
    std::thread reporter_thread_;
};

inline void track_alloc(Category cat, uint64_t bytes) { Accounting::instance().track_alloc(cat, bytes); }
inline void track_free(Category cat, uint64_t bytes) { Accounting::instance().track_free(cat, bytes); }
inline void track_vk_alloc(uint64_t handle, Category cat, uint64_t bytes) {
    Accounting::instance().track_vk_alloc(handle, cat, bytes);
}
inline void track_vk_free(uint64_t handle) { Accounting::instance().track_vk_free(handle); }
inline void track_evict(uint64_t bytes) { Accounting::instance().track_evict(bytes); }
inline void track_decommit(uint64_t bytes) { Accounting::instance().track_decommit(bytes); }
inline void track_mip_cap(uint64_t saved_bytes) { Accounting::instance().track_mip_cap(saved_bytes); }
inline void set_gauge(Category cat, uint64_t bytes) { Accounting::instance().set_gauge(cat, bytes); }
inline uint64_t get_bytes(Category cat) { return Accounting::instance().get_bytes(cat); }
inline void dump_mem_map(FILE* out = stderr) { Accounting::instance().dump_mem_map(out); }

// SOTM Component C11: Memory Governor & Preset Auto-Selector
enum class Preset : uint8_t {
    Auto,
    A, // 16GB RAM / 4-6GB VRAM, Series S, 2.5GB texture pool, 64MB staging
    B, // 24GB RAM / 6GB VRAM, Series S profile, 3.5GB texture pool, 64-128MB staging
    C  // 32GB RAM / 8-12GB VRAM, Series X, 5GB+ texture pool, 128-192MB staging
};

struct GovernorPreset {
    Preset preset;
    const char* name;
    const char* console_profile;
    uint32_t texture_pool_mb;
    uint32_t staging_mb;
    uint32_t ring_mb;
};

inline GovernorPreset detect_and_apply_governor() {
    uint64_t total_ram_mb = 16384;
#ifdef _WIN32
    MEMORYSTATUSEX mem_status{sizeof(MEMORYSTATUSEX)};
    if (GlobalMemoryStatusEx(&mem_status)) {
        total_ram_mb = mem_status.ullTotalPhys >> 20;
    }
#endif

    Preset chosen = Preset::B; // Default target: Owner's RTX 3050 6GB / 24GB RAM PC
    char env_preset[16]{};
#ifdef _WIN32
    DWORD len = GetEnvironmentVariableA("LEONIDA_MEM_PRESET", env_preset, sizeof(env_preset));
    if (len > 0) {
        if (_stricmp(env_preset, "A") == 0) chosen = Preset::A;
        else if (_stricmp(env_preset, "B") == 0) chosen = Preset::B;
        else if (_stricmp(env_preset, "C") == 0) chosen = Preset::C;
    } else {
        if (total_ram_mb <= 17000) chosen = Preset::A;
        else if (total_ram_mb <= 26000) chosen = Preset::B;
        else chosen = Preset::C;
    }
#endif

    GovernorPreset p{};
    p.preset = chosen;
    switch (chosen) {
    case Preset::A:
        p.name = "A (16GB RAM / 4-6GB VRAM)";
        p.console_profile = "series-s";
        p.texture_pool_mb = 2560;
        p.staging_mb = 64;
        p.ring_mb = 128;
        break;
    case Preset::B:
        p.name = "B (24GB RAM / 6GB VRAM - Primary Target)";
        p.console_profile = "series-s";
        p.texture_pool_mb = 3584;
        p.staging_mb = 64;
        p.ring_mb = 128;
        break;
    case Preset::C:
        p.name = "C (32GB+ RAM / 8-12GB VRAM)";
        p.console_profile = "series-x";
        p.texture_pool_mb = 5120;
        p.staging_mb = 128;
        p.ring_mb = 256;
        break;
    default:
        p.name = "B";
        p.console_profile = "series-s";
        p.texture_pool_mb = 3584;
        p.staging_mb = 64;
        p.ring_mb = 128;
        break;
    }

#ifdef _WIN32
    // If not explicitly overridden by user, apply governor defaults
    char existing[32]{};
    if (GetEnvironmentVariableA("LEONIDA_CONSOLE_PROFILE", existing, sizeof(existing)) == 0) {
        SetEnvironmentVariableA("LEONIDA_CONSOLE_PROFILE", p.console_profile);
    }
    if (GetEnvironmentVariableA("LEONIDA_MEM_STAGE_MB", existing, sizeof(existing)) == 0) {
        char val[16];
        snprintf(val, sizeof(val), "%u", p.staging_mb);
        SetEnvironmentVariableA("LEONIDA_MEM_STAGE_MB", val);
    }
    if (GetEnvironmentVariableA("LEONIDA_MEM_RING_MB", existing, sizeof(existing)) == 0) {
        char val[16];
        snprintf(val, sizeof(val), "%u", p.ring_mb);
        SetEnvironmentVariableA("LEONIDA_MEM_RING_MB", val);
    }
#endif

    std::fprintf(stderr,
                 "[sotm] Memory Governor: active Preset %s (System RAM: %llu MB, Profile: %s, Staging: %u MB, Ring: %u MB)\n",
                 p.name, static_cast<unsigned long long>(total_ram_mb), p.console_profile, p.staging_mb, p.ring_mb);
    return p;
}

}  // namespace leonida::sotm
