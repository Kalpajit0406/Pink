#pragma once
// Offline Memory Profile Loader (Milestone O8)
// Reads memory_profile.bin at boot, verifies ArtifactHeader,
// and provides O(log N) lookup for optimal tier classification.
// Section 7.5 of OFFLINE_PIPELINE_SPEC.md.

#include "artifact_header.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <span>
#include <vector>

namespace leonida::offline {

inline constexpr char kMagicMemProfile[8] = {'L', 'N', 'D', 'M', 'P', 'R', 'F', '1'};

enum MemoryTier : uint8_t {
    kTierGpuLocal    = 0,
    kTierHostVisible = 1,
    kTierPagedSystem = 2,
    kTierReadback    = 3,
};

#pragma pack(push, 1)

struct MemoryProfileEntry {
    uint64_t base_offset;      // range start offset
    uint64_t size_bytes;       // size
    uint8_t  recommended_tier; // MemoryTier enum
    uint8_t  access_pattern;   // bit flags
    uint16_t confidence_score; // 0..1000
    uint32_t sample_count;     // sessions observed
    uint8_t  reserved[8];      // padding
};

#pragma pack(pop)

static_assert(sizeof(MemoryProfileEntry) == 32, "MemoryProfileEntry must be 32 bytes");

class MemoryProfileLoader {
public:
    static MemoryProfileLoader& instance() {
        static MemoryProfileLoader s_inst;
        return s_inst;
    }

    bool open(const std::filesystem::path& path) {
        std::lock_guard lock(mutex_);
        if (loaded_) return true;

        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) return false;

        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) return false;

        const auto size = static_cast<std::size_t>(file.tellg());
        if (size < sizeof(ArtifactHeader)) return false;

        buffer_.resize(size);
        file.seekg(0, std::ios::beg);
        if (!file.read(reinterpret_cast<char*>(buffer_.data()), size)) {
            buffer_.clear();
            return false;
        }

        if (!validate_header(buffer_.data(), buffer_.size(), kMagicMemProfile, 1)) {
            buffer_.clear();
            return false;
        }

        const auto* hdr = get_header(buffer_.data(), buffer_.size());
        const auto* payload = get_payload(buffer_.data(), buffer_.size());
        if (!hdr || !payload) {
            buffer_.clear();
            return false;
        }

        entry_count_ = hdr->payload_size / sizeof(MemoryProfileEntry);
        entries_ = reinterpret_cast<const MemoryProfileEntry*>(payload);
        loaded_ = true;

        std::fprintf(stderr, "[rt] loader: loaded memory_profile.bin (%zu profile entries)\n", entry_count_);
        return true;
    }

    bool is_loaded() const { return loaded_; }
    std::size_t size() const { return entry_count_; }

    // Finds recommended tier for a guest allocation range
    uint8_t classify(uint64_t base, uint64_t size) const {
        if (!loaded_ || !entries_ || entry_count_ == 0) {
            return kTierPagedSystem; // Default conservative tier
        }

        const auto* first = entries_;
        const auto* last = entries_ + entry_count_;

        auto it = std::lower_bound(first, last, base, [](const MemoryProfileEntry& e, uint64_t val) {
            return e.base_offset < val;
        });

        if (it != last && it->base_offset == base) {
            return it->recommended_tier;
        }
        if (it != first) {
            --it;
            if (base < it->base_offset + it->size_bytes) {
                return it->recommended_tier;
            }
        }

        return kTierPagedSystem;
    }

private:
    std::mutex mutex_;
    bool loaded_ = false;
    std::vector<uint8_t> buffer_;
    const MemoryProfileEntry* entries_ = nullptr;
    std::size_t entry_count_ = 0;
};

}  // namespace leonida::offline
