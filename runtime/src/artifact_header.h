#pragma once
// Single-Owner Tiered Memory & Offline Pipeline — Section 5.2 Header
// Shared 88-byte binary header for all Leonida offline artifacts.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace leonida::offline {

#pragma pack(push, 1)
struct ArtifactHeader {
    char     magic[8];          // e.g. "LNDSHPK1", "LNDTXPK1", "LNDPTCH1", "LNDMPRF1"
    uint32_t format_version;
    uint32_t header_size;       // exactly 96
    uint8_t  key[32];           // SHA-256 over key inputs
    uint64_t created_utc;
    uint64_t payload_size;
    uint8_t  payload_sha256[32];
};
#pragma pack(pop)

static_assert(sizeof(ArtifactHeader) == 96, "ArtifactHeader must be exactly 96 bytes");

inline bool validate_header(const void* data, std::size_t size, const char* expected_magic, uint32_t expected_version) {
    if (!data || size < sizeof(ArtifactHeader)) return false;
    const auto* h = static_cast<const ArtifactHeader*>(data);
    if (std::memcmp(h->magic, expected_magic, 8) != 0) return false;
    if (h->format_version != expected_version) return false;
    if (h->header_size != sizeof(ArtifactHeader)) return false;
    if (size < sizeof(ArtifactHeader) + h->payload_size) return false;
    return true;
}

inline const ArtifactHeader* get_header(const void* data, std::size_t size) {
    if (!data || size < sizeof(ArtifactHeader)) return nullptr;
    const auto* h = static_cast<const ArtifactHeader*>(data);
    if (h->header_size != sizeof(ArtifactHeader)) return nullptr;
    return h;
}

inline const uint8_t* get_payload(const void* data, std::size_t size) {
    if (!data || size < sizeof(ArtifactHeader)) return nullptr;
    return static_cast<const uint8_t*>(data) + sizeof(ArtifactHeader);
}

}  // namespace leonida::offline
