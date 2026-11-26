#include "zugodb/storage/checkpoint.hpp"
#include "zugodb/common/string_util.hpp"
#include "zugodb/common/exception.hpp"
#include "zugodb/common/crc32.hpp"
#include "zugodb/common/types/value.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_set>
#include <vector>

namespace zugodb {

// ============================================================================
// Checksum and byte order
// ============================================================================

// Format 1001 is little-endian, and the helpers below copy integers in the
// host's order, so a big-endian host refuses rather than misreads.
static void RequireLittleEndianHost() {
    const uint16_t one = 1;
    uint8_t first;
    std::memcpy(&first, &one, 1);
    if (first != 1) {
        throw NotImplementedException(
            "Database files are little-endian; this big-endian host cannot read or write them");
    }
}

uint32_t Checkpoint::ComputeChecksum(const uint8_t *data, size_t size) {
    const size_t begin = static_cast<size_t>(FILE_CHECKSUM_OFFSET);
    const size_t end = begin + sizeof(uint32_t);
    const uint8_t zeros[sizeof(uint32_t)] = {0, 0, 0, 0};
    uint32_t crc = Crc32(data, std::min(size, begin));
    if (size > begin) crc = Crc32(zeros, std::min(size, end) - begin, crc);
    if (size > end) crc = Crc32(data + end, size - end, crc);
    return crc;
}

uint32_t Checkpoint::ChecksumFile(BlockManager &bm, idx_t size) {
    static constexpr idx_t PIECE = 1 << 20;
    std::vector<uint8_t> buf(static_cast<size_t>(std::min<idx_t>(size, PIECE)));
    uint32_t crc = 0;
    for (idx_t pos = 0; pos < size;) {
        auto n = std::min<idx_t>(PIECE, size - pos);
        bm.ReadData(buf.data(), n, pos);
        // The bytes of the stored checksum count as zero, wherever the
        // piece boundaries fall.
        for (idx_t i = FILE_CHECKSUM_OFFSET; i < FILE_CHECKSUM_OFFSET + sizeof(uint32_t); i++) {
            if (i >= pos && i < pos + n) buf[i - pos] = 0;
        }
        crc = Crc32(buf.data(), n, crc);
        pos += n;
    }
    return crc;
}

// ============================================================================
// Serialization helpers
// ============================================================================

void Checkpoint::WriteString(BlockManager &bm, idx_t &offset, const std::string &str) {
    WriteU32(bm, offset, static_cast<uint32_t>(str.size()));
    if (!str.empty()) {
        bm.WriteData(reinterpret_cast<const_data_ptr_t>(str.data()), str.size(), offset);
        offset += str.size();
    }
}

void Checkpoint::WriteU8(BlockManager &bm, idx_t &offset, uint8_t val) {
    bm.WriteData(reinterpret_cast<const_data_ptr_t>(&val), 1, offset);
    offset += 1;
}

void Checkpoint::WriteU32(BlockManager &bm, idx_t &offset, uint32_t val) {
    bm.WriteData(reinterpret_cast<const_data_ptr_t>(&val), 4, offset);
    offset += 4;
}

void Checkpoint::WriteU64(BlockManager &bm, idx_t &offset, uint64_t val) {
    bm.WriteData(reinterpret_cast<const_data_ptr_t>(&val), 8, offset);
    offset += 8;
}

void Checkpoint::WriteI32(BlockManager &bm, idx_t &offset, int32_t val) {
    bm.WriteData(reinterpret_cast<const_data_ptr_t>(&val), 4, offset);
    offset += 4;
}

void Checkpoint::WriteI64(BlockManager &bm, idx_t &offset, int64_t val) {
    bm.WriteData(reinterpret_cast<const_data_ptr_t>(&val), 8, offset);
    offset += 8;
}

void Checkpoint::WriteDouble(BlockManager &bm, idx_t &offset, double val) {
    bm.WriteData(reinterpret_cast<const_data_ptr_t>(&val), 8, offset);
    offset += 8;
}

// ============================================================================
// Value serialization
// ============================================================================

void Checkpoint::WriteValue(BlockManager &bm, idx_t &offset, const Value &val,
                             const LogicalType &type) {
    // Write null flag.
    WriteU8(bm, offset, val.IsNull() ? 1 : 0);
    if (val.IsNull()) return;

    switch (type.id()) {
    case LogicalTypeId::BOOLEAN:
        WriteU8(bm, offset, val.GetValue<bool>() ? 1 : 0);
        break;
    case LogicalTypeId::TINYINT:
        WriteU8(bm, offset, static_cast<uint8_t>(val.GetValue<int8_t>()));
        break;
    case LogicalTypeId::SMALLINT:
        WriteI32(bm, offset, val.GetValue<int16_t>());
        break;
    case LogicalTypeId::INTEGER:
    case LogicalTypeId::DATE:
        WriteI32(bm, offset, val.GetValue<int32_t>());
        break;
    case LogicalTypeId::BIGINT:
    case LogicalTypeId::TIMESTAMP:
    case LogicalTypeId::TIME:
        WriteI64(bm, offset, val.GetValue<int64_t>());
        break;
    case LogicalTypeId::FLOAT:
        WriteI32(bm, offset, 0); // placeholder
        {
            float f = val.GetValue<float>();
            int32_t bits;
            std::memcpy(&bits, &f, 4);
            // rewrite
            offset -= 4;
            bm.WriteData(reinterpret_cast<const_data_ptr_t>(&f), 4, offset);
            offset += 4;
        }
        break;
    case LogicalTypeId::DOUBLE:
        WriteDouble(bm, offset, val.GetValue<double>());
        break;
    case LogicalTypeId::VARCHAR:
    case LogicalTypeId::BLOB:
        WriteString(bm, offset, val.GetValue<std::string>());
        break;
    default:
        // Fallback: serialize as string.
        WriteString(bm, offset, val.ToString());
        break;
    }
}

// ============================================================================
// Save: serialize entire database
// ============================================================================

void Checkpoint::Save(Catalog &catalog, const std::string &path) {
    RequireLittleEndianHost();
    BlockManager bm;
    bm.CreateFile(path);

    // Start writing after the header area (4096 bytes reserved).
    idx_t offset = FILE_DATA_OFFSET;

    // Get all tables from the default schema.
    auto &schema = catalog.GetSchema();
    auto table_names = schema.GetTableNames();

    // Write number of tables.
    WriteU32(bm, offset, static_cast<uint32_t>(table_names.size()));

    for (auto &name : table_names) {
        auto *entry = catalog.GetTable(name);
        if (!entry) continue;

        // Write table name.
        WriteString(bm, offset, name);

        // Write column definitions.
        auto &cols = entry->GetColumns();
        WriteU32(bm, offset, static_cast<uint32_t>(cols.size()));
        for (auto &col : cols) {
            WriteString(bm, offset, col.name);
            WriteU8(bm, offset, static_cast<uint8_t>(col.type.id()));
        }

        // Write row data.
        auto &storage = entry->GetStorage();
        auto types = entry->GetTypes();
        WriteU64(bm, offset, storage.Count());

        auto state = storage.InitScan();
        DataChunk chunk;
        while (true) {
            chunk.Initialize(types);
            if (!storage.Scan(state, chunk)) break;
            for (idx_t row = 0; row < chunk.size(); row++) {
                for (idx_t col = 0; col < types.size(); col++) {
                    WriteValue(bm, offset, chunk.GetValue(col, row), types[col]);
                }
            }
        }
    }

    // Update header with catalog info.
    FileHeader header;
    header.catalog_offset = FILE_DATA_OFFSET;
    header.catalog_size = offset - FILE_DATA_OFFSET;
    header.data_offset = FILE_DATA_OFFSET;
    header.checksum = 0;
    bm.WriteHeader(header);
    bm.Sync();

    // The checksum is computed over the bytes read back from the file, so
    // it covers what was written, and stored last.
    header.checksum = ChecksumFile(bm, bm.GetFileSize());
    bm.WriteHeader(header);
    bm.Sync();
}


// ============================================================================
// Load: deserialize entire database
// ============================================================================

namespace {

[[noreturn]] void Damaged(const std::string &what) {
    throw IOException(ErrorCode::CORRUPT_DATA, "Damaged database file: " + what);
}

std::string At(idx_t offset) {
    return " at offset " + std::to_string(offset);
}

// The smallest a table can be: its name's length, its column count, one
// column (its name's length and its type) and its row count.
constexpr idx_t MIN_TABLE_BYTES = 4 + 4 + (4 + 1) + 8;
// The smallest a column definition can be: its name's length and its type.
constexpr idx_t MIN_COLUMN_BYTES = 4 + 1;
// Every value, NULL or not, starts with its 1-byte null flag.
constexpr idx_t MIN_VALUE_BYTES = 1;

// Reads the bytes [begin, end) of an open database file in pieces, and
// refuses any read that would pass `end`: every length, count and offset
// the loader takes from the file goes through it.
class BoundedReader {
public:
    BoundedReader(BlockManager &bm, idx_t begin, idx_t end) : bm_(bm), pos_(begin), end_(end) {}

