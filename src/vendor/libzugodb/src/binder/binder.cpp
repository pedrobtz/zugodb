#include "zugodb/binder/binder.hpp"
#include "zugodb/common/exception.hpp"
#include "zugodb/common/string_util.hpp"
#include "zugodb/execution/expression_executor.hpp"
#include "zugodb/main/query_materialize.hpp"
#include "zugodb/parser/expression/parsed_expression.hpp"
#include <functional>

namespace zugodb {

// BEGIN FUNCTION REGISTRY (tools/check_dialect.sh reads the names between
// these markers; docs/dialect.md lists the same ones).
// Each scalar function with DuckDB's argument counts (its signatures at
// the pinned tag, where the engine computes them): a call with too few
// read past the executor's arguments (date_part('year') overflowed a heap
// buffer, Stage 9's SQL fuzzer), and too many were ignored.
struct FunctionArity { const char *name; size_t lo, hi; };
static const FunctionArity kScalarFunctions[] = {
    // Numbers.
    {"ABS", 1, 1}, {"ACOS", 1, 1}, {"ACOSH", 1, 1}, {"ASIN", 1, 1}, {"ASINH", 1, 1},
    {"ATAN", 1, 1}, {"ATAN2", 2, 2}, {"ATANH", 1, 1}, {"CBRT", 1, 1}, {"CEIL", 1, 1},
    {"CEILING", 1, 1}, {"COS", 1, 1}, {"COSH", 1, 1}, {"DEGREES", 1, 1}, {"EXP", 1, 1},
    {"FACTORIAL", 1, 1}, {"FLOOR", 1, 1}, {"GCD", 2, 2}, {"ISFINITE", 1, 1},
    {"ISINF", 1, 1}, {"ISNAN", 1, 1}, {"LCM", 2, 2}, {"LN", 1, 1}, {"LOG", 1, 2},
    {"LOG10", 1, 1}, {"LOG2", 1, 1}, {"MOD", 2, 2}, {"PI", 0, 0}, {"POW", 2, 2},
    {"POWER", 2, 2},
    {"RADIANS", 1, 1}, {"ROUND", 1, 2}, {"SIGN", 1, 1}, {"SIN", 1, 1}, {"SINH", 1, 1},
    {"SQRT", 1, 1}, {"TAN", 1, 1}, {"TANH", 1, 1}, {"TRUNC", 1, 2},
    // Text and bytes.
    {"ASCII", 1, 1}, {"BIT_LENGTH", 1, 1}, {"CHR", 1, 1}, {"CONCAT", 1, SIZE_MAX},
    {"CONCAT_WS", 2, SIZE_MAX}, {"CONTAINS", 2, 2}, {"ENDS_WITH", 2, 2},
    {"FROM_BASE64", 1, 1}, {"HEX", 1, 1}, {"INSTR", 2, 2}, {"LEFT", 2, 2}, {"LENGTH", 1, 1},
    {"LOWER", 1, 1}, {"LPAD", 3, 3}, {"LTRIM", 1, 2}, {"MD5", 1, 1},
    {"OCTET_LENGTH", 1, 1}, {"POSITION", 2, 2}, {"PREFIX", 2, 2}, {"REPEAT", 2, 2},
    {"REPLACE", 3, 3}, {"REVERSE", 1, 1}, {"RIGHT", 2, 2}, {"RPAD", 3, 3},
    {"RTRIM", 1, 2}, {"SHA1", 1, 1}, {"SHA256", 1, 1}, {"SPLIT_PART", 3, 3},
    {"STARTS_WITH", 2, 2}, {"STRLEN", 1, 1}, {"STRPOS", 2, 2}, {"SUBSTR", 2, 3},
    {"SUBSTRING", 2, 3}, {"SUFFIX", 2, 2}, {"TO_BASE64", 1, 1}, {"TO_HEX", 1, 1},
    {"TRANSLATE", 3, 3}, {"TRIM", 1, 2}, {"UNHEX", 1, 1}, {"UPPER", 1, 1},
    // Dates and times.
    {"CENTURY", 1, 1}, {"DATE_DIFF", 3, 3}, {"DATEDIFF", 3, 3}, {"DATE_PART", 2, 2},
    {"DATE_TRUNC", 2, 2}, {"DAY", 1, 1}, {"DAYNAME", 1, 1},
    {"DAYOFMONTH", 1, 1}, {"DAYOFWEEK", 1, 1}, {"DAYOFYEAR", 1, 1}, {"DECADE", 1, 1},
    {"EPOCH_MS", 1, 1}, {"HOUR", 1, 1}, {"ISODOW", 1, 1},
    {"ISOYEAR", 1, 1}, {"LAST_DAY", 1, 1}, {"MAKE_DATE", 3, 3}, {"MAKE_TIMESTAMP", 6, 6},
    {"MILLENNIUM", 1, 1}, {"MINUTE", 1, 1}, {"MONTH", 1, 1},
    {"MONTHNAME", 1, 1}, {"QUARTER", 1, 1}, {"SECOND", 1, 1}, {"STRFTIME", 2, 2},
    {"WEEK", 1, 1}, {"YEAR", 1, 1},
    // Conditions and NULLs.
    {"COALESCE", 1, SIZE_MAX}, {"GREATEST", 1, SIZE_MAX}, {"IF", 3, 3}, {"IFNULL", 2, 2},
    {"LEAST", 1, SIZE_MAX}, {"NULLIF", 2, 2},
    // The session.
    {"CURRENT_SCHEMA", 0, 0},
};
static const char *const kAggregateFunctions[] = {
    "AVG", "BOOL_AND", "BOOL_OR", "COUNT", "GROUP_CONCAT", "LISTAGG", "MAX", "MEDIAN", "MIN",
    "STDDEV", "STDDEV_POP", "STDDEV_SAMP", "STRING_AGG", "SUM", "VAR_POP", "VAR_SAMP",
    "VARIANCE",
};
static const char *const kWindowFunctions[] = {
    "DENSE_RANK", "FIRST_VALUE", "LAG", "LAST_VALUE", "LEAD", "NTILE", "RANK",
    "ROW_NUMBER",
};
// END FUNCTION REGISTRY

// Names the parser makes for syntax (CASE, x BETWEEN a AND b, x IN (...),
// EXTRACT(part FROM x)): not functions a query calls by name.
static const char *const kSyntaxFunctions[] = {"CASE", "BETWEEN", "IN", "EXTRACT", "EXISTS"};

static bool IsRegisteredFunction(const std::string &name) {
    for (auto &f : kScalarFunctions) if (name == f.name) return true;
    for (auto *f : kAggregateFunctions) if (name == f) return true;
    for (auto *f : kWindowFunctions) if (name == f) return true;
    for (auto *f : kSyntaxFunctions) if (name == f) return true;
    return false;
}

// The error for a name outside the registry: why it is refused, where the
// engine knows (design L4, L23, L25, L26).
[[noreturn]] static void RefuseFunction(const std::string &name) {
    static const char *const kNotDuckDB[] = {
        "NVL", "IIF", "GETDATE", "CURDATE", "CURTIME", "FROM_UNIXTIME", "UNIX_TIMESTAMP",
        "DATE_FORMAT", "FORMAT_TIMESTAMP", "DATEADD", "TIMESTAMPADD", "TIMESTAMPDIFF",
        "SUBDATE", "ADDDATE", "BASE64_ENCODE", "BASE64_DECODE", "INITCAP", "SHA512",
        "SHA2_256", "SHA2_512", "SHA_1", "RAND", "TRUNCATE", "STR_TO_DATE"};
    for (auto *f : kNotDuckDB) {
        if (name == f) {
            throw BinderException(ErrorCode::TYPE_MISMATCH,
                "Scalar Function with name " + StringUtil::Lower(name) +
                " does not exist: DuckDB has no such function (design L4)");
        }
    }
    static const char *const kProvided[] = {"RANDOM", "SETSEED", "UUID", "GEN_RANDOM_UUID",
                                            "UUIDV4", "UUIDV7"};
    for (auto *f : kProvided) {
        if (name == f) {
            throw NotImplementedException(
                "Function " + StringUtil::Lower(name) +
                " is not supported: random numbers and UUIDs come from a host-supplied "
                "provider (libzugodb#40, design L25), which this engine does not have");
        }
    }
    throw NotImplementedException("Function " + StringUtil::Lower(name) + " is not supported");
}

// A condition is BOOLEAN: AND, OR and NOT's operands, WHERE, HAVING,
// QUALIFY, ON and CASE WHEN. Another type is cast to BOOLEAN, DuckDB's
// implicit cast (10 is true, 'abc' an error). The bytes of an INTEGER
// were read as a bool: NOT 10 was true, CASE WHEN 2 took the ELSE, and
// 1 WHERE 'abc' gave no row where DuckDB raises (Stage 9's SQL fuzzer).
static void AsCondition(BoundExprPtr &e) {
    if (!e) return;
    auto t = e->GetReturnType().id();
    if (t == LogicalTypeId::BOOLEAN || t == LogicalTypeId::SQLNULL) return;
    e = std::make_unique<BoundCast>(std::move(e), LogicalType::BOOLEAN());
}


static bool ContainsAggregate(const BoundExpression &expr);
// Returns true if `expr` contains a column reference that is neither
// listed in `group_by` (by column_index) nor enclosed in an aggregate
// function. Used to reject ungrouped non-aggregated columns in SELECT
// and HAVING of an aggregated query.
static bool HasUngroupedColumn(const BoundExpression &expr,
                               const std::vector<BoundExprPtr> &group_by);

// ============================================================================
// BindContext
// ============================================================================

// DuckDB casts to a BLOB only from text (reading its \\xHH escapes) and
// from a BLOB to text only; any other cast with a BLOB is an error there,
// under TRY_CAST too (#31).
static void CheckBlobCast(const LogicalType &from, const LogicalType &to) {
    auto f = from.id(), t = to.id();
    if (f == t || f == LogicalTypeId::SQLNULL) return;
    if (t == LogicalTypeId::BLOB && f != LogicalTypeId::VARCHAR) {
        throw BinderException(ErrorCode::TYPE_MISMATCH,
            "Unimplemented type for cast (" + from.ToString() + " -> BLOB)");
    }
    if (f == LogicalTypeId::BLOB && t != LogicalTypeId::VARCHAR) {
        throw BinderException(ErrorCode::TYPE_MISMATCH,
            "Unimplemented type for cast (BLOB -> " + to.ToString() + ")");
    }
}

// Beside a BLOB, a string literal is read as a BLOB, as DuckDB casts a
// literal to the other operand's type; any other type is refused, since
// DuckDB has no implicit cast from it to a BLOB (#31).
static void CoerceBesideBlob(BoundExprPtr &e, const char *what) {
    auto id = e->GetReturnType().id();
    if (id == LogicalTypeId::BLOB || id == LogicalTypeId::SQLNULL) return;
    if (id == LogicalTypeId::VARCHAR && e->GetExpressionType() == BoundExpressionType::CONSTANT) {
        e = std::make_unique<BoundCast>(std::move(e), LogicalType::BLOB());
        return;
    }
    throw BinderException(ErrorCode::TYPE_MISMATCH,
        std::string(what) + " of a BLOB and a " + e->GetReturnType().ToString() +
        " is not supported");
}

void BindContext::AddTable(const std::string &alias, TableCatalogEntry *entry) {
    // Two tables of one alias, in any case, are DuckDB's error (FROM t, t;
    // FROM t AS x, t AS X); the second replaced the first's columns.
    if (tables.count(alias)) {
        throw BinderException(ErrorCode::AMBIGUOUS_COLUMN,
            "Ambiguous reference to table \"" + alias + "\" (duplicate alias)");
    }
    tables[alias] = entry;
    // Compute offset: sum of all columns from previously added tables.
    idx_t offset = 0;
    for (auto &[tbl_alias, tbl_offset] : table_order) {
        offset = tbl_offset + tables[tbl_alias]->ColumnCount();
    }
    table_order.push_back({alias, offset});

    for (idx_t i = 0; i < entry->ColumnCount(); i++) {
        auto &col = entry->GetColumns()[i];
        idx_t combined_idx = offset + i;
        if (columns.count(col.name) && columns[col.name].first != alias) {
            // Mark as ambiguous.
            columns[col.name] = {"", INVALID_INDEX};
        } else {
            columns[col.name] = {alias, combined_idx};
        }
    }
}

std::pair<std::string, idx_t> BindContext::ResolveColumn(
    const std::string &col_name, const std::string &table_name) const {

    if (!table_name.empty()) {
        auto it = tables.find(table_name);
        if (it == tables.end()) {
            throw BinderException(ErrorCode::TABLE_NOT_FOUND,
                                   "Table '" + table_name + "' not found");
        }
        auto idx = it->second->GetColumnIndex(col_name);
        if (idx == INVALID_INDEX) {
            throw BinderException(ErrorCode::COLUMN_NOT_FOUND,
                                   "Column '" + col_name + "' not found in table '" + table_name + "'");
        }
        // Add table offset for JOIN context.
        idx += GetTableOffset(table_name);
        return {table_name, idx};
    }

    auto it = columns.find(col_name);
    if (it == columns.end()) {
        throw BinderException(ErrorCode::COLUMN_NOT_FOUND,
                               "Column '" + col_name + "' not found");
    }
    if (it->second.second == INVALID_INDEX) {
        throw BinderException(ErrorCode::AMBIGUOUS_COLUMN,
                               "Column '" + col_name + "' is ambiguous");
    }
    return it->second;
}

LogicalType BindContext::GetColumnType(const std::string &col_name,
                                        const std::string &table_name) const {
    auto [tbl, combined_idx] = ResolveColumn(col_name, table_name);
    // Get the local index within the table.
    idx_t offset = GetTableOffset(tbl);
    idx_t local_idx = combined_idx - offset;
    return tables.at(tbl)->GetColumns()[local_idx].type;
}

idx_t BindContext::GetTableOffset(const std::string &alias) const {
    for (auto &[tbl_alias, offset] : table_order) {
        if (IdentifierEquals(tbl_alias, alias)) return offset;
    }
    return 0;
}

// ============================================================================
// Binder
// ============================================================================

Binder::Binder(Catalog &catalog) : catalog_(catalog) {}

BoundStmtPtr Binder::Bind(const ParsedStatement &stmt) {
    switch (stmt.GetType()) {
    case StatementType::SELECT:
        return BindSelect(static_cast<const SelectStatement &>(stmt));
    case StatementType::CREATE_TABLE:
        return BindCreateTable(static_cast<const CreateTableStatement &>(stmt));
    case StatementType::DROP_TABLE:
        return BindDropTable(static_cast<const DropTableStatement &>(stmt));
    case StatementType::INSERT:
        return BindInsert(static_cast<const InsertStatement &>(stmt));
    case StatementType::UPDATE:
        return BindUpdate(static_cast<const UpdateStatement &>(stmt));
    case StatementType::DELETE_STMT:
        return BindDelete(static_cast<const DeleteStatement &>(stmt));
    default:
        throw InternalException("Unknown statement type in binder");
    }
}

// ============================================================================
// Bind SELECT
// ============================================================================

BoundStmtPtr Binder::BindSelect(const SelectStatement &stmt) {
    auto result = std::make_unique<BoundSelectStatement>();
    result->is_distinct = stmt.is_distinct;
    BindContext context;

    // Bind FROM.
    if (stmt.from_table) {
        BindTableRef(*stmt.from_table, context);
        // Set primary table.
        if (!context.tables.empty()) {
            // Use the left-most table name.
            std::string primary_name = stmt.from_table->table_name;
            std::string primary_alias = stmt.from_table->alias.empty()
                ? primary_name : stmt.from_table->alias;
            auto it = context.tables.find(primary_alias);
            if (it != context.tables.end()) {
                result->table = it->second;
                result->table_alias = it->first;
            } else {
                auto first = context.tables.begin();
                result->table = first->second;
                result->table_alias = first->first;
            }

            // Detect JOIN chain. SQL-92 comma-FROM (`FROM a, b, c`) plus
            // explicit JOINs both walk the right-leaning chain on the
            // parsed TableRef. Each chain link contributes one JoinInfo;
            // the planner folds them into a left-deep cascade. The
            // running column-count tracks the cumulative width of the
            // left side so each JoinInfo's left_col_count is meaningful
            // for downstream binder/planner code that uses it.
            idx_t running_left_count = result->table->ColumnCount();
            const TableRef *cur = stmt.from_table.get();
            while (cur->right && !cur->join_type.empty()) {
                auto join_info = std::make_unique<BoundSelectStatement::JoinInfo>();
                auto right_alias = cur->right->alias.empty()
                    ? cur->right->table_name : cur->right->alias;
                auto right_it = context.tables.find(right_alias);
                if (right_it != context.tables.end()) {
                    join_info->right_table = right_it->second;
                    join_info->right_alias = right_alias;
                }
                join_info->join_type = cur->join_type;
                join_info->left_col_count = running_left_count;
                join_info->right_col_count = join_info->right_table
                    ? join_info->right_table->ColumnCount() : 0;

                if (cur->on_condition) {
                    join_info->condition = BindExpression(*cur->on_condition, context);
                    AsCondition(join_info->condition);
                }

                running_left_count += join_info->right_col_count;
                result->joins.push_back(std::move(join_info));
                cur = cur->right.get();
            }
        }
    }

    // Bind select list - expand stars.
    for (auto &expr : stmt.select_list) {
        if (expr->GetExpressionType() == ExpressionType::STAR) {
            auto &star = static_cast<const StarExpression &>(*expr);
            std::vector<BoundExprPtr> expanded;
            BindStar(star, context, expanded);
            for (auto &e : expanded) {
                result->result_names.push_back(e->alias);
                result->result_types.push_back(e->GetReturnType());
                result->select_list.push_back(std::move(e));
            }
        } else {
            auto bound = BindExpression(*expr, context);
            std::string name = expr->alias.empty()
                ? (expr->GetExpressionType() == ExpressionType::COLUMN_REF
                    ? (bound->GetExpressionType() == BoundExpressionType::COLUMN_REF
                           ? static_cast<const BoundColumnRef &>(*bound).column_name
                           : static_cast<const ColumnRefExpression &>(*expr).column_name)
                    : "expr")
                : expr->alias;
            bound->alias = name;
            result->result_names.push_back(name);
            result->result_types.push_back(bound->GetReturnType());

            // Track aggregates and window functions.
            if (bound->GetExpressionType() == BoundExpressionType::FUNCTION) {
                auto *fn = static_cast<BoundFunction *>(bound.get());
                if (fn->is_aggregate) {
                    result->aggregates.push_back(fn);
                    result->has_aggregation = true;
                }
            }
            if (bound->GetExpressionType() == BoundExpressionType::WINDOW) {
                result->has_window = true;
            }

            result->select_list.push_back(std::move(bound));
        }
    }

    // Bind WHERE. Reject aggregates in WHERE per SQL standard — they
    // were silently routed to the executor before, surfacing as
    // "NotImplemented Error: Function execution for: COUNT" which
    // misleadingly suggested COUNT itself was missing. Clean
    // BinderException is what SQL standard requires.
    if (stmt.where_clause) {
        result->where_clause = BindExpression(*stmt.where_clause, context);
        AsCondition(result->where_clause);
        if (ContainsAggregate(*result->where_clause)) {
            throw BinderException(ErrorCode::AGGREGATE_IN_WHERE,
                "aggregate functions are not allowed in WHERE clause");
        }
    }

    // Bind GROUP BY. Resolve positional ordinals (`GROUP BY 1`) and
    // select-list aliases (e.g. `extract(minute FROM EventTime) AS m ...
    // GROUP BY m`) to their underlying SELECT-list expression. Some
    // queries need the ordinal path; others need the alias path.
    for (auto &expr : stmt.group_by) {
        bool resolved = false;
        // Positional ordinal: GROUP BY <integer-literal>
        if (expr->GetExpressionType() == ExpressionType::CONSTANT) {
            auto bound = BindExpression(*expr, context);
            if (bound->GetExpressionType() == BoundExpressionType::CONSTANT) {
                auto &val = static_cast<BoundConstant &>(*bound).value;
                int64_t ord = -1;
                if (val.type().id() == LogicalTypeId::INTEGER) {
                    ord = val.GetValue<int32_t>();
                } else if (val.type().id() == LogicalTypeId::BIGINT) {
                    ord = val.GetValue<int64_t>();
                }
                // Reference the post-expansion select_list so that
                // `SELECT * FROM t GROUP BY 1, 2` resolves to actual
                // expanded columns rather than the STAR placeholder.
                // Previously this used stmt.select_list.size() (pre-
                // expansion = 1 for `*`) and silently fell through
                // when ord matched a STAR, leaving trailing GROUP BY
                // columns dangling with corrupt aggregate output.
                if (ord >= 1 && ord <= static_cast<int64_t>(result->select_list.size())) {
                    idx_t sl_idx = static_cast<idx_t>(ord - 1);
                    // result->select_list[sl_idx] is already a bound
                    // expression (column ref / arithmetic / etc) —
                    // reuse it directly by re-binding the original
                    // parsed expression when available, or wrap a
                    // bound copy via the result_names index for
                    // star-expanded columns. The simplest correct
                    // path: peek at the source. If the ordinal points
                    // beyond stmt.select_list (i.e., into a star-
                    // expanded slot), look up the column by name from
                    // the bind context using result_names[sl_idx];
                    // otherwise re-bind from stmt.select_list when
                    // it's a non-STAR scalar.
                    if (sl_idx < stmt.select_list.size() &&
                        stmt.select_list[sl_idx]->GetExpressionType()
                            != ExpressionType::STAR) {
                        auto gk = BindExpression(*stmt.select_list[sl_idx], context);
                        // GROUP BY <ordinal> pointing at an aggregate
                        // expression is illegal SQL. Previously this
                        // pushed the bound aggregate as a group key and
                        // the executor later crashed with a misleading
                        // "Function execution for: COUNT". Reject with a
                        // clean diagnostic instead.
                        if (ContainsAggregate(*gk)) {
                            throw BinderException(ErrorCode::TYPE_MISMATCH,
                                "aggregate functions are not allowed in GROUP BY");
                        }
                        result->group_by.push_back(std::move(gk));
                        resolved = true;
                    } else if (sl_idx < result->result_names.size()) {
                        // Star-expanded column: re-resolve by name.
                        auto col = std::make_unique<ColumnRefExpression>(
                            result->result_names[sl_idx]);
                        try {
                            result->group_by.push_back(BindExpression(*col, context));
                            resolved = true;
                        } catch (...) {
                            // Fall through to alias-name path.
                        }
                    }
                }
            }
        }
        // Alias: GROUP BY <result-name>. A name that is also a column of
        // the FROM clause is the column, as in DuckDB (having_alias.test:
        // `SELECT a+1 AS a FROM t1 GROUP BY a` groups by column a).
        if (!resolved && expr->GetExpressionType() == ExpressionType::COLUMN_REF) {
            auto &col_ref = static_cast<ColumnRefExpression &>(*expr);
            if (col_ref.table_name.empty() && !context.columns.count(col_ref.column_name)) {
                auto ref_upper = StringUtil::Upper(col_ref.column_name);
                for (idx_t i = 0; i < result->result_names.size(); i++) {
                    if (StringUtil::Upper(result->result_names[i]) == ref_upper) {
                        idx_t sl_idx = std::min<idx_t>(i, stmt.select_list.size() - 1);
                        if (stmt.select_list[sl_idx]->GetExpressionType()
                                == ExpressionType::STAR) {
                            break; // fall through to normal binding
                        }
                        auto gk = BindExpression(*stmt.select_list[sl_idx], context);
                        // GROUP BY <alias> resolving to an aggregate is
                        // illegal SQL — reject cleanly (see ordinal path).
                        if (ContainsAggregate(*gk)) {
                            throw BinderException(ErrorCode::TYPE_MISMATCH,
                                "aggregate functions are not allowed in GROUP BY");
                        }
                        result->group_by.push_back(std::move(gk));
                        resolved = true;
                        break;
                    }
                }
            }
        }
        if (!resolved) {
            auto gk = BindExpression(*expr, context);
            // Direct `GROUP BY COUNT(*)` / `GROUP BY SUM(x)` is illegal.
            if (ContainsAggregate(*gk)) {
                throw BinderException(ErrorCode::TYPE_MISMATCH,
                    "aggregate functions are not allowed in GROUP BY");
            }
            result->group_by.push_back(std::move(gk));
        }
        result->has_aggregation = true;
    }

    // GROUP BY ALL: derive group keys from non-aggregate, non-window
    // entries in the SELECT list. Top-level aggregate-function detection
    // only — nested aggregates like `sum(x) + 1` are not yet handled.
    if (stmt.group_by_all && result->group_by.empty()) {
        for (size_t i = 0; i < stmt.select_list.size() && i < result->select_list.size(); i++) {
            const auto &bound = result->select_list[i];
            bool is_aggregated = false;
            if (bound->GetExpressionType() == BoundExpressionType::FUNCTION) {
                auto *fn = static_cast<const BoundFunction *>(bound.get());
                if (fn->is_aggregate) is_aggregated = true;
            }
            if (bound->GetExpressionType() == BoundExpressionType::WINDOW) {
                is_aggregated = true;
            }
            if (is_aggregated) continue;
            result->group_by.push_back(BindExpression(*stmt.select_list[i], context));
        }
        if (!result->group_by.empty()) result->has_aggregation = true;
    }

    // Bind HAVING. Populate the alias map so column refs inside HAVING
    // (e.g. `HAVING c > 100000` where c is an alias for COUNT(*)) re-bind
    // to the matching select-list expression. The map is cleared after
    // binding so other clauses get default column-resolution.
    if (stmt.having_clause) {
        context.select_list_aliases.clear();
        for (idx_t i = 0; i < result->result_names.size() &&
                          i < stmt.select_list.size(); i++) {
            // Skip STAR entries: re-binding a STAR throws. With multiple
            // expanded names mapping to a single STAR, we'd also pick the
            // wrong one. Plain column refs match the source schema directly,
            // so we don't need an alias entry for them either.
            if (stmt.select_list[i]->GetExpressionType() == ExpressionType::STAR) continue;
            if (stmt.select_list[i]->GetExpressionType() == ExpressionType::COLUMN_REF) continue;
            auto key = StringUtil::Upper(result->result_names[i]);
            // A name that is both a grouped column and an alias binds to
            // the group, as in DuckDB (having_alias.test): `SELECT a+1 AS
            // a ... GROUP BY a HAVING a=42` compares the column.
            bool is_group_column = false;
            for (auto &g : result->group_by) {
                if (g && g->GetExpressionType() == BoundExpressionType::COLUMN_REF &&
                    StringUtil::Upper(static_cast<const BoundColumnRef &>(*g).column_name) == key) {
                    is_group_column = true;
                    break;
                }
            }
            if (is_group_column) continue;
            // First-wins: a later select-list entry with the same alias
            // would shadow the first; SQL doesn't define HAVING in that
            // case so we just keep the first.
            if (!context.select_list_aliases.count(key))
                context.select_list_aliases[key] = stmt.select_list[i].get();
        }
        result->having_clause = BindExpression(*stmt.having_clause, context);
        AsCondition(result->having_clause);
        context.select_list_aliases.clear();
        // HAVING implies aggregation even when there's no GROUP BY and
        // no aggregate in the SELECT list:
        //   SELECT 1 FROM t HAVING COUNT(*) > 100
        // Without this, the planner's HAVING-emit block at
        // planner.cpp:583-609 is gated on has_aggregation and silently
        // drops the predicate, returning every base-table row instead
        // of the SQL-standard implicit single-group result.
        result->has_aggregation = true;
        // Reject ungrouped non-aggregated column references in HAVING.
        // Previously `HAVING b > 5` (b not in GROUP BY, not in agg)
        // silently returned 0 rows — wrong-result, undetectable from
        // output.
        if (HasUngroupedColumn(*result->having_clause, result->group_by)) {
            throw BinderException(ErrorCode::TYPE_MISMATCH,
                "column reference in HAVING must appear in GROUP BY "
                "or be used in an aggregate function");
        }
    }

    // Reject ungrouped non-aggregated columns in the SELECT list of an
    // aggregated query. `SELECT a, SUM(b) FROM t` (a not in GROUP BY)
    // silently overwrote `a` with the aggregate value — pure garbage,
    // undetectable from output. SQL standard requires every SELECT-list
    // column reference to be in GROUP BY or inside an aggregate when the
    // query is aggregated. Window-only queries (has_window without
    // has_aggregation) are a separate path and skipped.
    if (result->has_aggregation && !result->has_window) {
        for (auto &sel : result->select_list) {
            if (sel && HasUngroupedColumn(*sel, result->group_by)) {
                throw BinderException(ErrorCode::TYPE_MISMATCH,
                    "column in SELECT list must appear in GROUP BY "
                    "or be used in an aggregate function");
            }
        }
    }

    // Bind QUALIFY.
    if (stmt.qualify_clause) {
        result->qualify_clause = BindExpression(*stmt.qualify_clause, context);
        AsCondition(result->qualify_clause);
        result->has_window = true;
    }

    // Bind ORDER BY - resolve positional ordinals + select-list aliases.
    for (auto &item : stmt.order_by) {
        BoundOrderBy bound_item;
        bool resolved = false;
        // Positional ordinal: ORDER BY <integer-literal> binds to
        // select_list[N-1] (1-based), mirroring the GROUP BY ordinal
        // path above. Required so `ORDER BY 2 DESC`
        // sorts by COUNT(*), not by the constant 2. Out-of-range
        // ordinals raise BinderException.
        if (item.expression->GetExpressionType() == ExpressionType::CONSTANT) {
            auto bound_const = BindExpression(*item.expression, context);
            if (bound_const->GetExpressionType() == BoundExpressionType::CONSTANT) {
                auto &val = static_cast<BoundConstant &>(*bound_const).value;
                int64_t ord = -1;
                bool is_int_ordinal = false;
                if (val.type().id() == LogicalTypeId::INTEGER) {
                    ord = val.GetValue<int32_t>();
                    is_int_ordinal = true;
                } else if (val.type().id() == LogicalTypeId::BIGINT) {
                    ord = val.GetValue<int64_t>();
                    is_int_ordinal = true;
                }
                if (!is_int_ordinal) {
                    // DuckDB's error: a constant sorts nothing (#35).
                    throw BinderException(ErrorCode::TYPE_MISMATCH,
                        "ORDER BY non-integer literal has no effect");
                }
                if (is_int_ordinal) {
                    // Reference the post-expansion select_list so
                    // `SELECT * FROM t ORDER BY 2` works (previously
                    // raised "out of range" because stmt.select_list
                    // size 1 (STAR) was used instead of the expanded
                    // result_names size N).
                    if (ord >= 1 &&
                        ord <= static_cast<int64_t>(result->select_list.size())) {
                        idx_t sl_idx = static_cast<idx_t>(ord - 1);
                        // With a * in the list, positions count its
                        // columns, so stmt.select_list does not line up.
                        bool has_star = false;
                        for (auto &e : stmt.select_list) {
                            has_star = has_star || e->GetExpressionType() == ExpressionType::STAR;
                        }
                        if (!has_star && sl_idx < stmt.select_list.size()) {
                            bound_item.expression = BindExpression(
                                *stmt.select_list[sl_idx], context);
                            bound_item.select_index = sl_idx;
                            resolved = true;
                        } else if (sl_idx < result->result_names.size() &&
                                   result->select_list[sl_idx]->GetExpressionType() ==
                                       BoundExpressionType::COLUMN_REF) {
                            // Star-expanded column: the column itself. By
                            // its name it was the other table's column of
                            // that name after a join (SELECT * FROM a JOIN
                            // b ... ORDER BY 1 sorted by b.i, #38).
                            auto &cr = static_cast<const BoundColumnRef &>(*result->select_list[sl_idx]);
                            bound_item.expression = std::make_unique<BoundColumnRef>(
                                cr.column_name, cr.column_index, cr.GetReturnType());
                            bound_item.select_index = sl_idx;
                            resolved = true;
                        } else {
                            throw NotImplementedException(
                                "ORDER BY position " + std::to_string(ord) +
                                " of this select list is not supported");
                        }
                    } else {
                        throw BinderException(
                            ErrorCode::OUT_OF_RANGE,
                            "ORDER BY position " + std::to_string(ord) +
                            " is out of range (select list has " +
                            std::to_string(result->select_list.size()) +
                            " entries)");
                    }
                }
            }
        }
        // Check if this ORDER BY expression is a select-list alias.
        // The alias-resolution branch re-binds the ORIGINAL (unbound)
        // select_list expression; that only works when select_list[i] is
        // itself a scalar. With SELECT *, result_names comes from star
        // expansion but select_list[0] is still an unbound STAR - re-
        // binding it as a scalar throws "Unhandled expression type in
        // binder". Skip alias-resolution for any index whose select_list
        // entry is a star; fall through to normal column-name resolution.
        if (!resolved && item.expression->GetExpressionType() == ExpressionType::COLUMN_REF) {
            auto &col_ref = static_cast<ColumnRefExpression &>(*item.expression);
            if (col_ref.table_name.empty()) {
                auto ref_upper = StringUtil::Upper(col_ref.column_name);
                for (idx_t i = 0; i < result->result_names.size(); i++) {
                    if (StringUtil::Upper(result->result_names[i]) == ref_upper) {
                        // Map back to the select_list entry that produced
                        // this result-name. With a bare STAR at index 0,
                        // every expanded name maps to select_list[0] -
                        // which is the star itself. Guard against that.
                        idx_t sl_idx = std::min<idx_t>(i, stmt.select_list.size() - 1);
                        if (stmt.select_list[sl_idx]->GetExpressionType()
                                == ExpressionType::STAR) {
                            break; // fall through to generic binding
                        }
                        // Re-bind the original select-list expression.
                        bound_item.expression = BindExpression(*stmt.select_list[sl_idx], context);
                        bound_item.select_index = i;
                        resolved = true;
                        break;
                    }
                }
            }
        }
        if (!resolved) {
            bound_item.expression = BindExpression(*item.expression, context);
        }
        // Reject ungrouped non-aggregated columns in ORDER BY of an
        // aggregated query — symmetric to the SELECT-list (f931ed3) and
        // HAVING (4ae0b65) checks. `SELECT a, SUM(b) ... GROUP BY a
        // ORDER BY b` silently sorted by a meaningless collapsed value.
        // Ordinal (ORDER BY 2) and alias (ORDER BY s) forms have already
        // re-bound to the grouped/aggregate expression above, so only
        // genuinely ungrouped column refs trip this.
        if (result->has_aggregation && !result->has_window &&
            bound_item.expression &&
            HasUngroupedColumn(*bound_item.expression, result->group_by)) {
            throw BinderException(ErrorCode::TYPE_MISMATCH,
                "column in ORDER BY must appear in GROUP BY "
                "or be used in an aggregate function");
        }
        bound_item.ascending = item.ascending;
        bound_item.nulls_first = item.nulls_first;
        result->order_by.push_back(std::move(bound_item));
    }

    // Bind LIMIT/OFFSET. NULL operand means "no limit" / "no offset"
    // per PostgreSQL/SQLite — defaults stay (-1 / 0). Previously this
    // checked the bound expression's type was literally CONSTANT, so
    // any computed expression (LIMIT 1+1, LIMIT 2*3, LIMIT 5-2) was
    // silently ignored and the query returned all rows. Now we fold
    // any constant-foldable expression via ExecuteScalar (which
    // handles arithmetic, functions, CASE, etc).
    auto extract_int = [](const Value &val, int64_t &out) -> bool {
        if (val.IsNull()) return false;
        switch (val.type().id()) {
        case LogicalTypeId::TINYINT:  out = val.GetValue<int8_t>();  return true;
        case LogicalTypeId::SMALLINT: out = val.GetValue<int16_t>(); return true;
        case LogicalTypeId::INTEGER:  out = val.GetValue<int32_t>(); return true;
        case LogicalTypeId::BIGINT:   out = val.GetValue<int64_t>(); return true;
        default: return false;
        }
    };
    // LIMIT/OFFSET must be column-free; ExpressionExecutor::ExecuteScalar
    // with an empty input would segfault on a COLUMN_REF child.
    std::function<bool(const BoundExpression &)> contains_col_ref =
        [&](const BoundExpression &e) -> bool {
        switch (e.GetExpressionType()) {
        case BoundExpressionType::COLUMN_REF:
            return true;
        case BoundExpressionType::ARITHMETIC: {
            auto &a = static_cast<const BoundArithmetic &>(e);
            return (a.left && contains_col_ref(*a.left)) ||
                   (a.right && contains_col_ref(*a.right));
        }
        case BoundExpressionType::COMPARISON: {
            auto &c = static_cast<const BoundComparison &>(e);
            return (c.left && contains_col_ref(*c.left)) ||
                   (c.right && contains_col_ref(*c.right));
        }
        case BoundExpressionType::CONJUNCTION: {
            auto &c = static_cast<const BoundConjunction &>(e);
            return (c.left && contains_col_ref(*c.left)) ||
                   (c.right && contains_col_ref(*c.right));
        }
        case BoundExpressionType::NEGATION:
            return contains_col_ref(*static_cast<const BoundNegation &>(e).child);
        case BoundExpressionType::IS_NULL:
            return contains_col_ref(*static_cast<const BoundIsNull &>(e).child);
        case BoundExpressionType::IS_BOOL:
            return contains_col_ref(*static_cast<const BoundIsBool &>(e).child);
        case BoundExpressionType::UNARY_MINUS:
            return contains_col_ref(*static_cast<const BoundUnaryMinus &>(e).child);
        case BoundExpressionType::CAST:
            return contains_col_ref(*static_cast<const BoundCast &>(e).child);
        case BoundExpressionType::FUNCTION: {
            auto &f = static_cast<const BoundFunction &>(e);
            for (auto &a : f.arguments) {
                if (a && contains_col_ref(*a)) return true;
            }
            return false;
        }
        default: return false;
        }
    };
    if (stmt.limit) {
        auto bound = BindExpression(*stmt.limit, context);
        if (contains_col_ref(*bound)) {
            throw BinderException(ErrorCode::TYPE_MISMATCH,
                "LIMIT must be a constant expression (no column references)");
        }
        Value val;
        try {
            val = ExpressionExecutor::ExecuteScalar(*bound);
        } catch (const std::exception &) {
            throw BinderException(ErrorCode::TYPE_MISMATCH,
                "LIMIT must be a constant integer expression");
        }
        int64_t v = -1;
        if (extract_int(val, v)) {
            if (v < 0) {
                throw BinderException(ErrorCode::OUT_OF_RANGE, "LIMIT cannot be negative");
            }
            result->limit_count = v;
        } else if (!val.IsNull()) {
            throw BinderException(ErrorCode::TYPE_MISMATCH,
                "LIMIT must be a constant integer expression");
        }
        // val.IsNull() => leave default -1 (no limit)
    }
    if (stmt.offset) {
        auto bound = BindExpression(*stmt.offset, context);
        if (contains_col_ref(*bound)) {
            throw BinderException(ErrorCode::TYPE_MISMATCH,
                "OFFSET must be a constant expression (no column references)");
        }
        Value val;
        try {
            val = ExpressionExecutor::ExecuteScalar(*bound);
        } catch (const std::exception &) {
            throw BinderException(ErrorCode::TYPE_MISMATCH,
                "OFFSET must be a constant integer expression");
        }
        int64_t v = 0;
        if (extract_int(val, v)) {
            if (v < 0) {
                throw BinderException(ErrorCode::OUT_OF_RANGE, "OFFSET cannot be negative");
            }
            result->offset_count = v;
        } else if (!val.IsNull()) {
            throw BinderException(ErrorCode::TYPE_MISMATCH,
                "OFFSET must be a constant integer expression");
        }
    }

    return result;
}

// ============================================================================
// Bind CREATE TABLE
// ============================================================================

BoundStmtPtr Binder::BindCreateTable(const CreateTableStatement &stmt) {
    auto result = std::make_unique<BoundCreateTableStatement>();
    result->table_name = stmt.table_name;
    result->if_not_exists = stmt.if_not_exists;

    for (auto &col : stmt.columns) {
        // Two columns of one name, in any case, are DuckDB's error; both
        // were made, and a reference read the first (design L26).
        for (auto &prev : result->columns) {
            if (IdentifierEquals(prev.name, col.name)) {
                throw CatalogException("Column with name \"" + col.name + "\" already exists");
            }
        }
        auto type = ResolveTypeName(col.type_name);
        // A PRIMARY KEY column is implicitly NOT NULL.
        result->columns.emplace_back(col.name, type,
                                     col.not_null || col.is_primary_key,
                                     col.is_primary_key);
    }
    return result;
}

// ============================================================================
// Bind DROP TABLE
// ============================================================================

BoundStmtPtr Binder::BindDropTable(const DropTableStatement &stmt) {
    auto result = std::make_unique<BoundDropTableStatement>();
    result->table_name = stmt.table_name;
    result->if_exists = stmt.if_exists;

    if (!stmt.if_exists && !catalog_.GetTable(stmt.table_name)) {
        throw BinderException(ErrorCode::TABLE_NOT_FOUND,
                               "Table '" + stmt.table_name + "' does not exist");
    }
    return result;
}

// ============================================================================
// Bind INSERT
// ============================================================================

BoundStmtPtr Binder::BindInsert(const InsertStatement &stmt) {
    auto result = std::make_unique<BoundInsertStatement>();

    auto *entry = catalog_.GetTable(stmt.table_name);
    if (!entry) {
        throw BinderException(ErrorCode::TABLE_NOT_FOUND,
                               "Table '" + stmt.table_name + "' not found");
    }
    result->table = entry;

    BindContext context;
    auto col_types = entry->GetTypes();
    // Wrap each bound value in a BoundCast when the source type
    // doesn't match the target column. Required for VARCHAR -> DATE /
    // TIMESTAMP literals and for type-widening (SMALLINT -> INT, etc).
    auto wrap_for_target = [&](BoundExprPtr bound, idx_t target) -> BoundExprPtr {
        if (target >= col_types.size()) return bound;
        auto src_id = bound->GetReturnType().id();
        auto dst_id = col_types[target].id();
        if (src_id != dst_id && src_id != LogicalTypeId::SQLNULL) {
            CheckBlobCast(bound->GetReturnType(), col_types[target]);
            bound = std::make_unique<BoundCast>(std::move(bound), col_types[target]);
        }
        return bound;
    };
    // Resolve the column-name list once, validating each name and
    // detecting duplicates. When the user omits the column list, the
    // default mapping is positional (column 0..N-1 in declared order).
    std::vector<idx_t> target_indices;
    if (!stmt.column_names.empty()) {
        target_indices.reserve(stmt.column_names.size());
        std::vector<bool> seen(col_types.size(), false);
        for (auto &col_name : stmt.column_names) {
            idx_t idx = entry->GetColumnIndex(col_name);
            if (idx == INVALID_INDEX) {
                throw BinderException(ErrorCode::COLUMN_NOT_FOUND,
                    "Column '" + col_name + "' not found in table '" +
                    stmt.table_name + "'");
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
    auto &table_cols = entry->GetColumns();
    // Bind-time NULL detection: peel BoundCast wrappers (added by
    // wrap_for_target) so CAST(NULL AS INT) and similar are also
    // caught. Without the peel, `INSERT INTO nn VALUES (CAST(NULL
    // AS INT))` silently accepted NULL into a NOT NULL column.
    auto check_not_null = [&](const BoundExpression &val, idx_t col) {
        if (col >= table_cols.size() || !table_cols[col].not_null) return;
        const BoundExpression *cur = &val;
        while (cur && cur->GetExpressionType() == BoundExpressionType::CAST) {
            cur = static_cast<const BoundCast &>(*cur).child.get();
        }
        if (!cur || cur->GetExpressionType() != BoundExpressionType::CONSTANT) return;
        auto &bc = static_cast<const BoundConstant &>(*cur);
        if (bc.value.IsNull()) {
            throw BinderException(ErrorCode::TYPE_MISMATCH,
                "NOT NULL constraint violation: column '" +
                table_cols[col].name + "'");
        }
    };
    for (auto &row : stmt.values) {
        if (!target_indices.empty()) {
            // Permute: bound value at user position i lands at table
            // column target_indices[i]. Unspecified columns default to
            // typed NULL (matches SQL standard implicit-default-NULL).
            if (row.size() != target_indices.size()) {
                throw BinderException(ErrorCode::TYPE_MISMATCH,
                    "INSERT value count (" + std::to_string(row.size()) +
                    ") does not match column list count (" +
                    std::to_string(target_indices.size()) + ")");
            }
            std::vector<BoundExprPtr> permuted(col_types.size());
            for (idx_t i = 0; i < row.size(); i++) {
                auto bound = BindExpression(*row[i], context);
                idx_t tgt = target_indices[i];
                permuted[tgt] = wrap_for_target(std::move(bound), tgt);
                check_not_null(*permuted[tgt], tgt);
            }
            // Fill missing slots with NULL constants of the column's type.
            for (idx_t c = 0; c < col_types.size(); c++) {
                if (!permuted[c]) {
                    if (c < table_cols.size() && table_cols[c].not_null) {
                        throw BinderException(ErrorCode::TYPE_MISMATCH,
                            "NOT NULL constraint violation: column '" +
                            table_cols[c].name +
                            "' has no value in INSERT");
                    }
                    permuted[c] = std::make_unique<BoundConstant>(Value());
                }
            }
            result->values.push_back(std::move(permuted));
        } else {
            // Positional path: row[c] -> column c. Validate arity.
            if (row.size() != col_types.size()) {
                throw BinderException(ErrorCode::TYPE_MISMATCH,
                    "INSERT value count (" + std::to_string(row.size()) +
                    ") does not match column count (" +
                    std::to_string(col_types.size()) + ")");
            }
            std::vector<BoundExprPtr> bound_row;
            bound_row.reserve(row.size());
            for (idx_t c = 0; c < row.size(); c++) {
                auto bound = BindExpression(*row[c], context);
                bound_row.push_back(wrap_for_target(std::move(bound), c));
                check_not_null(*bound_row.back(), c);
            }
            result->values.push_back(std::move(bound_row));
        }
    }
    return result;
}

// ============================================================================
// Bind UPDATE
// ============================================================================

BoundStmtPtr Binder::BindUpdate(const UpdateStatement &stmt) {
    auto result = std::make_unique<BoundUpdateStatement>();
    auto *entry = catalog_.GetTable(stmt.table_name);
    if (!entry) {
        throw BinderException(ErrorCode::TABLE_NOT_FOUND,
                               "Table '" + stmt.table_name + "' not found");
    }
    result->table = entry;

    BindContext context;
    context.AddTable(stmt.table_name, entry);

    auto col_types = entry->GetTypes();
    for (auto &assign : stmt.assignments) {
        auto idx = entry->GetColumnIndex(assign.column_name);
        if (idx == INVALID_INDEX) {
            throw BinderException(ErrorCode::COLUMN_NOT_FOUND,
                                   "Column '" + assign.column_name + "' not found");
        }
        BoundUpdateAssignment ba;
        ba.column_index = idx;
        ba.value = BindExpression(*assign.value, context);
        if (ContainsAggregate(*ba.value)) {
            throw BinderException(ErrorCode::AGGREGATE_IN_WHERE,
                "aggregate functions are not allowed in UPDATE assignment");
        }
        // Wrap the RHS in a BoundCast when its bound type doesn't
        // match the target column type. Mirrors the INSERT VALUES
        // fix (fe38e2d): previously UPDATE silently bit-wrapped a
        // BIGINT into an INTEGER column (9999999999 -> 1410065407)
        // or wrote a VARCHAR-typed Value into a DATE slot which
        // read back as epoch.
        if (idx < col_types.size()) {
            auto src_id = ba.value->GetReturnType().id();
            auto dst_id = col_types[idx].id();
            if (src_id != dst_id && src_id != LogicalTypeId::SQLNULL) {
                CheckBlobCast(ba.value->GetReturnType(), col_types[idx]);
                ba.value = std::make_unique<BoundCast>(
                    std::move(ba.value), col_types[idx]);
            }
        }
        // NOT NULL constraint: reject literal NULL into a NOT NULL
        // column at bind time. Peel BoundCast wrappers so CAST(NULL
        // AS INT) is also caught. Computed NULLs at runtime are not
        // caught here (deferred to executor-side enforcement).
        auto &upd_cols = entry->GetColumns();
        if (idx < upd_cols.size() && upd_cols[idx].not_null) {
            const BoundExpression *cur = ba.value.get();
            while (cur && cur->GetExpressionType() == BoundExpressionType::CAST) {
                cur = static_cast<const BoundCast &>(*cur).child.get();
            }
            if (cur && cur->GetExpressionType() == BoundExpressionType::CONSTANT) {
                auto &bc = static_cast<const BoundConstant &>(*cur);
                if (bc.value.IsNull()) {
                    throw BinderException(ErrorCode::TYPE_MISMATCH,
                        "NOT NULL constraint violation: column '" +
                        upd_cols[idx].name + "'");
                }
            }
        }
        result->assignments.push_back(std::move(ba));
    }

    if (stmt.where_clause) {
        result->where_clause = BindExpression(*stmt.where_clause, context);
        AsCondition(result->where_clause);
        if (ContainsAggregate(*result->where_clause)) {
            throw BinderException(ErrorCode::AGGREGATE_IN_WHERE,
                "aggregate functions are not allowed in WHERE clause");
        }
    }

    return result;
}

// ============================================================================
// Bind DELETE
// ============================================================================

BoundStmtPtr Binder::BindDelete(const DeleteStatement &stmt) {
    auto result = std::make_unique<BoundDeleteStatement>();
    auto *entry = catalog_.GetTable(stmt.table_name);
    if (!entry) {
        throw BinderException(ErrorCode::TABLE_NOT_FOUND,
                               "Table '" + stmt.table_name + "' not found");
    }
    result->table = entry;

    BindContext context;
    context.AddTable(stmt.table_name, entry);

    if (stmt.where_clause) {
        result->where_clause = BindExpression(*stmt.where_clause, context);
        AsCondition(result->where_clause);
        if (ContainsAggregate(*result->where_clause)) {
            throw BinderException(ErrorCode::AGGREGATE_IN_WHERE,
                "aggregate functions are not allowed in WHERE clause");
        }
    }

    return result;
}

// ============================================================================
// Bind FROM
// ============================================================================

void Binder::BindTableRef(const TableRef &ref, BindContext &context) {
    auto *entry = catalog_.GetTable(ref.table_name);
    if (!entry) {
        throw BinderException(ErrorCode::TABLE_NOT_FOUND,
                               "Table '" + ref.table_name + "' not found");
    }
    auto alias = ref.alias.empty() ? ref.table_name : ref.alias;
    context.AddTable(alias, entry);

    // Recursively bind every joined table on the chain. Previously this
    // only handled the immediate right side, so `FROM a, b, c` (SQL-92
    // comma-FROM) or `a JOIN b JOIN c` only ever added a + b, and any
    // column ref into c failed at bind. Recursion makes the chain length
    // unbounded.
    if (ref.right) {
        BindTableRef(*ref.right, context);
    }
}

// ============================================================================
// Bind expressions
// ============================================================================

BoundExprPtr Binder::BindExpression(const ParsedExpression &expr, BindContext &context) {
    switch (expr.GetExpressionType()) {
    case ExpressionType::COLUMN_REF:
        return BindColumnRef(static_cast<const ColumnRefExpression &>(expr), context);
    case ExpressionType::CONSTANT:
        return BindConstant(static_cast<const ConstantExpression &>(expr));
    case ExpressionType::COMPARISON:
        return BindComparison(static_cast<const ComparisonExpression &>(expr), context);
    case ExpressionType::CONJUNCTION:
        return BindConjunction(static_cast<const ConjunctionExpression &>(expr), context);
    case ExpressionType::NEGATION:
        return BindNegation(static_cast<const NegationExpression &>(expr), context);
    case ExpressionType::IS_NULL:
        return BindIsNull(static_cast<const IsNullExpression &>(expr), context);
    case ExpressionType::IS_BOOL:
        return BindIsBool(static_cast<const IsBoolExpression &>(expr), context);
    case ExpressionType::ARITHMETIC:
        return BindArithmetic(static_cast<const ArithmeticExpression &>(expr), context);
    case ExpressionType::UNARY_MINUS:
        return BindUnaryMinus(static_cast<const UnaryMinusExpression &>(expr), context);
    case ExpressionType::FUNCTION:
        return BindFunction(static_cast<const FunctionExpression &>(expr), context);
    case ExpressionType::CAST:
        return BindCast(static_cast<const CastExpression &>(expr), context);
    case ExpressionType::WINDOW:
        return BindWindow(static_cast<const WindowExpression &>(expr), context);
    case ExpressionType::SUBQUERY:
        return BindSubquery(static_cast<const SubqueryExpression &>(expr), context);
    default:
        throw InternalException("Unhandled expression type in binder");
    }
}

BoundExprPtr Binder::BindColumnRef(const ColumnRefExpression &expr, BindContext &context) {
    // SELECT-list alias resolution (HAVING): when the active context has
    // an alias map populated, an unqualified column ref whose name matches
    // a select-list alias re-binds the original select-list expression.
    // Required for SQL forms like `... COUNT(*) AS c ... HAVING c > N`.
    if (expr.table_name.empty() && !context.select_list_aliases.empty()) {
        auto it = context.select_list_aliases.find(StringUtil::Upper(expr.column_name));
        if (it != context.select_list_aliases.end() && it->second) {
            // The aliased expression's own names are columns, not aliases:
            // `sum(a) AS a ... HAVING a > 0` reads column a inside sum. With
            // the map in place, binding it recursed without end (libzugodb
            // Stage 7, #11).
            const auto *target = it->second;
            auto aliases = std::move(context.select_list_aliases);
            context.select_list_aliases.clear();
            struct Restore {
                BindContext &ctx;
                decltype(aliases) &saved;
                ~Restore() { ctx.select_list_aliases = std::move(saved); }
            } restore{context, aliases};
            return BindExpression(*target, context);
        }
    }
    auto [table, combined_idx] = context.ResolveColumn(expr.column_name, expr.table_name);
    idx_t offset = context.GetTableOffset(table);
    idx_t local_idx = combined_idx - offset;
    auto &column = context.tables.at(table)->GetColumns()[local_idx];
    // The catalog's spelling, as DuckDB names the result column `Foo` for
    // SELECT foo of a column Foo (design L26).
    return std::make_unique<BoundColumnRef>(column.name, combined_idx, column.type);
}

BoundExprPtr Binder::BindConstant(const ConstantExpression &expr) {
    if (expr.is_null) {
        return std::make_unique<BoundConstant>(Value());
    }

    switch (expr.literal_type) {
    case TokenType::INTEGER_LITERAL: {
        // Out-of-range integer literals previously leaked raw
        // std::out_of_range from std::stoll up to the shell as
        // "Error: stoll argument out of range". Promote to a classified
        // ParserException. INT64_MAX+1 and beyond falls here; INT64_MIN
        // is parsed as NEG(INT64_MAX+1) by the unary-minus path which
        // means -9223372036854775808 still hits this branch (the
        // absolute value overflows by one) — documented limitation.
        int64_t val;
        try {
            val = std::stoll(expr.value);
        } catch (const std::out_of_range &) {
            throw ParserException(
                "Numeric literal '" + expr.value +
                "' exceeds BIGINT range");
        } catch (const std::invalid_argument &) {
            throw ParserException(
                "Could not parse numeric literal '" + expr.value + "'");
        }
        if (val >= INT32_MIN && val <= INT32_MAX) {
            return std::make_unique<BoundConstant>(Value::INTEGER(static_cast<int32_t>(val)));
        }
        return std::make_unique<BoundConstant>(Value::BIGINT(val));
    }
    case TokenType::FLOAT_LITERAL: {
        double d;
        try {
            d = std::stod(expr.value);
        } catch (const std::out_of_range &) {
            throw ParserException(
                "Floating-point literal '" + expr.value +
                "' exceeds DOUBLE range");
        } catch (const std::invalid_argument &) {
            throw ParserException(
                "Could not parse floating-point literal '" + expr.value + "'");
        }
        return std::make_unique<BoundConstant>(Value::DOUBLE(d));
    }
    case TokenType::STRING_LITERAL:
        return std::make_unique<BoundConstant>(Value::VARCHAR(expr.value));
    case TokenType::KW_TRUE:
        return std::make_unique<BoundConstant>(Value::BOOLEAN(true));
    case TokenType::KW_FALSE:
        return std::make_unique<BoundConstant>(Value::BOOLEAN(false));
    case TokenType::KW_DATE:
        // SQL-92 typed literals, read by DuckDB's grammar (design L19).
        return std::make_unique<BoundConstant>(Value::ParseTemporal(expr.value, LogicalTypeId::DATE));
    case TokenType::KW_TIMESTAMP:
        return std::make_unique<BoundConstant>(Value::ParseTemporal(expr.value, LogicalTypeId::TIMESTAMP));
    case TokenType::KW_TIME:
        return std::make_unique<BoundConstant>(Value::ParseTemporal(expr.value, LogicalTypeId::TIME));
    default:
        throw InternalException("Unknown constant literal type");
    }
}

BoundExprPtr Binder::BindComparison(const ComparisonExpression &expr, BindContext &context) {
    auto left = BindExpression(*expr.left, context);
    auto right = BindExpression(*expr.right, context);
    if (left->GetReturnType().id() == LogicalTypeId::BLOB ||
        right->GetReturnType().id() == LogicalTypeId::BLOB) {
        CoerceBesideBlob(left, "A comparison");
        CoerceBesideBlob(right, "A comparison");
    }

    // Promote a literal constant to the column's type when one side is a
    // column reference and the other is a CONSTANT of a compatible numeric
    // type. Without this the executor falls to a string-stod-per-row slow
    // path on `WHERE bigint_col > 50` (50 binds as INTEGER, the column is
    // BIGINT, types differ, so the typed-compare branch is skipped). This
    // changes a 2.5-second filter scan over 10M rows into a ~30 ms one.
    auto promote_literal = [](BoundExpression &target_typed, BoundExpression *literal) {
        if (literal->GetExpressionType() != BoundExpressionType::CONSTANT) return;
        auto &konst = *static_cast<BoundConstant *>(literal);
        if (konst.value.IsNull()) return;
        auto src = konst.value.type().id();
        auto dst = target_typed.GetReturnType().id();
        if (src == dst) return;
        auto is_int = [](LogicalTypeId t) {
            return t == LogicalTypeId::TINYINT || t == LogicalTypeId::SMALLINT ||
                   t == LogicalTypeId::INTEGER || t == LogicalTypeId::BIGINT;
        };
        if (!is_int(src) || !is_int(dst)) return;
        int64_t v = 0;
        switch (src) {
        case LogicalTypeId::TINYINT:  v = konst.value.GetValue<int8_t>(); break;
        case LogicalTypeId::SMALLINT: v = konst.value.GetValue<int16_t>(); break;
        case LogicalTypeId::INTEGER:  v = konst.value.GetValue<int32_t>(); break;
        case LogicalTypeId::BIGINT:   v = konst.value.GetValue<int64_t>(); break;
        default: return;
        }
        switch (dst) {
        case LogicalTypeId::TINYINT:
            if (v < INT8_MIN || v > INT8_MAX) return;
            konst.value = Value::TINYINT(static_cast<int8_t>(v)); break;
        case LogicalTypeId::SMALLINT:
            if (v < INT16_MIN || v > INT16_MAX) return;
            konst.value = Value::SMALLINT(static_cast<int16_t>(v)); break;
        case LogicalTypeId::INTEGER:
            if (v < INT32_MIN || v > INT32_MAX) return;
            konst.value = Value::INTEGER(static_cast<int32_t>(v)); break;
        case LogicalTypeId::BIGINT:
            konst.value = Value::BIGINT(v); break;
        default: return;
        }
        konst.SetReturnType(target_typed.GetReturnType());
    };
    // Coerce a strict 'YYYY-MM-DD' VARCHAR literal against an integer date
    // column (USMALLINT/INTEGER days-since-epoch, e.g. a packed date column)
    // so the typed-compare fast path fires instead of falling to per-row
    // string-stod. Date-literal filter queries land here.
    auto promote_date_literal = [](BoundExpression &target_typed, BoundExpression *literal) {
        if (literal->GetExpressionType() != BoundExpressionType::CONSTANT) return;
        auto &konst = *static_cast<BoundConstant *>(literal);
        if (konst.value.IsNull()) return;
        if (konst.value.type().id() != LogicalTypeId::VARCHAR) return;
        auto dst = target_typed.GetReturnType().id();
        if (dst != LogicalTypeId::USMALLINT && dst != LogicalTypeId::SMALLINT &&
            dst != LogicalTypeId::INTEGER && dst != LogicalTypeId::BIGINT) return;
        const std::string &s = konst.value.GetValue<std::string>();
        int32_t days = 0;
        if (!Value::TryParseDateStringEpochDays(s.data(), s.size(), days)) return;
        switch (dst) {
        case LogicalTypeId::USMALLINT:
            if (days < 0 || days > UINT16_MAX) return;
            konst.value = Value::USMALLINT(static_cast<uint16_t>(days)); break;
        case LogicalTypeId::SMALLINT:
            if (days < INT16_MIN || days > INT16_MAX) return;
            konst.value = Value::SMALLINT(static_cast<int16_t>(days)); break;
        case LogicalTypeId::INTEGER:
            konst.value = Value::INTEGER(days); break;
        case LogicalTypeId::BIGINT:
            konst.value = Value::BIGINT(days); break;
        default: return;
        }
        konst.SetReturnType(target_typed.GetReturnType());
    };
    if (left->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
        promote_date_literal(*left, right.get());
        promote_literal(*left, right.get());
    } else if (right->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
        promote_date_literal(*right, left.get());
        promote_literal(*right, left.get());
    }

    return std::make_unique<BoundComparison>(expr.op, std::move(left), std::move(right));
}

// The type of a CASE's branches and of COALESCE's, NULLIF's, GREATEST's
// and LEAST's arguments, DuckDB's where the engine computes it the same
// way (Stage 9): NULLs aside, one type is kept as it is (BOOLEAN, DATE,
// FLOAT, TINYINT, ...), numbers widen to the widest (an integer with a
// FLOAT is a FLOAT), and a string constant beside one other type takes
// that type, as DuckDB's string literal does. Every other mix is
// refused: the engine unified anything but a VARCHAR as an INTEGER, so
// CASE WHEN c THEN true ELSE false END was an INTEGER, a FLOAT became a
// DOUBLE and a DATE an error, and a VARCHAR column beside a number made
// both VARCHAR where DuckDB's answer depends on which side is a literal.
static LogicalType UnifyTypes(const std::vector<const BoundExpression *> &exprs,
                              const std::string &fn) {
    auto int_rank = [](LogicalTypeId x) -> int {
        switch (x) {
        case LogicalTypeId::TINYINT: return 1;
        case LogicalTypeId::SMALLINT: return 2;
        case LogicalTypeId::INTEGER: return 3;
        case LogicalTypeId::BIGINT: return 4;
        default: return 0;
        }
    };
    std::vector<LogicalType> others;
    bool string_constant = false;
    for (auto *e : exprs) {
        auto t = e->GetReturnType();
        if (t.id() == LogicalTypeId::SQLNULL) continue;
        if (t.id() == LogicalTypeId::VARCHAR &&
            e->GetExpressionType() == BoundExpressionType::CONSTANT) {
            string_constant = true;
            continue;
        }
        others.push_back(t);
    }
    if (others.empty()) {
        return string_constant ? LogicalType::VARCHAR() : LogicalType::SQLNULL();
    }
    bool same = true;
    for (auto &t : others) same = same && t.id() == others[0].id();
    if (same) return others[0];
    bool numeric = true, has_float = false, has_double = false;
    int widest = 0;
    LogicalType widest_int = LogicalType::INTEGER();
    for (auto &t : others) {
        if (t.id() == LogicalTypeId::FLOAT) has_float = true;
        else if (t.id() == LogicalTypeId::DOUBLE) has_double = true;
        else if (int_rank(t.id()) > 0) {
            if (int_rank(t.id()) > widest) { widest = int_rank(t.id()); widest_int = t; }
        } else numeric = false;
    }
    if (numeric) {
        if (has_double) return LogicalType::DOUBLE();
        if (has_float) return LogicalType::FLOAT();
        return widest_int;
    }
    std::string list;
    for (auto &t : others) list += (list.empty() ? "" : ", ") + t.ToString();
    throw NotImplementedException(fn + " of the types " + list + " is not supported");
}

BoundExprPtr Binder::BindConjunction(const ConjunctionExpression &expr, BindContext &context) {
    auto left = BindExpression(*expr.left, context);
    auto right = BindExpression(*expr.right, context);
    AsCondition(left);
    AsCondition(right);
    return std::make_unique<BoundConjunction>(expr.op, std::move(left), std::move(right));
}

BoundExprPtr Binder::BindNegation(const NegationExpression &expr, BindContext &context) {
    auto child = BindExpression(*expr.child, context);
    AsCondition(child);
    return std::make_unique<BoundNegation>(std::move(child));
}

BoundExprPtr Binder::BindIsNull(const IsNullExpression &expr, BindContext &context) {
    auto child = BindExpression(*expr.child, context);
    return std::make_unique<BoundIsNull>(std::move(child), expr.is_not);
}

BoundExprPtr Binder::BindIsBool(const IsBoolExpression &expr, BindContext &context) {
    auto child = BindExpression(*expr.child, context);
    auto child_type = child->GetReturnType().id();
    BoundIsBool::Predicate p = BoundIsBool::Predicate::TRUE_;
    switch (expr.pred) {
        case IsBoolExpression::Predicate::TRUE_:    p = BoundIsBool::Predicate::TRUE_; break;
        case IsBoolExpression::Predicate::FALSE_:   p = BoundIsBool::Predicate::FALSE_; break;
        case IsBoolExpression::Predicate::UNKNOWN_: p = BoundIsBool::Predicate::UNKNOWN_; break;
    }
    // IS UNKNOWN accepts any operand (it's equivalent to IS NULL). The
    // TRUE/FALSE forms require a BOOLEAN operand per SQL standard, but
    // we accept SQLNULL too (NULL IS TRUE is FALSE, NULL IS FALSE is FALSE).
    if (p != BoundIsBool::Predicate::UNKNOWN_) {
        if (child_type != LogicalTypeId::BOOLEAN &&
            child_type != LogicalTypeId::SQLNULL) {
            throw BinderException(ErrorCode::TYPE_MISMATCH,
                "IS [NOT] TRUE/FALSE requires a boolean operand, got " +
                child->GetReturnType().ToString());
        }
    }
    return std::make_unique<BoundIsBool>(std::move(child), p, expr.is_not);
}

// An integer literal, `5` or `-5`, and its value. DuckDB types one as
// INTEGER_LITERAL, which casts implicitly to any integer type that holds
// it, so `x + 1` of a TINYINT `x` is TINYINT arithmetic and overflows at
// 127 (#43). `(1 + 0)` or `1::INTEGER` is not a literal.
static bool IntegerLiteralValue(const ParsedExpression &e, int64_t &out) {
    if (e.GetExpressionType() == ExpressionType::CONSTANT) {
        auto &c = static_cast<const ConstantExpression &>(e);
        if (c.is_null || c.literal_type != TokenType::INTEGER_LITERAL) return false;
        try {
            size_t used = 0;
            out = std::stoll(c.value, &used);
            return used == c.value.size();
        } catch (const std::exception &) {
            return false;
        }
    }
    if (e.GetExpressionType() == ExpressionType::UNARY_MINUS) {
        int64_t v = 0;
        if (!IntegerLiteralValue(*static_cast<const UnaryMinusExpression &>(e).child, v) ||
            v == std::numeric_limits<int64_t>::min()) {
            return false;
        }
        out = -v;
        return true;
    }
    return false;
}

// A decimal literal, `2.5` or `-2.5` (not `25e-1`, a DOUBLE in DuckDB):
// DuckDB types it DECIMAL, the engine DOUBLE.
static bool IsDecimalLiteral(const ParsedExpression &e) {
    const ParsedExpression *x = &e;
    while (x->GetExpressionType() == ExpressionType::UNARY_MINUS) {
        x = static_cast<const UnaryMinusExpression *>(x)->child.get();
    }
    if (x->GetExpressionType() != ExpressionType::CONSTANT) return false;
    auto &c = static_cast<const ConstantExpression &>(*x);
    return !c.is_null && c.literal_type == TokenType::FLOAT_LITERAL &&
           c.value.find_first_of("eE") == std::string::npos;
}

// Whether DuckDB types the operand `p`, bound as `b`, FLOAT or DOUBLE,
// rather than DECIMAL or an integer. The engine's DOUBLE operand is a
// DuckDB DECIMAL when it is a decimal literal, or arithmetic other than
// `/` of such operands; any other DOUBLE (a column, a cast, a function, an
// exponent literal) is DuckDB's DOUBLE too (#42).
static bool DuckDBFloating(const ParsedExpression &p, const BoundExpression &b) {
    auto id = b.GetReturnType().id();
    if (id != LogicalTypeId::FLOAT && id != LogicalTypeId::DOUBLE) return false;
    if (IsDecimalLiteral(p)) return false;
    if (p.GetExpressionType() == ExpressionType::UNARY_MINUS &&
        b.GetExpressionType() == BoundExpressionType::UNARY_MINUS) {
        return DuckDBFloating(*static_cast<const UnaryMinusExpression &>(p).child,
                              *static_cast<const BoundUnaryMinus &>(b).child);
    }
    if (p.GetExpressionType() == ExpressionType::ARITHMETIC &&
        b.GetExpressionType() == BoundExpressionType::ARITHMETIC) {
        auto &pa = static_cast<const ArithmeticExpression &>(p);
        auto &ba = static_cast<const BoundArithmetic &>(b);
        if (pa.op == "/") return true;
        return DuckDBFloating(*pa.left, *ba.left) || DuckDBFloating(*pa.right, *ba.right);
    }
    return true;
}

static int IntegerRank(LogicalTypeId t) {
    switch (t) {
    case LogicalTypeId::TINYINT: return 1;
    case LogicalTypeId::SMALLINT: return 2;
    case LogicalTypeId::INTEGER: return 3;
    case LogicalTypeId::BIGINT: return 4;
    default: return 0;
    }
}

// The literal `v` as a constant of `target`, an integer type, when that
// type holds it; otherwise the literal is left as bound.
static void NarrowIntegerLiteral(BoundExprPtr &bound, int64_t v, const LogicalType &target) {
    switch (target.id()) {
    case LogicalTypeId::TINYINT:
        if (v >= INT8_MIN && v <= INT8_MAX)
            bound = std::make_unique<BoundConstant>(Value::TINYINT(static_cast<int8_t>(v)));
        break;
    case LogicalTypeId::SMALLINT:
        if (v >= INT16_MIN && v <= INT16_MAX)
            bound = std::make_unique<BoundConstant>(Value::SMALLINT(static_cast<int16_t>(v)));
        break;
    default:
        break;
    }
}

BoundExprPtr Binder::BindArithmetic(const ArithmeticExpression &expr, BindContext &context) {
    auto left = BindExpression(*expr.left, context);
    auto right = BindExpression(*expr.right, context);
    // DuckDB's integer literals take the other operand's integer type when
    // they fit it (#43). Only TINYINT and SMALLINT differ from the bound
    // literal, an INTEGER or a BIGINT, so only those are narrowed.
    if (expr.op != "||") {
        int64_t lit = 0;
        bool left_lit = IntegerLiteralValue(*expr.left, lit);
        int64_t rlit = 0;
        bool right_lit = IntegerLiteralValue(*expr.right, rlit);
        if (left_lit && !right_lit && IntegerRank(right->GetReturnType().id()) > 0) {
            NarrowIntegerLiteral(left, lit, right->GetReturnType());
        } else if (right_lit && !left_lit && IntegerRank(left->GetReturnType().id()) > 0) {
            NarrowIntegerLiteral(right, rlit, left->GetReturnType());
        }
    }
    // || is string concatenation.
    if (expr.op == "||") {
        if (left->GetReturnType().id() == LogicalTypeId::BLOB ||
            right->GetReturnType().id() == LogicalTypeId::BLOB) {
            CoerceBesideBlob(left, "||");
            CoerceBesideBlob(right, "||");
            return std::make_unique<BoundArithmetic>(expr.op, std::move(left), std::move(right),
                                                      LogicalType::BLOB());
        }
        return std::make_unique<BoundArithmetic>(expr.op, std::move(left), std::move(right),
                                                  LogicalType::VARCHAR());
    }
    // DATE arithmetic per SQL standard:
    //   DATE + integer  -> DATE   (commutative)
    //   DATE - integer  -> DATE   (DATE on left only)
    //   DATE - DATE     -> INTEGER (days between)
    //   DATE * / / etc. -> error
    // Without this, every DATE math expression returned raw epoch days
    // (DATE '2024-01-01' + 5 -> 19728 instead of 2024-01-06) because
    // ResolveArithmeticType fell through to the INTEGER branch.
    auto left_id = left->GetReturnType().id();
    auto right_id = right->GetReturnType().id();
    bool left_date  = (left_id  == LogicalTypeId::DATE);
    bool right_date = (right_id == LogicalTypeId::DATE);
    // DuckDB adds an INTEGER (or narrower) to a DATE; DATE + BIGINT is no
    // function there (Stage 9).
    auto is_int_id = [](LogicalTypeId t) {
        return t == LogicalTypeId::TINYINT  || t == LogicalTypeId::SMALLINT ||
               t == LogicalTypeId::INTEGER;
    };
    if (left_date || right_date) {
        if (expr.op == "+") {
            // DATE + INT or INT + DATE
            if (left_date && (is_int_id(right_id) || right_id == LogicalTypeId::SQLNULL)) {
                return std::make_unique<BoundArithmetic>(
                    expr.op, std::move(left), std::move(right), LogicalType::DATE());
            }
            if (right_date && (is_int_id(left_id) || left_id == LogicalTypeId::SQLNULL)) {
                return std::make_unique<BoundArithmetic>(
                    expr.op, std::move(left), std::move(right), LogicalType::DATE());
            }
        } else if (expr.op == "-") {
            // DATE - INT -> DATE; DATE - DATE -> INTEGER (days).
            if (left_date && right_date) {
                return std::make_unique<BoundArithmetic>(
                    expr.op, std::move(left), std::move(right), LogicalType::INTEGER());
            }
            if (left_date && (is_int_id(right_id) || right_id == LogicalTypeId::SQLNULL)) {
                return std::make_unique<BoundArithmetic>(
                    expr.op, std::move(left), std::move(right), LogicalType::DATE());
            }
        }
        // Any other DATE-involving combination is a type error.
        throw BinderException(ErrorCode::TYPE_MISMATCH,
            "Invalid DATE arithmetic: " + left->GetReturnType().ToString() +
            " " + expr.op + " " + right->GetReturnType().ToString());
    }
    // TIMESTAMP arithmetic. DuckDB has no TIMESTAMP + INTEGER, and its
    // TIMESTAMP - TIMESTAMP is an INTERVAL, a type the engine does not
    // hold; the engine took the integer as seconds and gave the
    // difference as BIGINT seconds. Both are refused (Stage 9).
    bool left_ts  = (left_id  == LogicalTypeId::TIMESTAMP || left_id == LogicalTypeId::TIMESTAMP_TZ);
    bool right_ts = (right_id == LogicalTypeId::TIMESTAMP || right_id == LogicalTypeId::TIMESTAMP_TZ);
    if (left_ts || right_ts) {
        if (expr.op == "-" && left_ts && right_ts) {
            throw NotImplementedException(
                "TIMESTAMP - TIMESTAMP is not supported: DuckDB's result is an INTERVAL; "
                "date_diff gives the difference in a part");
        }
        throw BinderException(ErrorCode::TYPE_MISMATCH,
            "No function matches " + left->GetReturnType().ToString() + " " + expr.op + " " +
            right->GetReturnType().ToString());
    }
    // Arithmetic of numbers only. A VARCHAR, BOOLEAN or TIME operand was
    // typed INTEGER and read as garbage (1::TINYINT + 1::VARCHAR gave 1);
    // DuckDB has no such function, or casts a literal (#34).
    auto numeric_or_null = [](LogicalTypeId t) {
        switch (t) {
        case LogicalTypeId::TINYINT: case LogicalTypeId::SMALLINT: case LogicalTypeId::INTEGER:
        case LogicalTypeId::BIGINT: case LogicalTypeId::UTINYINT: case LogicalTypeId::USMALLINT:
        case LogicalTypeId::UINTEGER: case LogicalTypeId::UBIGINT: case LogicalTypeId::FLOAT:
        case LogicalTypeId::DOUBLE: case LogicalTypeId::SQLNULL:
            return true;
        default:
            return false;
        }
    };
    bool bitwise = expr.op == "&" || expr.op == "|" || expr.op == "<<" || expr.op == ">>";
    auto floating = [](LogicalTypeId t) {
        return t == LogicalTypeId::FLOAT || t == LogicalTypeId::DOUBLE;
    };
    if (!numeric_or_null(left_id) || !numeric_or_null(right_id) ||
        (bitwise && (floating(left_id) || floating(right_id)))) {
        throw BinderException(ErrorCode::TYPE_MISMATCH,
            "No function matches " + left->GetReturnType().ToString() + " " + expr.op + " " +
            right->GetReturnType().ToString());
    }
    // DuckDB's ^ is POWER, not the bitwise exclusive or (2 ^ 3 gave 1).
    if (expr.op == "^") {
        std::vector<BoundExprPtr> pargs;
        pargs.push_back(std::move(left));
        pargs.push_back(std::move(right));
        return std::make_unique<BoundFunction>("POWER", std::move(pargs), LogicalType::DOUBLE(),
                                               false);
    }
    auto result_type = ResolveArithmeticType(left->GetReturnType(), right->GetReturnType());
    // DuckDB's / divides as doubles (integer_division is false by
    // default): 7 / 2 is 3.5, not 3 (#33).
    if (expr.op == "/" && result_type.id() != LogicalTypeId::DOUBLE &&
        result_type.id() != LogicalTypeId::FLOAT) {
        result_type = LogicalType::DOUBLE();
    }
    bool decimal_mod = expr.op == "%" &&
                       (result_type.id() == LogicalTypeId::DOUBLE ||
                        result_type.id() == LogicalTypeId::FLOAT) &&
                       !DuckDBFloating(*expr.left, *left) && !DuckDBFloating(*expr.right, *right);
    auto bound = std::make_unique<BoundArithmetic>(expr.op, std::move(left), std::move(right),
                                                   result_type);
    bound->decimal_mod = decimal_mod;
    return bound;
}

BoundExprPtr Binder::BindUnaryMinus(const UnaryMinusExpression &expr, BindContext &context) {
    // `-5` is one literal, as in DuckDB: -2147483648 is an INTEGER, so
    // `-2147483648 - 1` overflows as INTEGER arithmetic (#43).
    int64_t lit = 0;
    if (IntegerLiteralValue(expr, lit)) {
        if (lit >= INT32_MIN && lit <= INT32_MAX) {
            return std::make_unique<BoundConstant>(Value::INTEGER(static_cast<int32_t>(lit)));
        }
        return std::make_unique<BoundConstant>(Value::BIGINT(lit));
    }
    auto child = BindExpression(*expr.child, context);
    // `-0.0` is DuckDB's DECIMAL zero, which has no sign: 1.0 / -0.0 is
    // inf there. As a DOUBLE it was -0.0 (#42).
    if (IsDecimalLiteral(expr) && child->GetExpressionType() == BoundExpressionType::CONSTANT) {
        auto &v = static_cast<const BoundConstant &>(*child).value;
        if (!v.IsNull() && v.type().id() == LogicalTypeId::DOUBLE) {
            double d = -v.GetValue<double>();
            return std::make_unique<BoundConstant>(Value::DOUBLE(d == 0.0 ? 0.0 : d));
        }
    }
    return std::make_unique<BoundUnaryMinus>(std::move(child));
}

// A regex function through the host's provider (Stage 11.1, design L29):
// regexp_matches, regexp_full_match (SIMILAR TO), regexp_replace and
// regexp_extract with a group number. The options are a constant of 'c'
// and 'i' (the last wins), with 'g' for regexp_replace; any other letter
// is refused. A constant pattern is compiled here, once for the query.
static BoundExprPtr BindRegexCall(const std::string &name, std::vector<BoundExprPtr> args,
                                  const zugodb_regex_v1 &api) {
    auto call = std::make_shared<ProviderRegexCall>();
    call->api = &api;
    call->what = StringUtil::Lower(name);
    size_t lo = 2, hi = 3;
    LogicalType ret = LogicalType::BOOLEAN();
    if (name == "REGEXP_MATCHES") {
        call->kind = ProviderRegexCall::Kind::MATCHES;
    } else if (name == "REGEXP_FULL_MATCH") {
        call->kind = ProviderRegexCall::Kind::FULL_MATCH;
    } else if (name == "REGEXP_REPLACE") {
        call->kind = ProviderRegexCall::Kind::REPLACE;
        lo = 3;
        hi = 4;
        ret = LogicalType::VARCHAR();
    } else {
        call->kind = ProviderRegexCall::Kind::EXTRACT;
        lo = 2;
        hi = 4;
        ret = LogicalType::VARCHAR();
    }
    size_t n = args.size();
    if (n < lo || n > hi) {
        throw BinderException(ErrorCode::TYPE_MISMATCH,
            "No function matches " + call->what + "() with " + std::to_string(n) +
            " argument(s); it takes " + std::to_string(lo) + " to " + std::to_string(hi));
    }
    // The text arguments: the string, the pattern and the replacement.
    size_t text_args = call->kind == ProviderRegexCall::Kind::REPLACE ? 3 : 2;
    for (size_t i = 0; i < text_args; i++) {
        auto t = args[i]->GetReturnType().id();
        if (t != LogicalTypeId::VARCHAR && t != LogicalTypeId::SQLNULL) {
            throw BinderException(ErrorCode::TYPE_MISMATCH,
                "No function matches " + call->what + "() of a " +
                args[i]->GetReturnType().ToString());
        }
    }
    auto constant_of = [](const BoundExprPtr &e) -> const BoundConstant * {
        return e->GetExpressionType() == BoundExpressionType::CONSTANT
                   ? static_cast<const BoundConstant *>(e.get())
                   : nullptr;
    };
    // regexp_extract's group: a constant integer, as DuckDB's.
    size_t opt_index = text_args;
    if (call->kind == ProviderRegexCall::Kind::EXTRACT && n >= 3) {
        auto *g = constant_of(args[2]);
        auto gt = args[2]->GetReturnType().id();
        if (!g || g->value.IsNull() ||
            (gt != LogicalTypeId::TINYINT && gt != LogicalTypeId::SMALLINT &&
             gt != LogicalTypeId::INTEGER && gt != LogicalTypeId::BIGINT)) {
            throw NotImplementedException(
                "regexp_extract's group must be a constant integer");
        }
        int64_t gv = g->value.GetValue<int64_t>();
        if (gv < 0 || gv > 1000) {
            throw InvalidInputException("regexp_extract: group index " + std::to_string(gv) +
                                        " is out of range");
        }
        call->group = static_cast<int>(gv);
        opt_index = 3;
    }
    if (n > opt_index) {
        auto *o = constant_of(args[opt_index]);
        if (!o || o->value.IsNull() || o->value.type().id() != LogicalTypeId::VARCHAR) {
            throw InvalidInputException(call->what + ": the options must be a constant string");
        }
        bool caseless = false;
        for (char c : o->value.GetValue<std::string>()) {
            if (c == 'c') {
                caseless = false;
            } else if (c == 'i') {
                caseless = true;
            } else if (c == 'g' && call->kind == ProviderRegexCall::Kind::REPLACE) {
                call->global = true;
            } else {
                throw NotImplementedException(call->what + ": the option '" + std::string(1, c) +
                                              "' is not supported");
            }
        }
        if (caseless) call->flags |= ZUGODB_REGEX_CASELESS;
    }
    if (auto *p = constant_of(args[1])) {
        if (p->value.IsNull()) {
            call->constant_null = true;
        } else {
            call->constant = ProviderRegex::Compile(api, call->what,
                                                    p->value.GetValue<std::string>(), call->flags);
            if (call->group > call->constant->GroupCount()) {
                throw InvalidInputException(call->what + ": the pattern has " +
                                            std::to_string(call->constant->GroupCount()) +
                                            " group(s), not " + std::to_string(call->group));
            }
        }
    }
    auto fn = std::make_unique<BoundFunction>(name, std::move(args), ret, false);
    fn->regex = std::move(call);
    return fn;
}

BoundExprPtr Binder::BindFunction(const FunctionExpression &expr, BindContext &context) {
    auto name = StringUtil::Upper(expr.function_name);
    // Canonical alias normalization — collapse SQL-standard synonyms
    // onto SlothDB's primary names so the binder/executor only need to
    // recognise one spelling. byte-length semantics (SlothDB's LENGTH
    // returns bytes today, matching the byte-oriented LEFT/RIGHT/
    // SUBSTRING family); true UTF-8 codepoint length is a separate
    // future change.
    if (name == "CHAR_LENGTH" || name == "CHARACTER_LENGTH") {
        name = "LENGTH";
    }
    bool is_agg = IsAggregateFunction(name);
    // DuckDB's aggregates the engine does not have. Bound as an unknown
    // scalar function, one was an error per row, so over no rows it gave
    // no row where DuckDB gives one (Stage 8, #12).
    static const char *const kMissingAggregates[] = {
        "ANY_VALUE", "ARBITRARY", "FIRST", "LAST", "ARG_MAX", "ARG_MIN", "ARGMAX", "ARGMIN",
        "MAX_BY", "MIN_BY", "ARG_MAX_NULL", "ARG_MIN_NULL", "APPROX_COUNT_DISTINCT",
        "APPROX_QUANTILE", "APPROX_TOP_K", "RESERVOIR_QUANTILE", "MEAN", "BIT_AND", "BIT_OR",
        "BIT_XOR", "BITSTRING_AGG", "COUNT_IF", "COUNTIF", "CORR", "COVAR_POP", "COVAR_SAMP",
        "ENTROPY", "FAVG", "FSUM", "SUMKAHAN", "KAHAN_SUM", "GEOMEAN", "GEOMETRIC_MEAN",
        "HISTOGRAM", "HISTOGRAM_EXACT", "KURTOSIS", "KURTOSIS_POP", "LIST", "ARRAY_AGG", "MAD",
        "MODE", "PRODUCT", "QUANTILE", "QUANTILE_CONT", "QUANTILE_DISC", "PERCENTILE_CONT",
        "PERCENTILE_DISC", "REGR_AVGX", "REGR_AVGY", "REGR_COUNT", "REGR_INTERCEPT", "REGR_R2",
        "REGR_SLOPE", "REGR_SXX", "REGR_SXY", "REGR_SYY", "SEM", "SKEWNESS", "SUM_NO_OVERFLOW",
        "WEIGHTED_AVG", "WAVG", "BOOL_XOR"};
    for (auto *m : kMissingAggregates) {
        if (name == m) throw NotImplementedException("Aggregate function " + name + " is not supported");
    }

    std::vector<BoundExprPtr> args;
    for (auto &arg : expr.arguments) {
        args.push_back(BindExpression(*arg, context));
    }
    // Arguments DuckDB refuses (#34): GCD, LCM and FACTORIAL of anything
    // but integers (a string was read as 0), and CONTAINS of two NULLs,
    // which matches more than one of DuckDB's overloads.
    if (name == "GCD" || name == "LCM" || name == "FACTORIAL") {
        for (auto &a : args) {
            auto t = a->GetReturnType().id();
            if (t != LogicalTypeId::TINYINT && t != LogicalTypeId::SMALLINT &&
                t != LogicalTypeId::INTEGER && t != LogicalTypeId::BIGINT &&
                t != LogicalTypeId::SQLNULL) {
                throw BinderException(ErrorCode::TYPE_MISMATCH,
                    "No function matches " + StringUtil::Lower(name) + "() of a " +
                    a->GetReturnType().ToString());
            }
        }
    }
    if (name == "CONTAINS" && !args.empty()) {
        bool all_null = true;
        for (auto &a : args) all_null = all_null && a->GetReturnType().id() == LogicalTypeId::SQLNULL;
        if (all_null) {
            throw BinderException(ErrorCode::TYPE_MISMATCH,
                "Could not choose a best candidate function for contains(NULL, NULL)");
        }
    }
    // Regular expressions are refused, every function of them (#32,
    // design L24): they come from a host's RE2 provider (#40).
    static const char *const kRegexFunctions[] = {
        "REGEXP_MATCHES", "REGEXP_FULL_MATCH", "REGEXP_REPLACE", "REGEXP_EXTRACT",
        "REGEXP_EXTRACT_ALL", "REGEXP_SPLIT_TO_ARRAY", "REGEXP_SPLIT_TO_TABLE",
        "STRING_SPLIT_REGEX", "STR_SPLIT_REGEX", "REGEXP_ESCAPE", "REGEXP", "REGEXP_LIKE",
        "REGEXP_MATCH", "SIMILAR_TO"};
    const ProviderSet *providers = catalog_.GetProviders();
    const zugodb_regex_v1 *regex_api = providers ? providers->Regex() : nullptr;
    if (regex_api && (name == "REGEXP_MATCHES" || name == "REGEXP_FULL_MATCH" ||
                      name == "REGEXP_REPLACE" || name == "REGEXP_EXTRACT")) {
        return BindRegexCall(name, std::move(args), *regex_api);
    }
    for (auto *f : kRegexFunctions) {
        if (name == f) RefuseRegex("Function " + StringUtil::Lower(name));
    }
    // The function registry (Stage 9, design L26): the names the engine
    // computes, the list docs/dialect.md gives (tools/check_dialect.sh
    // compares the two). Any other name is refused here; it bound with an
    // INTEGER type and failed per row, or gave a value DuckDB never gives.
    if (!IsRegisteredFunction(name)) {
        RefuseFunction(name);
    }
    for (const auto &f : kScalarFunctions) {
        if (name != f.name) continue;
        size_t n = args.size();
        if (n < f.lo || n > f.hi) {
            throw BinderException(ErrorCode::TYPE_MISMATCH,
                "No function matches " + StringUtil::Lower(name) + "() with " +
                std::to_string(n) + " argument(s); it takes " +
                (f.lo == f.hi     ? std::to_string(f.lo)
                 : f.hi == SIZE_MAX ? "at least " + std::to_string(f.lo)
                                    : std::to_string(f.lo) + " to " + std::to_string(f.hi)));
        }
        break;
    }
    // Text and number functions take the types DuckDB's do ('S' text, 'N'
    // a number, 'I' an integer; '*' is anything, read by the function
    // itself). Another type was read as a string_t:
    // UPPER(x LIKE y) read a BOOLEAN's byte as a string's header and past
    // its buffer (Stage 9's SQL fuzzer). DuckDB has no such function.
    {
        struct TextArgs { const char *name; const char *kinds; };
        static const TextArgs kTextArgs[] = {
            {"UPPER", "S"}, {"LOWER", "S"}, {"LENGTH", "S"}, {"STRLEN", "S"},
            {"REVERSE", "S"}, {"ASCII", "S"}, {"MD5", "S"}, {"SHA1", "S"}, {"SHA256", "S"},
            {"TRIM", "SS"}, {"LTRIM", "SS"}, {"RTRIM", "SS"}, {"CONTAINS", "SS"},
            {"STARTS_WITH", "SS"}, {"ENDS_WITH", "SS"}, {"PREFIX", "SS"}, {"SUFFIX", "SS"},
            {"INSTR", "SS"}, {"STRPOS", "SS"}, {"POSITION", "SS"}, {"TRANSLATE", "SSS"},
            {"REPLACE", "SSS"}, {"BIT_LENGTH", "S"}, {"OCTET_LENGTH", "S"},
            {"FROM_BASE64", "S"}, {"UNHEX", "S"}, {"LEFT", "S*"}, {"RIGHT", "S*"},
            {"REPEAT", "S*"}, {"SUBSTR", "S**"}, {"SUBSTRING", "S**"}, {"LPAD", "S*S"},
            {"RPAD", "S*S"}, {"SPLIT_PART", "SS*"}, {"STRFTIME", "**"},
            // Numbers ('N'), integers ('I'): SQRT of a VARCHAR column read its
            // bytes as a double, ROUND of a DATE gave its day number.
            {"ABS", "N"}, {"ACOS", "N"}, {"ACOSH", "N"}, {"ASIN", "N"}, {"ASINH", "N"},
            {"ATAN", "N"}, {"ATAN2", "NN"}, {"ATANH", "N"}, {"CBRT", "N"}, {"CEIL", "N"},
            {"CEILING", "N"}, {"COS", "N"}, {"COSH", "N"}, {"DEGREES", "N"}, {"EXP", "N"},
            {"FACTORIAL", "I"}, {"FLOOR", "N"}, {"GCD", "II"}, {"LCM", "II"},
            {"ISFINITE", "N"}, {"ISINF", "N"}, {"ISNAN", "N"}, {"LN", "N"}, {"LOG", "NN"},
            {"LOG10", "N"}, {"LOG2", "N"}, {"MOD", "NN"}, {"POW", "NN"}, {"POWER", "NN"},
            {"RADIANS", "N"}, {"ROUND", "NI"}, {"SIGN", "N"}, {"SIN", "N"}, {"SINH", "N"},
            {"SQRT", "N"}, {"TAN", "N"}, {"TANH", "N"}, {"TRUNC", "NI"}, {"CHR", "I"},
            {"MAKE_DATE", "III"}, {"MAKE_TIMESTAMP", "IIIIIN"},
        };
        for (const auto &t : kTextArgs) {
            if (name != t.name) continue;
            for (size_t k = 0; k < args.size() && t.kinds[k] != '\0'; k++) {
                if (t.kinds[k] == '*') continue;
                auto id = args[k]->GetReturnType().id();
                bool integer = id == LogicalTypeId::TINYINT || id == LogicalTypeId::SMALLINT ||
                               id == LogicalTypeId::INTEGER || id == LogicalTypeId::BIGINT;
                bool number = integer || id == LogicalTypeId::FLOAT || id == LogicalTypeId::DOUBLE;
                bool ok = id == LogicalTypeId::SQLNULL ||
                          (t.kinds[k] == 'S' && (id == LogicalTypeId::VARCHAR ||
                                                 id == LogicalTypeId::BLOB)) ||
                          (t.kinds[k] == 'N' && number) || (t.kinds[k] == 'I' && integer);
                if (!ok) {
                    throw BinderException(ErrorCode::TYPE_MISMATCH,
                        "No function matches " + StringUtil::Lower(name) + "() of a " +
                        args[k]->GetReturnType().ToString());
                }
            }
            break;
        }
    }
    // Date and time functions take a DATE or a TIMESTAMP, as DuckDB's do.
    // Another argument was read as one: an INTEGER as seconds, a BIGINT as
    // seconds or microseconds by its size, a TIME as midnight
    // (EXTRACT(YEAR FROM bigint_col), STRFTIME(1, '%Y')), where DuckDB has
    // no such function (Stage 9). EPOCH_MS(INTEGER), DuckDB's TIMESTAMP
    // from milliseconds, is refused with them.
    {
        struct DateArgs { const char *name; int first, last; bool timestamp_only; };
        static const DateArgs kDateArgs[] = {
            {"DAYNAME", 0, 0, false},   {"MONTHNAME", 0, 0, false}, {"YEAR", 0, 0, false},
            {"MONTH", 0, 0, false},     {"DAY", 0, 0, false},       {"HOUR", 0, 0, false},
            {"MINUTE", 0, 0, false},    {"SECOND", 0, 0, false},    {"DAYOFWEEK", 0, 0, false},
            {"DAYOFMONTH", 0, 0, false}, {"DAYOFYEAR", 0, 0, false}, {"QUARTER", 0, 0, false},
            {"WEEK", 0, 0, false},      {"ISODOW", 0, 0, false},    {"ISOYEAR", 0, 0, false},
            {"CENTURY", 0, 0, false},   {"DECADE", 0, 0, false},    {"MILLENNIUM", 0, 0, false},
            {"LAST_DAY", 0, 0, false},  {"EXTRACT", 1, 1, false},   {"DATE_PART", 1, 1, false},
            {"DATE_TRUNC", 1, 1, false}, {"DATE_DIFF", 1, 2, false}, {"DATEDIFF", 1, 2, false},
            {"EPOCH_MS", 0, 0, true},
        };
        for (const auto &d : kDateArgs) {
            if (name != d.name) continue;
            if (name == "DATE_DIFF" || name == "DATEDIFF") {
                if (args.size() != 3) {
                    throw BinderException(ErrorCode::TYPE_MISMATCH,
                        "No function matches " + StringUtil::Lower(name) + "() with " +
                        std::to_string(args.size()) +
                        " argument(s); it takes 3 (part, start, end), as DuckDB's (design L4)");
                }
            }
            for (int k = d.first; k <= d.last; k++) {
                if (static_cast<size_t>(k) >= args.size()) break;
                auto t = args[k]->GetReturnType().id();
                bool ok = t == LogicalTypeId::TIMESTAMP || t == LogicalTypeId::SQLNULL ||
                          (!d.timestamp_only && t == LogicalTypeId::DATE);
                if (!ok) {
                    throw BinderException(ErrorCode::TYPE_MISMATCH,
                        "No function matches " + StringUtil::Lower(name) + "() of a " +
                        args[k]->GetReturnType().ToString());
                }
            }
            break;
        }
        if (name == "STRFTIME") {
            if (args.size() != 2) {
                throw BinderException(ErrorCode::TYPE_MISMATCH,
                    "No function matches strftime() with " + std::to_string(args.size()) +
                    " argument(s); it takes 2");
            }
            auto t0 = args[0]->GetReturnType().id(), t1 = args[1]->GetReturnType().id();
            auto when = [](LogicalTypeId t) {
                return t == LogicalTypeId::DATE || t == LogicalTypeId::TIMESTAMP ||
                       t == LogicalTypeId::SQLNULL;
            };
            auto text = [](LogicalTypeId t) {
                return t == LogicalTypeId::VARCHAR || t == LogicalTypeId::SQLNULL;
            };
            if (!((when(t0) && text(t1)) || (text(t0) && when(t1)))) {
                throw BinderException(ErrorCode::TYPE_MISMATCH,
                    "No function matches strftime(" + args[0]->GetReturnType().ToString() + ", " +
                    args[1]->GetReturnType().ToString() + ")");
            }
        }
    }
    // A BLOB argument: the functions that read a BLOB's bytes as DuckDB
    // does. The others read it as text, gave a VARCHAR of its bytes or
    // unified it with no type (#31).
    for (auto &a : args) {
        if (a->GetReturnType().id() != LogicalTypeId::BLOB) continue;
        static const char *const kBlobFunctions[] = {
            "COUNT", "MIN", "MAX", "OCTET_LENGTH", "MD5", "SHA1", "SHA256"};
        bool ok = false;
        for (auto *f : kBlobFunctions) ok = ok || name == f;
        if (!ok) {
            throw NotImplementedException("Function " + name + " of a BLOB is not supported");
        }
    }
    // STRING_AGG's separator: a constant string (NULL and a column are
    // not computed as DuckDB does).
    if ((name == "STRING_AGG" || name == "LISTAGG" || name == "GROUP_CONCAT") && args.size() == 2) {
        bool ok = args[1]->GetExpressionType() == BoundExpressionType::CONSTANT &&
                  !static_cast<const BoundConstant &>(*args[1]).value.IsNull() &&
                  static_cast<const BoundConstant &>(*args[1]).value.type().id() == LogicalTypeId::VARCHAR;
        if (!ok) throw NotImplementedException(name + " with a separator other than a string constant is not supported");
    }

    // Reject nested aggregates (`SUM(COUNT(*))`, `AVG(SUM(x))`). SQL
    // standard forbids aggregate calls inside aggregate arguments.
    // Previously this crashed the executor with a misleading
    // "Function execution for: COUNT" because the inner aggregate
    // was handed to the per-row scalar evaluator.
    if (is_agg) {
        for (auto &a : args) {
            if (a && ContainsAggregate(*a)) {
                throw BinderException(ErrorCode::TYPE_MISMATCH,
                    "aggregate function calls cannot be nested");
            }
        }
    }

    // Validate aggregate arity. SQL standard: most aggregates take
    // exactly one operand. COUNT additionally allows zero args via
    // the COUNT(*) form (parser sets expr.is_star). Without these
    // checks, COUNT(a,b) silently returned COUNT(a) — silent wrong
    // result that hid data-quality issues — and MIN() / SUM() with
    // zero args silently returned NULL/0.
    if (is_agg) {
        size_t n = args.size();
        auto need = [&](size_t lo, size_t hi) {
            if (n < lo || n > hi) {
                throw BinderException(ErrorCode::TYPE_MISMATCH,
                    name + " expects " +
                    (lo == hi ? std::to_string(lo)
                              : std::to_string(lo) + "-" + std::to_string(hi)) +
                    " argument(s), got " + std::to_string(n));
            }
        };
        if (name == "COUNT") {
            if (!expr.is_star) need(1, 1);
            // else 0 args is fine
        } else if (name == "STRING_AGG" || name == "LISTAGG" ||
                   name == "GROUP_CONCAT") {
            need(1, 2);
        } else if (name == "BOOL_AND" || name == "BOOL_OR" ||
                   name == "MIN" || name == "MAX" ||
                   name == "SUM" || name == "AVG" ||
                   name == "MEDIAN" ||
                   name == "STDDEV" || name == "STDDEV_SAMP" ||
                   name == "STDDEV_POP" || name == "VARIANCE" ||
                   name == "VAR_SAMP" || name == "VAR_POP") {
            need(1, 1);
        }
    }

    // Scalar arity, as DuckDB's signatures (libzugodb Stage 7, #11). A
    // call with too few arguments read past the end of the executor's
    // argument vector; too many were ignored. The executor also checks
    // each argument it reads (Arg in expression_executor.cpp), for the
    // functions not listed here.
    if (!is_agg) {
        struct ScalarArity { const char *name; size_t lo, hi; };
        static const ScalarArity kScalarArity[] = {
            {"ASCII", 1, 1},       {"BIT_LENGTH", 1, 1},   {"OCTET_LENGTH", 1, 1},
            {"LENGTH", 1, 1},      {"STRLEN", 1, 1},       {"REVERSE", 1, 1},
            {"UPPER", 1, 1},       {"LOWER", 1, 1},        {"MD5", 1, 1},
            {"SHA1", 1, 1},        {"SHA256", 1, 1},       {"REPEAT", 2, 2},
            {"REPLACE", 3, 3},     {"TRANSLATE", 3, 3},    {"SPLIT_PART", 3, 3},
            {"LPAD", 3, 3},        {"RPAD", 3, 3},         {"LEFT", 2, 2},
            {"RIGHT", 2, 2},       {"TRIM", 1, 2},         {"LTRIM", 1, 2},
            {"RTRIM", 1, 2},       {"CONCAT_WS", 2, SIZE_MAX},
            {"GCD", 2, 2},         {"LCM", 2, 2},          {"FACTORIAL", 1, 1},
            {"CHR", 1, 1},
        };
        for (const auto &a : kScalarArity) {
            if (name != a.name) continue;
            size_t n = args.size();
            if (n < a.lo || n > a.hi) {
                throw BinderException(ErrorCode::TYPE_MISMATCH,
                    "No function matches " + StringUtil::Lower(name) + "() with " +
                    std::to_string(n) + " argument(s); it takes " +
                    (a.lo == a.hi     ? std::to_string(a.lo)
                     : a.hi == SIZE_MAX ? "at least " + std::to_string(a.lo)
                                        : std::to_string(a.lo) + " to " + std::to_string(a.hi)));
            }
            break;
        }
    }

    // Determine return type.
    LogicalType return_type = LogicalType::INTEGER(); // default
    if (name == "COUNT") {
        return_type = LogicalType::BIGINT();
    } else if (name == "SUM" || name == "AVG") {
        // SUM/AVG require a numeric operand. The accumulator is `double`
        // (and casts to BIGINT for SUM's integer types at emit). Allowing
        // VARCHAR / BOOLEAN / STRUCT / LIST inputs through has been silently
        // wrong:
        //   AVG('a','b','c')  -> 0
        //   SUM('1','2','3')  -> 0    (no string-to-number coercion happens
        //                              in the hot loop; ReadDouble returns
        //                              0.0 for VARCHAR)
        //   SUM(true,false)   -> false  (treated as bool aggregate; wrong)
        // Per SQL standard these should be type errors. Reject at bind.
        // SQLNULL is allowed (degenerate column of NULLs returns NULL/0
        // through the existing empty-set path).
        if (!args.empty()) {
            auto arg_id = args[0]->GetReturnType().id();
            bool numeric =
                arg_id == LogicalTypeId::TINYINT  || arg_id == LogicalTypeId::SMALLINT ||
                arg_id == LogicalTypeId::INTEGER  || arg_id == LogicalTypeId::BIGINT   ||
                arg_id == LogicalTypeId::HUGEINT  ||
                arg_id == LogicalTypeId::UTINYINT || arg_id == LogicalTypeId::USMALLINT ||
                arg_id == LogicalTypeId::UINTEGER || arg_id == LogicalTypeId::UBIGINT  ||
                arg_id == LogicalTypeId::FLOAT    || arg_id == LogicalTypeId::DOUBLE   ||
                arg_id == LogicalTypeId::DECIMAL  || arg_id == LogicalTypeId::SQLNULL;
            if (!numeric) {
                throw BinderException(ErrorCode::TYPE_MISMATCH,
                    name + " requires a numeric argument, got " +
                    args[0]->GetReturnType().ToString());
            }
        }
        if (name == "SUM") {
            return_type = args.empty() ? LogicalType::BIGINT() : args[0]->GetReturnType();
            // SUM of small integers widens to BIGINT to give more headroom
            // before overflow (still uses double internally — full-fidelity
            // BIGINT SUM requires an integer accumulator, deferred).
            if (return_type.id() == LogicalTypeId::INTEGER ||
                return_type.id() == LogicalTypeId::SMALLINT ||
                return_type.id() == LogicalTypeId::TINYINT) {
                return_type = LogicalType::BIGINT();
            }
            // DuckDB's SUM of a FLOAT is a DOUBLE; typed FLOAT, the
            // aggregate wrote its double sum into a float slot and read
            // back 0 (#36).
            if (return_type.id() == LogicalTypeId::FLOAT) {
                return_type = LogicalType::DOUBLE();
            }
        } else {
            return_type = LogicalType::DOUBLE();
        }
    } else if (name == "MIN" || name == "MAX") {
        return_type = args.empty() ? LogicalType::INTEGER() : args[0]->GetReturnType();
    }
    // Scalar function return types.
    else if (name == "LENGTH" || name == "STRLEN" || name == "ASCII" ||
             name == "OCTET_LENGTH" || name == "BIT_LENGTH") {
        return_type = LogicalType::INTEGER();
    } else if (name == "UPPER" || name == "LOWER" || name == "TRIM" ||
               name == "LTRIM" || name == "RTRIM" || name == "REPLACE" ||
               name == "SUBSTRING" || name == "SUBSTR" || name == "CONCAT" ||
               name == "CONCAT_WS" || name == "CHR" ||
               name == "HEX" || name == "TO_HEX" || name == "UNHEX" ||
               name == "MD5" || name == "SHA1" || name == "SHA_1" ||
               name == "SHA256" || name == "SHA2_256" ||
               name == "SHA512" || name == "SHA2_512" ||
               name == "BASE64_ENCODE" || name == "BASE64_DECODE" ||
               name == "TO_BASE64" || name == "FROM_BASE64" ||
               name == "TRANSLATE") {
        return_type = LogicalType::VARCHAR();
    } else if (name == "GCD" || name == "LCM" || name == "FACTORIAL") {
        return_type = LogicalType::BIGINT();
    } else if (name == "ABS") {
        return_type = args.empty() ? LogicalType::INTEGER() : args[0]->GetReturnType();
    } else if ((name == "ROUND" || name == "TRUNC") && !args.empty() &&
               IntegerRank(args[0]->GetReturnType().id()) > 0) {
        // DuckDB's ROUND and TRUNC of an integer keep its type (typeof
        // round(7::TINYINT, -1) is TINYINT); they were DOUBLEs, whose text
        // is "7.0" since #42.
        return_type = args[0]->GetReturnType();
    } else if (name == "MOD" && args.size() == 2 && expr.arguments.size() == 2 &&
               IntegerRank(args[0]->GetReturnType().id()) > 0 &&
               IntegerRank(args[1]->GetReturnType().id()) > 0) {
        // MOD of integers is DuckDB's `%`: the operands' integer type
        // (mod(42, 5) is an INTEGER, mod(42::TINYINT, 5) a TINYINT) and
        // "Division by zero" for 0. It was a DOUBLE (#42).
        int64_t lit = 0;
        bool left_lit = IntegerLiteralValue(*expr.arguments[0], lit);
        int64_t rlit = 0;
        bool right_lit = IntegerLiteralValue(*expr.arguments[1], rlit);
        if (left_lit && !right_lit) NarrowIntegerLiteral(args[0], lit, args[1]->GetReturnType());
        else if (right_lit && !left_lit) NarrowIntegerLiteral(args[1], rlit, args[0]->GetReturnType());
        auto type = ResolveArithmeticType(args[0]->GetReturnType(), args[1]->GetReturnType());
        return std::make_unique<BoundArithmetic>("%", std::move(args[0]), std::move(args[1]), type);
    } else if (name == "CEIL" || name == "CEILING" || name == "FLOOR" ||
               name == "ROUND" || name == "SQRT" || name == "CBRT" ||
               name == "POWER" || name == "POW" || name == "MOD") {
        return_type = LogicalType::DOUBLE();
    } else if (name == "COALESCE" || name == "IFNULL" || name == "NVL") {
        // Result type unifies across all args via SQL numeric promotion
        // (matches CASE/IF/IIF logic below). Previously this used the
        // first non-NULL-typed arg, which caused the same wrong-result
        // bugs as the pre-fix CASE branch:
        //   COALESCE(1, 2.5)                     -> 1   (DOUBLE 2.5 cast
        //                                                to INT slot)
        //   COALESCE(int_col, double_col)         -> 0   (when int_col is
        //                                                 NULL, DOUBLE
        //                                                 reads as garbage
        //                                                 from INT slot)
        //   COALESCE(int_col, 'foo')              -> int_col (varchar
        //                                                  silently dropped)
        {
            std::vector<const BoundExpression *> branch;
            for (auto &a : args) branch.push_back(a.get());
            return_type = UnifyTypes(branch, name);
        }
        // Wrap mismatched args in BoundCast so the executor reads each
        // through a type-compatible buffer. SQLNULL is preserved (NULL
        // is type-agnostic — the executor uses validity).
        for (auto &a : args) {
            if (a->GetReturnType().id() != return_type.id() &&
                a->GetReturnType().id() != LogicalTypeId::SQLNULL) {
                a = std::make_unique<BoundCast>(std::move(a), return_type);
            }
        }
    } else if (name == "NULLIF") {
        // Same unification pattern as CASE/COALESCE. Without it,
        //   NULLIF(1, 1.0) returns 1 instead of NULL — the DOUBLE 1.0
        //   was read through an INTEGER Value union slot and compared
        //   as a different value, so the equality check failed.
        {
            std::vector<const BoundExpression *> branch;
            for (auto &a : args) branch.push_back(a.get());
            return_type = UnifyTypes(branch, name);
        }
        for (auto &a : args) {
            if (a->GetReturnType().id() != return_type.id() &&
                a->GetReturnType().id() != LogicalTypeId::SQLNULL) {
                a = std::make_unique<BoundCast>(std::move(a), return_type);
            }
        }
    } else if (name == "CASE" || name == "IF" || name == "IIF") {
        // CASE(when, then, [when, then, ...] [else]) — args layout:
        //   indices 0, 2, 4, ... = WHEN predicates
        //   indices 1, 3, 5, ... = THEN expressions
        //   last arg when size is odd = ELSE expression
        // SQL standard: result type unifies all THEN + ELSE types via
        // numeric promotion (widest wins), or falls back to VARCHAR when
        // mixing string and non-string, or SQLNULL when every branch is
        // NULL. Previously the return type was hardcoded to the first
        // THEN's type, so CASE WHEN 1=2 THEN 1 ELSE 2.5 END returned 0
        // (the DOUBLE 2.5 was written into an INTEGER-typed Value union
        // slot and read back as garbage). Same pattern for any mixed
        // type branch.
        // Each WHEN is a condition (AsCondition).
        for (size_t i = 0; i + 1 < args.size(); i += 2) AsCondition(args[i]);
        {
            std::vector<const BoundExpression *> branch;
            for (size_t i = 1; i < args.size(); i += 2) branch.push_back(args[i].get());
            if (args.size() % 2 == 1 && !args.empty()) branch.push_back(args.back().get());
            return_type = UnifyTypes(branch, "CASE");
        }
        // Wrap mismatched THEN/ELSE in a BoundCast so the executor's
        // per-row write into the unified-type result Vector reads from
        // a compatible source. SQLNULL is preserved verbatim (NULL is
        // type-agnostic; ExpressionExecutor handles it via validity).
        for (size_t i = 1; i < args.size(); i += 2) {
            if (args[i]->GetReturnType().id() != return_type.id() &&
                args[i]->GetReturnType().id() != LogicalTypeId::SQLNULL) {
                args[i] = std::make_unique<BoundCast>(std::move(args[i]), return_type);
            }
        }
        if (args.size() % 2 == 1 && !args.empty()) {
            auto &last = args.back();
            if (last->GetReturnType().id() != return_type.id() &&
                last->GetReturnType().id() != LogicalTypeId::SQLNULL) {
                last = std::make_unique<BoundCast>(std::move(last), return_type);
            }
        }
    } else if (name == "IN" || name == "BETWEEN") {
        return_type = LogicalType::BOOLEAN();
        // The value and the list, or the bounds, compared in one type, as
        // DuckDB casts them to theirs (#42). The executor compares Values,
        // and two of different types compared as neither less nor equal:
        // a BIGINT or DOUBLE BETWEEN 1 AND 10 was true of every row, and
        // IN (2, 3) of none; a DATE BETWEEN two strings was always true.
        std::vector<const BoundExpression *> all;
        for (auto &a : args) all.push_back(a.get());
        auto common = UnifyTypes(all, name);
        if (common.id() != LogicalTypeId::SQLNULL) {
            for (auto &a : args) {
                if (a->GetReturnType().id() != common.id() &&
                    a->GetReturnType().id() != LogicalTypeId::SQLNULL) {
                    a = std::make_unique<BoundCast>(std::move(a), common);
                }
            }
        }
    } else if (name == "NOW" || name == "CURRENT_TIMESTAMP" ||
               name == "LOCALTIMESTAMP" || name == "MAKE_TIMESTAMP" ||
               name == "GETDATE" || name == "FROM_UNIXTIME") {
        return_type = LogicalType::TIMESTAMP();
    } else if (name == "CURRENT_USER" || name == "SESSION_USER" ||
               name == "USER" || name == "CURRENT_SCHEMA" ||
               name == "CURRENT_DATABASE" || name == "VERSION" ||
               name == "UUID" || name == "GEN_RANDOM_UUID" ||
               name == "CURTIME") {
        return_type = LogicalType::VARCHAR();
    } else if (name == "TO_TIMESTAMP" || name == "DATE_TRUNC") {
        return_type = LogicalType::BIGINT(); // microseconds since epoch
    } else if (name == "CURRENT_DATE" || name == "CURDATE") {
        return_type = LogicalType::DATE();
    } else if (name == "EXTRACT" || name == "DATE_PART" ||
               // PG/MySQL 1-arg date-part aliases — dispatched through the
               // EXTRACT executor branch.
               name == "YEAR" || name == "MONTH" || name == "DAY" ||
               name == "DAYOFMONTH" || name == "DAYOFWEEK" || name == "DAYOFYEAR" ||
               name == "HOUR" || name == "MINUTE" || name == "SECOND" ||
               name == "WEEK" || name == "QUARTER" ||
               name == "MILLENNIUM" || name == "CENTURY" || name == "DECADE" ||
               name == "ISODOW" || name == "ISOYEAR" ||
               name == "UNIX_TIMESTAMP") {
        return_type = LogicalType::BIGINT();
    } else if (name == "EPOCH_MS") {
        return_type = LogicalType::DOUBLE();
    }
    // Additional string functions.
    else if (name == "POSITION" || name == "STRPOS" || name == "INSTR") {
        return_type = LogicalType::INTEGER();
    } else if (name == "LEFT" || name == "RIGHT" || name == "LPAD" || name == "RPAD" ||
               name == "REVERSE" || name == "REPEAT" || name == "SPLIT_PART" ||
               name == "INITCAP") {
        return_type = LogicalType::VARCHAR();
    } else if (name == "STARTS_WITH" || name == "ENDS_WITH" || name == "CONTAINS" ||
               name == "PREFIX" || name == "SUFFIX") {
        return_type = LogicalType::BOOLEAN();
    }
    // Additional math functions.
    else if (name == "LOG" || name == "LN" || name == "LOG2" || name == "LOG10" ||
             name == "EXP" || name == "PI" || name == "RANDOM" || name == "RAND" ||
             name == "SIN" || name == "COS" || name == "TAN" ||
             name == "ASIN" || name == "ACOS" || name == "ATAN" ||
             name == "SINH" || name == "COSH" || name == "TANH" ||
             name == "ASINH" || name == "ACOSH" || name == "ATANH") {
        return_type = LogicalType::DOUBLE();
    } else if (name == "ISNAN" || name == "ISINF" || name == "ISFINITE") {
        return_type = LogicalType::BOOLEAN();
    } else if (name == "SIGN") {
        return_type = LogicalType::INTEGER();
    } else if (name == "ATAN2" || name == "DEGREES" || name == "RADIANS" ||
               name == "TRUNC" || name == "TRUNCATE") {
        return_type = LogicalType::DOUBLE();
    } else if (name == "INITCAP") {
        return_type = LogicalType::VARCHAR();
    } else if (name == "EXISTS") {
        return_type = LogicalType::BOOLEAN();
    } else if (name == "LEAST" || name == "GREATEST") {
        // Same numeric/string unification as COALESCE/NULLIF: walk all
        // args, widen to the highest numeric type, fall back to VARCHAR
        // when types mix, SQLNULL when every arg is NULL. Without this,
        // GREATEST(1, 2.5) returned 1 because args[0]'s INTEGER type
        // truncated the DOUBLE second arg at the result-Vector write.
        {
            std::vector<const BoundExpression *> branch;
            for (auto &a : args) branch.push_back(a.get());
            return_type = UnifyTypes(branch, name);
        }
        for (auto &a : args) {
            if (a->GetReturnType().id() != return_type.id() &&
                a->GetReturnType().id() != LogicalTypeId::SQLNULL) {
                a = std::make_unique<BoundCast>(std::move(a), return_type);
            }
        }
    }
    // Additional date functions.
    else if (name == "DATE_DIFF" || name == "DATEDIFF" || name == "TIMESTAMPDIFF" ||
             name == "AGE") {
        return_type = LogicalType::BIGINT();
    } else if (name == "DATE_ADD" || name == "DATEADD" ||
               name == "TIMESTAMPADD" || name == "DATE_SUB" || name == "SUBDATE") {
        // Return type:
        // - TIMESTAMP if input is TIMESTAMP, or if the unit is sub-day
        //   (HOUR/MINUTE/SECOND/MILLISECOND/MICROSECOND) — the result
        //   no longer aligns to a DATE.
        // - DATE if input is DATE AND unit is DAY/WEEK (statically
        //   determinable from a literal first arg).
        // The executor independently re-checks the unit so a non-literal
        // part still produces correct values at runtime; the binder
        // sets the static upper-bound type so downstream consumers
        // (CAST, comparisons, storage) see the correct shape.
        // Accept both arg orders (DATEADD(unit,N,date) and
        // DATE_ADD(date,N,unit)) — mirror the executor's detection so
        // the result type matches. The unit is the VARCHAR argument.
        idx_t b_unit_idx = 0, b_date_idx = 2;
        if (args.size() >= 3 &&
            args[2]->GetReturnType().id() == LogicalTypeId::VARCHAR &&
            args[0]->GetReturnType().id() != LogicalTypeId::VARCHAR) {
            b_unit_idx = 2; b_date_idx = 0;
        }
        bool input_is_date = false;
        if (args.size() >= 3 &&
            args[b_date_idx]->GetReturnType().id() == LogicalTypeId::DATE) {
            input_is_date = true;
        }
        bool day_grain = false;
        if (input_is_date && args.size() >= 3) {
            // Peek if the unit arg is a string literal we can read at
            // bind time. If so, decide DATE vs TIMESTAMP; if not, fall
            // back to TIMESTAMP to be safe.
            if (auto *bc = dynamic_cast<BoundConstant *>(args[b_unit_idx].get())) {
                if (bc->value.type().id() == LogicalTypeId::VARCHAR) {
                    auto part = StringUtil::Upper(bc->value.GetValue<std::string>());
                    day_grain = (part == "DAY" || part == "DAYS" ||
                                 part == "WEEK" || part == "WEEKS" ||
                                 part == "MONTH" || part == "MONTHS" ||
                                 part == "QUARTER" || part == "QUARTERS" ||
                                 part == "YEAR" || part == "YEARS");
                }
            }
        }
        if (input_is_date && day_grain) {
            return_type = LogicalType::DATE();
        } else {
            return_type = LogicalType::TIMESTAMP();
        }
    } else if (name == "STRFTIME" || name == "FORMAT_TIMESTAMP" ||
               name == "DATE_FORMAT" ||
               name == "MONTHNAME" || name == "DAYNAME") {
        return_type = LogicalType::VARCHAR();
    } else if (name == "LAST_DAY") {
        return_type = LogicalType::DATE();
    } else if (name == "MAKE_DATE") {
        return_type = LogicalType::DATE();
    }
    // Aggregate functions that work both as window and regular.
    else if (name == "STRING_AGG" || name == "LISTAGG" || name == "GROUP_CONCAT") {
        return_type = LogicalType::VARCHAR();
        is_agg = true;
    } else if (name == "STDDEV" || name == "STDDEV_SAMP" || name == "STDDEV_POP" ||
               name == "VARIANCE" || name == "VAR_SAMP" || name == "VAR_POP") {
        return_type = LogicalType::DOUBLE();
        is_agg = true;
    } else if (name == "BOOL_AND" || name == "BOOL_OR") {
        return_type = LogicalType::BOOLEAN();
        is_agg = true;
    } else if (name == "MEDIAN") {
        return_type = LogicalType::DOUBLE();
        is_agg = true;
    }

    // SQL:2003 FILTER (WHERE ...) — bind in the same row context as the
    // function arguments. Only valid on aggregate functions; reject on
    // scalars.
    //
    // Strategy: rewrite to a CASE-lift at bind time. FUNC(arg) FILTER (WHERE c)
    // becomes FUNC(CASE WHEN c THEN arg END). The CASE returns NULL when c is
    // false (or NULL — matches SQL standard), and SUM / COUNT / AVG / MIN /
    // MAX all already treat NULL as absent. For COUNT(*) which has no arg,
    // the rewrite is COUNT(CASE WHEN c THEN 1 END). This keeps every code
    // path that handles `col_idx == INVALID_INDEX` (the per-row expression
    // eval branch in ComputeAggregates) on the same hot path — no separate
    // filter plumbing needed in the aggregator.
    if (expr.filter) {
        if (!is_agg) {
            throw BinderException(ErrorCode::TYPE_MISMATCH,
                "FILTER clause is only valid on aggregate functions, not on '" + name + "'");
        }
        auto bound_filter = BindExpression(*expr.filter, context);
        if (args.empty()) {
            // COUNT(*) FILTER (WHERE c) -> COUNT(CASE WHEN c THEN 1 END).
            std::vector<BoundExprPtr> case_args;
            case_args.push_back(std::move(bound_filter));
            case_args.push_back(std::make_unique<BoundConstant>(Value::INTEGER(1)));
            args.push_back(std::make_unique<BoundFunction>(
                "CASE", std::move(case_args), LogicalType::INTEGER(), false));
        } else if (args.size() == 1) {
            // FUNC(arg) FILTER (WHERE c) -> FUNC(CASE WHEN c THEN arg END).
            auto arg_type = args[0]->GetReturnType();
            std::vector<BoundExprPtr> case_args;
            case_args.push_back(std::move(bound_filter));
            case_args.push_back(std::move(args[0]));
            args[0] = std::make_unique<BoundFunction>(
                "CASE", std::move(case_args), arg_type, false);
        } else {
            // Multi-arg aggregates (STRING_AGG, etc) with FILTER need each
            // arg wrapped in the same CASE. The filter must be cloneable
            // for that; current BoundExpression has no clone. Reject
            // explicitly so this surfaces at bind time rather than being
            // silently wrong.
            throw BinderException(ErrorCode::TYPE_MISMATCH,
                "FILTER on multi-argument aggregate '" + name + "' is not yet supported");
        }
    }

    auto bound_fn = std::make_unique<BoundFunction>(name, std::move(args), return_type,
                                                    is_agg, expr.is_distinct);
    // LENGTH counts code points through the host's UTF-8 table when it has
    // one (Stage 11.1, design L29).
    if (name == "LENGTH" && providers) bound_fn->utf8 = providers->Utf8();
    if (name == "MOD" && bound_fn->arguments.size() == 2 && expr.arguments.size() == 2) {
        bound_fn->decimal_mod = !DuckDBFloating(*expr.arguments[0], *bound_fn->arguments[0]) &&
                                !DuckDBFloating(*expr.arguments[1], *bound_fn->arguments[1]);
    }
    return bound_fn;
}

// A decimal literal (`2.5`, `-2.5`; not `25e-1`) cast to an integer type,
// as DuckDB casts its DECIMAL: rounded half away from zero (2.5 is 3), from
// the literal's digits. The engine reads the literal as a DOUBLE, whose
// cast rounds half to even (2.5 was 2) (Stage 9). False when `e` is not
// such a literal.
static bool DecimalLiteralToInteger(const ParsedExpression &e, LogicalTypeId target,
                                    bool is_try, Value &out) {
    const ParsedExpression *x = &e;
    bool negative = false;
    while (x->GetExpressionType() == ExpressionType::UNARY_MINUS) {
        negative = !negative;
        x = static_cast<const UnaryMinusExpression *>(x)->child.get();
    }
    if (x->GetExpressionType() != ExpressionType::CONSTANT) return false;
    auto &c = static_cast<const ConstantExpression &>(*x);
    if (c.is_null || c.literal_type != TokenType::FLOAT_LITERAL) return false;
    const std::string &t = c.value;
    if (t.find_first_of("eE") != std::string::npos) return false;
    auto dot = t.find('.');
    std::string whole = dot == std::string::npos ? t : t.substr(0, dot);
    bool round_up = dot != std::string::npos && dot + 1 < t.size() && t[dot + 1] >= '5';
    // The magnitude, digit by digit, checked against 2^63.
    unsigned long long mag = 0;
    const unsigned long long kLimit = 9223372036854775808ULL;
    bool over = false;
    for (char ch : whole) {
        if (ch < '0' || ch > '9') return false;
        if (mag > (kLimit - static_cast<unsigned>(ch - '0')) / 10) { over = true; break; }
        mag = mag * 10 + static_cast<unsigned>(ch - '0');
    }
    if (!over && round_up) {
        if (mag >= kLimit) over = true; else mag++;
    }
    long long lo, hi;
    switch (target) {
    case LogicalTypeId::TINYINT: lo = INT8_MIN; hi = INT8_MAX; break;
    case LogicalTypeId::SMALLINT: lo = INT16_MIN; hi = INT16_MAX; break;
    case LogicalTypeId::INTEGER: lo = INT32_MIN; hi = INT32_MAX; break;
    case LogicalTypeId::BIGINT: lo = INT64_MIN; hi = INT64_MAX; break;
    default: return false;
    }
    bool fits = !over && (negative ? mag <= static_cast<unsigned long long>(-(lo + 1)) + 1
                                   : mag <= static_cast<unsigned long long>(hi));
    // TRY_CAST past the range is NULL: the DOUBLE path gives it.
    if (!fits && is_try) return false;
    if (!fits) {
        throw ConversionException("Failed to cast decimal value " + std::string(negative ? "-" : "") +
                                  t + " to type " + LogicalType(target).ToString());
    }
    long long v = negative ? (mag == 0 ? 0 : -static_cast<long long>(mag - 1) - 1)
                           : static_cast<long long>(mag);
    switch (target) {
    case LogicalTypeId::TINYINT: out = Value::TINYINT(static_cast<int8_t>(v)); break;
    case LogicalTypeId::SMALLINT: out = Value::SMALLINT(static_cast<int16_t>(v)); break;
    case LogicalTypeId::INTEGER: out = Value::INTEGER(static_cast<int32_t>(v)); break;
    default: out = Value::BIGINT(static_cast<int64_t>(v)); break;
    }
    return true;
}

BoundExprPtr Binder::BindCast(const CastExpression &expr, BindContext &context) {
    auto target = ResolveTypeName(expr.target_type);
    Value literal;
    if (DecimalLiteralToInteger(*expr.child, target.id(), expr.is_try, literal)) {
        return std::make_unique<BoundConstant>(literal);
    }
    auto child = BindExpression(*expr.child, context);
    CheckBlobCast(child->GetReturnType(), target);
    return std::make_unique<BoundCast>(std::move(child), target, expr.is_try);
}

// The window operator (PhysicalWindow) computes these functions, each
// over a column, with the frames SQL gives them by default; any other
// shape it computed wrongly (a NULL, the whole partition for a running
// frame, the first ORDER BY key alone for peers), so it is refused here
// (Stage 8, #12; design L21).
static void CheckWindowShape(const std::string &name, const WindowExpression &expr,
                             const std::vector<BoundExprPtr> &args,
                             const std::vector<BoundWindowOrder> &order) {
    auto refuse = [&](const std::string &what) {
        throw NotImplementedException("Window function " + name + ": " + what + " is not supported");
    };
    bool ranking = name == "ROW_NUMBER" || name == "RANK" || name == "DENSE_RANK" || name == "NTILE";
    bool offset = name == "LAG" || name == "LEAD";
    bool value = name == "FIRST_VALUE" || name == "LAST_VALUE";
    bool aggregate = name == "SUM" || name == "COUNT" || name == "AVG" || name == "MIN" || name == "MAX";
    if (!ranking && !offset && !value && !aggregate) refuse("this function");
    if (expr.is_distinct) refuse("DISTINCT");
    if (expr.filter) refuse("FILTER");
    auto is_column = [](const BoundExpression &e) {
        return e.GetExpressionType() == BoundExpressionType::COLUMN_REF;
    };
    auto is_int_constant = [](const BoundExpression &e) {
        if (e.GetExpressionType() != BoundExpressionType::CONSTANT) return false;
        auto &v = static_cast<const BoundConstant &>(e).value;
        auto t = v.type().id();
        return !v.IsNull() && (t == LogicalTypeId::INTEGER || t == LogicalTypeId::BIGINT);
    };
    if (name == "ROW_NUMBER" || name == "RANK" || name == "DENSE_RANK") {
        if (!args.empty()) refuse("an argument");
    } else if (name == "NTILE") {
        if (args.size() != 1 || !is_int_constant(*args[0])) refuse("an argument other than an integer");
    } else if (offset) {
        if (args.empty() || args.size() > 2 || !is_column(*args[0]) ||
            (args.size() == 2 && !is_int_constant(*args[1]))) {
            refuse("arguments other than a column and an integer offset");
        }
    } else if (name == "COUNT" && args.empty()) {
        // COUNT(*)
    } else if (args.size() != 1 || !is_column(*args[0])) {
        refuse("an argument other than one column");
    }
    if ((name == "SUM" || name == "AVG") && !args.empty()) {
        auto t = args[0]->GetReturnType().id();
        if (t != LogicalTypeId::TINYINT && t != LogicalTypeId::SMALLINT && t != LogicalTypeId::INTEGER &&
            t != LogicalTypeId::BIGINT && t != LogicalTypeId::FLOAT && t != LogicalTypeId::DOUBLE) {
            refuse("an argument of type " + args[0]->GetReturnType().ToString());
        }
    }
    for (auto &o : order) {
        if (!is_column(*o.expression)) refuse("an ORDER BY key other than a column");
    }
    // Peers by one key only, and LAST_VALUE's frame end at the current
    // row's last peer is not computed.
    bool peers = name == "RANK" || name == "DENSE_RANK" || aggregate;
    if (peers && order.size() > 1) refuse("ORDER BY with more than one key");
    if (name == "LAST_VALUE" && !order.empty()) refuse("ORDER BY");
}

BoundExprPtr Binder::BindWindow(const WindowExpression &expr, BindContext &context) {
    auto name = StringUtil::Upper(expr.function_name);

    std::vector<BoundExprPtr> args;
    for (auto &arg : expr.arguments) {
        args.push_back(BindExpression(*arg, context));
    }

    // Determine return type.
    LogicalType return_type = LogicalType::BIGINT();
    if (name == "ROW_NUMBER" || name == "RANK" || name == "DENSE_RANK" || name == "NTILE") {
        return_type = LogicalType::BIGINT();
    } else if (name == "LEAD" || name == "LAG" || name == "FIRST_VALUE" ||
               name == "LAST_VALUE" || name == "NTH_VALUE") {
        return_type = args.empty() ? LogicalType::SQLNULL() : args[0]->GetReturnType();
    } else if (name == "SUM") {
        return_type = args.empty() ? LogicalType::BIGINT() : args[0]->GetReturnType();
        if (return_type.id() == LogicalTypeId::INTEGER ||
            return_type.id() == LogicalTypeId::SMALLINT ||
            return_type.id() == LogicalTypeId::TINYINT) {
            return_type = LogicalType::BIGINT();
        }
        // As the aggregate: a FLOAT's sum is a DOUBLE (#36).
        if (return_type.id() == LogicalTypeId::FLOAT) return_type = LogicalType::DOUBLE();
    } else if (name == "COUNT") {
        return_type = LogicalType::BIGINT();
    } else if (name == "AVG") {
        return_type = LogicalType::DOUBLE();
    } else if (name == "MIN" || name == "MAX") {
        return_type = args.empty() ? LogicalType::SQLNULL() : args[0]->GetReturnType();
    }

    auto window = std::make_unique<BoundWindowExpression>(name, std::move(args), return_type);

    for (auto &p : expr.partition_by) {
        window->partition_by.push_back(BindExpression(*p, context));
    }
    for (auto &o : expr.order_by) {
        BoundWindowOrder bo;
        bo.expression = BindExpression(*o.expression, context);
        bo.ascending = o.ascending;
        window->order_by.push_back(std::move(bo));
    }
    CheckWindowShape(name, expr, window->arguments, window->order_by);

    return window;
}

BoundExprPtr Binder::BindSubquery(const SubqueryExpression &expr, BindContext &context) {
    BoundSubqueryExpression::Type subtype;
    LogicalType return_type = LogicalType::BOOLEAN();

    // Materialise any derived-table FROM sources inside the subquery
    // (`(VALUES..)`, `(SELECT..)`) into temp catalog tables, rewriting
    // the AST once. Without this the subquery failed to bind/execute
    // ("Table '' not found"), and SCALAR return-type discovery below
    // couldn't see the derived table (returned SQLNULL -> wrong NULL
    // result). The rewrite persists so runtime re-execution is
    // idempotent; the temp tables are not dropped (connection-lifetime;
    // a known bounded leak for repeated distinct subqueries).
    if (expr.subquery) {
        auto *sq = const_cast<SelectStatement *>(expr.subquery.get());
        std::vector<std::string> created;
        if (sq->from_table)
            MaterializeFromSubqueries(catalog_, *sq->from_table, created);
        for (auto &cte : sq->ctes) {
            if (cte.query && cte.query->from_table)
                MaterializeFromSubqueries(catalog_, *cte.query->from_table, created);
        }
    }

    switch (expr.subquery_type) {
    case SubqueryType::EXISTS:
        subtype = BoundSubqueryExpression::Type::EXISTS;
        break;
    case SubqueryType::NOT_EXISTS:
        subtype = BoundSubqueryExpression::Type::NOT_EXISTS;
        break;
    case SubqueryType::SCALAR: {
        subtype = BoundSubqueryExpression::Type::SCALAR;
        // Bind the subquery to discover its first projection's type.
        // Without this the return_type stays SQLNULL, which makes
        // every `expr = (SELECT ...)` short-circuit to NULL via the
        // SQLNULL-operand branch in ExecuteComparison. The inner
        // binder is re-created so the subquery doesn't share our
        // outer BindContext (correlated subqueries deferred).
        if (expr.subquery) {
            Binder inner_binder(catalog_);
            try {
                auto inner = inner_binder.Bind(*expr.subquery);
                if (inner) {
                    auto &bs = static_cast<BoundSelectStatement &>(*inner);
                    if (!bs.result_types.empty()) {
                        return_type = bs.result_types[0];
                    }
                }
            } catch (...) {
                // Fall back to SQLNULL — runtime path still works for
                // EXPLAIN / catalog-dependent inner SELECT.
                return_type = LogicalType::SQLNULL();
            }
        } else {
            return_type = LogicalType::SQLNULL();
        }
        break;
    }
    case SubqueryType::IN_SUBQUERY:
        subtype = BoundSubqueryExpression::Type::IN_SUBQUERY;
        break;
    }

    auto result = std::make_unique<BoundSubqueryExpression>(subtype, return_type);

    // Store the parsed query so we can execute it at runtime.
    // Use shared_ptr<void> with a custom no-op deleter since we don't own the query.
    // The parsed statement tree is owned by the Parser output.
    // Actually, we need to clone or keep the pointer alive. Since ParsedStatement
    // is owned by the statement vector in Connection::Query, we store a raw pointer
    // wrapped in shared_ptr with no-op deleter.
    result->parsed_query = std::shared_ptr<void>(
        const_cast<SelectStatement *>(expr.subquery.get()),
        [](void *) {}); // non-owning

    // Bind child expression for IN subquery.
    if (expr.child) {
        result->child = BindExpression(*expr.child, context);
    }

    return result;
}

BoundExprPtr Binder::BindStar(const StarExpression &expr, BindContext &context,
                               std::vector<BoundExprPtr> &expanded) {
    if (!expr.table_name.empty()) {
        auto it = context.tables.find(expr.table_name);
        if (it == context.tables.end()) {
            throw BinderException(ErrorCode::TABLE_NOT_FOUND,
                                   "Table '" + expr.table_name + "' not found");
        }
        auto *entry = it->second;
        idx_t offset = context.GetTableOffset(expr.table_name);
        for (idx_t i = 0; i < entry->ColumnCount(); i++) {
            auto &col = entry->GetColumns()[i];
            auto ref = std::make_unique<BoundColumnRef>(col.name, offset + i, col.type);
            ref->alias = col.name;
            expanded.push_back(std::move(ref));
        }
    } else {
        // Expand in table insertion order.
        for (auto &[alias, offset] : context.table_order) {
            auto *entry = context.tables.at(alias);
            for (idx_t i = 0; i < entry->ColumnCount(); i++) {
                auto &col = entry->GetColumns()[i];
                auto ref = std::make_unique<BoundColumnRef>(col.name, offset + i, col.type);
                ref->alias = col.name;
                expanded.push_back(std::move(ref));
            }
        }
    }
    return nullptr;
}

// ============================================================================
// Type resolution
// ============================================================================

LogicalType Binder::ResolveTypeName(const std::string &name) {
    auto upper = StringUtil::Upper(name);

    // Strip parameters: DECIMAL(10,2) -> DECIMAL
    auto base = upper.substr(0, upper.find('('));
    // Type parameters as DuckDB reads them (#34): FLOAT(p) is a FLOAT for
    // p of 1 to 24 and a DOUBLE for 25 to 53; VARCHAR, DECIMAL and
    // TIMESTAMP take theirs below; any other type takes none (they were
    // dropped, INTEGER(1000) was an INTEGER).
    if (upper != base) {
        auto params = upper.substr(base.size());
        if (base == "FLOAT" || base == "REAL" || base == "FLOAT4") {
            if (params.size() < 3 || params.front() != '(' || params.back() != ')') {
                throw BinderException(ErrorCode::TYPE_MISMATCH, "Invalid type " + name);
            }
            auto digits = params.substr(1, params.size() - 2);
            long long p = -1;
            if (!digits.empty() && digits.size() <= 6 &&
                digits.find_first_not_of("0123456789") == std::string::npos) {
                p = std::stoll(digits);
            }
            if (p < 1) {
                throw BinderException(ErrorCode::TYPE_MISMATCH,
                    "precision for type float must be at least 1 bit");
            }
            if (p > 53) {
                throw BinderException(ErrorCode::TYPE_MISMATCH,
                    "precision for type float must be less than 54 bits");
            }
            return p <= 24 ? LogicalType::FLOAT() : LogicalType::DOUBLE();
        }
        bool takes_params = base == "VARCHAR" || base == "TEXT" || base == "STRING" ||
                            base == "CHAR" || base == "DECIMAL" || base == "NUMERIC" ||
                            base == "TIMESTAMP";
        if (!takes_params) {
            throw BinderException(ErrorCode::TYPE_MISMATCH,
                "Type " + base + " does not accept type parameters");
        }
    }

    if (base == "INTEGER" || base == "INT" || base == "INT4") return LogicalType::INTEGER();
    if (base == "BIGINT" || base == "INT8") return LogicalType::BIGINT();
    if (base == "SMALLINT" || base == "INT2") return LogicalType::SMALLINT();
    if (base == "TINYINT" || base == "INT1") return LogicalType::TINYINT();
    // HUGEINT, INTERVAL and UUID are refused, in every database: the engine
    // holds no correct non-NULL value of them (a HUGEINT or UUID came back
    // as garbage, an INTERVAL could not be stored) and the database file
    // has no encoding for them (design L12).
    if (base == "HUGEINT" || base == "INTERVAL" || base == "UUID") {
        throw NotImplementedException(base + " is not supported");
    }
    if (base == "FLOAT" || base == "REAL" || base == "FLOAT4") return LogicalType::FLOAT();
    if (base == "DOUBLE" || base == "FLOAT8") return LogicalType::DOUBLE();
    if (base == "BOOLEAN" || base == "BOOL") return LogicalType::BOOLEAN();
    if (base == "VARCHAR" || base == "TEXT" || base == "STRING" || base == "CHAR") {
        // As DuckDB does: a length, VARCHAR(n), is accepted and has no
        // effect; it is neither enforced nor kept (design L13).
        return LogicalType::VARCHAR();
    }
    if (base == "BLOB" || base == "BYTEA") return LogicalType::BLOB();
    if (base == "DATE") return LogicalType::DATE();
    if (base == "TIME") return LogicalType::TIME();
    // The engine's TIMESTAMP is DuckDB's: microseconds, no time zone.
    // DuckDB's seconds, milliseconds and nanoseconds timestamps and its
    // TIMESTAMPTZ round, print and compare otherwise; aliased to TIMESTAMP
    // they gave wrong values, so they are refused (Stage 8, #12). So is a
    // precision other than DuckDB's microsecond ones, 4 to 6.
    if (base == "TIMESTAMP_S" || base == "TIMESTAMP_MS" || base == "TIMESTAMP_NS" ||
        base == "TIMESTAMPTZ" || base == "TIMESTAMP_TZ" || base == "TIMETZ" ||
        base == "TIME_TZ") {
        throw NotImplementedException(base + " is not supported");
    }
    if (base == "TIMESTAMP" && upper != base) {
        auto p = upper.substr(base.size());
        if (p != "(4)" && p != "(5)" && p != "(6)") {
            throw NotImplementedException("TIMESTAMP" + p + " is not supported");
        }
    }
    if (base == "TIMESTAMP" || base == "DATETIME") return LogicalType::TIMESTAMP();

    if (base == "DECIMAL" || base == "NUMERIC") {
        // The engine has no fixed-point type: DECIMAL(p,s) bound to DOUBLE
        // and dropped p and s, so its arithmetic, rounding, text and
        // overflow were a double's, not DuckDB's. Refused until a real
        // DECIMAL exists (#26). Decimal literals (1.5) stay DOUBLE.
        throw NotImplementedException(base + " is not supported");
    }

    throw BinderException(ErrorCode::TYPE_MISMATCH, "Unknown type: " + name);
}

LogicalType Binder::ResolveArithmeticType(const LogicalType &left, const LogicalType &right) {
    // Simple numeric promotion rules.
    if (left.id() == LogicalTypeId::DOUBLE || right.id() == LogicalTypeId::DOUBLE)
        return LogicalType::DOUBLE();
    // FLOAT with a FLOAT, an integer or NULL is FLOAT arithmetic, as
    // DuckDB's ('-2'::FLOAT / '1.5'::FLOAT is -1.3333334); it was DOUBLE
    // arithmetic, whose digits DuckDB does not give (#42).
    if (left.id() == LogicalTypeId::FLOAT || right.id() == LogicalTypeId::FLOAT)
        return LogicalType(LogicalTypeId::FLOAT);
    if (left.id() == LogicalTypeId::BIGINT || right.id() == LogicalTypeId::BIGINT)
        return LogicalType::BIGINT();
    if (left.id() == LogicalTypeId::INTEGER || right.id() == LogicalTypeId::INTEGER)
        return LogicalType::INTEGER();
    // DuckDB computes TINYINT and SMALLINT arithmetic in the wider of the
    // two types and raises on overflow; it does not widen to INTEGER
    // (#43). A NULL beside one takes its type.
    auto small = [](LogicalTypeId t) {
        return t == LogicalTypeId::TINYINT || t == LogicalTypeId::SMALLINT ||
               t == LogicalTypeId::SQLNULL;
    };
    if (small(left.id()) && small(right.id())) {
        if (left.id() == LogicalTypeId::SMALLINT || right.id() == LogicalTypeId::SMALLINT)
            return LogicalType::SMALLINT();
        if (left.id() == LogicalTypeId::TINYINT || right.id() == LogicalTypeId::TINYINT)
            return LogicalType::TINYINT();
    }
    return LogicalType::INTEGER();
}

// Recursively check whether a bound expression contains any aggregate
// function. Used by WHERE / UPDATE-SET / DELETE-WHERE validation to
// raise a clean BinderException instead of letting an aggregate slip
// into row-context evaluation. Subqueries are NOT recursed into —
// aggregates inside a correlated subquery are legal.
// Structural equality of two bound expressions — used to recognize a
// SELECT/HAVING/ORDER BY expression that matches a GROUP BY key
// (e.g. GROUP BY a+1 with SELECT a+1, or GROUP BY CASE.. / LENGTH(s)).
static bool BoundExprEqual(const BoundExpression &a, const BoundExpression &b) {
    if (a.GetExpressionType() != b.GetExpressionType()) return false;
    switch (a.GetExpressionType()) {
    case BoundExpressionType::COLUMN_REF:
        return static_cast<const BoundColumnRef &>(a).column_index ==
               static_cast<const BoundColumnRef &>(b).column_index;
    case BoundExpressionType::CONSTANT:
        return static_cast<const BoundConstant &>(a).value ==
               static_cast<const BoundConstant &>(b).value;
    case BoundExpressionType::ARITHMETIC: {
        auto &x = static_cast<const BoundArithmetic &>(a);
        auto &y = static_cast<const BoundArithmetic &>(b);
        return x.op == y.op && x.left && y.left && x.right && y.right &&
               BoundExprEqual(*x.left, *y.left) && BoundExprEqual(*x.right, *y.right);
    }
    case BoundExpressionType::COMPARISON: {
        auto &x = static_cast<const BoundComparison &>(a);
        auto &y = static_cast<const BoundComparison &>(b);
        return x.op == y.op && x.left && y.left && x.right && y.right &&
               BoundExprEqual(*x.left, *y.left) && BoundExprEqual(*x.right, *y.right);
    }
    case BoundExpressionType::CONJUNCTION: {
        auto &x = static_cast<const BoundConjunction &>(a);
        auto &y = static_cast<const BoundConjunction &>(b);
        return x.op == y.op && x.left && y.left && x.right && y.right &&
               BoundExprEqual(*x.left, *y.left) && BoundExprEqual(*x.right, *y.right);
    }
    case BoundExpressionType::NEGATION:
        return BoundExprEqual(*static_cast<const BoundNegation &>(a).child,
                              *static_cast<const BoundNegation &>(b).child);
    case BoundExpressionType::IS_NULL:
        return BoundExprEqual(*static_cast<const BoundIsNull &>(a).child,
                              *static_cast<const BoundIsNull &>(b).child);
    case BoundExpressionType::UNARY_MINUS:
        return BoundExprEqual(*static_cast<const BoundUnaryMinus &>(a).child,
                              *static_cast<const BoundUnaryMinus &>(b).child);
    case BoundExpressionType::CAST: {
        auto &x = static_cast<const BoundCast &>(a);
        auto &y = static_cast<const BoundCast &>(b);
        return x.GetReturnType().id() == y.GetReturnType().id() &&
               x.child && y.child && BoundExprEqual(*x.child, *y.child);
    }
    case BoundExpressionType::FUNCTION: {
        auto &x = static_cast<const BoundFunction &>(a);
        auto &y = static_cast<const BoundFunction &>(b);
        if (x.function_name != y.function_name) return false;
        if (x.arguments.size() != y.arguments.size()) return false;
        for (size_t i = 0; i < x.arguments.size(); i++) {
            if (!x.arguments[i] || !y.arguments[i] ||
                !BoundExprEqual(*x.arguments[i], *y.arguments[i]))
                return false;
        }
        return true;
    }
    default:
        return false;
    }
}

static bool HasUngroupedColumn(const BoundExpression &expr,
                               const std::vector<BoundExprPtr> &group_by) {
    // If the whole expression structurally matches a GROUP BY key, it's
    // grouped — accept it without recursing. This handles GROUP BY on
    // expressions (a+1, CASE.., LENGTH(s)) where the bare columns inside
    // are not themselves group keys.
    for (auto &g : group_by) {
        if (g && BoundExprEqual(expr, *g)) return false;
    }
    switch (expr.GetExpressionType()) {
    case BoundExpressionType::COLUMN_REF: {
        auto &cr = static_cast<const BoundColumnRef &>(expr);
        for (auto &g : group_by) {
            if (g && g->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                auto &gcr = static_cast<const BoundColumnRef &>(*g);
                if (gcr.column_index == cr.column_index) return false;
            }
        }
        return true;
    }
    case BoundExpressionType::FUNCTION: {
        auto &fn = static_cast<const BoundFunction &>(expr);
        if (fn.is_aggregate) return false; // aggregate consumes its column refs
        for (auto &a : fn.arguments) {
            if (a && HasUngroupedColumn(*a, group_by)) return true;
        }
        if (fn.filter && HasUngroupedColumn(*fn.filter, group_by)) return true;
        return false;
    }
    case BoundExpressionType::COMPARISON: {
        auto &c = static_cast<const BoundComparison &>(expr);
        return (c.left && HasUngroupedColumn(*c.left, group_by)) ||
               (c.right && HasUngroupedColumn(*c.right, group_by));
    }
    case BoundExpressionType::CONJUNCTION: {
        auto &c = static_cast<const BoundConjunction &>(expr);
        return (c.left && HasUngroupedColumn(*c.left, group_by)) ||
               (c.right && HasUngroupedColumn(*c.right, group_by));
    }
    case BoundExpressionType::NEGATION:
        return HasUngroupedColumn(*static_cast<const BoundNegation &>(expr).child, group_by);
    case BoundExpressionType::IS_NULL:
        return HasUngroupedColumn(*static_cast<const BoundIsNull &>(expr).child, group_by);
    case BoundExpressionType::IS_BOOL:
        return HasUngroupedColumn(*static_cast<const BoundIsBool &>(expr).child, group_by);
    case BoundExpressionType::ARITHMETIC: {
        auto &a = static_cast<const BoundArithmetic &>(expr);
        return (a.left && HasUngroupedColumn(*a.left, group_by)) ||
               (a.right && HasUngroupedColumn(*a.right, group_by));
    }
    case BoundExpressionType::UNARY_MINUS:
        return HasUngroupedColumn(*static_cast<const BoundUnaryMinus &>(expr).child, group_by);
    case BoundExpressionType::CAST:
        return HasUngroupedColumn(*static_cast<const BoundCast &>(expr).child, group_by);
    case BoundExpressionType::SUBQUERY:
        return false; // subqueries are independent scopes
    default:
        return false;
    }
}

static bool ContainsAggregate(const BoundExpression &expr) {
    switch (expr.GetExpressionType()) {
    case BoundExpressionType::FUNCTION: {
        auto &fn = static_cast<const BoundFunction &>(expr);
        if (fn.is_aggregate) return true;
        for (auto &a : fn.arguments) {
            if (a && ContainsAggregate(*a)) return true;
        }
        if (fn.filter && ContainsAggregate(*fn.filter)) return true;
        return false;
    }
    case BoundExpressionType::COMPARISON: {
        auto &c = static_cast<const BoundComparison &>(expr);
        return (c.left && ContainsAggregate(*c.left)) ||
               (c.right && ContainsAggregate(*c.right));
    }
    case BoundExpressionType::CONJUNCTION: {
        auto &c = static_cast<const BoundConjunction &>(expr);
        return (c.left && ContainsAggregate(*c.left)) ||
               (c.right && ContainsAggregate(*c.right));
    }
    case BoundExpressionType::NEGATION: {
        auto &n = static_cast<const BoundNegation &>(expr);
        return n.child && ContainsAggregate(*n.child);
    }
    case BoundExpressionType::IS_NULL: {
        auto &i = static_cast<const BoundIsNull &>(expr);
        return i.child && ContainsAggregate(*i.child);
    }
    case BoundExpressionType::IS_BOOL: {
        auto &i = static_cast<const BoundIsBool &>(expr);
        return i.child && ContainsAggregate(*i.child);
    }
    case BoundExpressionType::ARITHMETIC: {
        auto &a = static_cast<const BoundArithmetic &>(expr);
        return (a.left && ContainsAggregate(*a.left)) ||
               (a.right && ContainsAggregate(*a.right));
    }
    case BoundExpressionType::UNARY_MINUS: {
        auto &u = static_cast<const BoundUnaryMinus &>(expr);
        return u.child && ContainsAggregate(*u.child);
    }
    case BoundExpressionType::CAST: {
        auto &c = static_cast<const BoundCast &>(expr);
        return c.child && ContainsAggregate(*c.child);
    }
    case BoundExpressionType::SUBQUERY:
        // Aggregates inside a correlated subquery are legal at SQL
        // standard level; do not recurse.
        return false;
    default:
        return false;
    }
}

bool Binder::IsAggregateFunction(const std::string &name) const {
    return name == "COUNT" || name == "SUM" || name == "AVG" ||
           name == "MIN" || name == "MAX" ||
           name == "STRING_AGG" || name == "LISTAGG" || name == "GROUP_CONCAT" ||
           name == "STDDEV" || name == "STDDEV_SAMP" || name == "STDDEV_POP" ||
           name == "VARIANCE" || name == "VAR_SAMP" || name == "VAR_POP" ||
           name == "MEDIAN" || name == "BOOL_AND" || name == "BOOL_OR";
}

} // namespace zugodb
