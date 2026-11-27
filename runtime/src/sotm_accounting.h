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
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

#include "memory_figures.h"

namespace leonida::sotm {

enum class Category : uint8_t {
    GuestHeapCommitted = 0,
    GuestHeapReserved,
    TextureVram,
    RenderTargetVram,
    BufferVram,
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

        if (n < (int)sizeof(buf) - 2) {
            buf[n++] = '}';
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
    }
    ~Accounting() {
        stop_reporter();
    }

    std::atomic<uint64_t> categories_[static_cast<size_t>(Category::Count)];
    std::atomic<bool> reporter_running_{false};
    int interval_s_{5};
    std::string log_path_;
    std::chrono::steady_clock::time_point start_time_;
    std::thread reporter_thread_;
};

inline void track_alloc(Category cat, uint64_t bytes) { Accounting::instance().track_alloc(cat, bytes); }
inline void track_free(Category cat, uint64_t bytes) { Accounting::instance().track_free(cat, bytes); }
inline void set_gauge(Category cat, uint64_t bytes) { Accounting::instance().set_gauge(cat, bytes); }
inline uint64_t get_bytes(Category cat) { return Accounting::instance().get_bytes(cat); }
inline void dump_mem_map(FILE* out = stderr) { Accounting::instance().dump_mem_map(out); }

}  // namespace leonida::sotm
