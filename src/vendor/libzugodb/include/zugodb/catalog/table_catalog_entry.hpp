#pragma once

#include "zugodb/catalog/catalog_entry.hpp"
#include "zugodb/common/types/logical_type.hpp"
#include <memory>
#include <string>
#include <vector>

namespace zugodb {

class DataTable;

// Describes a column in a table.
struct ColumnDefinition {
    std::string name;
    LogicalType type;
    bool not_null = false;
    bool is_primary_key = false;

    ColumnDefinition(const std::string &name, const LogicalType &type)
        : name(name), type(type) {}
    ColumnDefinition(const std::string &name, const LogicalType &type, bool not_null_)
        : name(name), type(type), not_null(not_null_) {}
    ColumnDefinition(const std::string &name, const LogicalType &type,
                     bool not_null_, bool is_pk_)
        : name(name), type(type), not_null(not_null_), is_primary_key(is_pk_) {}
};

// Catalog entry for a table. Owns the column definitions and
// a reference to the physical DataTable.
class TableCatalogEntry : public CatalogEntry {
public:
    TableCatalogEntry(const std::string &schema, const std::string &name,
                      std::vector<ColumnDefinition> columns);

    const std::string &GetSchema() const { return schema_; }
    const std::vector<ColumnDefinition> &GetColumns() const { return columns_; }
    idx_t ColumnCount() const { return static_cast<idx_t>(columns_.size()); }

    // Get column index by name. Returns INVALID_INDEX if not found.
    idx_t GetColumnIndex(const std::string &col_name) const;

    // Indices of PRIMARY KEY columns (for uniqueness enforcement).
    std::vector<idx_t> GetPrimaryKeyColumns() const {
        std::vector<idx_t> pk;
        for (idx_t i = 0; i < columns_.size(); i++) {
            if (columns_[i].is_primary_key) pk.push_back(i);
        }
        return pk;
    }

    // Throw ConstraintException if appending `chunk` would create a
    // duplicate PRIMARY KEY value — against existing storage rows or
    // among rows within `chunk` itself. No-op when the table has no
    // primary key or no storage. Only called from INSERT paths.
    void CheckPrimaryKeyForChunk(class DataChunk &chunk) const;

    // Get the column types as a vector.
    std::vector<LogicalType> GetTypes() const;

    // The physical storage for this table.
    DataTable &GetStorage() { return *storage_; }
    const DataTable &GetStorage() const { return *storage_; }
    void SetStorage(std::shared_ptr<DataTable> storage) { storage_ = std::move(storage); }

    // Virtual view support: stores the original SQL query for re-execution.
    bool IsView() const { return is_view_; }
    void SetViewQuery(const std::string &sql) { view_query_ = sql; is_view_ = true; }
    const std::string &GetViewQuery() const { return view_query_; }

private:
    std::string schema_;
    std::vector<ColumnDefinition> columns_;
    std::shared_ptr<DataTable> storage_;
    bool is_view_ = false;
    std::string view_query_;
};

} // namespace zugodb
