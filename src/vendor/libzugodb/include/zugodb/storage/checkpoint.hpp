#pragma once

#include "zugodb/storage/block_manager.hpp"
#include "zugodb/catalog/catalog.hpp"
#include "zugodb/storage/data_table.hpp"
#include <cstddef>
#include <cstdint>
#include <string>

namespace zugodb {

// Checkpoint: serialize/deserialize the entire database to/from a .zugodb file.
class Checkpoint {
public:
    // Write the entire database to a file.
    static void Save(Catalog &catalog, const std::string &path);

    // Load a database from a file. Refuses a file that BlockManager::OpenFile
    // refuses, then one whose checksum does not match, before reading any
    // table. Every length, count and offset the file holds is then checked
    // against the bytes left before it is used, so a file that passes the
    // checksum but was not written by Save is refused too (design L12).
    // The tables are added to `catalog` only once the whole file has been
    // read: a refused file leaves it as it was.
    static void Load(Catalog &catalog, const std::string &path);

    // The checksum of a whole database file image: CRC-32 (Crc32 in
    // zugodb/common/crc32.hpp) of all `size` bytes, with the 4 bytes at
    // FILE_CHECKSUM_OFFSET, where the checksum is stored, read as zero.
    // Save stores it and Load checks it; a fuzz harness recomputes it.
    static uint32_t ComputeChecksum(const uint8_t *data, size_t size);

private:
    // ComputeChecksum over the first `size` bytes of an open file, read in
    // pieces.
    static uint32_t ChecksumFile(BlockManager &bm, idx_t size);

    // Serialization helpers. Reading goes through a bounded reader in
    // checkpoint.cpp.
    static void WriteString(BlockManager &bm, idx_t &offset, const std::string &str);
    static void WriteU32(BlockManager &bm, idx_t &offset, uint32_t val);
    static void WriteU64(BlockManager &bm, idx_t &offset, uint64_t val);
    static void WriteI32(BlockManager &bm, idx_t &offset, int32_t val);
    static void WriteI64(BlockManager &bm, idx_t &offset, int64_t val);
    static void WriteDouble(BlockManager &bm, idx_t &offset, double val);
    static void WriteU8(BlockManager &bm, idx_t &offset, uint8_t val);

    // Serialize a Value.
    static void WriteValue(BlockManager &bm, idx_t &offset, const Value &val,
                            const LogicalType &type);
};

} // namespace zugodb
