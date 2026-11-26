#include "zugodb/main/connection.hpp"
#include "zugodb/main/query_materialize.hpp"
#include "zugodb/common/exception.hpp"
#include "zugodb/common/types/blob.hpp"
#include "zugodb/common/types/float_order.hpp"
#include "zugodb/execution/group_key.hpp"
#include "zugodb/parser/parser.hpp"
#include "zugodb/binder/binder.hpp"
#include "zugodb/planner/planner.hpp"
#include "zugodb/execution/physical_planner.hpp"
#include "zugodb/execution/expression_executor.hpp"
#include "zugodb/optimizer/optimizer.hpp"
#include "zugodb/common/string_util.hpp"
#include <cmath>
#include <sstream>
#include <unordered_set>
#include <chrono>
#include <cstdio>
#include <filesystem>

namespace zugodb {

// Bulk-load rows into a DataTable using chunked appends (VECTOR_SIZE rows per chunk).
// This avoids creating one DataChunk per row, which is the primary bottleneck.
static void BulkLoadRows(DataTable &storage, const std::vector<LogicalType> &types,
                          const std::vector<std::vector<Value>> &rows) {
    const idx_t chunk_size = VECTOR_SIZE; // 2048 rows per chunk
    idx_t total = static_cast<idx_t>(rows.size());
    idx_t num_cols = static_cast<idx_t>(types.size());

    for (idx_t offset = 0; offset < total; offset += chunk_size) {
        idx_t count = std::min(chunk_size, total - offset);
        DataChunk chunk;
        chunk.Initialize(types);
        for (idx_t r = 0; r < count; r++) {
            auto &row = rows[offset + r];
            for (idx_t c = 0; c < num_cols && c < static_cast<idx_t>(row.size()); c++) {
                chunk.SetValue(c, r, row[c]);
            }
        }
        chunk.SetCardinality(count);
        storage.Append(chunk);
    }
}

// SQL-standard wider common type for two branches of a UNION / INTERSECT /
// EXCEPT chain (per-column). The pairwise reduction over a chain produces
// the type each branch's values must coerce to before set-op dedup or
// storage. Falls back to VARCHAR (universal lossless string) on any
// unknown combination; the caller is responsible for coercing values.
static LogicalType ComputeSetOpCommonType(const LogicalType &a, const LogicalType &b) {
    if (a.id() == b.id()) return a;
    if (a.id() == LogicalTypeId::INVALID || a.id() == LogicalTypeId::SQLNULL) return b;
    if (b.id() == LogicalTypeId::INVALID || b.id() == LogicalTypeId::SQLNULL) return a;

    // VARCHAR mixed with any other type unifies to VARCHAR — the only
    // universal target the engine carries. Must come BEFORE the float
    // branch: previously VARCHAR + DOUBLE/FLOAT returned a float type,
    // and CoerceValueForSetOp then ran the string through a numeric
    // coercion that returned 0.0 for non-numeric text — silently
    // corrupting '1' -> 0 and 'x' -> 0 across UNION and VALUES.
    if (a.id() == LogicalTypeId::VARCHAR || b.id() == LogicalTypeId::VARCHAR) {
        return LogicalType(LogicalTypeId::VARCHAR);
    }

    auto is_signed_int = [](LogicalTypeId id) {
        return id == LogicalTypeId::TINYINT || id == LogicalTypeId::SMALLINT ||
               id == LogicalTypeId::INTEGER || id == LogicalTypeId::BIGINT;
    };
    auto is_unsigned_int = [](LogicalTypeId id) {
        return id == LogicalTypeId::UTINYINT || id == LogicalTypeId::USMALLINT ||
               id == LogicalTypeId::UINTEGER || id == LogicalTypeId::UBIGINT;
    };
    auto is_float = [](LogicalTypeId id) {
        return id == LogicalTypeId::FLOAT || id == LogicalTypeId::DOUBLE;
    };
    auto width = [&](LogicalTypeId id) -> int {
        switch (id) {
            case LogicalTypeId::BOOLEAN:   return 1;
            case LogicalTypeId::TINYINT:   case LogicalTypeId::UTINYINT:   return 8;
            case LogicalTypeId::SMALLINT:  case LogicalTypeId::USMALLINT:  return 16;
            case LogicalTypeId::INTEGER:   case LogicalTypeId::UINTEGER:   return 32;
            case LogicalTypeId::BIGINT:    case LogicalTypeId::UBIGINT:    return 64;
            case LogicalTypeId::FLOAT:     return 32;
            case LogicalTypeId::DOUBLE:    return 64;
            case LogicalTypeId::HUGEINT:   return 128;
            default: return 0;
        }
    };

    // Mixed float + int → DOUBLE if either is DOUBLE or width≥32 int, else FLOAT.
    if (is_float(a.id()) || is_float(b.id())) {
        if (a.id() == LogicalTypeId::DOUBLE || b.id() == LogicalTypeId::DOUBLE ||
            width(a.id()) >= 32 || width(b.id()) >= 32) {
            return LogicalType::DOUBLE();
        }
        return LogicalType(LogicalTypeId::FLOAT);
    }

    // Mixed signed + unsigned int → next-up signed (UBIGINT vs INTEGER stays
    // at BIGINT — a true UBIGINT span needs HUGEINT but those queries are
    // rare and the current codebase doesn't carry HUGEINT well; clamp at
    // BIGINT and accept the rare overflow rather than escalate to a type
    // the rest of the engine can't handle).
    if ((is_signed_int(a.id()) && is_unsigned_int(b.id())) ||
        (is_unsigned_int(a.id()) && is_signed_int(b.id()))) {
        int w = std::max(width(a.id()), width(b.id()));
        if (w <= 8)  return LogicalType(LogicalTypeId::SMALLINT);
        if (w <= 16) return LogicalType(LogicalTypeId::INTEGER);
        return LogicalType::BIGINT();
    }

    // Same family, different width.
    if (is_signed_int(a.id()) && is_signed_int(b.id())) {
        return (width(a.id()) >= width(b.id())) ? a : b;
    }
    if (is_unsigned_int(a.id()) && is_unsigned_int(b.id())) {
        return (width(a.id()) >= width(b.id())) ? a : b;
    }

    // Anything else (DATE/TIMESTAMP/BLOB/LIST mixed with non-matching) →
    // VARCHAR is the only universal target the engine can carry.
    return LogicalType(LogicalTypeId::VARCHAR);
}

// Coerce a single Value to the target type. NULLs are passed through with
// the target type tag. Falls back to VARCHAR via ToString() on any path
// not explicitly handled — same fallback policy as ExecuteCast.
static Value CoerceValueForSetOp(const Value &v, const LogicalType &target) {
    if (v.IsNull()) return Value();
    if (v.type().id() == target.id()) return v;

    // Read the source as int64 / double / string in a type-dispatched way.
    auto src_id = v.type().id();
    auto src_as_int64 = [&]() -> int64_t {
        switch (src_id) {
            case LogicalTypeId::BOOLEAN:  return v.GetValue<bool>() ? 1 : 0;
            case LogicalTypeId::TINYINT:  return v.GetValue<int8_t>();
            case LogicalTypeId::SMALLINT: return v.GetValue<int16_t>();
            case LogicalTypeId::INTEGER:  return v.GetValue<int32_t>();
            case LogicalTypeId::BIGINT:   return v.GetValue<int64_t>();
            case LogicalTypeId::UTINYINT: return v.GetValue<uint8_t>();
            case LogicalTypeId::USMALLINT:return v.GetValue<uint16_t>();
            case LogicalTypeId::UINTEGER: return v.GetValue<uint32_t>();
            case LogicalTypeId::UBIGINT:  return static_cast<int64_t>(v.GetValue<uint64_t>());
            case LogicalTypeId::FLOAT:
            case LogicalTypeId::DOUBLE: {
                // A NaN or a value past BIGINT is no integer: casting it
                // was undefined (#42).
                double d = src_id == LogicalTypeId::FLOAT ? v.GetValue<float>() : v.GetValue<double>();
                if (IsNaN(d) || !(d >= -9223372036854775808.0 && d < 9223372036854775808.0)) {
                    throw ConversionException("Value " + v.ToString() +
                                              " is out of range for the destination type");
                }
                return static_cast<int64_t>(d);
            }
            default: return 0;
        }
    };
    auto src_as_double = [&]() -> double {
        switch (src_id) {
            case LogicalTypeId::BOOLEAN:  return v.GetValue<bool>() ? 1.0 : 0.0;
            case LogicalTypeId::TINYINT:  return v.GetValue<int8_t>();
            case LogicalTypeId::SMALLINT: return v.GetValue<int16_t>();
            case LogicalTypeId::INTEGER:  return v.GetValue<int32_t>();
            case LogicalTypeId::BIGINT:   return static_cast<double>(v.GetValue<int64_t>());
            case LogicalTypeId::UTINYINT: return v.GetValue<uint8_t>();
            case LogicalTypeId::USMALLINT:return v.GetValue<uint16_t>();
            case LogicalTypeId::UINTEGER: return v.GetValue<uint32_t>();
            case LogicalTypeId::UBIGINT:  return static_cast<double>(v.GetValue<uint64_t>());
            case LogicalTypeId::FLOAT:    return v.GetValue<float>();
            case LogicalTypeId::DOUBLE:   return v.GetValue<double>();
            default: return 0.0;
        }
    };

    switch (target.id()) {
        case LogicalTypeId::TINYINT:  return Value::TINYINT(static_cast<int8_t>(src_as_int64()));
        case LogicalTypeId::SMALLINT: return Value::SMALLINT(static_cast<int16_t>(src_as_int64()));
        case LogicalTypeId::INTEGER:  return Value::INTEGER(static_cast<int32_t>(src_as_int64()));
        case LogicalTypeId::BIGINT:   return Value::BIGINT(src_as_int64());
        case LogicalTypeId::UTINYINT: return Value::UTINYINT(static_cast<uint8_t>(src_as_int64()));
        case LogicalTypeId::USMALLINT:return Value::USMALLINT(static_cast<uint16_t>(src_as_int64()));
        case LogicalTypeId::UINTEGER: return Value::UINTEGER(static_cast<uint32_t>(src_as_int64()));
        case LogicalTypeId::UBIGINT:  return Value::UBIGINT(static_cast<uint64_t>(src_as_int64()));
        case LogicalTypeId::FLOAT:    return Value::FLOAT(static_cast<float>(src_as_double()));
        case LogicalTypeId::DOUBLE:   return Value::DOUBLE(src_as_double());
        case LogicalTypeId::VARCHAR:  return Value::VARCHAR(v.ToString());
        default: return v;  // unknown target — pass through
    }
}

// Hash + equality on a row of Values, used for UNION / INTERSECT / EXCEPT
// dedup. NULL == NULL per set-op semantics (Value::operator== already
// handles this correctly). Type-aware: INT 1 and VARCHAR "1" don't
// collide, unlike the prior string-concatenation key.
struct SetOpRowHash {
    size_t operator()(const std::vector<Value> &row) const noexcept {
        size_t h = row.size();
        for (auto &v : row) {
            size_t vh;
            if (v.IsNull()) {
                vh = 0xDEAD9E37;
            } else {
                // Equal values hash alike: -0.0 and 0.0 (#38), and every
                // NaN, whose text is "nan" or "-nan" (#42).
                std::string k;
                auto id = v.type().id();
                if (id == LogicalTypeId::DOUBLE || id == LogicalTypeId::FLOAT) AppendKeyValue(k, v);
                else k = v.ToString();
                vh = std::hash<std::string>{}(k);
                vh ^= static_cast<size_t>(id) * 0x9e3779b9u;
            }
            h ^= vh + 0x9e3779b9 + (h << 6) + (h >> 2);
        }
        return h;
    }
};
struct SetOpRowEqual {
    bool operator()(const std::vector<Value> &a, const std::vector<Value> &b) const noexcept {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++) {
            if (!(a[i] == b[i])) return false;
        }
        return true;
    }
};
using SetOpRowSet = std::unordered_set<std::vector<Value>, SetOpRowHash, SetOpRowEqual>;

// `rows` OP `right`, for UNION, INTERSECT and EXCEPT: each a set, so a row
// is kept once. Rows repeated within the left side were all kept (Stage
// 10): `SELECT 1 FROM (VALUES (1), (1)) UNION SELECT 2` gave three rows.
static void ApplyDistinctSetOp(const std::string &op, std::vector<std::vector<Value>> &rows,
                               std::vector<std::vector<Value>> &right) {
    SetOpRowSet seen;
    std::vector<std::vector<Value>> kept;
    if (op == "UNION") {
        for (auto *side : {&rows, &right}) {
            for (auto &row : *side) {
                if (seen.insert(row).second) kept.push_back(std::move(row));
            }
        }
    } else {
        SetOpRowSet rk(right.begin(), right.end());
        const bool want = op == "INTERSECT";
        for (auto &row : rows) {
            if ((rk.count(row) > 0) == want && seen.insert(row).second) kept.push_back(std::move(row));
        }
    }
    rows = std::move(kept);
}

