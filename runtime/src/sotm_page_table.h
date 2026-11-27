#pragma once
// Single-Owner Tiered Memory (SOTM) — Component C4 / Section 4.4: 64 KiB Page Table
// Manages single-owner state, classification, and provenance per 64 KiB guest page.

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace leonida::sotm {

constexpr size_t kPageSize = 64 * 1024; // 64 KiB granularity
constexpr uint32_t kInvalidIndex = 0xFFFFFFFF;

enum class PageState : uint8_t {
    HostOnly = 0,        // CPU owns; no device copy
    DeviceOnly,          // GPU owns; host pages decommitted (address stays reserved)
    BothClean,           // Transient: uploaded, host copy not yet dropped
    BothDirty,           // CPU wrote after upload; needs re-upload
    EvictedRestreamable  // Neither tier holds it; provenance can rebuild it from package
};

enum class AllocClass : uint8_t {
    GpuOnly = 0,
    UploadOnce,
    CpuWriteGpuRead,
    GpuWriteCpuRead,
    ColdShared,
    CpuOnly
};

struct Provenance {
    uint32_t file_id = 0;    // Index into package file table
    uint64_t offset = 0;     // Offset within the file
    uint32_t length = 0;     // Compressed / source length
    uint16_t codec = 0;      // 0=raw, 1=zlib, 2=bcpack, etc.
    uint16_t reserved = 0;
};

struct PageEntry {
    PageState   state = PageState::HostOnly;
    AllocClass  cls = AllocClass::CpuOnly;
    uint16_t    faultCount = 0;
    uint32_t    lastUseFrame = 0;
    uint32_t    deviceBlock = kInvalidIndex;
    uint32_t    provIndex = kInvalidIndex;
};

class PageTable {
public:
    static PageTable& instance() {
        static PageTable inst;
        return inst;
    }

    // Two-level directory: covers up to 64 GB of guest address space
    // Directory level: bits 25..35 (2048 entries, each represents 32 MiB)
    // Leaf level: bits 16..24 (512 entries per leaf, each represents 64 KiB)
    static constexpr size_t kDirBits = 11;
    static constexpr size_t kDirSize = 1ULL << kDirBits; // 2048
    static constexpr size_t kLeafBits = 9;
    static constexpr size_t kLeafSize = 1ULL << kLeafBits; // 512

    PageEntry* get_page(uintptr_t addr) {
        const size_t dir_idx = (addr >> 25) & (kDirSize - 1);
        const size_t leaf_idx = (addr >> 16) & (kLeafSize - 1);

        LeafTable* leaf = root_[dir_idx].load(std::memory_order_acquire);
        if (!leaf) return nullptr;
        return &leaf->entries[leaf_idx];
    }

    PageEntry* get_or_create_page(uintptr_t addr) {
        const size_t dir_idx = (addr >> 25) & (kDirSize - 1);
        const size_t leaf_idx = (addr >> 16) & (kLeafSize - 1);

        LeafTable* leaf = root_[dir_idx].load(std::memory_order_acquire);
        if (!leaf) {
            std::lock_guard lock(alloc_mutex_);
            leaf = root_[dir_idx].load(std::memory_order_relaxed);
            if (!leaf) {
                leaf = new LeafTable();
                root_[dir_idx].store(leaf, std::memory_order_release);
            }
        }
        return &leaf->entries[leaf_idx];
    }

    void set_range(uintptr_t base, size_t size, PageState state, AllocClass cls) {
        if (size == 0) return;
        const uintptr_t start_page = base & ~(kPageSize - 1);
        const uintptr_t end_page = (base + size + kPageSize - 1) & ~(kPageSize - 1);

        for (uintptr_t p = start_page; p < end_page; p += kPageSize) {
            PageEntry* entry = get_or_create_page(p);
            if (entry) {
                entry->state = state;
                entry->cls = cls;
            }
        }
    }

    void mark_gpu_upload(uintptr_t base, size_t size, uint32_t frame) {
        if (size == 0) return;
        const uintptr_t start_page = base & ~(kPageSize - 1);
        const uintptr_t end_page = (base + size + kPageSize - 1) & ~(kPageSize - 1);

        for (uintptr_t p = start_page; p < end_page; p += kPageSize) {
            PageEntry* entry = get_page(p);
            if (entry) {
                if (entry->state == PageState::HostOnly || entry->state == PageState::BothDirty) {
                    entry->state = PageState::BothClean;
                }
                entry->lastUseFrame = frame;
            }
        }
    }

    void mark_cpu_write(uintptr_t base, size_t size) {
        if (size == 0) return;
        const uintptr_t start_page = base & ~(kPageSize - 1);
        const uintptr_t end_page = (base + size + kPageSize - 1) & ~(kPageSize - 1);

        for (uintptr_t p = start_page; p < end_page; p += kPageSize) {
            PageEntry* entry = get_page(p);
            if (entry) {
                if (entry->state == PageState::BothClean || entry->state == PageState::DeviceOnly) {
                    entry->state = PageState::BothDirty;
                }
                entry->provIndex = kInvalidIndex; // Page modified; provenance invalidated
            }
        }
    }

    uint32_t record_provenance(uintptr_t base, size_t size, uint32_t file_id, uint64_t offset, uint32_t length, uint16_t codec) {
        std::lock_guard lock(prov_mutex_);
        const uint32_t idx = static_cast<uint32_t>(provenance_table_.size());
        Provenance prov;
        prov.file_id = file_id;
        prov.offset = offset;
        prov.length = length;
        prov.codec = codec;
        provenance_table_.push_back(prov);

        const uintptr_t start_page = base & ~(kPageSize - 1);
        const uintptr_t end_page = (base + size + kPageSize - 1) & ~(kPageSize - 1);

        for (uintptr_t p = start_page; p < end_page; p += kPageSize) {
            PageEntry* entry = get_or_create_page(p);
            if (entry) {
                entry->provIndex = idx;
            }
        }
        return idx;
    }

    const Provenance* get_provenance(uintptr_t addr) const {
        const size_t dir_idx = (addr >> 25) & (kDirSize - 1);
        const size_t leaf_idx = (addr >> 16) & (kLeafSize - 1);

        LeafTable* leaf = root_[dir_idx].load(std::memory_order_acquire);
        if (!leaf) return nullptr;

        const uint32_t idx = leaf->entries[leaf_idx].provIndex;
        if (idx == kInvalidIndex || idx >= provenance_table_.size()) return nullptr;
        return &provenance_table_[idx];
    }

    struct PageStats {
        size_t host_only = 0;
        size_t device_only = 0;
        size_t both_clean = 0;
        size_t both_dirty = 0;
        size_t evicted = 0;
    };

    PageStats collect_stats() const {
        PageStats s;
        for (size_t d = 0; d < kDirSize; ++d) {
            LeafTable* leaf = root_[d].load(std::memory_order_acquire);
            if (!leaf) continue;
            for (size_t l = 0; l < kLeafSize; ++l) {
                switch (leaf->entries[l].state) {
                case PageState::HostOnly:            s.host_only++; break;
                case PageState::DeviceOnly:          s.device_only++; break;
                case PageState::BothClean:           s.both_clean++; break;
                case PageState::BothDirty:           s.both_dirty++; break;
                case PageState::EvictedRestreamable: s.evicted++; break;
                }
            }
        }
        return s;
    }

private:
    struct LeafTable {
        PageEntry entries[kLeafSize];
    };

    PageTable() {
        for (auto& slot : root_) slot.store(nullptr, std::memory_order_relaxed);
    }

    ~PageTable() {
        for (auto& slot : root_) {
            LeafTable* leaf = slot.load(std::memory_order_relaxed);
            delete leaf;
        }
    }

    std::atomic<LeafTable*> root_[kDirSize];
    std::mutex alloc_mutex_;
    std::mutex prov_mutex_;
    std::vector<Provenance> provenance_table_;
};

inline PageTable& page_table() { return PageTable::instance(); }

} // namespace leonida::sotm