    idx_t Offset() const { return pos_; }
    idx_t Left() const { return end_ - pos_; }

    void Read(void *dst, idx_t n, const char *what) {
        if (n > Left()) {
            Damaged(std::string(what) + At(pos_) + " takes " + std::to_string(n) +
                    " bytes, but only " + std::to_string(Left()) + " are left");
        }
        auto *out = static_cast<uint8_t *>(dst);
        while (n > 0) {
            if (buf_pos_ == buf_.size()) Fill();
            auto k = std::min<idx_t>(n, buf_.size() - buf_pos_);
            std::memcpy(out, buf_.data() + buf_pos_, static_cast<size_t>(k));
            out += k;
            n -= k;
            buf_pos_ += k;
            pos_ += k;
        }
    }

    template <class T>
    T Get(const char *what) {
        T val;
        Read(&val, sizeof(T), what);
        return val;
    }

    // A 4-byte length, then that many bytes; the length is checked against
    // the bytes left before anything is allocated.
    std::string String(const char *what) {
        auto at = pos_;
        auto len = Get<uint32_t>(what);
        if (len > Left()) {
            Damaged(std::string(what) + At(at) + " is " + std::to_string(len) +
                    " bytes long, but only " + std::to_string(Left()) + " are left");
        }
        std::string str(len, '\0');
        if (len > 0) Read(str.data(), len, what);
        return str;
    }

private:
    static constexpr idx_t PIECE = 1 << 16;