// Apply union-level ORDER BY / LIMIT / OFFSET to a row vector.
//
// The parser attaches ORDER BY / LIMIT / OFFSET that appear after a UNION /
// INTERSECT / EXCEPT to the rightmost (leaf) SelectStatement of the chain,
// because it walks left-to-right and consumes those clauses as part of the
// final ParseSelectStatement call. Per SQL standard those clauses apply to
// the entire set-op result, not to the leaf branch alone. The connection
// layer stashes them off the leaf, executes the chain unsorted, then calls
// this to sort + trim the combined rows.
//
// Sort key resolution is limited to positional integer literals
// (`ORDER BY 1 DESC`) and bare column-name refs (`ORDER BY a`) since the
// chain-combined row vector isn't a fully-bound DataChunk and a generic
// expression evaluator would need to be re-bound against the result-set
// schema. Items that don't resolve are skipped — the rows stay in
// chain-walk order for that key, which matches what SlothDB did before
// this fix.
//
// LIMIT / OFFSET must be integer literals. Non-literal expressions are
// silently ignored. This is the same restriction sqllogictest's corpus
// uses everywhere, and matches the parser's expectations.
static void ApplyUnionOrderLimit(
        std::vector<std::vector<Value>> &rows,
        const std::vector<OrderByItem> &order_by,
        const ParsedExprPtr &limit_expr,
        const ParsedExprPtr &offset_expr,
        const std::vector<std::string> &col_names) {
    if (!order_by.empty()) {
        struct Key { int col_idx; bool ascending; bool nulls_first; };
        std::vector<Key> keys;
        keys.reserve(order_by.size());
        for (auto &item : order_by) {
            Key k{ -1, item.ascending, item.nulls_first };
            auto &e = *item.expression;
            // A key that is not an output position or name was skipped,
            // leaving the rows in the chain's order (#35): refused.
            if (e.GetExpressionType() == ExpressionType::CONSTANT) {
                auto &c = static_cast<const ConstantExpression &>(e);
                if (c.literal_type != TokenType::INTEGER_LITERAL) {
                    throw BinderException(ErrorCode::TYPE_MISMATCH,
                        "ORDER BY non-integer literal has no effect");
                }
                long long v = 0;
                try { v = std::stoll(c.value); } catch (...) { v = 0; }
                if (v < 1 || v > static_cast<long long>(col_names.size())) {
                    throw BinderException(ErrorCode::OUT_OF_RANGE,
                        "ORDER term out of range - should be between 1 and " +
                        std::to_string(col_names.size()));
                }
                k.col_idx = static_cast<int>(v - 1);
            } else if (e.GetExpressionType() == ExpressionType::COLUMN_REF &&
                       static_cast<const ColumnRefExpression &>(e).table_name.empty()) {
                auto &cr = static_cast<const ColumnRefExpression &>(e);
                for (size_t i = 0; i < col_names.size(); i++) {
                    if (StringUtil::Upper(col_names[i]) == StringUtil::Upper(cr.column_name)) {
                        k.col_idx = static_cast<int>(i);
                        break;
                    }
                }
            }
            if (k.col_idx < 0) {
                throw NotImplementedException(
                    "ORDER BY over a set operation takes an output column's name or "
                    "position only");
            }
            keys.push_back(k);
        }

        std::sort(rows.begin(), rows.end(),
            [&](const std::vector<Value> &a, const std::vector<Value> &b) {
                for (auto &k : keys) {
                    if (k.col_idx < 0) continue;
                    if (k.col_idx >= static_cast<int>(a.size()) ||
                        k.col_idx >= static_cast<int>(b.size())) continue;
                    const Value &va = a[k.col_idx];
                    const Value &vb = b[k.col_idx];
                    if (va.IsNull() && vb.IsNull()) continue;
                    if (va.IsNull()) return k.nulls_first;
                    if (vb.IsNull()) return !k.nulls_first;
                    if (va < vb) return k.ascending;
                    if (vb < va) return !k.ascending;
                }
                return false;
            });
    }

    int64_t offset_val = 0;
    int64_t limit_val = -1;
    // An integer literal, or none; any other LIMIT or OFFSET of a set
    // operation was ignored (#35) and is refused.
    auto extract_int_literal = [](const ParsedExprPtr &p) -> int64_t {
        if (!p) return -1;
        if (p->GetExpressionType() == ExpressionType::CONSTANT) {
            auto &c = static_cast<const ConstantExpression &>(*p);
            if (c.literal_type == TokenType::INTEGER_LITERAL) {
                try { return std::stoll(c.value); } catch (...) {}
            }
        }
        throw NotImplementedException(
            "LIMIT and OFFSET of a set operation take a non-negative integer literal only");
    };
    int64_t lit_offset = extract_int_literal(offset_expr);
    int64_t lit_limit = extract_int_literal(limit_expr);
    if (lit_offset > 0) offset_val = lit_offset;
    if (lit_limit >= 0) limit_val = lit_limit;
    if (offset_val > 0 || limit_val >= 0) {
        size_t start = std::min<size_t>(static_cast<size_t>(offset_val), rows.size());
        size_t end = rows.size();
        if (limit_val >= 0) {
            end = std::min<size_t>(start + static_cast<size_t>(limit_val), rows.size());
        }
        std::vector<std::vector<Value>> trimmed;
        if (end > start) trimmed.reserve(end - start);
        for (size_t i = start; i < end; i++) {
            trimmed.push_back(std::move(rows[i]));
        }
        rows = std::move(trimmed);
    }
}

// The DISPLAY logical type for one SELECT-list expression: DATE_TRUNC(...)
// is re-tagged TIMESTAMP (its result is a timestamp). Returns INVALID when no
// re-tag applies.
static LogicalType ResolveResultDisplayType(const BoundExpression &expr) {
    if (expr.GetExpressionType() == BoundExpressionType::FUNCTION) {
        auto &fn = static_cast<const BoundFunction &>(expr);
        if (StringUtil::Upper(fn.function_name) == "DATE_TRUNC") return LogicalType::TIMESTAMP();
    }
    return LogicalType(LogicalTypeId::INVALID);
}

Connection::Connection(Database &database) : db_(database) {}
Connection::~Connection() = default;

// ============================================================================
// SQL Execution Pipeline: parse -> bind -> plan -> physical plan -> execute
// ============================================================================

// Split a multi-statement SQL string into per-statement source slices,
// respecting string literals ('...'), double-quoted identifiers ("..."),
// `--` line comments and `/* */` block comments. Used so that CREATE
// VIEW can capture only its own SELECT body and not silently absorb
// trailing statements.
static std::vector<std::string> SplitTopLevelStatements(const std::string &sql) {
    std::vector<std::string> out;
    size_t i = 0;
    size_t start = 0;
    while (i < sql.size()) {
        char c = sql[i];
        if (c == '\'') {
            i++;
            while (i < sql.size()) {
                if (sql[i] == '\'') {
                    if (i + 1 < sql.size() && sql[i + 1] == '\'') { i += 2; continue; }
                    i++; break;
                }
                i++;
            }
        } else if (c == '"') {
            i++;
            while (i < sql.size() && sql[i] != '"') i++;
            if (i < sql.size()) i++;
        } else if (c == '-' && i + 1 < sql.size() && sql[i + 1] == '-') {
            while (i < sql.size() && sql[i] != '\n') i++;
        } else if (c == '/' && i + 1 < sql.size() && sql[i + 1] == '*') {
            i += 2;
            while (i + 1 < sql.size() && !(sql[i] == '*' && sql[i + 1] == '/')) i++;
            if (i + 1 < sql.size()) i += 2;
        } else if (c == ';') {
            out.push_back(sql.substr(start, i - start));
            i++;
            start = i;
        } else {
            i++;
        }
    }
    if (start < sql.size()) {
        std::string tail = sql.substr(start);
        bool only_ws = true;
        for (char ch : tail) {
            if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') {
                only_ws = false; break;
            }
        }
        if (!only_ws) out.push_back(tail);
    }
    return out;
}

// Materialise a SelectStatement (incl. VALUES / set-op chains) into a
// freshly-created catalog table named `dest_name`, returning its storage.
// Shared by the top-level subquery-in-FROM / CTE paths (Connection::Query)
// and by ExpressionExecutor::ExecuteSubquery (so derived-table FROM works
// inside IN / scalar / EXISTS subqueries too). The planner has no logical
// UNION node, so the set-op chain is walked here at materialise time.
std::shared_ptr<DataTable> MaterializeSelectIntoTable(
        Catalog &catalog, SelectStatement &sel, const std::string &dest_name) {
    Binder lb(catalog);
    auto lbound = lb.Bind(sel);
    auto &lsel = static_cast<BoundSelectStatement &>(*lbound);

    if (sel.set_op.empty()) {
        std::vector<ColumnDefinition> cols;
        for (idx_t i = 0; i < lsel.result_names.size(); i++)
            cols.emplace_back(lsel.result_names[i], lsel.result_types[i]);
        auto &entry = catalog.CreateTable(dest_name, std::move(cols));
        auto storage = std::make_shared<DataTable>(lsel.result_types);
        entry.SetStorage(storage);
        auto llogical = Planner::Plan(*lbound);
        llogical = Optimizer::Optimize(std::move(llogical));
        PhysicalPlanner lpp(catalog);
        auto lphysical = lpp.Plan(*llogical);
        lphysical->Init();
        DataChunk lchunk;
        while (lphysical->GetData(lchunk)) storage->Append(lchunk);
        return storage;
    }

    // Set-op chain: bind each branch, unify per-column types, drain +
    // coerce + apply set semantics, write combined rows.
    std::vector<std::unique_ptr<BoundStatement>> branch_bounds;
    branch_bounds.push_back(std::move(lbound));
    {
        SelectStatement *cur = &sel;
        while (!cur->set_op.empty() && cur->set_right) {
            Binder rb(catalog);
            branch_bounds.push_back(rb.Bind(*cur->set_right));
            cur = cur->set_right.get();
        }
    }
    std::vector<LogicalType> result_types;
    std::vector<std::string> result_names;
    {
        auto &first = static_cast<BoundSelectStatement &>(*branch_bounds[0]);
        result_types = first.result_types;
        result_names = first.result_names;
        for (size_t b = 1; b < branch_bounds.size(); b++) {
            auto &bs = static_cast<BoundSelectStatement &>(*branch_bounds[b]);
            if (bs.result_types.size() != result_types.size()) {
                throw BinderException(ErrorCode::TYPE_MISMATCH,
                    "each UNION/INTERSECT/EXCEPT query must return "
                    "the same number of columns (" +
                    std::to_string(result_types.size()) + " vs " +
                    std::to_string(bs.result_types.size()) + ")");
            }
            for (idx_t c = 0; c < result_types.size(); c++)
                result_types[c] = ComputeSetOpCommonType(result_types[c], bs.result_types[c]);
        }
    }
    auto drain_branch_coerced = [&](BoundStatement &bound,
                                    std::vector<std::vector<Value>> &out_rows) {
        auto logical = Planner::Plan(bound);
        logical = Optimizer::Optimize(std::move(logical));
        PhysicalPlanner pp(catalog);
        auto physical = pp.Plan(*logical);
        physical->Init();
        DataChunk chunk;
        while (physical->GetData(chunk)) {
            for (idx_t i = 0; i < chunk.size(); i++) {
                std::vector<Value> row;
                row.reserve(chunk.ColumnCount());
                for (idx_t c = 0; c < chunk.ColumnCount(); c++)
                    row.push_back(CoerceValueForSetOp(chunk.GetValue(c, i), result_types[c]));
                out_rows.push_back(std::move(row));
            }
        }
    };
    std::vector<std::vector<Value>> rows;
    drain_branch_coerced(*branch_bounds[0], rows);
    {
        SelectStatement *cur = &sel;
        size_t branch_idx = 1;
        while (!cur->set_op.empty() && cur->set_right) {
            std::vector<std::vector<Value>> right_rows;
            drain_branch_coerced(*branch_bounds[branch_idx++], right_rows);
            const std::string &op = cur->set_op;
            if (op == "UNION ALL") {
                for (auto &r : right_rows) rows.push_back(std::move(r));
            } else if (op == "UNION" || op == "INTERSECT" || op == "EXCEPT") {
                ApplyDistinctSetOp(op, rows, right_rows);
            }
            cur = cur->set_right.get();
        }
    }
    std::vector<ColumnDefinition> cols;
    for (idx_t i = 0; i < result_names.size(); i++)
        cols.emplace_back(result_names[i], result_types[i]);
    auto &entry = catalog.CreateTable(dest_name, std::move(cols));
    auto storage = std::make_shared<DataTable>(result_types);
    entry.SetStorage(storage);
    BulkLoadRows(*storage, result_types, rows);
    return storage;
}

