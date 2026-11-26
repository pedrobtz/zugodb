#pragma once

#include "zugodb/catalog/catalog.hpp"
#include "zugodb/main/provider_set.hpp"
#include "zugodb/transaction/transaction.hpp"
#include <atomic>
#include <memory>
#include <string>

namespace zugodb {

// Configuration for a zugodb database instance.
struct DatabaseConfig {
    std::string path;           // Empty = in-memory only.
    idx_t memory_limit = 0;     // 0 = unlimited.
    idx_t threads = 0;          // 0 = auto-detect.
    // zugodb: opened read-only. Connection::Query refuses any statement
    // that would change the catalog or its data, and the destructor does
    // not save the file.
    bool read_only = false;
};

// A zugodb database instance. Owns the catalog and storage.
class Database {
public:
    Database();
    explicit Database(const std::string &path);
    explicit Database(DatabaseConfig config);
    ~Database();

    Catalog &GetCatalog() { return *catalog_; }
    TransactionManager &GetTransactionManager() { return *txn_mgr_; }
    const DatabaseConfig &GetConfig() const { return config_; }

    // The host's function tables (providers.hpp; Stage 11.1, design L29).
    // Once per Database, before its first query: a second call, or one
    // after a query, throws InvalidInputException, as does a set that is
    // NULL or not of this engine's ABI. A service table that cannot be
    // used leaves its functions refused; GetProviderState says why.
    void SetProviders(const zugodb_providers *providers);
    ProviderState GetProviderState(ProviderService service) const {
        return providers_.State(service);
    }
    const ProviderSet &GetProviders() const { return providers_; }
    // Connection::Query calls this first; SetProviders is refused after.
    void MarkQueried() { queried_.store(true); }

private:
    DatabaseConfig config_;
    ProviderSet providers_;
    std::atomic<bool> providers_set_{false};
    std::atomic<bool> queried_{false};
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<TransactionManager> txn_mgr_;
};

} // namespace zugodb