    void Fill() {
        auto n = std::min<idx_t>(PIECE, end_ - pos_);
        buf_.resize(static_cast<size_t>(n));
        bm_.ReadData(buf_.data(), n, pos_);
        buf_pos_ = 0;
    }

    BlockManager &bm_;
    idx_t pos_;
    idx_t end_;
    std::vector<uint8_t> buf_;
    size_t buf_pos_ = 0;
};

// The column types a file may hold: the ones a table made by SQL can have,
// which Save writes. Any other tag is refused.
bool KnownColumnType(uint8_t tag) {
    switch (static_cast<LogicalTypeId>(tag)) {
    case LogicalTypeId::SQLNULL:
    case LogicalTypeId::BOOLEAN:
    case LogicalTypeId::TINYINT:
    case LogicalTypeId::SMALLINT:
    case LogicalTypeId::INTEGER:
    case LogicalTypeId::BIGINT:
    case LogicalTypeId::HUGEINT:
    case LogicalTypeId::FLOAT:
    case LogicalTypeId::DOUBLE:
    case LogicalTypeId::VARCHAR:
    case LogicalTypeId::BLOB:
    case LogicalTypeId::DATE:
    case LogicalTypeId::TIME:
    case LogicalTypeId::TIMESTAMP:
    case LogicalTypeId::INTERVAL:
    case LogicalTypeId::UUID:
        return true;
    default:
        return false;
    }
}

// One value as Save writes it: a null flag (0 or 1), then, unless NULL,
// the value in the column type's encoding.
Value ReadValue(BoundedReader &in, const LogicalType &type) {
    auto at = in.Offset();
    auto flag = in.Get<uint8_t>("a null flag");
    if (flag == 1) return Value();
    if (flag != 0) {
        Damaged("null flag " + std::to_string(flag) + At(at) + " is neither 0 nor 1");
    }
    at = in.Offset();
    switch (type.id()) {
    case LogicalTypeId::BOOLEAN: {
        auto b = in.Get<uint8_t>("a BOOLEAN value");
        if (b > 1) {
            Damaged("BOOLEAN value " + std::to_string(b) + At(at) + " is neither 0 nor 1");
        }
        return Value::BOOLEAN(b != 0);
    }
    case LogicalTypeId::TINYINT:
        return Value::TINYINT(in.Get<int8_t>("a TINYINT value"));
    case LogicalTypeId::SMALLINT: {
        // Save writes a SMALLINT in 4 bytes.
        auto v = in.Get<int32_t>("a SMALLINT value");
        if (v < INT16_MIN || v > INT16_MAX) {
            Damaged("SMALLINT value " + std::to_string(v) + At(at) + " is out of range");
        }
        return Value::SMALLINT(static_cast<int16_t>(v));
    }
    case LogicalTypeId::INTEGER:
    case LogicalTypeId::DATE:
        return Value::INTEGER(in.Get<int32_t>("an INTEGER or DATE value"));
    case LogicalTypeId::BIGINT:
    case LogicalTypeId::TIMESTAMP:
    case LogicalTypeId::TIME:
        return Value::BIGINT(in.Get<int64_t>("a BIGINT, TIMESTAMP or TIME value"));
    // Any bits are a value: a NaN, of either sign and any payload, is
    // held as DuckDB holds it (#42; 0.3.0 refused it, design L23).
    case LogicalTypeId::FLOAT:
        return Value::FLOAT(in.Get<float>("a FLOAT value"));
    case LogicalTypeId::DOUBLE:
        return Value::DOUBLE(in.Get<double>("a DOUBLE value"));
    case LogicalTypeId::VARCHAR:
    case LogicalTypeId::BLOB:
        return Value::VARCHAR(in.String("a VARCHAR or BLOB value"));
    default:
        // SQLNULL holds only NULL; Save writes HUGEINT, INTERVAL and UUID
        // values as text that the engine cannot read back into the column,
        // so only their NULLs load.
        Damaged("a non-NULL " + type.ToString() + " value" + At(at) +
                ", which this engine cannot read back");
    }
}

struct LoadedTable {
    std::string name;
    std::vector<ColumnDefinition> columns;
    std::shared_ptr<DataTable> storage;
};

// One table: its name, its columns, its row count and its rows.
LoadedTable ReadTable(BoundedReader &in) {
    LoadedTable table;
    auto name_at = in.Offset();
    table.name = in.String("a table name");
    // SQL makes no empty name: "" is DuckDB's error (Stage 9, design L26).
    if (table.name.empty()) Damaged("a table with no name" + At(name_at));

    auto at = in.Offset();
    auto num_cols = in.Get<uint32_t>("a column count");
    if (num_cols == 0) Damaged("a table with no columns" + At(at));
    if (num_cols > in.Left() / MIN_COLUMN_BYTES) {
        Damaged("column count " + std::to_string(num_cols) + At(at) + " is more than the " +
                std::to_string(in.Left()) + " bytes left can hold");
    }
    std::vector<LogicalType> types;
    table.columns.reserve(num_cols);
    types.reserve(num_cols);
    for (uint32_t c = 0; c < num_cols; c++) {
        auto col_at = in.Offset();
        auto col_name = in.String("a column name");
        if (col_name.empty()) Damaged("a column with no name" + At(col_at));
        at = in.Offset();
        auto tag = in.Get<uint8_t>("a column type");
        if (!KnownColumnType(tag)) {
            Damaged("unknown column type " + std::to_string(tag) + At(at));
        }
        LogicalType type(static_cast<LogicalTypeId>(tag));
        // A column name the table already has, ignoring ASCII case, is
        // one SQL cannot make (design L26); the catalog would rename it.
        for (auto &prev : table.columns) {
            if (IdentifierEquals(prev.name, col_name)) {
                Damaged("a second column of the same name" + At(at));
            }
        }
        table.columns.emplace_back(col_name, type);
        types.push_back(type);
    }

    at = in.Offset();
    auto row_count = in.Get<uint64_t>("a row count");
    if (row_count > in.Left() / (MIN_VALUE_BYTES * num_cols)) {
        Damaged("row count " + std::to_string(row_count) + At(at) + " is more than the " +
                std::to_string(in.Left()) + " bytes left can hold");
    }

    // Each row group, and each chunk, is sized to the rows the file holds,
    // so what is allocated follows the file's size.
    table.storage = std::make_shared<DataTable>(types);
    std::vector<std::unique_ptr<RowGroup>> groups;
    for (idx_t done = 0; done < row_count;) {
        auto group_rows = std::min<idx_t>(ROW_GROUP_SIZE, row_count - done);
        auto group = std::make_unique<RowGroup>(types, 0, group_rows);
        for (idx_t in_group = 0; in_group < group_rows;) {
            auto batch = std::min<idx_t>(VECTOR_SIZE, group_rows - in_group);
            DataChunk chunk;
            chunk.Initialize(types, batch);
            for (idx_t row = 0; row < batch; row++) {
                for (idx_t col = 0; col < types.size(); col++) {
                    chunk.SetValue(col, row, ReadValue(in, types[col]));
                }
            }
            group->Append(chunk, 0);
            in_group += batch;
        }
        groups.push_back(std::move(group));
        done += group_rows;
    }
    table.storage->AppendRowGroups(std::move(groups));
    return table;
}

} // namespace

void Checkpoint::Load(Catalog &catalog, const std::string &path) {
    RequireLittleEndianHost();
    BlockManager bm;
    bm.OpenFile(path);

    auto header = bm.ReadHeader();
    auto file_size = bm.GetFileSize();

    auto computed = ChecksumFile(bm, file_size);
    if (computed != header.checksum) {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "Damaged database file: checksum 0x%08X stored, 0x%08X computed "
                      "(format version %u)",
                      static_cast<unsigned>(header.checksum), static_cast<unsigned>(computed),
                      static_cast<unsigned>(header.version));
        throw IOException(ErrorCode::CORRUPT_DATA, buf);
    }