// Recursively materialise derived-table FROM sources (`FROM (SELECT..)`,
// `FROM (VALUES..)`) into temp catalog tables, rewriting each TableRef to
// the temp table name. Appends created names to `created` for cleanup.
void MaterializeFromSubqueries(Catalog &catalog, TableRef &tref,
                               std::vector<std::string> &created) {
    if (tref.subquery) {
        if (tref.subquery->from_table)
            MaterializeFromSubqueries(catalog, *tref.subquery->from_table, created);
        for (auto &cte : tref.subquery->ctes) {
            if (cte.query && cte.query->from_table)
                MaterializeFromSubqueries(catalog, *cte.query->from_table, created);
        }
        static int subq_unique_id = 0;
        std::string subq_name = "__subq_" + std::to_string(++subq_unique_id) + "__";
        MaterializeSelectIntoTable(catalog, *tref.subquery, subq_name);
        created.push_back(subq_name);
        tref.table_name = subq_name;
        tref.subquery.reset();
    }
    if (tref.right) MaterializeFromSubqueries(catalog, *tref.right, created);
}

QueryResult Connection::Query(const std::string &sql) {
    // The providers are fixed from here on (design L29).
    db_.MarkQueried();
    return RunStatements(sql);
}

QueryResult Connection::RunStatements(const std::string &sql) {
    // 1. Parse.
    // SIMILAR TO parses only when a regex provider is in use; without one
    // the parser refuses it, as before providers (design L24, L29).
    ParseOptions parse_options;
    parse_options.regex = db_.GetProviders().Regex() != nullptr;
    auto statements = Parser::Parse(sql, parse_options);
    // Per-statement source slices — used by CREATE VIEW and any other
    // path that needs to capture its own statement text without
    // accidentally absorbing trailing statements (see commit 1628b3f
    // for the same-shape bug fixed in CREATE TABLE AS).
    auto stmt_slices = SplitTopLevelStatements(sql);

    // zugodb: a read-only database refuses the whole batch, before any
    // statement runs, if one of them would change the catalog or its data.
    if (db_.GetConfig().read_only) {
        for (auto &stmt : statements) {
            switch (stmt->GetType()) {
            case StatementType::SELECT:
            case StatementType::EXPLAIN:
            case StatementType::DESCRIBE:
            case StatementType::PRAGMA:
            case StatementType::SHOW:
            case StatementType::BEGIN_TXN:
            case StatementType::COMMIT_TXN:
            case StatementType::ROLLBACK_TXN:
                break;
            default:
                throw InvalidInputException(
                    "Cannot execute a statement that changes the database: "
                    "it is open read-only");
            }
        }
    }

    // No transactions (design L26, CLAUDE.md): BEGIN, COMMIT and ROLLBACK
    // are refused, the whole batch before any of it runs. BEGIN and COMMIT
    // were accepted around autocommitted statements, so a statement after
    // a failed one inside them was kept where DuckDB rolls the whole
    // transaction back, and later queries saw rows DuckDB's do not.
    for (auto &stmt : statements) {
        auto t = stmt->GetType();
        if (t == StatementType::BEGIN_TXN || t == StatementType::COMMIT_TXN ||
            t == StatementType::ROLLBACK_TXN) {
            throw NotImplementedException(
                "Transactions are not supported: every statement commits as it runs, and "
                "BEGIN, COMMIT and ROLLBACK are refused");
        }
    }

    QueryResult final_result;
    for (auto &stmt : statements) {
        idx_t stmt_idx = static_cast<idx_t>(&stmt - statements.data());
        // A multi-statement batch returns the LAST statement's result
        // (DuckDB/standard). Reset between statements so e.g. an INSERT's
        // affected-row count doesn't leak into a following SELECT.
        final_result = QueryResult{};
        // Handle transaction control.
        if (stmt->GetType() == StatementType::BEGIN_TXN) {
            BeginTransaction();
            continue;
        }
        if (stmt->GetType() == StatementType::COMMIT_TXN) {
            CommitTransaction();
            continue;
        }
        if (stmt->GetType() == StatementType::ROLLBACK_TXN) {
            RollbackTransaction();
            continue;
        }

        // Handle PRAGMA: read-only catalog introspection for BI tool drivers.
        if (stmt->GetType() == StatementType::PRAGMA) {
            auto &pr = static_cast<PragmaStatement &>(*stmt);
            auto name = StringUtil::Upper(pr.name);
            // Accept both `PRAGMA table_info` and the `PRAGMA pragma_table_info`
            // alias DuckDB exposes for table-valued function use.
            if (name == "PRAGMA_TABLE_INFO") name = "TABLE_INFO";
            if (name == "PRAGMA_DATABASE_LIST") name = "DATABASE_LIST";

            if (name == "TABLE_INFO") {
                if (pr.arg.empty()) {
                    throw BinderException("PRAGMA table_info requires a table name");
                }
                auto *tbl = db_.GetCatalog().GetTable(pr.arg);
                if (!tbl) {
                    throw CatalogException("Table '" + pr.arg + "' not found");
                }
                final_result.column_names = {"cid", "name", "type", "notnull",
                                             "dflt_value", "pk"};
                final_result.column_types = {
                    LogicalType::INTEGER(), LogicalType::VARCHAR(),
                    LogicalType::VARCHAR(), LogicalType::BOOLEAN(),
                    LogicalType::VARCHAR(), LogicalType::BOOLEAN()};
                auto &cols = tbl->GetColumns();
                for (idx_t i = 0; i < cols.size(); i++) {
                    final_result.rows.push_back({
                        Value::INTEGER(static_cast<int32_t>(i)),
                        Value::VARCHAR(cols[i].name),
                        Value::VARCHAR(cols[i].type.ToString()),
                        // DuckDB's: NOT NULL and PRIMARY KEY (it gave false
                        // for both, Stage 9).
                        Value::BOOLEAN(cols[i].not_null || cols[i].is_primary_key),
                        Value(),
                        Value::BOOLEAN(cols[i].is_primary_key)});
                }
                continue;
            }
            if (name == "DATABASE_LIST") {
                // DuckDB's seq is the database's internal object id, which
                // the engine has none of: refused (Stage 9).
                throw NotImplementedException("PRAGMA database_list is not supported");
            }
            throw BinderException("Unknown PRAGMA: " + pr.name);
        }

        // Handle SHOW TABLES / SHOW DATABASES / SHOW COLUMNS FROM t.
        // Standardized across DuckDB / MySQL / ClickHouse; result shape matches
        // DuckDB (1-col for TABLES/DATABASES, 6-col MySQL-compatible for
        // COLUMNS) so bindings get the same UX as the native CLI.
        if (stmt->GetType() == StatementType::SHOW) {
            auto &sh = static_cast<ShowStatement &>(*stmt);

            // Tiny SQL LIKE matcher: '%' = any run, '_' = one char. Enough
            // for pattern filters on SHOW; reuse across all three kinds.
            auto like_match = [](const std::string &name, const std::string &pat) {
                if (pat.empty()) return true;
                size_t ni = 0, pi = 0, star_n = std::string::npos, star_p = 0;
                while (ni < name.size()) {
                    if (pi < pat.size() && pat[pi] == '%') {
                        star_p = ++pi; star_n = ni;
                    } else if (pi < pat.size() &&
                               (pat[pi] == '_' || pat[pi] == name[ni])) {
                        pi++; ni++;
                    } else if (star_n != std::string::npos) {
                        pi = star_p; ni = ++star_n;
                    } else {
                        return false;
                    }
                }
                while (pi < pat.size() && pat[pi] == '%') pi++;
                return pi == pat.size();
            };

            // DuckDB has no LIKE on SHOW, and no SHOW COLUMNS: both were
            // accepted (Stage 9).
            if (!sh.like_pattern.empty()) {
                throw ParserException("SHOW ... LIKE is not supported");
            }
            if (sh.kind == ShowStatement::Kind::COLUMNS) {
                throw ParserException("SHOW COLUMNS is not supported; DESCRIBE gives a table's columns");
            }
            if (sh.kind == ShowStatement::Kind::TABLES) {
                final_result.column_names = {"name"};
                final_result.column_types = {LogicalType::VARCHAR()};
                auto &schema = db_.GetCatalog().GetSchema();
                // In DuckDB's order, by name, byte by byte; it was the hash
                // map's order.
                auto names = schema.GetTableNames();
                std::sort(names.begin(), names.end());
                for (const auto &name : names) {
                    if (like_match(name, sh.like_pattern)) {
                        final_result.rows.push_back({Value::VARCHAR(name)});
                    }
                }
                continue;
            }

            if (sh.kind == ShowStatement::Kind::DATABASES) {
                final_result.column_names = {"database_name"};
                final_result.column_types = {LogicalType::VARCHAR()};
                // No ATTACH yet; always the single in-memory database.
                if (like_match("memory", sh.like_pattern)) {
                    final_result.rows.push_back({Value::VARCHAR("memory")});
                }
                continue;
            }

            // SHOW COLUMNS FROM t: same 6-col shape as DESCRIBE.
            auto *tbl = db_.GetCatalog().GetTable(sh.table_name);
            if (!tbl) throw CatalogException("Table '" + sh.table_name + "' not found");
            final_result.column_names = {"column_name", "column_type", "null",
                                         "key", "default", "extra"};
            final_result.column_types = {
                LogicalType::VARCHAR(), LogicalType::VARCHAR(),
                LogicalType::VARCHAR(), LogicalType::VARCHAR(),
                LogicalType::VARCHAR(), LogicalType::VARCHAR()};
            for (const auto &c : tbl->GetColumns()) {
                if (!sh.like_pattern.empty() && !like_match(c.name, sh.like_pattern)) continue;
                final_result.rows.push_back({
                    Value::VARCHAR(c.name),
                    Value::VARCHAR(c.type.ToString()),
                    Value::VARCHAR("YES"),
                    Value(), Value(), Value()});
            }
            continue;
        }

        // Handle DESCRIBE: bind the inner statement and emit its result schema.
        if (stmt->GetType() == StatementType::DESCRIBE) {
            auto &desc = static_cast<DescribeStatement &>(*stmt);
            std::vector<std::string> names;
            std::vector<LogicalType> types;

            {
                Binder binder(db_.GetCatalog());
                auto bound = binder.Bind(*desc.inner);
                if (bound->GetType() != BoundStatementType::SELECT) {
                    throw BinderException("DESCRIBE is only supported for SELECT queries");
                }
                auto &sel = static_cast<BoundSelectStatement &>(*bound);
                names = sel.result_names;
                types = sel.result_types;
            }

            final_result.column_names = {"column_name", "column_type", "null",
                                         "key", "default", "extra"};
            final_result.column_types = {
                LogicalType::VARCHAR(), LogicalType::VARCHAR(),
                LogicalType::VARCHAR(), LogicalType::VARCHAR(),
                LogicalType::VARCHAR(), LogicalType::VARCHAR()};
            // DESCRIBE <table>: DuckDB's null is NO for a NOT NULL or PRIMARY
            // KEY column and key PRI for the latter (they were YES and NULL,
            // Stage 9). A query's columns are all YES.
            TableCatalogEntry *described =
                desc.table_name.empty() ? nullptr : db_.GetCatalog().GetTable(desc.table_name);
            for (idx_t i = 0; i < names.size(); i++) {
                bool not_null = false, pk = false;
                if (described && i < described->GetColumns().size()) {
                    auto &c = described->GetColumns()[i];
                    not_null = c.not_null || c.is_primary_key;
                    pk = c.is_primary_key;
                }
                final_result.rows.push_back({
                    Value::VARCHAR(names[i]),
                    Value::VARCHAR(types[i].ToString()),
                    Value::VARCHAR(not_null ? "NO" : "YES"),
                    pk ? Value::VARCHAR("PRI") : Value(), Value(), Value()});
            }
            continue;
        }

        // Handle EXPLAIN: bind and plan the inner statement, return plan as text.
        if (stmt->GetType() == StatementType::EXPLAIN) {
            auto &explain = static_cast<ExplainStatement &>(*stmt);
            Binder binder(db_.GetCatalog());
            auto bound = binder.Bind(*explain.inner);
            auto logical = Planner::Plan(*bound);

            // Build a simple plan description.
            std::string plan_text;
            std::function<void(const LogicalOperator &, int)> describe;
            describe = [&](const LogicalOperator &op, int indent) {
                for (int i = 0; i < indent; i++) plan_text += "  ";
                auto type = op.GetOperatorType();
                switch (type) {
                case LogicalOperatorType::GET: plan_text += "SCAN"; break;
                case LogicalOperatorType::FILTER: plan_text += "FILTER"; break;
                case LogicalOperatorType::PROJECTION: plan_text += "PROJECTION"; break;
                case LogicalOperatorType::ORDER_BY: plan_text += "ORDER_BY"; break;
                case LogicalOperatorType::LIMIT: plan_text += "LIMIT"; break;
                case LogicalOperatorType::AGGREGATE: plan_text += "AGGREGATE"; break;
                case LogicalOperatorType::JOIN: plan_text += "JOIN"; break;
                case LogicalOperatorType::DISTINCT: plan_text += "DISTINCT"; break;
                case LogicalOperatorType::WINDOW: plan_text += "WINDOW"; break;
                case LogicalOperatorType::INSERT: plan_text += "INSERT"; break;
                case LogicalOperatorType::CREATE_TABLE: plan_text += "CREATE_TABLE"; break;
                case LogicalOperatorType::DROP_TABLE: plan_text += "DROP_TABLE"; break;
                case LogicalOperatorType::UPDATE: plan_text += "UPDATE"; break;
                case LogicalOperatorType::DELETE_STMT: plan_text += "DELETE"; break;
                case LogicalOperatorType::DUMMY_SCAN: plan_text += "DUMMY_SCAN"; break;
                }
                plan_text += "\n";
                for (auto &child : op.children) {
                    describe(*child, indent + 1);
                }
            };
            describe(*logical, 0);

            final_result.column_names = {"plan"};
            final_result.column_types = {LogicalType::VARCHAR()};
            final_result.rows.push_back({Value::VARCHAR(plan_text)});
            continue;
        }

        // Handle CREATE TABLE AS SELECT (CTAS): execute the SELECT, materialize
        // the result into a new table. Schema is inferred from the query's
        // result types/names. Mirrors the CREATE VIEW materialization path but
        // without SetViewQuery - this is a real table, not a virtual view.
        if (stmt->GetType() == StatementType::CREATE_TABLE) {
            auto &ct = static_cast<CreateTableStatement &>(*stmt);
            if (ct.query) {
                if (ct.if_not_exists && db_.GetCatalog().GetTable(ct.table_name)) {
                    continue;
                }

                // Route CTAS through the shared MaterializeSelectIntoTable.
                // The previous implementation bound/planned/drained ct.query
                // directly, which:
                //  (a) only materialised the LEFT branch of a set-op chain,
                //      so `CREATE TABLE t AS VALUES (1),(2),(3)` (VALUES
                //      desugars to UNION ALL) kept only the FIRST row — a
                //      huge cascade since CTAS-AS-VALUES is a common test
                //      setup; every downstream SELECT then saw 1 row; and
                //  (b) failed on a derived-table FROM in the SELECT
                //      (`CREATE TABLE t AS SELECT * FROM (VALUES..)`)
                //      with "Table '' not found".
                // MaterializeSelectIntoTable handles the set-op chain and
                // creates the table; we add derived-table FROM
                // materialisation (walking the set-op chain) around it.
                std::vector<std::string> ctas_temp;
                struct CtasTempGuard {
                    Catalog *cat; std::vector<std::string> *names;
                    ~CtasTempGuard() { for (auto &n : *names) cat->DropTable(n); }
                } _ctas_guard{&db_.GetCatalog(), &ctas_temp};
                for (SelectStatement *cur = ct.query.get(); cur;
                     cur = cur->set_right.get()) {
                    if (cur->from_table)
                        MaterializeFromSubqueries(db_.GetCatalog(),
                                                  *cur->from_table, ctas_temp);
                    for (auto &cte : cur->ctes) {
                        if (cte.query && cte.query->from_table)
                            MaterializeFromSubqueries(db_.GetCatalog(),
                                                      *cte.query->from_table, ctas_temp);
                    }
                }

                if (ct.or_replace) db_.GetCatalog().DropTable(ct.table_name);
                if (db_.GetCatalog().GetTable(ct.table_name)) {
                    throw CatalogException("Table '" + ct.table_name +
                                           "' already exists");
                }
                MaterializeSelectIntoTable(db_.GetCatalog(), *ct.query,
                                           ct.table_name);
                // DuckDB's result: the rows the table was made with (#38).
                auto *made = db_.GetCatalog().GetTable(ct.table_name);
                final_result.column_names = {"Count"};
                final_result.column_types = {LogicalType::BIGINT()};
                final_result.rows.push_back(
                    {Value::BIGINT(made ? static_cast<int64_t>(made->GetStorage().Count()) : 0)});
                final_result.is_count = true;
                continue;
            }
        }

        // Handle CREATE VIEW: store the SQL query for virtual re-execution.
        if (stmt->GetType() == StatementType::CREATE_VIEW) {
            auto &cv = static_cast<CreateViewStatement &>(*stmt);

            // Extract the SELECT SQL from THIS statement's slice, not
            // the whole multi-statement input. Previously the " AS "
            // substring search ran over the full sql string, silently
            // absorbing every trailing statement into the view body
            // (same shape as the CTAS bug fixed in 1628b3f).
            std::string view_sql;
            {
                const std::string &slice = (stmt_idx < stmt_slices.size())
                    ? stmt_slices[stmt_idx] : sql;
                auto upper_slice = StringUtil::Upper(slice);
                auto as_pos = upper_slice.find(" AS ");
                if (as_pos != std::string::npos) {
                    view_sql = slice.substr(as_pos + 4);
                    // Trim trailing semicolons and whitespace.
                    while (!view_sql.empty() && (view_sql.back() == ';' || view_sql.back() == ' ' || view_sql.back() == '\n' || view_sql.back() == '\r'))
                        view_sql.pop_back();
                    // Leading whitespace too.
                    size_t lead = 0;
                    while (lead < view_sql.size() &&
                           (view_sql[lead] == ' ' || view_sql[lead] == '\t' ||
                            view_sql[lead] == '\n' || view_sql[lead] == '\r')) lead++;
                    if (lead) view_sql.erase(0, lead);
                }
            }

            // Execute the view query once to determine column names and types.
            auto view_result = Query(view_sql);

            if (cv.or_replace) db_.GetCatalog().DropTable(cv.view_name);

            std::vector<ColumnDefinition> cols;
            for (idx_t i = 0; i < view_result.column_names.size(); i++) {
                cols.emplace_back(view_result.column_names[i], view_result.column_types[i]);
            }

            auto &entry = db_.GetCatalog().CreateTable(cv.view_name, std::move(cols));
            auto storage = std::make_shared<DataTable>(view_result.column_types);
            view_result.MaterialiseRows();
            BulkLoadRows(*storage, view_result.column_types, view_result.rows);
            entry.SetStorage(storage);
            entry.SetViewQuery(view_sql);
            continue;
        }

        // Handle TRUNCATE: replace storage with empty table.
        if (stmt->GetType() == StatementType::TRUNCATE) {
            auto &trunc = static_cast<TruncateStatement &>(*stmt);
            auto *entry = db_.GetCatalog().GetTable(trunc.table_name);
            if (!entry) throw CatalogException("Table '" + trunc.table_name + "' not found");
            entry->SetStorage(std::make_shared<DataTable>(entry->GetTypes()));
            continue;
        }

        // Handle ALTER TABLE.
        if (stmt->GetType() == StatementType::ALTER_TABLE) {
            auto &alter = static_cast<AlterTableStatement &>(*stmt);
            auto *entry = db_.GetCatalog().GetTable(alter.table_name);
            if (!entry) throw CatalogException("Table '" + alter.table_name + "' not found");

            auto old_cols = entry->GetColumns();
            auto old_types = entry->GetTypes();

            if (alter.action == AlterTableStatement::AlterAction::ADD_COLUMN) {
                // A name the table has, in any case, is DuckDB's error.
                for (auto &c : old_cols) {
                    if (IdentifierEquals(c.name, alter.column_name)) {
                        throw CatalogException("Column with name \"" + alter.column_name +
                                               "\" already exists");
                    }
                }
                // Scan all data, add NULL for new column, rebuild.
                Binder binder(db_.GetCatalog());
                auto new_type = binder.ResolveTypeName(alter.column_type);

                std::vector<ColumnDefinition> new_cols = old_cols;
                new_cols.emplace_back(alter.column_name, new_type);
                auto new_types = old_types;
                new_types.push_back(new_type);

                auto new_storage = std::make_shared<DataTable>(new_types);
                auto &old_storage = entry->GetStorage();
                auto state = old_storage.InitScan();
                DataChunk chunk;
                while (true) {
                    chunk.Initialize(old_types);
                    if (!old_storage.Scan(state, chunk)) break;
                    DataChunk new_chunk;
                    new_chunk.Initialize(new_types);
                    for (idx_t i = 0; i < chunk.size(); i++) {
                        for (idx_t c = 0; c < old_types.size(); c++)
                            new_chunk.SetValue(c, i, chunk.GetValue(c, i));
                        new_chunk.SetValue(old_types.size(), i, Value()); // NULL
                    }
                    new_storage->Append(new_chunk);
                }

                // Replace the table entry.
                db_.GetCatalog().DropTable(alter.table_name);
                auto &new_entry = db_.GetCatalog().CreateTable(alter.table_name, std::move(new_cols));
                new_entry.SetStorage(new_storage);
            } else if (alter.action == AlterTableStatement::AlterAction::DROP_COLUMN) {
                auto drop_idx = entry->GetColumnIndex(alter.column_name);
                if (drop_idx == INVALID_INDEX)
                    throw CatalogException("Column '" + alter.column_name + "' not found");

                std::vector<ColumnDefinition> new_cols;
                std::vector<LogicalType> new_types;
                for (idx_t c = 0; c < old_cols.size(); c++) {
                    if (c != drop_idx) {
                        new_cols.push_back(old_cols[c]);
                        new_types.push_back(old_types[c]);
                    }
                }

                auto new_storage = std::make_shared<DataTable>(new_types);
                auto &old_storage = entry->GetStorage();
                auto state = old_storage.InitScan();
                DataChunk chunk;
                while (true) {
                    chunk.Initialize(old_types);
                    if (!old_storage.Scan(state, chunk)) break;
                    DataChunk new_chunk;
                    new_chunk.Initialize(new_types);
                    for (idx_t i = 0; i < chunk.size(); i++) {
                        idx_t nc = 0;
                        for (idx_t c = 0; c < old_types.size(); c++) {
                            if (c != drop_idx)
                                new_chunk.SetValue(nc++, i, chunk.GetValue(c, i));
                        }
                    }
                    new_storage->Append(new_chunk);
                }

                db_.GetCatalog().DropTable(alter.table_name);
                auto &new_entry = db_.GetCatalog().CreateTable(alter.table_name, std::move(new_cols));
                new_entry.SetStorage(new_storage);
            } else if (alter.action == AlterTableStatement::AlterAction::RENAME_COLUMN) {
                auto rename_idx = entry->GetColumnIndex(alter.column_name);
                if (rename_idx == INVALID_INDEX)
                    throw CatalogException("Column '" + alter.column_name + "' not found");

                for (idx_t c = 0; c < old_cols.size(); c++) {
                    if (c != rename_idx && IdentifierEquals(old_cols[c].name, alter.new_column_name)) {
                        throw CatalogException("Column with name \"" + alter.new_column_name +
                                               "\" already exists");
                    }
                }
                std::vector<ColumnDefinition> new_cols = old_cols;
                new_cols[rename_idx].name = alter.new_column_name;


                // We need the actual shared_ptr. Just rebuild.
                auto new_storage = std::make_shared<DataTable>(old_types);
                auto &old_storage = entry->GetStorage();
                auto state = old_storage.InitScan();
                DataChunk chunk;
                while (true) {
                    chunk.Initialize(old_types);
                    if (!old_storage.Scan(state, chunk)) break;
                    new_storage->Append(chunk);
                }

                db_.GetCatalog().DropTable(alter.table_name);
                auto &new_entry = db_.GetCatalog().CreateTable(alter.table_name, std::move(new_cols));
                new_entry.SetStorage(new_storage);
            }
            continue;
        }

        // Handle INSERT INTO ... SELECT.
        if (stmt->GetType() == StatementType::INSERT) {
            auto &ins = static_cast<InsertStatement &>(*stmt);
            if (ins.select_source) {
                // Materialise derived-table FROM sources in the SELECT
                // (`INSERT INTO t SELECT ... FROM (VALUES..)` /
                // `FROM (SELECT..)`) before binding. Previously the
                // INSERT-SELECT path bound select_source directly and a
                // derived-table FROM failed with "Table '' not found".
                std::vector<std::string> ins_temp_tables;
                struct InsTempGuard {
                    Catalog *cat; std::vector<std::string> *names;
                    ~InsTempGuard() { for (auto &n : *names) cat->DropTable(n); }
                } _ins_guard{&db_.GetCatalog(), &ins_temp_tables};
                if (ins.select_source->from_table)
                    MaterializeFromSubqueries(db_.GetCatalog(),
                                              *ins.select_source->from_table, ins_temp_tables);
                for (auto &cte : ins.select_source->ctes) {
                    if (cte.query && cte.query->from_table)
                        MaterializeFromSubqueries(db_.GetCatalog(),
                                                  *cte.query->from_table, ins_temp_tables);
                }
                // Execute the SELECT.
                Binder sel_binder(db_.GetCatalog());
                auto sel_bound = sel_binder.Bind(*ins.select_source);
                auto sel_logical = Planner::Plan(*sel_bound);
                PhysicalPlanner sel_pp(db_.GetCatalog());
                auto sel_physical = sel_pp.Plan(*sel_logical);
                sel_physical->Init();

                auto *entry = db_.GetCatalog().GetTable(ins.table_name);
                if (!entry) throw CatalogException("Table '" + ins.table_name + "' not found");

                auto col_types = entry->GetTypes();
                // Resolve user column-list to table-column indices.
                // Empty list means positional (declaration order).
                // Mirrors the INSERT VALUES path fixed in 292527f —
                // without this, reordered or subset INSERT...SELECT
                // silently corrupted rows (or crashed on subset).
                std::vector<idx_t> target_indices;
                if (!ins.column_names.empty()) {
                    target_indices.reserve(ins.column_names.size());
                    std::vector<bool> seen(col_types.size(), false);
                    for (auto &col_name : ins.column_names) {
                        idx_t idx = entry->GetColumnIndex(col_name);
                        if (idx == INVALID_INDEX) {
                            throw BinderException(ErrorCode::COLUMN_NOT_FOUND,
                                "Column '" + col_name + "' not found in table '" +
                                ins.table_name + "'");
                        }
                        if (seen[idx]) {
                            throw BinderException(ErrorCode::TYPE_MISMATCH,
                                "Column '" + col_name + "' specified more than once "
                                "in INSERT column list");
                        }
                        seen[idx] = true;
                        target_indices.push_back(idx);
                    }
                }

                // Validate SELECT arity against the expected target.
                auto &sel_select = static_cast<BoundSelectStatement &>(*sel_bound);
                idx_t expected_cols = target_indices.empty()
                    ? col_types.size() : target_indices.size();
                if (sel_select.result_types.size() != expected_cols) {
                    throw BinderException(ErrorCode::TYPE_MISMATCH,
                        "INSERT SELECT column count (" +
                        std::to_string(sel_select.result_types.size()) +
                        ") does not match " +
                        (target_indices.empty() ? "table"
                                                : "column list") +
                        " count (" + std::to_string(expected_cols) + ")");
                }

                auto &tgt_cols = entry->GetColumns();
                auto check_not_null_chunk = [&](const DataChunk &c) {
                    for (idx_t col = 0; col < tgt_cols.size(); col++) {
                        if (!tgt_cols[col].not_null) continue;
                        auto &vec = const_cast<DataChunk &>(c).GetVector(col);
                        for (idx_t r = 0; r < c.size(); r++) {
                            if (!vec.GetValidity().RowIsValid(r)) {
                                throw BinderException(
                                    ErrorCode::TYPE_MISMATCH,
                                    "NOT NULL constraint violation: column '" +
                                    tgt_cols[col].name + "'");
                            }
                        }
                    }
                };
                // Coerce a single Value from its source type to the
                // declared target column type. Previously the INSERT-
                // SELECT loop wrote source-typed values directly into
                // target-typed Vector slots via SetValue, which read
                // the wrong union slot and produced silent garbage:
                //   BIGINT -> INTEGER         got 0,1,2 from 1,2,3
                //   INTEGER -> BIGINT         got 0, 8589934593
                //   DOUBLE -> INTEGER         got 0, 1073217536
                //   INTEGER -> VARCHAR        got empty strings
                auto coerce = [](const Value &src, const LogicalType &target) -> Value {
                    if (src.IsNull()) return Value();
                    auto src_id = src.type().id();
                    auto dst_id = target.id();
                    if (src_id == dst_id) return src;
                    // Numeric -> Numeric: route through int64/double.
                    auto to_int64 = [&](int64_t &out) -> bool {
                        switch (src_id) {
                        case LogicalTypeId::TINYINT:  out = src.GetValue<int8_t>();  return true;
                        case LogicalTypeId::SMALLINT: out = src.GetValue<int16_t>(); return true;
                        case LogicalTypeId::INTEGER:  out = src.GetValue<int32_t>(); return true;
                        case LogicalTypeId::BIGINT:   out = src.GetValue<int64_t>(); return true;
                        case LogicalTypeId::UTINYINT: out = src.GetValue<uint8_t>(); return true;
                        case LogicalTypeId::USMALLINT:out = src.GetValue<uint16_t>();return true;
                        case LogicalTypeId::UINTEGER: out = src.GetValue<uint32_t>();return true;
                        default: return false;
                        }
                    };
                    auto to_double = [&](double &out) -> bool {
                        switch (src_id) {
                        case LogicalTypeId::FLOAT:   out = src.GetValue<float>();  return true;
                        case LogicalTypeId::DOUBLE:  out = src.GetValue<double>(); return true;
                        default: {
                            int64_t v;
                            if (to_int64(v)) { out = static_cast<double>(v); return true; }
                            return false;
                        }
                        }
                    };
                    // A BLOB from text only, to text only (#31).
                    if (dst_id == LogicalTypeId::BLOB) {
                        if (src_id != LogicalTypeId::VARCHAR) {
                            throw ConversionException("Unimplemented type for cast (" +
                                src.type().ToString() + " -> BLOB)");
                        }
                        return Value::BLOB(blob::FromText(src.GetValue<std::string>()));
                    }
                    if (src_id == LogicalTypeId::BLOB && dst_id != LogicalTypeId::VARCHAR) {
                        throw ConversionException("Unimplemented type for cast (BLOB -> " +
                            target.ToString() + ")");
                    }
                    // Target VARCHAR: stringify.
                    if (dst_id == LogicalTypeId::VARCHAR) {
                        return Value::VARCHAR(src.ToString());
                    }
                    // Target int family: try numeric path then range-check.
                    if (dst_id == LogicalTypeId::TINYINT ||
                        dst_id == LogicalTypeId::SMALLINT ||
                        dst_id == LogicalTypeId::INTEGER ||
                        dst_id == LogicalTypeId::BIGINT) {
                        int64_t v;
                        if (!to_int64(v)) {
                            double d;
                            if (!to_double(d)) {
                                throw ConversionException(
                                    "Cannot coerce '" + src.ToString() + "' to integer");
                            }
                            // DuckDB's cast: rounded half to even, and a NaN
                            // or a value past BIGINT is out of range. It was
                            // truncated, and a NaN's cast undefined (#42).
                            d = std::nearbyint(d);
                            if (IsNaN(d) || !(d >= -9223372036854775808.0 && d < 9223372036854775808.0)) {
                                throw ConversionException(
                                    "Value " + src.ToString() + " out of range for target column");
                            }
                            v = static_cast<int64_t>(d);
                        }
                        auto chk = [&](int64_t lo, int64_t hi) {
                            if (v < lo || v > hi) {
                                throw ConversionException(
                                    "Value " + src.ToString() + " out of range for target column");
                            }
                        };
                        if (dst_id == LogicalTypeId::TINYINT) {
                            chk(std::numeric_limits<int8_t>::min(),
                                std::numeric_limits<int8_t>::max());
                            return Value::TINYINT(static_cast<int8_t>(v));
                        }
                        if (dst_id == LogicalTypeId::SMALLINT) {
                            chk(std::numeric_limits<int16_t>::min(),
                                std::numeric_limits<int16_t>::max());
                            return Value::SMALLINT(static_cast<int16_t>(v));
                        }
                        if (dst_id == LogicalTypeId::INTEGER) {
                            chk(std::numeric_limits<int32_t>::min(),
                                std::numeric_limits<int32_t>::max());
                            return Value::INTEGER(static_cast<int32_t>(v));
                        }
                        return Value::BIGINT(v);
                    }
                    if (dst_id == LogicalTypeId::DOUBLE) {
                        double d;
                        if (to_double(d)) return Value::DOUBLE(d);
                    }
                    if (dst_id == LogicalTypeId::FLOAT) {
                        double d;
                        if (to_double(d)) return Value::FLOAT(static_cast<float>(d));
                    }
                    if ((dst_id == LogicalTypeId::DATE || dst_id == LogicalTypeId::TIME ||
                         dst_id == LogicalTypeId::TIMESTAMP ||
                         dst_id == LogicalTypeId::TIMESTAMP_TZ) &&
                        src_id == LogicalTypeId::VARCHAR) {
                        // DuckDB's grammar; text it rejects is an error,
                        // not a value (design L19).
                        return Value::ParseTemporal(src.GetValue<std::string>(),
                                                    dst_id == LogicalTypeId::TIMESTAMP_TZ
                                                        ? LogicalTypeId::TIMESTAMP : dst_id);
                    }
                    // Fallback: let SetValue do whatever it does (legacy
                    // behavior); at least we tried the typed paths.
                    return src;
                };
                auto build_coerced_chunk = [&](const DataChunk &src_chunk,
                                                const std::vector<idx_t> *perm) -> DataChunk {
                    DataChunk out;
                    out.Initialize(col_types);
                    out.SetCardinality(src_chunk.size());
                    if (perm) {
                        // Default missing columns to NULL of target type.
                        for (idx_t c = 0; c < col_types.size(); c++) {
                            for (idx_t r = 0; r < src_chunk.size(); r++) {
                                out.GetVector(c).GetValidity().SetInvalid(r);
                            }
                        }
                        for (idx_t i = 0; i < perm->size(); i++) {
                            idx_t tgt = (*perm)[i];
                            for (idx_t r = 0; r < src_chunk.size(); r++) {
                                Value sv = const_cast<DataChunk &>(src_chunk).GetValue(i, r);
                                out.SetValue(tgt, r, coerce(sv, col_types[tgt]));
                            }
                        }
                    } else {
                        for (idx_t c = 0; c < col_types.size(); c++) {
                            for (idx_t r = 0; r < src_chunk.size(); r++) {
                                Value sv = const_cast<DataChunk &>(src_chunk).GetValue(c, r);
                                out.SetValue(c, r, coerce(sv, col_types[c]));
                            }
                        }
                    }
                    return out;
                };

                // The SELECT runs to its end before the first row is
                // appended, so it sees the table as it was when the
                // statement began, as in DuckDB. Appending chunk by chunk
                // let `INSERT INTO t SELECT ... FROM t` read its own rows
                // and never finish (#23). build_coerced_chunk copies each
                // value, so the kept chunks do not point into the source.
                DataChunk chunk;
                int64_t ins_count = 0;
                std::vector<DataChunk> pending;
                while (true) {
                    if (!sel_physical->GetData(chunk)) break;
                    ins_count += static_cast<int64_t>(chunk.size());
                    pending.push_back(build_coerced_chunk(
                        chunk, target_indices.empty() ? nullptr : &target_indices));
                }
                for (auto &coerced : pending) {
                    check_not_null_chunk(coerced);
                    entry->CheckPrimaryKeyForChunk(coerced);
                    entry->GetStorage().Append(coerced);
                }
                // INSERT-SELECT affected-row count (matches DuckDB).
                final_result.column_names = {"Count"};
                final_result.column_types = {LogicalType::BIGINT()};
                final_result.rows.push_back({Value::BIGINT(ins_count)});
                final_result.is_count = true;
                continue;
            }
        }

        // Handle MERGE.
        if (stmt->GetType() == StatementType::MERGE) {
            auto &merge = static_cast<MergeStatement &>(*stmt);
            auto *target = db_.GetCatalog().GetTable(merge.target_table);
            auto *source = db_.GetCatalog().GetTable(merge.source_table);
            if (!target) throw CatalogException("Target table '" + merge.target_table + "' not found");
            if (!source) throw CatalogException("Source table '" + merge.source_table + "' not found");

            auto target_types = target->GetTypes();
            auto source_types = source->GetTypes();

            // Collect all source rows.
            std::vector<std::vector<Value>> source_rows;
            auto src_state = source->GetStorage().InitScan();
            DataChunk src_chunk;
            while (true) {
                src_chunk.Initialize(source_types);
                if (!source->GetStorage().Scan(src_state, src_chunk)) break;
                for (idx_t i = 0; i < src_chunk.size(); i++) {
                    std::vector<Value> row;
                    for (idx_t c = 0; c < src_chunk.ColumnCount(); c++)
                        row.push_back(src_chunk.GetValue(c, i));
                    source_rows.push_back(std::move(row));
                }
            }

            // Collect all target rows.
            std::vector<std::vector<Value>> target_rows;
            auto tgt_state = target->GetStorage().InitScan();
            DataChunk tgt_chunk;
            while (true) {
                tgt_chunk.Initialize(target_types);
                if (!target->GetStorage().Scan(tgt_state, tgt_chunk)) break;
                for (idx_t i = 0; i < tgt_chunk.size(); i++) {
                    std::vector<Value> row;
                    for (idx_t c = 0; c < tgt_chunk.ColumnCount(); c++)
                        row.push_back(tgt_chunk.GetValue(c, i));
                    target_rows.push_back(std::move(row));
                }
            }

            // What the handler can do, and nothing else (Stage 10): ON one
            // column of the target equal to one of the source, of one type;
            // SET and VALUES from columns of the source, or NULL in VALUES.
            // It matched the first columns when ON was anything else, left
            // a SET of a constant undone, stored a VALUES constant as text
            // (a number read back 0), matched NULL with the text 'NULL',
            // put VALUES in the table's column order whatever INSERT named,
            // and matched later source rows with rows it had inserted.
            auto refuse = [](const std::string &what) {
                throw NotImplementedException("MERGE " + what + " is not supported");
            };
            if (!target->GetPrimaryKeyColumns().empty()) refuse("into a table with a primary key");
            auto names = [](const std::string &table, const std::string &alias) {
                std::vector<std::string> out{StringUtil::Lower(table)};
                if (!alias.empty()) out.push_back(StringUtil::Lower(alias));
                return out;
            };
            const auto target_names = names(merge.target_table, merge.target_alias);
            const auto source_names = names(merge.source_table, merge.source_alias);
            auto names_side = [](const ColumnRefExpression &ref, const std::vector<std::string> &side) {
                auto q = StringUtil::Lower(ref.table_name);
                for (auto &n : side) if (n == q) return true;
                return false;
            };
            // A source column, qualified by the source's name or alias.
            auto source_column = [&](const ParsedExpression &e, const std::string &where) -> idx_t {
                if (e.GetExpressionType() != ExpressionType::COLUMN_REF) {
                    refuse("with " + where + " other than a column of the source");
                }
                auto &ref = static_cast<const ColumnRefExpression &>(e);
                if (!names_side(ref, source_names)) {
                    refuse("with " + where + " other than a column of the source, named with its table");
                }
                auto idx = source->GetColumnIndex(ref.column_name);
                if (idx == INVALID_INDEX) {
                    throw BinderException("Column '" + ref.column_name + "' not found in '" +
                                          merge.source_table + "'");
                }
                return idx;
            };
            auto same_type = [&](idx_t target_col, idx_t source_col, const std::string &where) {
                if (target_types[target_col] != source_types[source_col]) {
                    refuse("with " + where + " of another type than its target column");
                }
            };
            idx_t target_join_col = INVALID_INDEX, source_join_col = INVALID_INDEX;
            {
                if (!merge.on_condition ||
                    merge.on_condition->GetExpressionType() != ExpressionType::COMPARISON) {
                    refuse("ON other than a column of the target = a column of the source");
                }
                auto &cmp = static_cast<ComparisonExpression &>(*merge.on_condition);
                if (cmp.op != "=" || cmp.left->GetExpressionType() != ExpressionType::COLUMN_REF ||
                    cmp.right->GetExpressionType() != ExpressionType::COLUMN_REF) {
                    refuse("ON other than a column of the target = a column of the source");
                }
                auto *l = static_cast<ColumnRefExpression *>(cmp.left.get());
                auto *r = static_cast<ColumnRefExpression *>(cmp.right.get());
                if (names_side(*r, target_names) && !names_side(*l, target_names)) std::swap(l, r);
                if (!names_side(*l, target_names) || !names_side(*r, source_names)) {
                    refuse("ON other than a column of the target = a column of the source, each named with its table");
                }
                target_join_col = target->GetColumnIndex(l->column_name);
                if (target_join_col == INVALID_INDEX) {
                    throw BinderException("Column '" + l->column_name + "' not found in '" +
                                          merge.target_table + "'");
                }
                source_join_col = source_column(*r, "ON");
                if (target_types[target_join_col] != source_types[source_join_col]) {
                    refuse("ON columns of two types");
                }
            }
            // SET: target column <- source column.
            std::vector<std::pair<idx_t, idx_t>> sets;
            for (auto &assign : merge.update_assignments) {
                auto idx = target->GetColumnIndex(assign.column_name);
                if (idx == INVALID_INDEX) {
                    throw BinderException("Column '" + assign.column_name + "' not found in '" +
                                          merge.target_table + "'");
                }
                auto src = source_column(*assign.value, "a SET value");
                same_type(idx, src, "a SET value");
                sets.emplace_back(idx, src);
            }
            // INSERT: per target column, a source column, or INVALID_INDEX
            // for NULL (a column not named, or NULL).
            std::vector<idx_t> inserts(target_types.size(), INVALID_INDEX);
            if (merge.has_insert) {
                std::vector<idx_t> cols;
                if (merge.insert_columns.empty()) {
                    for (idx_t c = 0; c < target_types.size(); c++) cols.push_back(c);
                } else {
                    std::vector<bool> named(target_types.size(), false);
                    for (auto &name : merge.insert_columns) {
                        auto idx = target->GetColumnIndex(name);
                        if (idx == INVALID_INDEX) {
                            throw BinderException("Column '" + name + "' not found in '" +
                                                  merge.target_table + "'");
                        }
                        if (named[idx]) throw BinderException("Column '" + name + "' named twice in MERGE INSERT");
                        named[idx] = true;
                        cols.push_back(idx);
                    }
                }
                if (cols.size() != merge.insert_values.size()) {
                    throw BinderException("MERGE INSERT has " + std::to_string(cols.size()) +
                                          " columns but " + std::to_string(merge.insert_values.size()) +
                                          " values");
                }
                for (idx_t c = 0; c < cols.size(); c++) {
                    auto &e = *merge.insert_values[c];
                    if (e.GetExpressionType() == ExpressionType::CONSTANT &&
                        static_cast<const ConstantExpression &>(e).is_null) {
                        continue;
                    }
                    auto src = source_column(e, "an INSERT value");
                    same_type(cols[c], src, "an INSERT value");
                    inserts[cols[c]] = src;
                }
            }

            auto new_storage = std::make_shared<DataTable>(target_types);
            const idx_t original_rows = target_rows.size();
            std::vector<bool> target_matched(original_rows, false);

            for (auto &src_row : source_rows) {
                bool matched = false;
                const Value &key = src_row[source_join_col];
                // A NULL key matches nothing. Only the target's own rows are
                // matched, not those this statement inserts.
                for (idx_t t = 0; t < original_rows && !key.IsNull(); t++) {
                    const Value &tk = target_rows[t][target_join_col];
                    if (tk.IsNull() || !(tk == key)) continue;
                    if (target_matched[t] && merge.has_update) {
                        // Which source row's values would it take?
                        throw InvalidInputException(
                            "MERGE: a row of the target matches more than one row of the source");
                    }
                    matched = true;
                    target_matched[t] = true;
                    if (merge.has_update) {
                        for (auto &set : sets) target_rows[t][set.first] = src_row[set.second];
                    }
                }
                if (!matched && merge.has_insert) {
                    std::vector<Value> new_row(target_types.size());
                    for (idx_t c = 0; c < target_types.size(); c++) {
                        if (inserts[c] != INVALID_INDEX) new_row[c] = src_row[inserts[c]];
                    }
                    target_rows.push_back(std::move(new_row));
                }
            }

            // Rebuild target table.
            BulkLoadRows(*new_storage, target_types, target_rows);
            target->SetStorage(new_storage);
            continue;
        }

        // Handle virtual views: if FROM references a view, re-execute the stored query
        // and replace the view's storage with fresh data from the file.
        std::vector<std::string> temp_tables;
        std::vector<std::string> cte_tables;

        // RAII cleanup - drops any temp / CTE tables created during
        // preprocessing even if binding / planning / execution throws.
        // Without this, a failing query leaves a zombie __auto_file__
        // in the catalog that collides on the next query.
        struct TableCleanupGuard {
            Catalog *catalog;
            std::vector<std::string> *tt;
            std::vector<std::string> *ct;
            ~TableCleanupGuard() {
                auto drop = [&](std::vector<std::string> &v) {
                    for (auto &t : v) {
                        if (catalog->GetTable(t)) catalog->DropTable(t);
                    }
                };
                drop(*tt);
                drop(*ct);
            }
        } _cleanup{&db_.GetCatalog(), &temp_tables, &cte_tables};

        if (stmt->GetType() == StatementType::SELECT) {
            auto &outer_sel = static_cast<SelectStatement &>(*stmt);
            // FROM preprocessing (view expansion, generate_series) runs for
            // every SELECT in a UNION / INTERSECT / EXCEPT chain.
            // Counter for auto-generated temp-table names - ensures left and
            // right sides of a UNION don't collide on the same entry.
            static thread_local int preproc_uid = 0;


            for (SelectStatement *cur_sel = &outer_sel; cur_sel;
                 cur_sel = cur_sel->set_right.get()) {
            auto &sel = *cur_sel;
            ++preproc_uid;

            // Wrap the whole preprocessing below in a zero-arg lambda so
            // we can re-run it for each TableRef in a JOIN chain. The
            // lambda reads/writes `sel.from_table`; after the main pass
            // we swap each JOIN right-hand-side into that slot in turn.
            auto run_preprocess = [&]() {

            // Expand virtual views in the FROM clause.
            auto expand_view = [&](TableRef &ref) {
                if (ref.is_table_function) return;
                auto *entry = db_.GetCatalog().GetTable(ref.table_name);
                if (!entry || !entry->IsView()) return;

                // Re-execute the view's stored query to get fresh data.
                // SELECT now collects results into `chunks` (avoids per-cell
                // boxing on the hot path). Materialise once for BulkLoadRows.
                auto view_result = Query(entry->GetViewQuery());
                view_result.MaterialiseRows();
                auto storage = std::make_shared<DataTable>(view_result.column_types);
                BulkLoadRows(*storage, view_result.column_types, view_result.rows);
                entry->SetStorage(storage);
            };

            if (sel.from_table && !sel.from_table->is_table_function) {
                expand_view(*sel.from_table);
                if (sel.from_table->right) expand_view(*sel.from_table->right);
            }
            if (sel.from_table && sel.from_table->is_table_function &&
                StringUtil::Upper(sel.from_table->table_name) == "GENERATE_SERIES") {
                // Evaluate args as constants. SQL standard variants:
                //   generate_series(stop)              — start=0, step=1
                //   generate_series(start, stop)       — step=1
                //   generate_series(start, stop, step)
                // Prior code only handled the 2+ arg forms and silently
                // left start=stop=0 for the 1-arg form, producing an
                // empty result — fix as a general engine improvement.
                // Each argument is an integer literal, `5` or `-5`; any
                // other expression was read as a constant's text it was not
                // (a UnaryMinusExpression cast to ConstantExpression, Stage
                // 9's SQL fuzzer), and is refused.
                auto &fa = sel.from_table->function_args;
                if (fa.empty() || fa.size() > 3) {
                    throw BinderException(ErrorCode::TYPE_MISMATCH,
                        "GENERATE_SERIES takes one to three arguments");
                }
                auto literal = [](const ParsedExpression &e) -> int64_t {
                    const ParsedExpression *x = &e;
                    bool negative = false;
                    if (x->GetExpressionType() == ExpressionType::UNARY_MINUS) {
                        negative = true;
                        x = static_cast<const UnaryMinusExpression *>(x)->child.get();
                    }
                    if (x->GetExpressionType() == ExpressionType::CONSTANT) {
                        auto &c = static_cast<const ConstantExpression &>(*x);
                        if (!c.is_null && c.literal_type == TokenType::INTEGER_LITERAL) {
                            try {
                                size_t used = 0;
                                int64_t v = std::stoll(c.value, &used);
                                if (used == c.value.size()) return negative ? -v : v;
                            } catch (const std::exception &) {
                            }
                        }
                    }
                    throw NotImplementedException(
                        "GENERATE_SERIES arguments other than BIGINT literals are not supported");
                };
                int64_t start_val = 0, stop_val = 0, step_val = 1;
                if (fa.size() == 1) {
                    stop_val = literal(*fa[0]);
                } else {
                    start_val = literal(*fa[0]);
                    stop_val = literal(*fa[1]);
                }
                if (fa.size() == 3) step_val = literal(*fa[2]);

                std::string tbl_name = sel.from_table->alias.empty()
                    ? ("__generate_series_" + std::to_string(preproc_uid) + "__") : sel.from_table->alias;
                // SQL standard column-alias list: AS t(v1) renames the
                // single output column. Without one, the column name is
                // the function name ("generate_series").
                std::string col_name = sel.from_table->column_aliases.empty()
                    ? "generate_series" : sel.from_table->column_aliases[0];
                if (sel.from_table->column_aliases.size() > 1) {
                    throw BinderException(ErrorCode::TYPE_MISMATCH,
                        "GENERATE_SERIES produces one column but column-alias list has " +
                        std::to_string(sel.from_table->column_aliases.size()));
                }
                sel.from_table->table_name = tbl_name;
                sel.from_table->is_table_function = false;

                if (db_.GetCatalog().GetTable(tbl_name)) db_.GetCatalog().DropTable(tbl_name);
                auto &entry = db_.GetCatalog().CreateTable(tbl_name,
                    {ColumnDefinition(col_name, LogicalType::BIGINT())});
                auto storage = std::make_shared<DataTable>(
                    std::vector<LogicalType>{LogicalType::BIGINT()});
                entry.SetStorage(storage);

                if (step_val == 0) throw InvalidInputException("GENERATE_SERIES step cannot be 0");
                // The row count in unsigned arithmetic, so that no sum or
                // difference of the bounds overflows: (stop - start) / step
                // overflowed, and a negative step was counted as 0 rows and
                // then ran without the limit.
                static constexpr uint64_t MAX_SERIES_ROWS = 10000000; // 10M limit
                uint64_t rows = 0;
                uint64_t magnitude = step_val > 0 ? static_cast<uint64_t>(step_val)
                                                  : static_cast<uint64_t>(-(step_val + 1)) + 1;
                if (step_val > 0 && stop_val >= start_val) {
                    rows = (static_cast<uint64_t>(stop_val) - static_cast<uint64_t>(start_val)) /
                               magnitude + 1;
                } else if (step_val < 0 && stop_val <= start_val) {
                    rows = (static_cast<uint64_t>(start_val) - static_cast<uint64_t>(stop_val)) /
                               magnitude + 1;
                }
                if (rows > MAX_SERIES_ROWS)
                    throw InvalidInputException("GENERATE_SERIES would produce too many rows (limit: 10M)");

                for (uint64_t r = 0; r < rows; r++) {
                    uint64_t offset = r * magnitude;
                    int64_t v = static_cast<int64_t>(
                        step_val > 0 ? static_cast<uint64_t>(start_val) + offset
                                     : static_cast<uint64_t>(start_val) - offset);
                    DataChunk chunk;
                    chunk.Initialize({LogicalType::BIGINT()});
                    chunk.SetValue(0, 0, Value::BIGINT(v));
                    storage->Append(chunk);
                }
                temp_tables.push_back(tbl_name);
            }

            }; // end run_preprocess lambda

            // Pass 1: process the original sel.from_table (the JOIN root).
            run_preprocess();

            // Pass 2+: walk the JOIN right-hand chain. For each parent
            // whose `->right` is a view or a table function, detach it, swap into sel.from_table, rerun the same
            // preprocessing, swap back. We process deepest-first so
            // each detach-swap-reattach cycle is independent.
            std::vector<TableRef*> join_parents;
            for (auto *node = sel.from_table.get();
                 node && node->right; node = node->right.get()) {
                join_parents.push_back(node);
            }
            for (auto it = join_parents.rbegin(); it != join_parents.rend(); ++it) {
                TableRef *parent = *it;
                auto rhs = std::move(parent->right);    // detach
                std::swap(sel.from_table, rhs);         // RHS is now the active from_table
                run_preprocess();
                std::swap(sel.from_table, rhs);         // put root back
                parent->right = std::move(rhs);         // reattach mutated RHS
            }
            } // end for-loop over set_right chain
        }

        // Handle CTEs: materialize each CTE as a temporary table.
        // (cte_tables is declared at the top of this
        //  iteration body and cleaned up by TableCleanupGuard.)

        // Materialise a SelectStatement into a freshly-created catalog
        // table, returning its storage. Used by the subquery-in-FROM and
        // non-recursive CTE paths.
        //
        // Fast path (no set_op chain): drains physical chunks straight to
        // storage. Identical cost to the pre-bugfix CTE code.
        //
        // Slow path (UNION / UNION ALL / INTERSECT / EXCEPT chain): walks
        // the right-leaning chain produced by the parser, binds every
        // segment to discover its result types, computes a per-column
        // common type across all branches (so INT + BIGINT lands in
        // BIGINT instead of overflowing the INT storage), then drains
        // every branch into rows, coerces each Value to the common type,
        // applies set semantics (UNION / INTERSECT / EXCEPT use
        // type-aware Value equality so INT 1 doesn't collide with
        // VARCHAR "1" the way the previous string-key dedup did), and
        // writes the combined rows to storage with the common types.
        // The planner has no logical UNION node — set_op is walked at the
        // Connection layer.
        // Thin wrappers over the file-scope free functions (shared with
        // ExpressionExecutor::ExecuteSubquery so derived-table FROM works
        // inside subqueries too). Behavior is identical to the prior
        // inline lambdas.
        auto materialize_select_into_new_table = [&](
                SelectStatement &sel,
                const std::string &dest_name) -> std::shared_ptr<DataTable> {
            return MaterializeSelectIntoTable(db_.GetCatalog(), sel, dest_name);
        };
        std::function<void(TableRef &)> materialize_from_subqueries =
                [&](TableRef &tref) {
            MaterializeFromSubqueries(db_.GetCatalog(), tref, cte_tables);
        };

        if (stmt->GetType() == StatementType::SELECT) {
            auto &sel0 = static_cast<SelectStatement &>(*stmt);
            // Walk the whole set-op chain so each branch's derived-table
            // FROM is materialised — `A UNION B` where both A and B read
            // from (VALUES..)/(SELECT..). Previously only the left
            // branch's from_table was materialised; set_right branches
            // failed with "Table '' not found".
            for (SelectStatement *cur = &sel0; cur; cur = cur->set_right.get()) {
                if (cur->from_table) materialize_from_subqueries(*cur->from_table);
                for (auto &cte : cur->ctes) {
                    if (cte.query && cte.query->from_table)
                        materialize_from_subqueries(*cte.query->from_table);
                }
            }
        }

        if (stmt->GetType() == StatementType::SELECT) {
            auto &sel = static_cast<SelectStatement &>(*stmt);
            for (auto &cte : sel.ctes) {
                if (cte.recursive && cte.query->set_right) {
                    // Recursive CTE: execute base case, then iterate.
                    // Base case is cte.query (left side of UNION ALL).
                    // Recursive case is cte.query->set_right.

                    // 1. Execute base case.
                    Binder base_binder(db_.GetCatalog());
                    auto base_bound = base_binder.Bind(*cte.query);
                    auto &base_sel = static_cast<BoundSelectStatement &>(*base_bound);

                    std::vector<ColumnDefinition> cte_cols;
                    for (idx_t i = 0; i < base_sel.result_names.size(); i++)
                        cte_cols.emplace_back(base_sel.result_names[i], base_sel.result_types[i]);

                    auto &entry = db_.GetCatalog().CreateTable(cte.name, cte_cols);
                    // Push for cleanup BEFORE the iteration runs. If binding
                    // or planning the recursive case throws, the table still
                    // gets dropped by the TableCleanupGuard on this query's
                    // exit. Without this, a failed recursive CTE leaves the
                    // catalog table in place and the next query that uses
                    // the same CTE name fails with "Table already exists".
                    cte_tables.push_back(cte.name);
                    auto storage = std::make_shared<DataTable>(base_sel.result_types);
                    entry.SetStorage(storage);

                    // SQL's recursion (#38): each step reads only the rows
                    // the step before made, the working table, and the CTE
                    // is every step's rows. Each step read the whole table
                    // so far, so rows multiplied (a count to 3 made 500,501
                    // rows), and a window in the step saw them all. UNION
                    // keeps a row once; a step past the caps is an error,
                    // not the rows so far.
                    if (cte.query->set_op != "UNION ALL" && cte.query->set_op != "UNION") {
                        throw NotImplementedException(
                            "A recursive CTE joins its parts with UNION or UNION ALL");
                    }
                    if (cte.query->set_right->set_right) {
                        throw NotImplementedException(
                            "A recursive CTE of more than two parts is not supported");
                    }
                    const bool rec_union_all = cte.query->set_op == "UNION ALL";
                    const auto &rec_types = base_sel.result_types;
                    auto make_table = [&](const std::vector<std::vector<Value>> &rows) {
                        auto t = std::make_shared<DataTable>(rec_types);
                        for (idx_t start = 0; start < rows.size(); start += VECTOR_SIZE) {
                            idx_t n = std::min<idx_t>(VECTOR_SIZE, rows.size() - start);
                            DataChunk c;
                            c.Initialize(rec_types);
                            for (idx_t r = 0; r < n; r++) {
                                for (idx_t col = 0; col < rec_types.size(); col++) {
                                    c.SetValue(col, r, rows[start + r][col]);
                                }
                            }
                            c.SetCardinality(n);
                            t->Append(c);
                        }
                        return t;
                    };
                    SetOpRowSet rec_seen;
                    std::vector<std::vector<Value>> rec_all, rec_work;
                    auto take = [&](PhysicalOperator &op, const std::vector<LogicalType> &types,
                                    std::vector<std::vector<Value>> &out) {
                        if (types.size() != rec_types.size()) {
                            throw BinderException(
                                "The parts of a recursive CTE return different numbers of columns");
                        }
                        for (idx_t col = 0; col < types.size(); col++) {
                            if (types[col].id() != rec_types[col].id() &&
                                types[col].id() != LogicalTypeId::SQLNULL) {
                                throw NotImplementedException(
                                    "A recursive CTE whose recursive part's types differ from its "
                                    "base part's is not supported");
                            }
                        }
                        DataChunk chunk;
                        while (op.GetData(chunk)) {
                            for (idx_t r = 0; r < chunk.size(); r++) {
                                std::vector<Value> row;
                                row.reserve(types.size());
                                for (idx_t col = 0; col < types.size(); col++) {
                                    auto v = chunk.GetValue(col, r);
                                    row.push_back(v.IsNull() ? Value() : v);
                                }
                                if (!rec_union_all && !rec_seen.insert(row).second) continue;
                                out.push_back(std::move(row));
                            }
                        }
                    };

                    auto base_logical = Planner::Plan(*base_bound);
                    PhysicalPlanner base_pp(db_.GetCatalog());
                    auto base_physical = base_pp.Plan(*base_logical);
                    base_physical->Init();
                    take(*base_physical, base_sel.result_types, rec_work);
                    rec_all = rec_work;

                    static constexpr idx_t MAX_RECURSIVE_ROWS = 10000000;
                    static constexpr int MAX_RECURSIVE_STEPS = 100000;
                    for (int iter = 0; !rec_work.empty(); iter++) {
                        if (iter >= MAX_RECURSIVE_STEPS || rec_all.size() > MAX_RECURSIVE_ROWS) {
                            throw NotImplementedException(
                                "Recursive CTE past " + std::to_string(MAX_RECURSIVE_STEPS) +
                                " steps or " + std::to_string(MAX_RECURSIVE_ROWS) + " rows");
                        }
                        entry.SetStorage(make_table(rec_work));
                        Binder rec_binder(db_.GetCatalog());
                        auto rec_bound = rec_binder.Bind(*cte.query->set_right);
                        auto &rec_sel = static_cast<BoundSelectStatement &>(*rec_bound);
                        auto rec_result_types = rec_sel.result_types;
                        auto rec_logical = Planner::Plan(*rec_bound);
                        PhysicalPlanner rec_pp(db_.GetCatalog());
                        auto rec_physical = rec_pp.Plan(*rec_logical);
                        rec_physical->Init();
                        std::vector<std::vector<Value>> next;
                        take(*rec_physical, rec_result_types, next);
                        rec_work = std::move(next);
                        rec_all.insert(rec_all.end(), rec_work.begin(), rec_work.end());
                    }
                    entry.SetStorage(make_table(rec_all));
                    // (cte_tables.push_back already happened right after
                    //  CreateTable above so a mid-iteration throw still cleans up.)
                } else {
                    // Non-recursive CTE.
                    auto cte_stmt = std::make_unique<SelectStatement>();
                    *cte_stmt = std::move(*cte.query);

                    // Helper drains the full set_op chain when present;
                    // otherwise takes the chunk-direct fast path.
                    materialize_select_into_new_table(*cte_stmt, cte.name);
                    cte_tables.push_back(cte.name);
                }
            }
        }

        // 2. Bind.
        Binder binder(db_.GetCatalog());
        auto bound = binder.Bind(*stmt);

        // Capture per-result-column DISPLAY types now — Planner::Plan below
        // moves the bound select-list out of the statement, so the date/
        // timestamp re-tag (applied to the final result further down) must
        // read the select-list expressions here while they still exist.
        std::vector<LogicalType> result_display_types;
        if (bound->GetType() == BoundStatementType::SELECT) {
            auto &sel = static_cast<BoundSelectStatement &>(*bound);
            result_display_types.reserve(sel.select_list.size());
            for (auto &e : sel.select_list) {
                result_display_types.push_back(
                    e ? ResolveResultDisplayType(*e)
                      : LogicalType(LogicalTypeId::INVALID));
            }
        }

        // 3. Logical plan.
        auto logical = Planner::Plan(*bound);

        // 3b. Optimize logical plan.
        logical = Optimizer::Optimize(std::move(logical));

        // 4. Physical plan.
        PhysicalPlanner physical_planner(db_.GetCatalog());
        auto physical = physical_planner.Plan(*logical);

        // 5. Execute.
        ExpressionExecutor::SetCatalog(&db_.GetCatalog());
        // Kick off column-projection pushdown from the root: every result
        // column is needed, intermediate operators expand to include any
        // column they reference (filter predicates, sort keys, agg args)
        // and forward to their children.
        {
            std::vector<bool> top_mask(physical->GetTypes().size(), true);
            physical->SetNeededOutputs(top_mask);
        }
        physical->Init();

        // Collect result column names/types from bound statement.
        if (bound->GetType() == BoundStatementType::SELECT) {
            auto &sel = static_cast<BoundSelectStatement &>(*bound);
            final_result.column_names = sel.result_names;
            final_result.column_types = sel.result_types;
        }

        // Stream chunks directly into the result, avoiding per-cell Value
        // boxing: the per-cell path goes through QueryResult::GetValue,
        // which boxes lazily, on demand.
        // 10M-row window query: drops 18 s of cell-boxing.
        DataChunk chunk;
        while (true) {
            if (!physical->GetData(chunk)) break;
            if (chunk.size() == 0) continue;
            final_result.chunks.push_back(std::move(chunk));
            chunk = DataChunk{};
        }

        // INSERT / UPDATE / DELETE: return the affected-row count as a
        // single-row BIGINT result, matching DuckDB/DataFusion (their
        // drivers return [(count,)] for DML run as a query). The operator
        // performed the mutation during the GetData drain above.
        int64_t affected = physical->AffectedRows();
        if (affected >= 0 && final_result.chunks.empty() &&
            final_result.rows.empty()) {
            final_result.column_names = {"Count"};
            final_result.column_types = {LogicalType::BIGINT()};
            final_result.rows.push_back({Value::BIGINT(affected)});
            final_result.is_count = true;
        } else if (stmt->GetType() == StatementType::CREATE_TABLE) {
            // DuckDB's result for CREATE TABLE: a Count column and no row
            // (#38).
            final_result.column_names = {"Count"};
            final_result.column_types = {LogicalType::BIGINT()};
            final_result.is_count = true;
        }

        // Re-tag DATE / TIMESTAMP result columns (ResolveResultDisplayType):
        // past every hot path, the matching result columns are re-tagged so QueryResult::GetValue
        // boxes a DATE/TIMESTAMP Value and the output renders an ISO string
        // (matching standard SQL / DuckDB) instead of the raw integer. Only
        // the logical type changes; the INT32/INT64 buffers are untouched
        // and layout-compatible (DATE==INT32, TIMESTAMP==INT64).
        // `result_display_types` was captured right after binding because
        // Planner::Plan moves the bound select-list out of the statement.
        for (idx_t c = 0;
             c < result_display_types.size() &&
             c < final_result.column_types.size();
             c++) {
            auto did = result_display_types[c].id();
            if (did != LogicalTypeId::DATE && did != LogicalTypeId::TIMESTAMP)
                continue;
            // Guard: only re-tag when the produced column's physical layout
            // matches (DATE<-INT32, TIMESTAMP<-INT64). A defensive check — a
            // mismatch would mean the plan emitted an unexpected type, so
            // leave such a column alone.
            auto cur = final_result.column_types[c].id();
            bool ok = (did == LogicalTypeId::DATE &&
                       cur == LogicalTypeId::INTEGER) ||
                      (did == LogicalTypeId::TIMESTAMP &&
                       cur == LogicalTypeId::BIGINT);
            if (!ok) continue;
            final_result.column_types[c] = result_display_types[c];
            for (auto &ch : final_result.chunks) {
                if (c < ch.ColumnCount())
                    ch.GetVector(c).SetType(result_display_types[c]);
            }
        }

        // Handle set operations (UNION / UNION ALL / INTERSECT / EXCEPT).
        //
        // Walks the right-leaning chain produced by the parser
        //   `a UNION b UNION c` → sel(a, set_op=UNION, set_right=sel(b,
        //                            set_op=UNION, set_right=sel(c)))
        // so 3+ branches now combine correctly (previously only the
        // first set_right ran, dropping all deeper branches).
        //
        // Stashes the leaf branch's ORDER BY / LIMIT / OFFSET before the
        // walk because the parser puts them there but SQL standard says
        // they apply to the union as a whole, then re-applies them to
        // the combined rows via ApplyUnionOrderLimit.
        //
        // Uses the same SetOpRowSet dedup as derived-table materialisation
        // (NULL-aware, type-aware — INT 1 no longer collides with
        // VARCHAR '1' the way the old string-key dedup did).
        if (stmt->GetType() == StatementType::SELECT) {
            auto &sel = static_cast<SelectStatement &>(*stmt);
            if (!sel.set_op.empty() && sel.set_right) {
                final_result.MaterialiseRows();
                final_result.chunks.clear();
                final_result.chunk_index_built = false;

                SelectStatement *leaf = sel.set_right.get();
                while (leaf->set_right) leaf = leaf->set_right.get();
                std::vector<OrderByItem> stashed_order_by =
                    std::move(leaf->order_by);
                ParsedExprPtr stashed_limit  = std::move(leaf->limit);
                ParsedExprPtr stashed_offset = std::move(leaf->offset);

                // Pre-bind every RIGHT branch so we know per-column types
                // before any execution and any dedup work. Without this,
                // INTERSECT INT vs DOUBLE returned 0 rows (the two Values
                // hashed differently because their types differed even
                // when values were equal). Mirror of the derived-table
                // path at lines ~2236-2299.
                std::vector<std::unique_ptr<BoundStatement>> right_bounds;
                {
                    SelectStatement *cur = &sel;
                    while (!cur->set_op.empty() && cur->set_right) {
                        Binder rb(db_.GetCatalog());
                        right_bounds.push_back(rb.Bind(*cur->set_right));
                        cur = cur->set_right.get();
                    }
                }

                // Per-column common type: start from LEFT branch's types
                // (already materialised in final_result.column_types) and
                // pairwise reduce against each right branch.
                auto result_types = final_result.column_types;
                for (auto &rb_stmt : right_bounds) {
                    auto &rb = static_cast<BoundSelectStatement &>(*rb_stmt);
                    // SQL standard: equal column counts across branches.
                    // The previous min-cap loop silently fabricated NULL
                    // columns (left wider) or dropped trailing columns
                    // (right wider).
                    if (rb.result_types.size() != result_types.size()) {
                        throw BinderException(ErrorCode::TYPE_MISMATCH,
                            "each UNION/INTERSECT/EXCEPT query must return "
                            "the same number of columns (" +
                            std::to_string(result_types.size()) + " vs " +
                            std::to_string(rb.result_types.size()) + ")");
                    }
                    for (idx_t c = 0; c < result_types.size(); c++) {
                        result_types[c] = ComputeSetOpCommonType(
                            result_types[c], rb.result_types[c]);
                    }
                }

                // Coerce already-materialised LEFT rows to the new common
                // types (only when the type changed).
                std::vector<bool> col_changed(result_types.size(), false);
                for (idx_t c = 0; c < result_types.size() &&
                     c < final_result.column_types.size(); c++) {
                    col_changed[c] = (result_types[c].id() !=
                                      final_result.column_types[c].id());
                }
                for (auto &row : final_result.rows) {
                    for (idx_t c = 0;
                         c < result_types.size() && c < row.size(); c++) {
                        if (col_changed[c]) {
                            row[c] = CoerceValueForSetOp(row[c], result_types[c]);
                        }
                    }
                }

                SelectStatement *cur = &sel;
                size_t branch_idx = 0;
                while (!cur->set_op.empty() && cur->set_right) {
                    auto right_logical = Planner::Plan(*right_bounds[branch_idx++]);
                    PhysicalPlanner right_pp(db_.GetCatalog());
                    auto right_physical = right_pp.Plan(*right_logical);
                    right_physical->Init();

                    std::vector<std::vector<Value>> right_rows;
                    DataChunk right_chunk;
                    while (true) {
                        if (!right_physical->GetData(right_chunk)) break;
                        for (idx_t ri = 0; ri < right_chunk.size(); ri++) {
                            std::vector<Value> row;
                            row.reserve(right_chunk.ColumnCount());
                            for (idx_t c = 0; c < right_chunk.ColumnCount(); c++) {
                                row.push_back(CoerceValueForSetOp(
                                    right_chunk.GetValue(c, ri),
                                    c < result_types.size()
                                        ? result_types[c]
                                        : right_chunk.GetVector(c).GetType()));
                            }
                            right_rows.push_back(std::move(row));
                        }
                    }

                    const std::string &op = cur->set_op;
                    if (op == "UNION ALL") {
                        for (auto &row : right_rows) {
                            final_result.rows.push_back(std::move(row));
                        }
                    } else if (op == "UNION" || op == "INTERSECT" || op == "EXCEPT") {
                        ApplyDistinctSetOp(op, final_result.rows, right_rows);
                    }
                    cur = cur->set_right.get();
                }

                // Update the result schema to the unified types. Names
                // stay as the LEFT branch's per SQL standard.
                final_result.column_types = result_types;

                if (!stashed_order_by.empty() || stashed_limit ||
                    stashed_offset) {
                    ApplyUnionOrderLimit(final_result.rows,
                                         stashed_order_by,
                                         stashed_limit, stashed_offset,
                                         final_result.column_names);
                }
            }
        }

        // temp_tables / cte_tables are dropped by the
        // TableCleanupGuard declared at the top of this loop iteration -
        // runs on both success and exception paths.
    }

    return final_result;
}

