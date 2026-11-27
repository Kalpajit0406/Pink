#pragma once
// Offline Shader Archive & Index Loader (Milestones O3 & O5)
// Memory-maps shaders.pack and provides O(log N) lookup into shaders.idx.
// Enables on-demand VkShaderModule creation and immediate reclamation.
// Section 7.1 of OFFLINE_PIPELINE_SPEC.md.

#include "artifact_header.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace leonida::offline {

// Magic constants
inline constexpr char kMagicShaderPack[8]  = {'L', 'N', 'D', 'S', 'H', 'P', 'K', '1'};
inline constexpr char kMagicShaderIndex[8] = {'L', 'N', 'D', 'S', 'H', 'I', 'X', '1'};

// Variant kinds
inline constexpr uint32_t kVariantKindTranslated = 0;
inline constexpr uint32_t kVariantKindSpirvOpt   = 1;
inline constexpr uint32_t kVariantKindNative32    = 2;
inline constexpr uint32_t kVariantKindLowCost     = 3;

// FlagsA bits
inline constexpr uint8_t kFlagWave64          = 1 << 0;
inline constexpr uint8_t kFlagCrossLane      = 1 << 1;
inline constexpr uint8_t kFlagCrossHalfLane  = 1 << 2;
inline constexpr uint8_t kFlagEmulatedW64    = 1 << 3;
inline constexpr uint8_t kFlagQuadOps        = 1 << 4;
inline constexpr uint8_t kFlagTierAWave32    = 1 << 5;

#pragma pack(push, 1)

struct ShaderVariant {
    uint32_t kind;      // 0 = translated, 1 = spirv-opt, 2 = native32, 3 = low_cost
    uint32_t length;    // bytes
    uint64_t offset;    // byte offset within shaders.pack payload
};

struct ShaderIndexEntry {
    uint64_t shader_hash;        // uint64_t derived from sha256 prefix
    uint8_t  fssm_sha256[32];     // 32-byte content hash for exact matching
    uint8_t  stage;              // 0=VS, 1=PS, 2=CS, 3=GS, 4=MS, 5=HS, 6=DS, 7=AS
    uint8_t  flagsA;             // Bit flags (kFlag*)
    uint16_t resource_layout_id; // Layout table index
    uint32_t variant_count;      // Number of active variants (1..4)
    ShaderVariant variants[4];   // Inlined variants
    uint8_t  reserved[16];       // Padding
};

#pragma pack(pop)

static_assert(sizeof(ShaderVariant) == 16, "ShaderVariant must be 16 bytes");
static_assert(sizeof(ShaderIndexEntry) == 128, "ShaderIndexEntry must be 128 bytes");

class ShaderPack {
public:
    static ShaderPack& instance() {
        static ShaderPack s_instance;
        return s_instance;
    }

    ShaderPack() = default;
    ~ShaderPack() { close(); }

    ShaderPack(const ShaderPack&) = delete;
    ShaderPack& operator=(const ShaderPack&) = delete;