    // The header fields Save writes as constants. OpenFile has checked that
    // catalog_offset + catalog_size is the file's size.
    if (header.block_size != BLOCK_SIZE) {
        Damaged("the header's block size is " + std::to_string(header.block_size) + ", not " +
                std::to_string(BLOCK_SIZE));
    }
    if (header.total_blocks != 0) {
        Damaged("the header's block count is " + std::to_string(header.total_blocks) +
                ", not 0");
    }
    if (header.catalog_offset != FILE_DATA_OFFSET || header.data_offset != FILE_DATA_OFFSET) {
        Damaged("the header records the tables at offsets " +
                std::to_string(header.catalog_offset) + " and " +
                std::to_string(header.data_offset) + ", not " +
                std::to_string(FILE_DATA_OFFSET));
    }
    {
        BoundedReader gap(bm, sizeof(FileHeader), FILE_DATA_OFFSET);
        while (gap.Left() > 0) {
            auto at = gap.Offset();
            if (gap.Get<uint8_t>("the header area") != 0) {
                Damaged("byte" + At(at) + ", between the header and the tables, is not zero");
            }
        }
    }

    BoundedReader in(bm, FILE_DATA_OFFSET, file_size);
    auto at = in.Offset();
    auto num_tables = in.Get<uint32_t>("the table count");
    if (num_tables > in.Left() / MIN_TABLE_BYTES) {
        Damaged("table count " + std::to_string(num_tables) + At(at) + " is more than the " +
                std::to_string(in.Left()) + " bytes left can hold");
    }

    // Every table is read before any reaches the catalog, so a refused file
    // leaves the catalog as it was.
    std::vector<LoadedTable> tables;
    tables.reserve(num_tables);
    // Names compare as identifiers do, ignoring ASCII case (design L26):
    // SQL cannot make two tables `t` and `T`.
    std::unordered_set<std::string, IdentifierHash, IdentifierEqual> names;
    for (uint32_t t = 0; t < num_tables; t++) {
        at = in.Offset();
        tables.push_back(ReadTable(in));
        auto &name = tables.back().name;
        if (!names.insert(name).second || catalog.GetTable(name)) {
            Damaged("a second table of the same name" + At(at));
        }
    }
    if (in.Left() != 0) {
        Damaged(std::to_string(in.Left()) + " bytes after the last table" + At(in.Offset()));
    }

    for (auto &table : tables) {
        auto &entry = catalog.CreateTable(table.name, std::move(table.columns));
        entry.SetStorage(std::move(table.storage));
    }
}

} // namespace zugodb