// ============================================================================
// Programmatic API (unchanged from before)
// ============================================================================

void Connection::CreateTable(const std::string &name,
                              std::vector<ColumnDefinition> columns) {
    auto types = std::vector<LogicalType>();
    types.reserve(columns.size());
    for (auto &col : columns) {
        types.push_back(col.type);
    }

    auto &entry = db_.GetCatalog().CreateTable(name, std::move(columns));
    entry.SetStorage(std::make_shared<DataTable>(types));
}

void Connection::DropTable(const std::string &name) {
    if (!db_.GetCatalog().DropTable(name)) {
        throw CatalogException("Table '" + name + "' does not exist");
    }
}

void Connection::Append(const std::string &table_name, DataChunk &chunk) {
    auto *entry = db_.GetCatalog().GetTable(table_name);
    if (!entry) {
        throw CatalogException("Table '" + table_name + "' not found");
    }
    entry->GetStorage().Append(chunk);
}

void Connection::Scan(const std::string &table_name,
                       std::function<void(DataChunk &)> callback) {
    auto *entry = db_.GetCatalog().GetTable(table_name);
    if (!entry) {
        throw CatalogException("Table '" + table_name + "' not found");
    }

    auto &table = entry->GetStorage();
    auto state = table.InitScan();
    DataChunk result;
    result.Initialize(table.GetTypes());

    while (table.Scan(state, result)) {
        callback(result);
    }
}

