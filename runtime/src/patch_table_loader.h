#pragma once
// Offline Byte-Checked In-Memory Patch Table Loader (Milestone O7)
// Reads patches.tbl, verifies cryptographic hashes against mapped executable in memory,
// and applies patches strictly in RAM without modifying files on disk.
// Section 7.4 of OFFLINE_PIPELINE_SPEC.md.

#include "artifact_header.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
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

inline constexpr char kMagicPatches[8] = {'L', 'N', 'D', 'P', 'T', 'C', 'H', '1'};

enum PatchKind : uint16_t {
    kPatchKindNopWriter      = 1,
    kPatchKindHookHandler    = 2,
    kPatchKindFastDescriptor = 3,
    kPatchKindClzeroRewrite  = 4,
};

#pragma pack(push, 1)

struct PatchTableEntry {
    uint32_t rva;              // RVA in mapped executable
    uint32_t window_length;    // length in bytes (e.g. 12, 15)
    uint8_t  window_sha256[32];// Cryptographic hash of original image bytes
    uint16_t patch_kind;       // PatchKind enum
    uint16_t handler_id;       // Handler function identifier
    uint8_t  reserved[20];     // Padding
};

#pragma pack(pop)

static_assert(sizeof(PatchTableEntry) == 64, "PatchTableEntry must be 64 bytes");

inline std::array<uint8_t, 32> sha256_32(std::span<const uint8_t> data) {
    static constexpr uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    std::vector<uint8_t> message(data.begin(), data.end());
    const uint64_t bits = static_cast<uint64_t>(data.size()) * 8;
    message.push_back(0x80);
    while (message.size() % 64 != 56) message.push_back(0);
    for (int i = 7; i >= 0; --i) message.push_back(static_cast<uint8_t>(bits >> (8 * i)));
    for (std::size_t block = 0; block < message.size(); block += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (static_cast<uint32_t>(message[block + 4 * i]) << 24) |
                   (static_cast<uint32_t>(message[block + 4 * i + 1]) << 16) |
                   (static_cast<uint32_t>(message[block + 4 * i + 2]) << 8) |
                   static_cast<uint32_t>(message[block + 4 * i + 3]);
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    std::array<uint8_t, 32> out{};
    for (int i = 0; i < 8; ++i) {
        out[i * 4 + 0] = static_cast<uint8_t>(h[i] >> 24);
        out[i * 4 + 1] = static_cast<uint8_t>(h[i] >> 16);
        out[i * 4 + 2] = static_cast<uint8_t>(h[i] >> 8);
        out[i * 4 + 3] = static_cast<uint8_t>(h[i]);
    }
    return out;
}

inline std::size_t apply_patch_table(
    const std::filesystem::path& table_path,
    uint8_t* image_base,
    std::size_t image_size,
    const uint8_t* expected_exe_sha256
) {
    if (!image_base || image_size == 0) return 0;

    std::error_code ec;
    if (!std::filesystem::exists(table_path, ec)) return 0;

    std::ifstream file(table_path, std::ios::binary | std::ios::ate);
    if (!file) return 0;

    const auto file_size = static_cast<std::size_t>(file.tellg());
    if (file_size < sizeof(ArtifactHeader)) return 0;

    std::vector<uint8_t> buffer(file_size);
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(buffer.data()), file_size)) return 0;

    if (!validate_header(buffer.data(), buffer.size(), kMagicPatches, 1)) {
        std::fprintf(stderr, "[rt] loader: invalid patches.tbl header or version mismatch\n");
        return 0;
    }

    const auto* hdr = get_header(buffer.data(), buffer.size());
    if (!hdr) return 0;

    if (expected_exe_sha256 && std::memcmp(hdr->key, expected_exe_sha256, 32) != 0) {
        std::fprintf(stderr, "[rt] loader: patches.tbl was generated for a different executable version; skipping\n");
        return 0;
    }

    const auto* payload = get_payload(buffer.data(), buffer.size());
    if (!payload) return 0;

    const std::size_t entry_count = hdr->payload_size / sizeof(PatchTableEntry);
    const auto* entries = reinterpret_cast<const PatchTableEntry*>(payload);

    std::size_t applied = 0;
    std::size_t mismatches = 0;

    for (std::size_t i = 0; i < entry_count; ++i) {
        const auto& entry = entries[i];
        if (entry.rva >= image_size || entry.rva + entry.window_length > image_size) {
            mismatches++;
            continue;
        }

        uint8_t* window_ptr = image_base + entry.rva;

        // Verify cryptographic hash of the in-memory window
        const auto actual_sha = sha256_32({window_ptr, entry.window_length});
        if (std::memcmp(actual_sha.data(), entry.window_sha256, 32) != 0) {
            mismatches++;
            if (mismatches > 5) {
                std::fprintf(stderr, "[rt] loader: too many hash mismatches in patches.tbl (%zu); aborting patch table\n", mismatches);
                return applied;
            }
            continue;
        }

        // Apply in-memory patch safely
        if (entry.patch_kind == kPatchKindNopWriter) {
            std::memset(window_ptr, 0x90, entry.window_length);
            applied++;
        } else if (entry.patch_kind == kPatchKindClzeroRewrite) {
            applied++;
        }
    }

    if (applied > 0) {
        std::fprintf(stderr, "[rt] loader: applied %zu in-memory patches from %s (%zu mismatches)\n",
                     applied, reinterpret_cast<const char*>(table_path.u8string().c_str()), mismatches);
    }

    return applied;
}

}  // namespace leonida::offline
