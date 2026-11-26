#pragma once

// CRC-32 as zlib, PNG and gzip compute it (IEEE 802.3, reflected polynomial
// 0xEDB88320, initial value and final xor 0xFFFFFFFF). The check value over
// "123456789" is 0xCBF43926. A running value chains as zlib's crc32():
// Crc32(b, nb, Crc32(a, na)) == Crc32(a then b). The database file's
// checksum (Checkpoint::ComputeChecksum) is this CRC.

#include <array>
#include <cstddef>
#include <cstdint>

namespace zugodb {

namespace crc32_detail {

constexpr std::array<uint32_t, 256> MakeCrc32Table() {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        table[i] = c;
    }
    return table;
}

// Constant-initialised: no static initialiser runs at load.
inline constexpr std::array<uint32_t, 256> CRC32_TABLE = MakeCrc32Table();

} // namespace crc32_detail

inline uint32_t Crc32(const uint8_t *data, size_t size, uint32_t seed = 0) {
    uint32_t c = seed ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < size; i++) {
        c = crc32_detail::CRC32_TABLE[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

} // namespace zugodb
