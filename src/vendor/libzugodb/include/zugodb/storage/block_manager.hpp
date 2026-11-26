#pragma once

#include "zugodb/common/constants.hpp"
#include "zugodb/storage/file_handle.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace zugodb {

// File format constants. The magic number is 'SLDB', from SlothDB, and is
// kept: its bytes on disk are "BDLS".
// Format 1001 is libzugodb's (upstream SlothDB writes 1): every integer in
// the file, header included, is little-endian, and Checkpoint::Save and
// Checkpoint::Load refuse to run on a big-endian host.
static constexpr uint32_t ZUGODB_MAGIC = 0x534C4442;
static constexpr uint32_t ZUGODB_VERSION = 1001;

// File header - stored at offset 0. Bytes from its end to data_offset
// (4096) are zero.
struct FileHeader {
    uint32_t magic = ZUGODB_MAGIC;
    uint32_t version = ZUGODB_VERSION;
    uint32_t block_size = BLOCK_SIZE;
    // CRC-32 of the whole file with these 4 bytes (offset 12) read as zero:
    // Checkpoint::ComputeChecksum.
    uint32_t checksum = 0;
    uint64_t catalog_offset = 0;    // Byte offset of catalog data.
    uint64_t catalog_size = 0;      // Size of catalog data in bytes.
    uint64_t total_blocks = 0;
    uint64_t data_offset = 0;       // Byte offset where row data starts.
};

static_assert(sizeof(FileHeader) <= 64, "FileHeader must fit in 64 bytes");
static_assert(sizeof(FileHeader) == 48, "format 1001's header is 48 bytes");
static_assert(offsetof(FileHeader, checksum) == 12, "the checksum sits at offset 12");
static constexpr idx_t FILE_CHECKSUM_OFFSET = 12;
// Where the tables start: catalog_offset and data_offset both hold it.
static constexpr idx_t FILE_DATA_OFFSET = 4096;

// Manages block-level I/O to a .zugodb file.
class BlockManager {
public:
    BlockManager() = default;

    // Create a new database file.
    void CreateFile(const std::string &path);

    // Open an existing database file. Refuses, in this order, a file
    // smaller than the header, a bad magic number, a version other than
    // ZUGODB_VERSION, and a size other than the header's catalog_offset
    // + catalog_size. The checksum is checked by Checkpoint::Load.
    void OpenFile(const std::string &path);

    // Write the file header.
    void WriteHeader(const FileHeader &header);

    // Read the file header.
    FileHeader ReadHeader();

    // Write raw data at an offset.
    void WriteData(const_data_ptr_t data, idx_t size, idx_t offset);

    // Read raw data at an offset.
    void ReadData(data_ptr_t data, idx_t size, idx_t offset);

    // Get file size.
    idx_t GetFileSize();

    // Flush.
    void Sync();

    bool IsOpen() const { return file_.IsOpen(); }

private:
    FileHandle file_;
    std::string path_;
};

} // namespace zugodb