// ============================================================================
// Transaction control
// ============================================================================

void Connection::BeginTransaction() {
    if (active_txn_) {
        throw TransactionException("Transaction already active");
    }
    active_txn_ = &db_.GetTransactionManager().BeginTransaction();
}

void Connection::CommitTransaction() {
    if (!active_txn_) {
        throw TransactionException("No active transaction to commit");
    }
    db_.GetTransactionManager().CommitTransaction(*active_txn_);
    active_txn_ = nullptr;
}

void Connection::RollbackTransaction() {
    if (!active_txn_) {
        throw TransactionException("No active transaction to rollback");
    }
    db_.GetTransactionManager().RollbackTransaction(*active_txn_);
    active_txn_ = nullptr;
    // zugodb: the engine has no undo, so a ROLLBACK that reported
    // success would hide that every statement since BEGIN is kept. The
    // transaction is closed, and the caller told.
    throw NotImplementedException(
        "ROLLBACK does not undo changes in zugodb: every statement since "
        "BEGIN is kept, and the transaction is closed");
}

// ============================================================================
// QueryResult
// ============================================================================

std::string QueryResult::ToString() const {
    std::ostringstream ss;
    for (idx_t i = 0; i < ColumnCount(); i++) {
        if (i > 0) ss << "\t";
        ss << column_names[i];
    }
    ss << "\n";
    // If data lives in chunks (SELECT fast path), iterate them directly
    // to avoid the row materialisation cost. Both code paths produce the
    // same per-cell ToString output.
    if (rows.empty() && !chunks.empty()) {
        idx_t ncols = ColumnCount();
        for (auto &c : chunks) {
            idx_t n = c.size();
            for (idx_t r = 0; r < n; r++) {
                for (idx_t k = 0; k < ncols; k++) {
                    if (k > 0) ss << "\t";
                    ss << c.GetValue(k, r).ToString();
                }
                ss << "\n";
            }
        }
    } else {
        for (auto &row : rows) {
            for (idx_t i = 0; i < row.size(); i++) {
                if (i > 0) ss << "\t";
                ss << row[i].ToString();
            }
            ss << "\n";
        }
    }
    return ss.str();
}

} // namespace zugodb
