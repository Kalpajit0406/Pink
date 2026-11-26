#pragma once
// The memory figures that the title HUD (shims/title_hud.h, LEONIDA_TITLE_HUD) and the dev overlay (dev_overlay.h,
// LEONIDA_DEV_OVERLAY) both show, and the text for each. Both views use this header, so they show the same figures
// and the same "n/a" where there is no figure, never a false 0:
//   GPU memory: in use and the budget over the device-local heaps, and in use in the other heaps, from the device's
//     VK_EXT_memory_budget (vulkan_memory_budget.h reads it). The device publishes a reader here (publish_vram_reader)
//     because the overlay runs outside the device, and latest_vram() calls it at most once per refresh_ms.
//   Private memory: the process's commit charge. Wine fills only PagefileUsage, which on Windows 8 and later is the
//     same figure as PrivateUsage.
// No Vulkan here, and Win32 only under _WIN32, so the overlay's test builds on every host.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#ifdef _MSC_VER
#pragma comment(lib, "psapi.lib")
#endif
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

namespace leonida {

struct VramBudget {
    // False when there is nothing to show: no VK_EXT_memory_budget on the device, no device, or a device that
    // reports no budget for any DEVICE_LOCAL heap. The views show "n/a" then, not a 0.
    bool available = false;
    std::uint64_t local_usage = 0;   // heapUsage summed over the DEVICE_LOCAL heaps
    std::uint64_t local_budget = 0;  // heapBudget summed over the DEVICE_LOCAL heaps
    std::uint64_t shared_usage = 0;  // heapUsage summed over the other heaps: system memory the GPU uses
};

namespace memory_figures {

inline double gb(std::uint64_t bytes) { return double(bytes) / double(1ull << 30); }

// Both figures are shown in whole MB: a title bar has room for the number a player can act on, and a GB figure
// rounds away the differences that matter on a 6 GiB card.
inline std::uint64_t mb(std::uint64_t bytes) { return (bytes + (1ull << 19)) >> 20; }

// "VRAM 3584/16384 MB +512 shared", or "VRAM n/a".
inline std::string vram_text(const VramBudget& vram) {
    if (!vram.available) return "VRAM n/a";
    char text[96];
    std::snprintf(text, sizeof text, "VRAM %llu/%llu MB +%llu shared", (unsigned long long)mb(vram.local_usage),
                  (unsigned long long)mb(vram.local_budget), (unsigned long long)mb(vram.shared_usage));
    return text;
}

struct ProcessMemory {
    bool read = false;  // GetProcessMemoryInfo answered
    std::uint64_t private_usage = 0, pagefile_usage = 0, working_set = 0;
};

// The commit charge. PrivateUsage and PagefileUsage are the same figure on Windows 8 and later. Wine's
// GetProcessMemoryInfo writes only the PROCESS_MEMORY_COUNTERS part, so PrivateUsage reads 0 there.
inline std::uint64_t private_bytes(const ProcessMemory& memory) {
    return (std::max)(memory.private_usage, memory.pagefile_usage);  // parenthesised: windows.h may define max
}

// "RAM 2560 MB (ws 1024)". "RAM n/a (ws 1024)" when the private bytes read 0, "RAM n/a" when nothing could be
// read. MB, like the VRAM figure, so the title bar shows the figures a player can compare with the card's capacity.
inline std::string ram_text(const ProcessMemory& memory) {
    char text[96];
    if (!memory.read)
        std::snprintf(text, sizeof text, "RAM n/a");
    else if (const std::uint64_t bytes = private_bytes(memory); !bytes)
        std::snprintf(text, sizeof text, "RAM n/a (ws %llu)", (unsigned long long)mb(memory.working_set));
    else
        std::snprintf(text, sizeof text, "RAM %llu MB (ws %llu)", (unsigned long long)mb(bytes),
                      (unsigned long long)mb(memory.working_set));
    return text;
}

#ifdef _WIN32
// This process's figures now.
inline ProcessMemory process_memory() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof counters;
    ProcessMemory out;
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                              sizeof counters))
        return out;
    out.read = true;
    out.private_usage = counters.PrivateUsage;
    out.pagefile_usage = counters.PagefileUsage;
    out.working_set = counters.WorkingSetSize;
    return out;
}
#endif

// The device's figure, published for views outside the device. A device with VK_EXT_memory_budget publishes a reader
// when it is created and withdraws it first thing when it goes. The views call latest_vram(), which calls the reader at
// most once per refresh_ms and returns the last figure in between. The lock is held during a read, so withdrawing
// waits for a read that is in progress, and a reader is never called after its device is gone.
using VramReader = VramBudget (*)(const void* context);
inline constexpr std::chrono::milliseconds refresh_ms{500};

class VramPublisher {
public:
    void publish(VramReader reader, const void* context) {
        std::lock_guard lock(mutex_);
        reader_ = reader;
        context_ = context;
        last_ = {};
        read_ = false;
    }
    // Only the device that published the current reader withdraws it. Another device may have published since.
    void withdraw(const void* context) {
        std::lock_guard lock(mutex_);
        if (context_ != context) return;
        reader_ = nullptr;
        context_ = nullptr;
        last_ = {};
        read_ = false;
    }
    VramBudget latest(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) {
        std::lock_guard lock(mutex_);
        if (!reader_) return {};
        if (!read_ || now - at_ >= refresh_ms) {
            last_ = reader_(context_);
            at_ = now;
            read_ = true;
        }
        return last_;
    }

private:
    std::mutex mutex_;
    VramReader reader_ = nullptr;
    const void* context_ = nullptr;
    VramBudget last_;
    std::chrono::steady_clock::time_point at_{};
    bool read_ = false;
};

inline VramPublisher& vram_publisher() {
    static VramPublisher instance;
    return instance;
}
inline void publish_vram_reader(VramReader reader, const void* context) { vram_publisher().publish(reader, context); }
inline void withdraw_vram_reader(const void* context) { vram_publisher().withdraw(context); }
inline VramBudget latest_vram() { return vram_publisher().latest(); }

}  // namespace memory_figures
}  // namespace leonida
