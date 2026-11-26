#include "zugodb/storage/block_manager.hpp"
#include "zugodb/common/exception.hpp"
#include <cstring>

namespace zugodb {

void BlockManager::CreateFile(const std::string &path) {
    path_ = path;
    file_ = FileHandle::Open(path, "w+b");

    // Write initial header.
    FileHeader header;
    header.data_offset = FILE_DATA_OFFSET; // Reserve 4KB for header area.
    WriteHeader(header);
}

void BlockManager::OpenFile(const std::string &path) {
    path_ = path;
    file_ = FileHandle::Open(path, "r+b");

    auto file_size = GetFileSize();
    if (file_size < sizeof(FileHeader)) {
        throw IOException(ErrorCode::CORRUPT_DATA,
                           "Invalid database file: " + std::to_string(file_size) +
                           " bytes, smaller than its " + std::to_string(sizeof(FileHeader)) +
                           "-byte header");
    }
    auto header = ReadHeader();
    if (header.magic != ZUGODB_MAGIC) {
        throw IOException(ErrorCode::CORRUPT_DATA,
                           "Invalid database file: bad magic number (expected \"BDLS\")");
    }
    if (header.version != ZUGODB_VERSION) {
        throw IOException(ErrorCode::CORRUPT_DATA,
                           "Unsupported database file format version " +
                           std::to_string(header.version) + "; this engine reads version " +
                           std::to_string(ZUGODB_VERSION) + " only");
    }
    // By subtraction, so a hostile header cannot overflow the sum.
    if (header.catalog_offset > file_size ||
        header.catalog_size != file_size - header.catalog_offset) {
        throw IOException(ErrorCode::CORRUPT_DATA,
                           "Damaged database file: it is " + std::to_string(file_size) +
                           " bytes, but its header records " +
                           std::to_string(header.catalog_size) + " bytes of data at offset " +
                           std::to_string(header.catalog_offset) + " (truncated or extended)");
    }
}

void BlockManager::WriteHeader(const FileHeader &header) {
    file_.WriteAt(reinterpret_cast<const_data_ptr_t>(&header), sizeof(FileHeader), 0);
    file_.Sync();
}

FileHeader BlockManager::ReadHeader() {
    FileHeader header;
    file_.ReadAt(reinterpret_cast<data_ptr_t>(&header), sizeof(FileHeader), 0);
    return header;
}

void BlockManager::WriteData(const_data_ptr_t data, idx_t size, idx_t offset) {
    file_.WriteAt(data, size, offset);
}

void BlockManager::ReadData(data_ptr_t data, idx_t size, idx_t offset) {
    file_.ReadAt(data, size, offset);
}

idx_t BlockManager::GetFileSize() {
    return file_.GetFileSize();
}

void BlockManager::Sync() {
    file_.Sync();
}

} // namespace zugodb
