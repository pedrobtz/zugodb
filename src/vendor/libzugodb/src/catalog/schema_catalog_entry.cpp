#include "zugodb/catalog/schema_catalog_entry.hpp"
#include "zugodb/common/exception.hpp"

namespace zugodb {

SchemaCatalogEntry::SchemaCatalogEntry(const std::string &name)
    : CatalogEntry(CatalogEntryType::SCHEMA, name) {}

TableCatalogEntry &SchemaCatalogEntry::CreateTable(const std::string &name,
                                                    std::vector<ColumnDefinition> columns) {
    if (tables_.count(name)) {
        // A name that differs only in case is the same name (design L26).
        throw CatalogException("Table with name \"" + name + "\" already exists");
    }
    // Column names are unique ignoring case, as DuckDB makes those of a
    // subquery, a view or CREATE TABLE AS: a later duplicate becomes
    // name_1, name_2, ... (SELECT 1 AS a, 2 AS A names them a and A_1).
    // CREATE TABLE and ALTER TABLE refuse a duplicate before this.
    for (size_t i = 0; i < columns.size(); i++) {
        auto taken = [&](const std::string &n, size_t upto) {
            for (size_t j = 0; j < upto; j++) {
                if (IdentifierEquals(columns[j].name, n)) return true;
            }
            return false;
        };
        if (!taken(columns[i].name, i)) continue;
        for (size_t k = 1;; k++) {
            std::string candidate = columns[i].name + "_" + std::to_string(k);
            if (!taken(candidate, columns.size())) {
                columns[i].name = candidate;
                break;
            }
        }
    }
    auto entry = std::make_unique<TableCatalogEntry>(GetName(), name, std::move(columns));
    auto *ptr = entry.get();
    tables_[name] = std::move(entry);
    return *ptr;
}

TableCatalogEntry *SchemaCatalogEntry::GetTable(const std::string &name) {
    auto it = tables_.find(name);
    if (it == tables_.end()) return nullptr;
    return it->second.get();
}

bool SchemaCatalogEntry::TableExists(const std::string &name) const {
    return tables_.count(name) > 0;
}

bool SchemaCatalogEntry::DropTable(const std::string &name) {
    return tables_.erase(name) > 0;
}

std::vector<std::string> SchemaCatalogEntry::GetTableNames() const {
    std::vector<std::string> names;
    names.reserve(tables_.size());
    for (auto &pair : tables_) {
        names.push_back(pair.second->GetName());
    }
    return names;
}

} // namespace zugodb
