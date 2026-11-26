#pragma once

#include "zugodb/storage/column_data.hpp"
#include "zugodb/common/types/data_chunk.hpp"
#include <vector>
#include <memory>

namespace zugodb {

// A RowGroup holds up to `capacity` rows of columnar data, ROW_GROUP_SIZE
// unless the caller knows it needs fewer (Checkpoint::Load, which sizes
// each group to the rows the file holds, so a file cannot make it allocate
// more than its own size accounts for).
class RowGroup {
public:
    RowGroup(const std::vector<LogicalType> &types, idx_t start_row,
             idx_t capacity = ROW_GROUP_SIZE);

    idx_t Count() const { return count_; }
    idx_t StartRow() const { return start_row_; }
    // Reset start_row_ - used when splicing row groups built by parallel
    // workers into a DataTable; the worker builds with start_row_=0 and the
    // splice step rewrites to the correct global row offset.
    void SetStartRow(idx_t start) { start_row_ = start; }
    bool IsFull() const { return count_ >= capacity_; }
    idx_t Remaining() const { return capacity_ - count_; }
    idx_t Capacity() const { return capacity_; }

    // Append a DataChunk into this row group. Returns number of rows actually appended.
    idx_t Append(DataChunk &chunk, idx_t offset);

    // Scan rows [start, start + count) into a DataChunk.
    void Scan(DataChunk &result, idx_t start, idx_t scan_count) const;

    // Number of columns.
    idx_t ColumnCount() const { return static_cast<idx_t>(columns_.size()); }

private:
    idx_t start_row_;     // Global row offset of this row group.
    idx_t count_;         // Current number of rows.
    idx_t capacity_;      // Rows its columns are allocated for.
    std::vector<std::unique_ptr<ColumnData>> columns_;
};

} // namespace zugodb