    bool open(const std::filesystem::path& folder) {
        std::lock_guard lock(mutex_);
        if (loaded_) return true;

        const auto idx_path = folder / "shaders.idx";
        const auto pack_path = folder / "shaders.pack";

        std::error_code ec;
        if (!std::filesystem::exists(idx_path, ec) || !std::filesystem::exists(pack_path, ec)) {
            return false;
        }

        // 1. Read shaders.idx into memory
        std::ifstream idx_file(idx_path, std::ios::binary | std::ios::ate);
        if (!idx_file) return false;
        const auto idx_size = static_cast<std::size_t>(idx_file.tellg());
        if (idx_size < sizeof(ArtifactHeader)) return false;

        idx_buffer_.resize(idx_size);
        idx_file.seekg(0, std::ios::beg);
        if (!idx_file.read(reinterpret_cast<char*>(idx_buffer_.data()), idx_size)) {
            idx_buffer_.clear();
            return false;
        }

        if (!validate_header(idx_buffer_.data(), idx_buffer_.size(), kMagicShaderIndex, 1)) {
            idx_buffer_.clear();
            return false;
        }

        const auto* idx_hdr = get_header(idx_buffer_.data(), idx_buffer_.size());
        const auto* idx_payload = get_payload(idx_buffer_.data(), idx_buffer_.size());
        if (!idx_hdr || !idx_payload) {
            idx_buffer_.clear();
            return false;
        }

        entry_count_ = idx_hdr->payload_size / sizeof(ShaderIndexEntry);
        entries_ = reinterpret_cast<const ShaderIndexEntry*>(idx_payload);

        // 2. Memory-map shaders.pack
#if defined(_WIN32)
        pack_file_handle_ = CreateFileW(
            pack_path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS,
            nullptr
        );
        if (pack_file_handle_ == INVALID_HANDLE_VALUE) {
            close_locked();
            return false;
        }

        LARGE_INTEGER file_size;
        if (!GetFileSizeEx(pack_file_handle_, &file_size)) {
            close_locked();
            return false;
        }
        pack_file_size_ = static_cast<std::size_t>(file_size.QuadPart);
        if (pack_file_size_ < sizeof(ArtifactHeader)) {
            close_locked();
            return false;
        }

        pack_mapping_handle_ = CreateFileMappingW(
            pack_file_handle_,
            nullptr,
            PAGE_READONLY,
            0,
            0,
            nullptr
        );
        if (!pack_mapping_handle_) {
            close_locked();
            return false;
        }

        pack_view_ = MapViewOfFile(
            pack_mapping_handle_,
            FILE_MAP_READ,
            0,
            0,
            0
        );
        if (!pack_view_) {
            close_locked();
            return false;
        }

        if (!validate_header(pack_view_, pack_file_size_, kMagicShaderPack, 1)) {
            close_locked();
            return false;
        }

        const auto* pack_hdr = get_header(pack_view_, pack_file_size_);
        pack_payload_ = get_payload(pack_view_, pack_file_size_);
        pack_payload_size_ = pack_hdr ? pack_hdr->payload_size : 0;
#else
        // Portable fallback: read whole pack or stream
        std::ifstream pack_file(pack_path, std::ios::binary | std::ios::ate);
        if (!pack_file) {
            close_locked();
            return false;
        }
        pack_file_size_ = static_cast<std::size_t>(pack_file.tellg());
        if (pack_file_size_ < sizeof(ArtifactHeader)) {
            close_locked();
            return false;
        }
        pack_buffer_.resize(pack_file_size_);
        pack_file.seekg(0, std::ios::beg);
        if (!pack_file.read(reinterpret_cast<char*>(pack_buffer_.data()), pack_file_size_)) {
            close_locked();
            return false;
        }
        if (!validate_header(pack_buffer_.data(), pack_buffer_.size(), kMagicShaderPack, 1)) {
            close_locked();
            return false;
        }
        const auto* pack_hdr = get_header(pack_buffer_.data(), pack_buffer_.size());
        pack_payload_ = get_payload(pack_buffer_.data(), pack_buffer_.size());
        pack_payload_size_ = pack_hdr ? pack_hdr->payload_size : 0;
#endif

        loaded_ = true;
        return true;
    }

    void close() {
        std::lock_guard lock(mutex_);
        close_locked();
    }

    bool is_loaded() const {
        return loaded_;
    }

    std::size_t size() const {
        return entry_count_;
    }

