#pragma once
// Single-Owner Tiered Memory (SOTM) — Real Budget Enforcement, Pressure Tiers & Adaptive Mip Policy.
// Reads LEONIDA_MAX_VRAM_MB, LEONIDA_MAX_HOST_RAM_MB, LEONIDA_MEM_STAGE_MB, LEONIDA_MEM_RING_MB,
// LEONIDA_MEM_MIP_CAP, LEONIDA_MEM_EVICT, LEONIDA_MEM_TEXTURE_STAGE_MS, and LEONIDA_MEM_HANDOFF.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "memory_figures.h"

namespace leonida::sotm {

enum class PressureTier : uint8_t {
    Normal = 0,
    Elevated,   // 70-85% effective budget
    Critical,   // 85-95% effective budget
    OverBudget  // >= 95% effective budget
};

inline uint64_t env_u64(const char* name, uint64_t fallback, uint64_t min_val, uint64_t max_val) {
    if (!name) return fallback;
    char buf[32]{};
#ifdef _WIN32
    const DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) return fallback;
#else
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    std::strncpy(buf, v, sizeof(buf) - 1);
#endif
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(buf, &end, 10);
    if (!end || end == buf) return fallback;
    return (std::clamp)(static_cast<uint64_t>(parsed), min_val, max_val);
}

inline bool env_bool(const char* name, bool fallback = false) {
    if (!name) return fallback;
#ifdef _WIN32
    char buf[8]{};
    const DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) return fallback;
    return buf[0] != '0';
#else
    const char* v = std::getenv(name);
    return v && *v ? (v[0] != '0') : fallback;
#endif
}

struct BudgetConfig {
    uint64_t max_vram_bytes = 0;        // LEONIDA_MAX_VRAM_MB * 1 MiB (0 = use device budget only)
    uint64_t max_host_ram_bytes = 0;    // LEONIDA_MAX_HOST_RAM_MB * 1 MiB (0 = unbounded)
    uint64_t stage_bytes = 64ull << 20; // LEONIDA_MEM_STAGE_MB (16..512 MiB)
    uint64_t ring_bytes = 128ull << 20; // LEONIDA_MEM_RING_MB (32..1024 MiB)
    uint32_t texture_stage_ms = 6;      // LEONIDA_MEM_TEXTURE_STAGE_MS (0..50 ms)
    uint32_t mip_max_normal = 1024;     // LEONIDA_MEM_MIP_MAX_DIM (256..4096)
    bool mip_cap = false;               // LEONIDA_MEM_MIP_CAP=1
    bool evict = false;                 // LEONIDA_MEM_EVICT=1
    bool handoff = false;               // LEONIDA_MEM_HANDOFF=1
};

inline BudgetConfig load_budget_config() {
    BudgetConfig c{};
    const uint64_t vram_mb = env_u64("LEONIDA_MAX_VRAM_MB", 0, 512, 65536);
    const uint64_t ram_mb = env_u64("LEONIDA_MAX_HOST_RAM_MB", 0, 2048, 262144);
    c.max_vram_bytes = (env_u64("LEONIDA_MAX_VRAM_MB", 0, 0, 65536) > 0) ? (vram_mb << 20) : 0;
    c.max_host_ram_bytes = (env_u64("LEONIDA_MAX_HOST_RAM_MB", 0, 0, 262144) > 0) ? (ram_mb << 20) : 0;
    c.stage_bytes = env_u64("LEONIDA_MEM_STAGE_MB", 64, 16, 512) << 20;
    c.ring_bytes = env_u64("LEONIDA_MEM_RING_MB", 128, 32, 1024) << 20;
    c.texture_stage_ms = static_cast<uint32_t>(env_u64("LEONIDA_MEM_TEXTURE_STAGE_MS", 6, 0, 50));
    c.mip_max_normal = static_cast<uint32_t>(env_u64("LEONIDA_MEM_MIP_MAX_DIM", 1024, 256, 4096));
    c.mip_cap = env_bool("LEONIDA_MEM_MIP_CAP", false);
    c.evict = env_bool("LEONIDA_MEM_EVICT", false);
    c.handoff = env_bool("LEONIDA_MEM_HANDOFF", false);
    return c;
}

inline const BudgetConfig& budget_config() {
    static const BudgetConfig cfg = load_budget_config();
    return cfg;
}

inline uint64_t effective_vram_budget(const VramBudget& vram, const BudgetConfig& cfg) {
    if (!vram.available || !vram.local_budget) return cfg.max_vram_bytes;
    return cfg.max_vram_bytes ? (std::min)(vram.local_budget, cfg.max_vram_bytes) : vram.local_budget;
}

inline PressureTier evaluate_pressure(const VramBudget& vram, uint64_t process_private_bytes,
                                      const BudgetConfig& cfg = budget_config()) {
    double ratio = 0.0;
    const uint64_t vram_cap = effective_vram_budget(vram, cfg);
    if (vram_cap > 0 && vram.available) {
        ratio = (std::max)(ratio, static_cast<double>(vram.local_usage) / static_cast<double>(vram_cap));
    }
    if (cfg.max_host_ram_bytes > 0 && process_private_bytes > 0) {
        ratio = (std::max)(ratio, static_cast<double>(process_private_bytes) / static_cast<double>(cfg.max_host_ram_bytes));
    }
    if (ratio >= 0.95) return PressureTier::OverBudget;
    if (ratio >= 0.85) return PressureTier::Critical;
    if (ratio >= 0.70) return PressureTier::Elevated;
    return PressureTier::Normal;
}

inline PressureTier current_pressure(const BudgetConfig& cfg = budget_config()) {
    static std::mutex mu;
    static auto last_sample = std::chrono::steady_clock::time_point{};
    static std::atomic<uint8_t> cached{static_cast<uint8_t>(PressureTier::Normal)};
    const auto now = std::chrono::steady_clock::now();
    if (now - last_sample < std::chrono::milliseconds(250)) {
        return static_cast<PressureTier>(cached.load(std::memory_order_relaxed));
    }
    std::lock_guard lock(mu);
    if (now - last_sample >= std::chrono::milliseconds(250)) {
        const VramBudget vram = memory_figures::latest_vram();
        const uint64_t priv = cfg.max_host_ram_bytes ? memory_figures::private_bytes(memory_figures::process_memory()) : 0;
        cached.store(static_cast<uint8_t>(evaluate_pressure(vram, priv, cfg)), std::memory_order_relaxed);
        last_sample = now;
    }
    return static_cast<PressureTier>(cached.load(std::memory_order_relaxed));
}

// Adaptive mip ceiling: drops top mips only on multi-mip textures while preserving mip chains >= 1 mip.
inline uint32_t choose_first_mip(uint32_t width, uint32_t height, uint32_t mip_levels, PressureTier tier,
                                 uint32_t max_normal_dim = 1024) {
    if (mip_levels <= 1) return 0;
    uint32_t target_max = max_normal_dim;
    switch (tier) {
    case PressureTier::Normal:     target_max = max_normal_dim; break;
    case PressureTier::Elevated:   target_max = (std::min)(max_normal_dim, 512u); break;
    case PressureTier::Critical:
    case PressureTier::OverBudget: target_max = (std::min)(max_normal_dim, 256u); break;
    }
    uint32_t first_mip = 0;
    uint32_t w = width, h = height;
    while (first_mip + 1 < mip_levels && (w > target_max || h > target_max)) {
        w = (std::max)(1u, w >> 1);
        h = (std::max)(1u, h >> 1);
        ++first_mip;
    }
    return first_mip;
}

}  // namespace leonida::sotm