    // Binary search by 64-bit integer hash and exact 32-byte sha256
    const ShaderIndexEntry* find_by_hash(uint64_t hash, const uint8_t* sha256 = nullptr) const {
        if (!loaded_ || !entries_ || entry_count_ == 0) return nullptr;

        const auto* first = entries_;
        const auto* last = entries_ + entry_count_;

        auto it = std::lower_bound(first, last, hash, [](const ShaderIndexEntry& e, uint64_t val) {
            return e.shader_hash < val;
        });

        if (it != last && it->shader_hash == hash) {
            if (sha256) {
                // If sha256 is provided, iterate while hash matches to verify exact 32 bytes
                for (auto cur = it; cur != last && cur->shader_hash == hash; ++cur) {
                    if (std::memcmp(cur->fssm_sha256, sha256, 32) == 0) {
                        return cur;
                    }
                }
                return nullptr;
            }
            return it;
        }
        return nullptr;
    }

    // Lookup by 64-character hex string (e.g. from fssm_sha256 or fspx_code_sha256)
    const ShaderIndexEntry* find_by_hex(std::string_view hex_str) const {
        if (hex_str.empty()) return nullptr;

        // Parse first 16 hex chars into uint64_t (little-endian byte order)
        uint8_t sha256_bytes[32] = {};
        for (std::size_t i = 0; i < 32 && (i * 2 + 1) < hex_str.size(); ++i) {
            const char c1 = hex_str[i * 2];
            const char c2 = hex_str[i * 2 + 1];
            auto parse_nibble = [](char c) -> uint8_t {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return 0;
            };
            sha256_bytes[i] = (parse_nibble(c1) << 4) | parse_nibble(c2);
        }

        uint64_t hash = 0;
        std::memcpy(&hash, sha256_bytes, sizeof(uint64_t));

        return find_by_hash(hash, sha256_bytes);
    }

    // Retrieve SPIR-V binary slice pointing directly into memory-mapped archive
    std::span<const uint32_t> get_spirv(const ShaderIndexEntry& entry, uint32_t preferred_kind = 0) const {
        if (!loaded_ || !pack_payload_) return {};

        // Find requested variant, fallback to kind 0
        const ShaderVariant* chosen = nullptr;
        for (uint32_t i = 0; i < entry.variant_count && i < 4; ++i) {
            if (entry.variants[i].kind == preferred_kind) {
                chosen = &entry.variants[i];
                break;
            }
        }
        if (!chosen && entry.variant_count > 0) {
            // Fallback to first available variant (typically kind 0 = translated)
            chosen = &entry.variants[0];
        }

        if (!chosen || chosen->length == 0) return {};
        if (chosen->offset + chosen->length > pack_payload_size_) return {};

        const auto* ptr = pack_payload_ + chosen->offset;
        return {reinterpret_cast<const uint32_t*>(ptr), chosen->length / sizeof(uint32_t)};
    }

private:
    void close_locked() {
#if defined(_WIN32)
        if (pack_view_) {
            UnmapViewOfFile(pack_view_);
            pack_view_ = nullptr;
        }
        if (pack_mapping_handle_) {
            CloseHandle(pack_mapping_handle_);
            pack_mapping_handle_ = nullptr;
        }
        if (pack_file_handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(pack_file_handle_);
            pack_file_handle_ = INVALID_HANDLE_VALUE;
        }
#else
        pack_buffer_.clear();
#endif
        idx_buffer_.clear();
        entries_ = nullptr;
        entry_count_ = 0;
        pack_payload_ = nullptr;
        pack_payload_size_ = 0;
        pack_file_size_ = 0;
        loaded_ = false;
    }

    mutable std::mutex mutex_;
    bool loaded_ = false;

    // Index data
    std::vector<uint8_t> idx_buffer_;
    const ShaderIndexEntry* entries_ = nullptr;
    std::size_t entry_count_ = 0;

    // Pack data
#if defined(_WIN32)
    HANDLE pack_file_handle_ = INVALID_HANDLE_VALUE;
    HANDLE pack_mapping_handle_ = nullptr;
    void* pack_view_ = nullptr;
#else
    std::vector<uint8_t> pack_buffer_;
#endif
    std::size_t pack_file_size_ = 0;
    const uint8_t* pack_payload_ = nullptr;
    std::size_t pack_payload_size_ = 0;
};

}  // namespace leonida::offline
