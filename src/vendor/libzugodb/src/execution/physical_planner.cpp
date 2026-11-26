#include "zugodb/execution/physical_planner.hpp"
#include "zugodb/common/profile_log.hpp"
#include "zugodb/execution/expression_executor.hpp"
#include "zugodb/execution/agg_emit_helpers.hpp"
#include "zugodb/common/exception.hpp"
#include "zugodb/common/parallel.hpp"
#include "zugodb/common/string_util.hpp"
#include "zugodb/storage/data_table.hpp"
#include "third_party/unordered_dense.h"
#include "zugodb/execution/simple_i64_set.hpp"
#include "zugodb/execution/literal_emit_filter.hpp"
#include "zugodb/execution/count_group_agg.hpp"
#include "zugodb/execution/group_key.hpp"
#include "zugodb/common/types/float_order.hpp"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <queue>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <array>
#include <atomic>
#include <unordered_map>
#include <unordered_set>

// AVX2 baseline (Haswell+, 2013). CMake adds /arch:AVX2 (MSVC) or -mavx2
// to zugodb so we can use 256-bit SIMD directly. Used by the FUSED
// PARQUET FAST PATH's INT32 SUM hot loop (SUM / AVG over 100M rows).
// MSVC defines __AVX2__ when /arch:AVX2 is set.
#if defined(__AVX2__)
#include <immintrin.h>
#define ZUGODB_PP_HAS_AVX2 1
#endif

namespace zugodb {

// DuckDB's NTILE (SQLite's ntileValueFunc): row `pos` of a partition of
// `ps` rows in `n` buckets, the first ps % n buckets one row larger, and
// never more buckets than rows. pos * n / ps + 1 gave bucket 251 of a
// 4-row partition for NTILE(1000) (Stage 9).
static int64_t NtileBucket(idx_t pos, idx_t ps, int64_t n) {
    int64_t total = static_cast<int64_t>(ps);
    if (n > total) n = total;
    int64_t size = total / n;
    int64_t large = total - n * size;
    int64_t small_end = large * (size + 1);
    int64_t row = static_cast<int64_t>(pos);
    if (row < small_end) return 1 + row / (size + 1);
    return 1 + large + (row - small_end) / size;
}


// Forward declarations - classes that reference each other across definitions.
class PhysicalHashJoin;
struct SimplePredicate;

// Defined after PhysicalHashJoin's full declaration. Returns nullptr if op is not one.
static PhysicalHashJoin *AsHashJoin(PhysicalOperator *op);

// LIKE '%needle%' substring search. memchr-anchored memcmp confirm — uses the
// CRT's SIMD memchr (MSVC) for the hot byte-scan, then memcmp to verify.
// Microbench (200K URL-shaped strings): ~6.8x faster than std::search vs
// the libc++ generic two-way searcher. Returns first hit or nullptr.
// Note: haystack is NOT NUL-terminated (it's a string_t slice into a parquet
// heap), so strstr is unsafe; this helper takes explicit lengths.
static inline const char *FindSubstr(const char *h, size_t hlen,
                                     const char *n, size_t nlen) {
    if (nlen == 0) return h;
    if (nlen > hlen) return nullptr;
    const char first = n[0];
    const char *end = h + (hlen - nlen) + 1;
    while (h < end) {
        const char *p = static_cast<const char *>(
            std::memchr(h, static_cast<unsigned char>(first),
                        static_cast<size_t>(end - h)));
        if (!p) return nullptr;
        if (std::memcmp(p, n, nlen) == 0) return p;
        h = p + 1;
    }
    return nullptr;
}

// ============================================================================
// Simple-predicate compiler & evaluator.
//
// Converts a conjunction of (ColumnRef OP Constant) comparisons into a flat
// struct-of-arrays form that can be evaluated per row against typed
// ParquetColumnData with no virtual dispatch. Used by the fused Parquet
// FILTER+AGG paths to skip PhysicalFilter's DataChunk copy.
// ============================================================================
enum class SimpleCmpOp { LT, LE, GT, GE, EQ, NE };

struct SimplePredicate {
    idx_t col_idx;
    SimpleCmpOp op;
    double dval;       // numeric form: comparison value cast to double
    int64_t ival = 0;  // numeric form: original int64 (BIGINT literals
                       // > 2^53 cannot round-trip through double — e.g.
                       // 435090932899640449 lost ~64 in dval).
                       // BIGINT/INTEGER paths must read ival, not (int64_t)dval.
    std::string sval;  // VARCHAR form: literal bytes (for EQ/NE: full literal;
                       // for like_contains: bare needle with %s stripped)
    bool str_form = false;
    bool like_contains = false;  // VARCHAR LIKE '%literal%' (case-sensitive,
                                 // no _, no escapes, exactly one %...%)
    bool like_negated = false;   // when like_contains: true means NOT LIKE
    // INTEGER/BIGINT `col IN (c0, c1, ...)`. The binder lowers `IN` to a
    // BoundFunction named "IN" (args[0] = column, args[1..] = constants),
    // which is neither a COMPARISON nor an AND CONJUNCTION — without an
    // explicit clause here it would not compile, and a fused FILTER+AGG
    // path that bypasses PhysicalFilter would silently drop the predicate.
    // When `int_in_form` is set, the row matches iff its int64 value equals
    // any element of `in_set`. Mutually exclusive with str_form/like_contains.
    bool int_in_form = false;
    std::vector<int64_t> in_set;  // sorted, deduped; membership via binary search
};

static bool ParseCmpOp(const std::string &s, SimpleCmpOp &out) {
    if (s == "<")  { out = SimpleCmpOp::LT; return true; }
    if (s == "<=") { out = SimpleCmpOp::LE; return true; }
    if (s == ">")  { out = SimpleCmpOp::GT; return true; }
    if (s == ">=") { out = SimpleCmpOp::GE; return true; }
    if (s == "=" || s == "==") { out = SimpleCmpOp::EQ; return true; }
    if (s == "!=" || s == "<>") { out = SimpleCmpOp::NE; return true; }
    return false;
}

// Extract an integer literal from a value expression. Accepts a plain
// CONSTANT and a UNARY_MINUS wrapping one — the parser turns `-1` into
// UnaryMinus(Constant(1)), and ConstantFolding does not descend into the
// sub-expressions of a WHERE clause, so a negative literal still arrives
// un-folded here. Returns false for anything else (column refs, NULLs,
// non-integer / floating constants).
static bool ExtractIntLiteral(const BoundExpression &e, int64_t &out) {
    if (e.GetExpressionType() == BoundExpressionType::UNARY_MINUS) {
        int64_t inner;
        if (!ExtractIntLiteral(
                *static_cast<const BoundUnaryMinus &>(e).child, inner))
            return false;
        out = -inner;
        return true;
    }
    if (e.GetExpressionType() != BoundExpressionType::CONSTANT) return false;
    const auto &con = static_cast<const BoundConstant &>(e);
    if (con.value.IsNull()) return false;
    try {
        switch (con.value.type().id()) {
        case LogicalTypeId::BOOLEAN:  out = con.value.GetValue<bool>() ? 1 : 0; return true;
        case LogicalTypeId::TINYINT:  out = (int64_t)con.value.GetValue<int8_t>();  return true;
        case LogicalTypeId::SMALLINT: out = (int64_t)con.value.GetValue<int16_t>(); return true;
        case LogicalTypeId::INTEGER:  out = (int64_t)con.value.GetValue<int32_t>(); return true;
        case LogicalTypeId::BIGINT:   out = con.value.GetValue<int64_t>(); return true;
        default: return false;
        }
    } catch (...) { return false; }
}

// Returns true iff `e` is an AND-tree of (ColumnRef OP Constant) comparisons
// on numeric columns; fills `out`. On `false`, `out` may be partially filled -
// caller should discard it.
static bool TryCompileSimplePredicateImpl(const BoundExpression &e,
                                          std::vector<SimplePredicate> &out) {
    if (e.GetExpressionType() == BoundExpressionType::CONJUNCTION) {
        auto &c = static_cast<const BoundConjunction &>(e);
        if (c.op != "AND") return false;
        return TryCompileSimplePredicateImpl(*c.left, out) &&
               TryCompileSimplePredicateImpl(*c.right, out);
    }
    // `col IN (const, const, ...)` on an INTEGER/BIGINT column. The binder
    // lowers IN to a BoundFunction("IN", {column, val0, val1, ...}). Compile
    // it to an int64 membership set so the fused FILTER+AGG paths keep
    // honoring the predicate instead of falling back. `NOT IN` arrives as a
    // BoundNegation around this and is intentionally not compiled here.
    if (e.GetExpressionType() == BoundExpressionType::FUNCTION) {
        auto &fn = static_cast<const BoundFunction &>(e);
        if (fn.function_name != "IN" || fn.is_aggregate) return false;
        if (fn.arguments.size() < 2) return false;
        if (fn.arguments[0]->GetExpressionType() !=
            BoundExpressionType::COLUMN_REF) return false;
        auto &col = static_cast<const BoundColumnRef &>(*fn.arguments[0]);
        auto ctid = col.GetReturnType().id();
        if (ctid != LogicalTypeId::BIGINT && ctid != LogicalTypeId::INTEGER)
            return false;
        SimplePredicate sp;
        sp.col_idx = col.column_index;
        sp.op = SimpleCmpOp::EQ;  // unused for IN; satisfies field init
        sp.int_in_form = true;
        sp.in_set.reserve(fn.arguments.size() - 1);
        for (size_t i = 1; i < fn.arguments.size(); i++) {
            int64_t iv;
            if (!ExtractIntLiteral(*fn.arguments[i], iv)) return false;
            sp.in_set.push_back(iv);
        }
        std::sort(sp.in_set.begin(), sp.in_set.end());
        sp.in_set.erase(std::unique(sp.in_set.begin(), sp.in_set.end()),
                        sp.in_set.end());
        out.push_back(std::move(sp));
        return true;
    }
    if (e.GetExpressionType() != BoundExpressionType::COMPARISON) return false;
    auto &cmp = static_cast<const BoundComparison &>(e);
    bool swapped = false;
    const BoundExpression *lhs = cmp.left.get();
    const BoundExpression *rhs = cmp.right.get();
    // Allow Constant OP Column (swap sides).
    if (lhs->GetExpressionType() == BoundExpressionType::CONSTANT &&
        rhs->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
        std::swap(lhs, rhs); swapped = true;
    }
    if (lhs->GetExpressionType() != BoundExpressionType::COLUMN_REF) return false;
    if (rhs->GetExpressionType() != BoundExpressionType::CONSTANT) return false;
    auto &col = static_cast<const BoundColumnRef &>(*lhs);
    auto &con = static_cast<const BoundConstant &>(*rhs);
    auto tid = col.GetReturnType().id();
    // LIKE '%literal%' (case-sensitive, no _, no escapes) on a VARCHAR
    // column. Stash the bare needle and route the row-loop through
    // memmem/std::search instead of the per-row backtracker. ILIKE keeps
    // falling through to the legacy expression executor. NOT LIKE goes
    // through the same path with like_negated=true.
    if (!swapped && (cmp.op == "LIKE" || cmp.op == "NOT LIKE") &&
        tid == LogicalTypeId::VARCHAR) {
        if (con.value.IsNull()) return false;
        if (con.value.type().id() != LogicalTypeId::VARCHAR) return false;
        const auto &pat = con.value.GetValue<std::string>();
        if (pat.size() < 2 || pat.front() != '%' || pat.back() != '%') return false;
        std::string needle = pat.substr(1, pat.size() - 2);
        for (char c : needle) {
            if (c == '%' || c == '_' || c == '\\') return false;
        }
        SimplePredicate sp;
        sp.col_idx = col.column_index;
        sp.op = SimpleCmpOp::EQ;  // unused for LIKE; satisfies field init
        sp.sval = std::move(needle);
        sp.str_form = true;
        sp.like_contains = true;
        sp.like_negated = (cmp.op == "NOT LIKE");
        out.push_back(std::move(sp));
        return true;
    }
    SimpleCmpOp op;
    if (!ParseCmpOp(cmp.op, op)) return false;
    if (swapped) {
        // `const OP col` ≡ `col OP' const` where OP' is the mirror op.
        switch (op) {
        case SimpleCmpOp::LT: op = SimpleCmpOp::GT; break;
        case SimpleCmpOp::LE: op = SimpleCmpOp::GE; break;
        case SimpleCmpOp::GT: op = SimpleCmpOp::LT; break;
        case SimpleCmpOp::GE: op = SimpleCmpOp::LE; break;
        case SimpleCmpOp::EQ: case SimpleCmpOp::NE: break;
        }
    }
    // VARCHAR EQ/NE against a string literal — supports the common
    // shape `WHERE Col <> ''` without falling
    // back to the slow generic path. Other ops on VARCHAR (LIKE, range
    // comparisons) stay unsupported here.
    if (tid == LogicalTypeId::VARCHAR) {
        if (op != SimpleCmpOp::EQ && op != SimpleCmpOp::NE) return false;
        if (con.value.IsNull()) return false;
        if (con.value.type().id() != LogicalTypeId::VARCHAR) return false;
        SimplePredicate sp;
        sp.col_idx = col.column_index;
        sp.op = op;
        sp.sval = con.value.GetValue<std::string>();
        sp.str_form = true;
        out.push_back(std::move(sp));
        return true;
    }
    if (tid != LogicalTypeId::BIGINT && tid != LogicalTypeId::INTEGER &&
        tid != LogicalTypeId::DOUBLE && tid != LogicalTypeId::FLOAT) return false;
    double d;
    int64_t iv = 0;
    try {
        auto vtid = con.value.type().id();
        switch (vtid) {
        case LogicalTypeId::BOOLEAN:  iv = con.value.GetValue<bool>() ? 1 : 0; d = (double)iv; break;
        case LogicalTypeId::TINYINT:  iv = (int64_t)con.value.GetValue<int8_t>();  d = (double)iv; break;
        case LogicalTypeId::SMALLINT: iv = (int64_t)con.value.GetValue<int16_t>(); d = (double)iv; break;
        case LogicalTypeId::INTEGER:  iv = (int64_t)con.value.GetValue<int32_t>(); d = (double)iv; break;
        case LogicalTypeId::BIGINT:   iv = con.value.GetValue<int64_t>(); d = (double)iv; break;
        case LogicalTypeId::FLOAT:    d = (double)con.value.GetValue<float>();  iv = (int64_t)d; break;
        case LogicalTypeId::DOUBLE:   d = con.value.GetValue<double>();         iv = (int64_t)d; break;
        default: return false;
        }
    } catch (...) { return false; }
    SimplePredicate sp;
    sp.col_idx = col.column_index;
    sp.op = op;
    sp.dval = d;
    sp.ival = iv;
    out.push_back(std::move(sp));
    return true;
}

// Top-level entry: compile then reorder so cheap+selective predicates run
// first. LIKE '%needle%' is per-row substring search; a substring-LIKE
// clause is often paired with a much cheaper `<> ''` predicate that
// rejects ~98% of rows, so eval'ing NE first short-circuits LIKE on most
// rows.
static bool TryCompileSimplePredicate(const BoundExpression &e,
                                      std::vector<SimplePredicate> &out) {
    size_t base = out.size();
    if (!TryCompileSimplePredicateImpl(e, out)) return false;
    std::stable_sort(out.begin() + base, out.end(),
        [](const SimplePredicate &a, const SimplePredicate &b) {
            return !a.like_contains && b.like_contains;
        });
    return true;
}

// Evaluate predicates against a DataChunk row (for CSV / JSON sources).
// Returns false if any predicate fails or the row is null in a predicate col.
static inline bool EvalSimplePredicatesChunk(
    const std::vector<SimplePredicate> &preds,
    const DataChunk &chunk, idx_t r) {
    for (const auto &p : preds) {
        auto &v = chunk.GetVector(p.col_idx);
        if (!v.GetValidity().RowIsValid(r)) return false;
        if (p.int_in_form) {
            int64_t iv;
            switch (v.GetType().id()) {
            case LogicalTypeId::BIGINT:  iv = v.GetData<int64_t>()[r]; break;
            case LogicalTypeId::INTEGER: iv = (int64_t)v.GetData<int32_t>()[r]; break;
            default: return false;
            }
            if (!std::binary_search(p.in_set.begin(), p.in_set.end(), iv))
                return false;
            continue;
        }
        if (p.str_form) {
            if (v.GetType().id() != LogicalTypeId::VARCHAR) return false;
            const auto &s = v.GetData<string_t>()[r];
            if (p.like_contains) {
                const char *hs = s.GetData();
                uint32_t hl = s.GetSize();
                bool match;
                if (p.sval.empty()) {
                    match = true;
                } else if (hl < p.sval.size()) {
                    match = false;
                } else {
                    match = (FindSubstr(hs, hl, p.sval.data(), p.sval.size()) != nullptr);
                }
                if (p.like_negated) match = !match;
                if (!match) return false;
                continue;
            }
            bool eq = (s.GetSize() == p.sval.size()) &&
                      (p.sval.empty() ||
                       std::memcmp(s.GetData(), p.sval.data(), p.sval.size()) == 0);
            if (p.op == SimpleCmpOp::EQ) { if (!eq) return false; }
            else                         { if (eq)  return false; }
            continue;
        }
        double val;
        switch (v.GetType().id()) {
        case LogicalTypeId::BIGINT:  val = static_cast<double>(v.GetData<int64_t>()[r]); break;
        case LogicalTypeId::INTEGER: val = static_cast<double>(v.GetData<int32_t>()[r]); break;
        case LogicalTypeId::DOUBLE:  val = v.GetData<double>()[r]; break;
        case LogicalTypeId::FLOAT:   val = static_cast<double>(v.GetData<float>()[r]); break;
        default: return false;
        }
        switch (p.op) {
        case SimpleCmpOp::LT: if (!(val <  p.dval)) return false; break;
        case SimpleCmpOp::LE: if (!(val <= p.dval)) return false; break;
        case SimpleCmpOp::GT: if (!(val >  p.dval)) return false; break;
        case SimpleCmpOp::GE: if (!(val >= p.dval)) return false; break;
        case SimpleCmpOp::EQ: if (val != p.dval) return false; break;
        case SimpleCmpOp::NE: if (val == p.dval) return false; break;
        }
    }
    return true;
}


// ============================================================================
// Physical Operators
// ============================================================================

class PhysicalTableScan : public PhysicalOperator {
public:
    PhysicalTableScan(TableCatalogEntry *table)
        : PhysicalOperator(PhysicalOperatorType::TABLE_SCAN, table->GetTypes()),
          table_(table) {}

    void Init() override {
        state_ = table_->GetStorage().InitScan();
    }

    bool GetData(DataChunk &result) override {
        result.Initialize(GetTypes());
        return table_->GetStorage().Scan(state_, result);
    }

private:
    TableCatalogEntry *table_;
    TableScanState state_;
};


class PhysicalFilter : public PhysicalOperator {
public:
    PhysicalFilter(BoundExprPtr condition, std::vector<LogicalType> types)
        : PhysicalOperator(PhysicalOperatorType::FILTER, std::move(types)),
          condition_(std::move(condition)) {}

    // Exposed so fused FILTER+AGG+SCAN paths can inline the predicate.
    const BoundExpression *GetCondition() const { return condition_.get(); }

    // Forward needed-output mask to child, augmented with columns the
    // filter condition itself references. Without this, a parquet scan
    // child sees no projection mask and decodes every column. (A
    // `SELECT col WHERE col = lit` went from 380ms RG decode → ~30ms.)
    void SetNeededOutputs(const std::vector<bool> &out_mask) override {
        if (children.empty()) return;
        idx_t in_cols = children[0]->GetTypes().size();
        std::vector<bool> in_mask = out_mask;
        if (in_mask.size() < in_cols) in_mask.resize(in_cols, false);
        if (condition_) {
            std::function<void(const BoundExpression &)> walk =
                [&](const BoundExpression &x) {
                    if (x.GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                        auto &c = static_cast<const BoundColumnRef &>(x);
                        if (c.column_index < in_mask.size()) in_mask[c.column_index] = true;
                    } else if (x.GetExpressionType() == BoundExpressionType::COMPARISON) {
                        auto &c = static_cast<const BoundComparison &>(x);
                        walk(*c.left); walk(*c.right);
                    } else if (x.GetExpressionType() == BoundExpressionType::ARITHMETIC) {
                        auto &a = static_cast<const BoundArithmetic &>(x);
                        walk(*a.left); walk(*a.right);
                    } else if (x.GetExpressionType() == BoundExpressionType::FUNCTION) {
                        auto &f = static_cast<const BoundFunction &>(x);
                        for (auto &a : f.arguments) walk(*a);
                    } else if (x.GetExpressionType() == BoundExpressionType::CAST) {
                        auto &c = static_cast<const BoundCast &>(x);
                        walk(*c.child);
                    } else if (x.GetExpressionType() == BoundExpressionType::CONJUNCTION) {
                        auto &c = static_cast<const BoundConjunction &>(x);
                        walk(*c.left); walk(*c.right);
                    } else if (x.GetExpressionType() == BoundExpressionType::NEGATION) {
                        auto &n = static_cast<const BoundNegation &>(x);
                        walk(*n.child);
                    } else if (x.GetExpressionType() == BoundExpressionType::IS_NULL) {
                        auto &n = static_cast<const BoundIsNull &>(x);
                        walk(*n.child);
                    } else if (x.GetExpressionType() == BoundExpressionType::UNARY_MINUS) {
                        auto &u = static_cast<const BoundUnaryMinus &>(x);
                        walk(*u.child);
                    }
                };
            walk(*condition_);
        }
        children[0]->SetNeededOutputs(in_mask);
    }

    void Init() override {
        for (auto &child : children) child->Init();
    }

    bool GetData(DataChunk &result) override {
        while (true) {
            DataChunk input;
            input.Initialize(GetTypes());
            if (!children[0]->GetData(input)) return false;
            if (input.size() == 0) continue;

            // Evaluate filter condition.
            Vector filter_result(LogicalType::BOOLEAN(), input.size());
            ExpressionExecutor::Execute(*condition_, input, filter_result, input.size());

            auto *filter_data = filter_result.GetData<bool>();
            auto &filter_validity = filter_result.GetValidity();

            // Build selection vector of matching rows.
            uint32_t sel[VECTOR_SIZE];
            idx_t sel_count = 0;
            for (idx_t i = 0; i < input.size(); i++) {
                if (filter_validity.RowIsValid(i) && filter_data[i]) {
                    sel[sel_count++] = static_cast<uint32_t>(i);
                }
            }

            if (sel_count == 0) continue; // all filtered - pull next chunk

            if (result.ColumnCount() != GetTypes().size()) {
                result.Initialize(GetTypes());
            } else {
                result.Reset();
            }

            idx_t num_cols = input.ColumnCount();
            for (idx_t c = 0; c < num_cols; c++) {
                auto &src = input.GetVector(c);
                auto &dst = result.GetVector(c);
                auto &src_val = src.GetValidity();
                auto &dst_val = dst.GetValidity();
                switch (src.GetType().id()) {
                case LogicalTypeId::BIGINT: {
                    auto *s = src.GetData<int64_t>();
                    auto *d = dst.GetData<int64_t>();
                    for (idx_t i = 0; i < sel_count; i++) {
                        d[i] = s[sel[i]];
                        if (!src_val.RowIsValid(sel[i])) dst_val.SetInvalid(i);
                    }
                    break;
                }
                case LogicalTypeId::INTEGER: {
                    auto *s = src.GetData<int32_t>();
                    auto *d = dst.GetData<int32_t>();
                    for (idx_t i = 0; i < sel_count; i++) {
                        d[i] = s[sel[i]];
                        if (!src_val.RowIsValid(sel[i])) dst_val.SetInvalid(i);
                    }
                    break;
                }
                case LogicalTypeId::DOUBLE: {
                    auto *s = src.GetData<double>();
                    auto *d = dst.GetData<double>();
                    for (idx_t i = 0; i < sel_count; i++) {
                        d[i] = s[sel[i]];
                        if (!src_val.RowIsValid(sel[i])) dst_val.SetInvalid(i);
                    }
                    break;
                }
                case LogicalTypeId::FLOAT: {
                    auto *s = src.GetData<float>();
                    auto *d = dst.GetData<float>();
                    for (idx_t i = 0; i < sel_count; i++) {
                        d[i] = s[sel[i]];
                        if (!src_val.RowIsValid(sel[i])) dst_val.SetInvalid(i);
                    }
                    break;
                }
                case LogicalTypeId::BOOLEAN: {
                    auto *s = src.GetData<bool>();
                    auto *d = dst.GetData<bool>();
                    for (idx_t i = 0; i < sel_count; i++) {
                        d[i] = s[sel[i]];
                        if (!src_val.RowIsValid(sel[i])) dst_val.SetInvalid(i);
                    }
                    break;
                }
                case LogicalTypeId::VARCHAR: {
                    // Zero-copy: copy string_t slices and share the source's
                    // string buffer so string_t pointers remain valid.
                    auto *s = src.GetData<string_t>();
                    auto *d = dst.GetData<string_t>();
                    for (idx_t i = 0; i < sel_count; i++) {
                        d[i] = s[sel[i]];
                        if (!src_val.RowIsValid(sel[i])) dst_val.SetInvalid(i);
                    }
                    dst.SetAuxiliaryPtr(src.GetAuxiliaryPtr());
                    break;
                }
                default: {
                    // Fallback for types without a fast path.
                    for (idx_t i = 0; i < sel_count; i++) {
                        dst.SetValue(i, src.GetValue(sel[i]));
                    }
                    break;
                }
                }
            }
            result.SetCardinality(sel_count);
            return true;
        }
    }

private:
    BoundExprPtr condition_;
};

class PhysicalProjection : public PhysicalOperator {
public:
    PhysicalProjection(std::vector<BoundExprPtr> expressions, std::vector<LogicalType> types)
        : PhysicalOperator(PhysicalOperatorType::PROJECTION, std::move(types)),
          expressions_(std::move(expressions)) {}

    // Exposed so multi-col GROUP BY fast path can peek through GROUP-BY-by-expr lift.
    const std::vector<BoundExprPtr> &GetExpressions() const { return expressions_; }

    // Pass row-limit hints through. LIMIT -> PROJECTION -> ORDER_BY is a
    // common plan shape; without forwarding here, ORDER_BY does a full
    // sort of millions of rows even when LIMIT 10 sits above.
    void SetRowLimit(idx_t n) override {
        if (!children.empty()) children[0]->SetRowLimit(n);
    }

    // Translate TopN hint's output col_idx through this projection. If
    // expressions_[col_idx] is a simple ColumnRef, we can forward the
    // underlying input col_idx; otherwise the projection materializes a
    // computed value our child can't reason about, so we drop the hint.
    void SetTopNHint(idx_t col_idx, bool ascending, idx_t limit) override {
        if (children.empty() || col_idx >= expressions_.size()) return;
        auto &e = expressions_[col_idx];
        if (!e || e->GetExpressionType() != BoundExpressionType::COLUMN_REF) return;
        auto &cref = static_cast<BoundColumnRef &>(*e);
        children[0]->SetTopNHint(cref.column_index, ascending, limit);
    }

    // Forward column pruning down through the projection. The needed-out
    // mask is stated in the projection's OUTPUT slots, which we map back
    // to INPUT slots by following each output expression's ColumnRef
    // (the only kind of expression that pulls from a single input column).
    // For non-ColumnRef projections we conservatively mark every input
    // column referenced by the expression. Without this, callers like
    // PhysicalWindow telling Projection "I only need cols 1,7" never
    // reaches PhysicalParquetScan and we decode all 10 columns of 10M
    // rows instead of 2.
    void SetNeededOutputs(const std::vector<bool> &out_mask) override {
        if (children.empty()) return;
        idx_t in_cols = children[0]->GetTypes().size();
        std::vector<bool> in_mask(in_cols, false);
        for (idx_t o = 0; o < expressions_.size() && o < out_mask.size(); o++) {
            if (!out_mask[o]) continue;
            auto *e = expressions_[o].get();
            if (!e) continue;
            // Walk the expression collecting input column refs.
            std::function<void(const BoundExpression &)> walk =
                [&](const BoundExpression &x) {
                    if (x.GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                        auto &c = static_cast<const BoundColumnRef &>(x);
                        if (c.column_index < in_mask.size()) in_mask[c.column_index] = true;
                    } else if (x.GetExpressionType() == BoundExpressionType::COMPARISON) {
                        auto &c = static_cast<const BoundComparison &>(x);
                        walk(*c.left); walk(*c.right);
                    } else if (x.GetExpressionType() == BoundExpressionType::ARITHMETIC) {
                        auto &a = static_cast<const BoundArithmetic &>(x);
                        walk(*a.left); walk(*a.right);
                    } else if (x.GetExpressionType() == BoundExpressionType::FUNCTION) {
                        auto &f = static_cast<const BoundFunction &>(x);
                        for (auto &a : f.arguments) walk(*a);
                    } else if (x.GetExpressionType() == BoundExpressionType::CAST) {
                        auto &c = static_cast<const BoundCast &>(x);
                        walk(*c.child);
                    } else if (x.GetExpressionType() == BoundExpressionType::CONJUNCTION) {
                        auto &c = static_cast<const BoundConjunction &>(x);
                        walk(*c.left); walk(*c.right);
                    } else if (x.GetExpressionType() == BoundExpressionType::NEGATION) {
                        auto &n = static_cast<const BoundNegation &>(x);
                        walk(*n.child);
                    } else if (x.GetExpressionType() == BoundExpressionType::IS_NULL) {
                        auto &n = static_cast<const BoundIsNull &>(x);
                        walk(*n.child);
                    } else if (x.GetExpressionType() == BoundExpressionType::UNARY_MINUS) {
                        auto &u = static_cast<const BoundUnaryMinus &>(x);
                        walk(*u.child);
                    }
                };
            walk(*e);
        }
        children[0]->SetNeededOutputs(in_mask);
    }

    // Detect SELECT * style identity projection: every output column is
    // a plain BoundColumnRef pointing at the same input slot. When this
    // holds, GetData becomes a passthrough — chunks flow from the child
    // unchanged, no per-row ExpressionExecutor::Execute pass. Cuts the
    // 10M-row `SELECT * FROM x ORDER BY y LIMIT 10` cost roughly in half.
    bool IsIdentityProjection() const {
        if (children.empty() || expressions_.size() != children[0]->GetTypes().size())
            return false;
        for (idx_t i = 0; i < expressions_.size(); i++) {
            auto &e = expressions_[i];
            if (!e) return false;
            if (e->GetExpressionType() != BoundExpressionType::COLUMN_REF) return false;
            auto &c = static_cast<BoundColumnRef &>(*e);
            if (c.column_index != i) return false;
        }
        return true;
    }

    void Init() override {
        identity_passthrough_ = IsIdentityProjection();
        for (auto &child : children) child->Init();
    }

    bool GetData(DataChunk &result) override {
        if (identity_passthrough_) {
            // Pass-through: no expression evaluation, no per-row copy —
            // child fills `result` directly.
            return children[0]->GetData(result);
        }
        DataChunk input;
        if (!children.empty()) {
            input.Initialize(children[0]->GetTypes());
            if (!children[0]->GetData(input)) return false;
            if (input.size() == 0) return false;
        }

        result.Initialize(GetTypes());
        idx_t count = children.empty() ? 1 : input.size();

        for (idx_t col = 0; col < expressions_.size(); col++) {
            ExpressionExecutor::Execute(*expressions_[col], input,
                                        result.GetVector(col), count);
        }
        result.SetCardinality(count);
        return true;
    }

private:
    std::vector<BoundExprPtr> expressions_;
    bool identity_passthrough_ = false;
};

class PhysicalOrderBy : public PhysicalOperator {
public:
    PhysicalOrderBy(std::vector<BoundOrderBy> orders, std::vector<LogicalType> types)
        : PhysicalOperator(PhysicalOperatorType::ORDER_BY, std::move(types)),
          orders_(std::move(orders)) {}

    // Parent-projection pushdown: when a PhysicalProjection above this OrderBy
    // only references columns {c1, c2, ...}, pass-2 of CollectTopN_Primitive
    // (which decodes the K winning rows from parquet) can skip decoding all
    // other columns. A query over a 105-column file that selects only one
    // column would otherwise decode 104 unused cols (~1.2s wasted).
    void SetParentProjectionCols(std::vector<bool> mask) {
        parent_proj_mask_ = std::move(mask);
    }

    // Top-N pushdown: PhysicalLimit propagates `limit + offset` here.
    // When set, we use a bounded min/max heap of that size instead of
    // collecting + sorting the full input. Critical for queries like
    // `ORDER BY x DESC LIMIT 10` over millions of rows — full sort
    // is O(N log N), bounded heap is O(N log K) which can be 1000x
    // faster when K << N.
    void SetRowLimit(idx_t n) override { row_limit_ = n; }

    void Init() override {
        // TopN pushdown: when row_limit_ is set (PhysicalLimit fed it to us)
        // AND the ORDER BY is a single simple ColumnRef, tell our child "you
        // only need the top-K rows by this output column". Aggregates can
        // honor this with a bounded heap instead of materializing all groups.
        // A high-card GROUP BY with ORDER BY count DESC LIMIT 10 → aggregate
        // produces only 10 rows after pushdown.
        if (row_limit_ > 0 && orders_.size() == 1 && !children.empty() &&
            orders_[0].expression &&
            orders_[0].expression->GetExpressionType() ==
                BoundExpressionType::COLUMN_REF) {
            auto &cref = static_cast<BoundColumnRef &>(*orders_[0].expression);
            children[0]->SetTopNHint(cref.column_index,
                                      orders_[0].ascending, row_limit_);
        }
        for (auto &child : children) child->Init();
        collected_ = false;
        emit_pos_ = 0;
    }

    bool GetData(DataChunk &result) override {
        if (!collected_) {
            CollectAll();
            collected_ = true;
        }

        if (emit_pos_ >= sorted_rows_.size()) return false;

        result.Initialize(GetTypes());
        idx_t count = 0;
        while (emit_pos_ < sorted_rows_.size() && count < VECTOR_SIZE) {
            auto &row = sorted_rows_[emit_pos_];
            for (idx_t col = 0; col < row.size(); col++) {
                result.SetValue(col, count, row[col]);
            }
            emit_pos_++;
            count++;
        }
        result.SetCardinality(count);
        return count > 0;
    }

private:
    // Compare(a, b) returns true if `a` should come before `b` in the
    // final OUTPUT order. Used both as the std::sort comparator and as
    // the priority_queue ordering function.
    //
    // For ORDER BY by plain column ref this reads `a[col_idx]` directly.
    // For ORDER BY by expression (`ORDER BY ROUND(AVG(salary))`,
    // `ORDER BY x + y`) the expression evaluator can't be invoked here
    // cheaply per comparison, so callers must precompute `sort_keys_`
    // (one Value per ORDER BY clause per row) and the comparator
    // dispatches on `sort_keys_[idx]` instead. To keep the call sites
    // simple, when sort_keys_ is populated, callers sort an `idx_perm_`
    // vector via LessIdx; the std::vector<Value> overload is preserved
    // for the all-column-ref fast path.
    bool Less(const std::vector<Value> &a, const std::vector<Value> &b) const {
        for (auto &order : orders_) {
            idx_t col_idx = 0;
            if (order.expression->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                col_idx = static_cast<BoundColumnRef &>(*order.expression).column_index;
            }
            auto &va = a[col_idx];
            auto &vb = b[col_idx];
            if (va.IsNull() && vb.IsNull()) continue;
            // SQL standard NULL placement: nulls_first controls where NULLs
            // go regardless of ascending/descending direction. Defaults
            // (ASC → NULLS LAST, DESC → NULLS FIRST) are applied at parse
            // time; explicit NULLS FIRST/LAST overrides.
            if (va.IsNull()) return order.nulls_first;
            if (vb.IsNull()) return !order.nulls_first;
            if (va < vb) return order.ascending;
            if (vb < va) return !order.ascending;
        }
        return false;
    }

    // Compare two precomputed key rows. Used when at least one ORDER BY
    // expression is a non-trivial scalar (function, arithmetic, cast).
    bool LessKeys(const std::vector<Value> &ka, const std::vector<Value> &kb) const {
        for (size_t k_i = 0; k_i < orders_.size(); k_i++) {
            auto &va = ka[k_i];
            auto &vb = kb[k_i];
            if (va.IsNull() && vb.IsNull()) continue;
            if (va.IsNull()) return orders_[k_i].nulls_first;
            if (vb.IsNull()) return !orders_[k_i].nulls_first;
            if (va < vb) return orders_[k_i].ascending;
            if (vb < va) return !orders_[k_i].ascending;
        }
        return false;
    }

    // True iff every ORDER BY clause is a plain column reference. The
    // hot CollectFull / CollectTopN paths use this to skip expression
    // evaluation entirely.
    bool AllOrderByAreColumnRefs() const {
        for (auto &o : orders_) {
            if (o.expression->GetExpressionType() != BoundExpressionType::COLUMN_REF) return false;
        }
        return true;
    }

    // Evaluate every ORDER BY expression on the given input chunk and
    // append per-row keys to `out_keys`. One Value per ORDER BY clause.
    void EvaluateOrderKeys(DataChunk &chunk,
                           std::vector<std::vector<Value>> &out_keys) const {
        std::vector<Vector> key_vecs;
        key_vecs.reserve(orders_.size());
        for (auto &o : orders_) {
            Vector v(o.expression->GetReturnType(), chunk.size());
            ExpressionExecutor::Execute(*o.expression, chunk, v, chunk.size());
            key_vecs.push_back(std::move(v));
        }
        for (idx_t i = 0; i < chunk.size(); i++) {
            std::vector<Value> key;
            key.reserve(orders_.size());
            for (idx_t k_i = 0; k_i < orders_.size(); k_i++) {
                key.push_back(key_vecs[k_i].GetValue(i));
            }
            out_keys.push_back(std::move(key));
        }
    }

    void CollectAll() {
        // Bounded-heap top-N path. Triggered when LIMIT pushed a small
        // limit down. Threshold guards against degenerate cases — for
        // very large K the heap loses its advantage and a full sort is
        // simpler.
        constexpr idx_t TOP_N_THRESHOLD = 65536;
        if (row_limit_ > 0 && row_limit_ <= TOP_N_THRESHOLD) {
            CollectTopN(row_limit_);
            return;
        }
        CollectFull();
    }

    // After ResortByFullKeys, trim down to the original K limit.
    void TrimToK(idx_t k) {
        if (sorted_rows_.size() > k) sorted_rows_.resize(k);
    }

    // Multi-key tiebreaker: after a primitive top-K on orders_[0], re-sort
    // sorted_rows_ by the full ORDER BY key list. K winners is small
    // (typically 10) so this is cheap even with full Value comparison.
    void ResortByFullKeys() {
        if (sorted_rows_.size() <= 1 || orders_.size() <= 1) return;
        // Each row is in source-schema order; ORDER BY clauses index into
        // that schema (LogicalOrderBy is built before the deferred projection).
        std::vector<idx_t> ord_cols;
        ord_cols.reserve(orders_.size());
        for (auto &o : orders_) {
            if (o.expression->GetExpressionType() != BoundExpressionType::COLUMN_REF) {
                // Non-trivial expression in second+ ORDER BY — skip resort,
                // fall back to whatever the primitive path produced. Caller
                // can route to the slow generic path if exactness matters.
                return;
            }
            ord_cols.push_back(static_cast<BoundColumnRef &>(*o.expression).column_index);
        }
        std::sort(sorted_rows_.begin(), sorted_rows_.end(),
            [this, &ord_cols](const std::vector<Value> &a, const std::vector<Value> &b) {
                for (size_t i = 0; i < orders_.size(); i++) {
                    idx_t c = ord_cols[i];
                    if (c >= a.size() || c >= b.size()) continue;
                    auto &va = a[c];
                    auto &vb = b[c];
                    if (va.IsNull() && vb.IsNull()) continue;
                    if (va.IsNull()) return orders_[i].nulls_first;
                    if (vb.IsNull()) return !orders_[i].nulls_first;
                    if (va < vb) return orders_[i].ascending;
                    if (vb < va) return !orders_[i].ascending;
                }
                return false;
            });
    }

    // Specialised path: single-column ORDER BY on a primitive numeric
    // column. Threshold-comparison happens against an int64/double
    // directly, no Value boxing on the loser path. This is the shape
    // of `ORDER BY col DESC LIMIT N` queries that show up everywhere.
    template <typename T>
    void CollectTopN_Primitive(idx_t k, idx_t key_col, bool ascending) {
        struct HeapEntry {
            T key;
            bool is_null = false;
            std::vector<Value> row;
        };
        // NULLS LAST: heap top is the worst candidate (first to evict).
        // For ASC top-K, top = greatest value; for DESC, top = smallest.
        // NULL must be the worst (= top) so any non-null evicts it.
        auto cmp = [ascending](const HeapEntry &a, const HeapEntry &b) {
            if (a.is_null && b.is_null) return false;
            if (a.is_null) return !ascending; // ASC: a is top, not < b
            if (b.is_null) return ascending;
            // DuckDB's order: NaN above +inf (#42).
            return ascending ? SqlLess(a.key, b.key) : SqlGreater(a.key, b.key);
        };

        // Sequential path.
        std::priority_queue<HeapEntry, std::vector<HeapEntry>, decltype(cmp)> pq(cmp);
        // Rows whose key is NULL, up to k: they were dropped, so ORDER BY
        // x LIMIT k lost them (#38). They go after the others, or before
        // them under NULLS FIRST.
        std::vector<std::vector<Value>> null_rows;
        DataChunk chunk;
        while (true) {
            chunk.Initialize(children[0]->GetTypes());
            if (!children[0]->GetData(chunk)) break;
            auto &kvec = chunk.GetVector(key_col);
            auto *kdata = kvec.GetData<T>();
            auto &kvalid = kvec.GetValidity();
            bool key_all_valid = kvalid.AllValid();
            idx_t n = chunk.size();
            for (idx_t i = 0; i < n; i++) {
                if (!key_all_valid && !kvalid.RowIsValid(i)) {
                    if (null_rows.size() < k) {
                        std::vector<Value> row;
                        row.reserve(chunk.ColumnCount());
                        for (idx_t col = 0; col < chunk.ColumnCount(); col++) {
                            row.push_back(chunk.GetValue(col, i));
                        }
                        null_rows.push_back(std::move(row));
                    }
                    continue;
                }
                T key = kdata[i];
                if (pq.size() < k) {
                    HeapEntry e; e.key = key;
                    e.row.reserve(chunk.ColumnCount());
                    for (idx_t col = 0; col < chunk.ColumnCount(); col++) {
                        e.row.push_back(chunk.GetValue(col, i));
                    }
                    pq.push(std::move(e));
                } else {
                    bool wins = ascending ? SqlLess(key, pq.top().key) : SqlGreater(key, pq.top().key);
                    if (!wins) continue;
                    HeapEntry e; e.key = key;
                    e.row.reserve(chunk.ColumnCount());
                    for (idx_t col = 0; col < chunk.ColumnCount(); col++) {
                        e.row.push_back(chunk.GetValue(col, i));
                    }
                    pq.pop();
                    pq.push(std::move(e));
                }
            }
        }
        sorted_rows_.reserve(pq.size() + null_rows.size());
        while (!pq.empty()) {
            sorted_rows_.push_back(std::move(const_cast<HeapEntry &>(pq.top()).row));
            pq.pop();
        }
        std::reverse(sorted_rows_.begin(), sorted_rows_.end());
        if (!null_rows.empty()) {
            bool nulls_first = !orders_.empty() && orders_[0].nulls_first;
            if (nulls_first) {
                null_rows.insert(null_rows.end(), std::make_move_iterator(sorted_rows_.begin()),
                                 std::make_move_iterator(sorted_rows_.end()));
                sorted_rows_ = std::move(null_rows);
            } else {
                sorted_rows_.insert(sorted_rows_.end(), std::make_move_iterator(null_rows.begin()),
                                    std::make_move_iterator(null_rows.end()));
            }
            if (orders_.size() <= 1 && sorted_rows_.size() > k) sorted_rows_.resize(k);
        }
    }

    void CollectTopN(idx_t k) {
        // Try the fast specialised path first. Single-column ORDER BY on a
        // primitive type avoids Value boxing per losing row — drops the
        // cost of `ORDER BY x DESC LIMIT 10` over 10M rows roughly 2x
        // by skipping the OrderKey allocation in the hot loop.
        //
        // Multi-col ORDER BY: route to primitive path on first column when
        // it's a column ref of primitive type. Oversample K' = K * 64 in
        // the primitive heap so all ties at the K-th boundary are likely
        // captured, then resort by full multi-key and trim to K. For an
        // `ORDER BY <ts>, <varchar> LIMIT 10` shape
        // this captures the 10-30 rows that share each unique timestamp.
        // One key only: with more, the heap kept k * 4 rows of the first
        // key's ties, chosen as they came, and the others decided among
        // those, not among all the ties (#38); the generic heap below
        // compares every key.
        if (orders_.size() == 1 &&
            orders_[0].expression->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
            auto &cref = static_cast<BoundColumnRef &>(*orders_[0].expression);
            auto tid = orders_[0].expression->GetReturnType().id();
            const bool multi = orders_.size() > 1;
            const idx_t kk = multi ? k * 4 : k;
            switch (tid) {
            case LogicalTypeId::BIGINT:
            case LogicalTypeId::TIMESTAMP:
            case LogicalTypeId::TIMESTAMP_TZ:
            case LogicalTypeId::TIME:
                // TIMESTAMP / TIMESTAMP_TZ / TIME store int64 microseconds
                // internally; reuse the int64 primitive heap path. An
                // `ORDER BY <timestamp> LIMIT 10` dropped through to the
                // slow Value-based path before this case.
                CollectTopN_Primitive<int64_t>(kk, cref.column_index, orders_[0].ascending);
                if (multi) { ResortByFullKeys(); TrimToK(k); }
                return;
            case LogicalTypeId::INTEGER:
            case LogicalTypeId::DATE:
                // DATE stores int32 days-since-epoch; same int32 path.
                CollectTopN_Primitive<int32_t>(kk, cref.column_index, orders_[0].ascending);
                if (multi) { ResortByFullKeys(); TrimToK(k); }
                return;
            case LogicalTypeId::DOUBLE:
                CollectTopN_Primitive<double>(kk, cref.column_index, orders_[0].ascending);
                if (multi) { ResortByFullKeys(); TrimToK(k); }
                return;
            case LogicalTypeId::FLOAT:
                CollectTopN_Primitive<float>(kk, cref.column_index, orders_[0].ascending);
                if (multi) { ResortByFullKeys(); TrimToK(k); }
                return;
            default: break;
            }
        }

        // priority_queue with Less-by-output-order at the top. Heap top
        // is therefore the WORST candidate (latest in output order); when
        // a better candidate arrives we pop top and push the new row.
        //
        // Hot-loop optimisation: the heap fills up after the first K rows,
        // and after that the VAST majority of input rows lose to the
        // heap top. Materialising every losing row into a vector<Value>
        // is wasted work. We extract the order-by KEYS only (cheap typed
        // values), compare those against the current heap-top key, and
        // only build the full vector<Value> for rows that actually go
        // into the heap. On `ORDER BY x DESC LIMIT 10` over 10M rows
        // this skips boxing 99.9999% of input rows.
        struct OrderKey {
            std::vector<Value> values; // one per ORDER BY clause
        };
        struct HeapEntry {
            OrderKey key;
            std::vector<Value> row;
        };
        // Collect order-key column indices once (constant across rows).
        // For non-trivial ORDER BY expressions (e.g. ROUND(AVG(x))) the
        // column-index path doesn't apply, so we evaluate the expression
        // on each chunk via ExpressionExecutor and extract keys from the
        // result vector instead.
        bool simple = AllOrderByAreColumnRefs();
        std::vector<idx_t> key_cols;
        if (simple) {
            key_cols.reserve(orders_.size());
            for (auto &o : orders_) {
                key_cols.push_back(static_cast<BoundColumnRef &>(*o.expression).column_index);
            }
        }

        auto less_keys = [this](const OrderKey &a, const OrderKey &b) {
            for (size_t k_i = 0; k_i < orders_.size(); k_i++) {
                auto &va = a.values[k_i];
                auto &vb = b.values[k_i];
                if (va.IsNull() && vb.IsNull()) continue;
                if (va.IsNull()) return orders_[k_i].nulls_first;
                if (vb.IsNull()) return !orders_[k_i].nulls_first;
                if (va < vb) return orders_[k_i].ascending;
                if (vb < va) return !orders_[k_i].ascending;
            }
            return false;
        };
        auto cmp = [&](const HeapEntry &a, const HeapEntry &b) {
            return less_keys(a.key, b.key);
        };
        std::priority_queue<HeapEntry, std::vector<HeapEntry>, decltype(cmp)> pq(cmp);

        DataChunk chunk;
        while (true) {
            chunk.Initialize(children[0]->GetTypes());
            if (!children[0]->GetData(chunk)) break;
            // For non-trivial ORDER BY expressions, evaluate them on the
            // whole chunk up front, then extract per-row keys cheaply.
            std::vector<Vector> expr_key_vecs;
            if (!simple) {
                expr_key_vecs.reserve(orders_.size());
                for (auto &o : orders_) {
                    Vector v(o.expression->GetReturnType(), chunk.size());
                    ExpressionExecutor::Execute(*o.expression, chunk, v, chunk.size());
                    expr_key_vecs.push_back(std::move(v));
                }
            }
            for (idx_t i = 0; i < chunk.size(); i++) {
                // Build the key (cheap — only the ORDER BY columns).
                OrderKey row_key;
                row_key.values.reserve(orders_.size());
                if (simple) {
                    for (idx_t kc : key_cols) row_key.values.push_back(chunk.GetValue(kc, i));
                } else {
                    for (idx_t k_i = 0; k_i < orders_.size(); k_i++) {
                        row_key.values.push_back(expr_key_vecs[k_i].GetValue(i));
                    }
                }

                if (pq.size() < k) {
                    HeapEntry e;
                    e.key = std::move(row_key);
                    e.row.reserve(chunk.ColumnCount());
                    for (idx_t col = 0; col < chunk.ColumnCount(); col++) {
                        e.row.push_back(chunk.GetValue(col, i));
                    }
                    pq.push(std::move(e));
                } else if (less_keys(row_key, pq.top().key)) {
                    HeapEntry e;
                    e.key = std::move(row_key);
                    e.row.reserve(chunk.ColumnCount());
                    for (idx_t col = 0; col < chunk.ColumnCount(); col++) {
                        e.row.push_back(chunk.GetValue(col, i));
                    }
                    pq.pop();
                    pq.push(std::move(e));
                }
                // else: row loses, no full materialisation. The hot path.
            }
        }

        // Drain: heap pops worst-first, so we get [worst .. best].
        // Reverse for the [best .. worst] output order.
        sorted_rows_.reserve(pq.size());
        while (!pq.empty()) {
            sorted_rows_.push_back(std::move(const_cast<HeapEntry &>(pq.top()).row));
            pq.pop();
        }
        std::reverse(sorted_rows_.begin(), sorted_rows_.end());
    }

    void CollectFull() {
        DataChunk chunk;
        bool simple = AllOrderByAreColumnRefs();
        std::vector<std::vector<Value>> keys; // populated only when !simple
        while (true) {
            chunk.Initialize(children[0]->GetTypes());
            if (!children[0]->GetData(chunk)) break;
            if (!simple) EvaluateOrderKeys(chunk, keys);
            for (idx_t i = 0; i < chunk.size(); i++) {
                std::vector<Value> row;
                for (idx_t col = 0; col < chunk.ColumnCount(); col++) {
                    row.push_back(chunk.GetValue(col, i));
                }
                sorted_rows_.push_back(std::move(row));
            }
        }
        if (simple) {
            std::sort(sorted_rows_.begin(), sorted_rows_.end(),
                [this](const std::vector<Value> &a, const std::vector<Value> &b) {
                    return Less(a, b);
                });
        } else {
            // Sort an index permutation by precomputed keys, then
            // permute sorted_rows_ in one pass.
            std::vector<idx_t> perm(sorted_rows_.size());
            for (idx_t i = 0; i < perm.size(); i++) perm[i] = i;
            std::sort(perm.begin(), perm.end(),
                [&](idx_t a, idx_t b) { return LessKeys(keys[a], keys[b]); });
            std::vector<std::vector<Value>> reordered;
            reordered.reserve(sorted_rows_.size());
            for (auto i : perm) reordered.push_back(std::move(sorted_rows_[i]));
            sorted_rows_ = std::move(reordered);
        }
    }

    std::vector<BoundOrderBy> orders_;
    std::vector<std::vector<Value>> sorted_rows_;
    bool collected_ = false;
    idx_t emit_pos_ = 0;
    idx_t row_limit_ = 0;  // 0 = no limit pushdown, full sort path
    // Parent-projection pushdown mask. Empty = no info (decode all cols).
    // Non-empty: parent_proj_mask_[c] = true → col c is consumed downstream.
    std::vector<bool> parent_proj_mask_;
};

class PhysicalLimit : public PhysicalOperator {
public:
    PhysicalLimit(int64_t limit_count, int64_t offset_count, std::vector<LogicalType> types)
        : PhysicalOperator(PhysicalOperatorType::LIMIT, std::move(types)),
          limit_count_(limit_count), offset_count_(offset_count) {}

    void Init() override {
        // Push LIMIT hint down so partial-sort / early-cut optimizations kick in.
        if (limit_count_ >= 0 && !children.empty()) {
            idx_t total = static_cast<idx_t>(limit_count_) + static_cast<idx_t>(offset_count_);
            children[0]->SetRowLimit(total);
        }
        for (auto &child : children) child->Init();
        rows_emitted_ = 0;
        rows_skipped_ = 0;
    }

    bool GetData(DataChunk &result) override {
        if (limit_count_ >= 0 && rows_emitted_ >= static_cast<idx_t>(limit_count_)) {
            return false;
        }

        while (true) {
            DataChunk input;
            input.Initialize(children[0]->GetTypes());
            if (!children[0]->GetData(input)) return false;

            result.Initialize(GetTypes());
            idx_t result_count = 0;

            for (idx_t i = 0; i < input.size(); i++) {
                if (rows_skipped_ < static_cast<idx_t>(offset_count_)) {
                    rows_skipped_++;
                    continue;
                }
                if (limit_count_ >= 0 &&
                    rows_emitted_ >= static_cast<idx_t>(limit_count_)) {
                    break;
                }
                for (idx_t col = 0; col < input.ColumnCount(); col++) {
                    result.SetValue(col, result_count, input.GetValue(col, i));
                }
                result_count++;
                rows_emitted_++;
            }

            if (result_count > 0) {
                result.SetCardinality(result_count);
                return true;
            }
        }
    }

private:
    int64_t limit_count_;
    int64_t offset_count_;
    idx_t rows_emitted_ = 0;
    idx_t rows_skipped_ = 0;
};

class PhysicalInsert : public PhysicalOperator {
public:
    PhysicalInsert(TableCatalogEntry *table, std::vector<std::vector<BoundExprPtr>> values)
        : PhysicalOperator(PhysicalOperatorType::INSERT, {}),
          table_(table), values_(std::move(values)) {}

    void Init() override { done_ = false; affected_ = 0; }
    int64_t AffectedRows() const override { return affected_; }

    bool GetData(DataChunk &result) override {
        if (done_) return false;
        done_ = true;

        auto types = table_->GetTypes();
        DataChunk chunk;
        // zugodb: every row of the VALUES list goes into this one chunk,
        // so it is sized for them all; at the default 2048 rows a longer
        // list wrote past its buffers (#45).
        chunk.Initialize(types, std::max<idx_t>(VECTOR_SIZE, values_.size()));
        affected_ = static_cast<int64_t>(values_.size());

        for (auto &row : values_) {
            idx_t row_idx = chunk.size();
            for (idx_t col = 0; col < row.size(); col++) {
                auto val = ExpressionExecutor::ExecuteScalar(*row[col]);
                chunk.SetValue(col, row_idx, val);
            }
        }

        // Enforce PRIMARY KEY uniqueness before committing the rows.
        table_->CheckPrimaryKeyForChunk(chunk);
        table_->GetStorage().Append(chunk);
        return false; // No result set.
    }

private:
    TableCatalogEntry *table_;
    std::vector<std::vector<BoundExprPtr>> values_;
    bool done_ = false;
    int64_t affected_ = 0;
};

class PhysicalCreateTable : public PhysicalOperator {
public:
    PhysicalCreateTable(Catalog &catalog, const std::string &name,
                        std::vector<ColumnDefinition> columns, bool if_not_exists)
        : PhysicalOperator(PhysicalOperatorType::CREATE_TABLE, {}),
          catalog_(catalog), table_name_(name), columns_(std::move(columns)),
          if_not_exists_(if_not_exists) {}

    void Init() override { done_ = false; }

    bool GetData(DataChunk &result) override {
        if (done_) return false;
        done_ = true;

        if (if_not_exists_ && catalog_.GetTable(table_name_)) {
            return false;
        }

        auto types_vec = std::vector<LogicalType>();
        for (auto &col : columns_) types_vec.push_back(col.type);

        auto &entry = catalog_.CreateTable(table_name_, columns_);
        entry.SetStorage(std::make_shared<DataTable>(types_vec));
        return false;
    }

private:
    Catalog &catalog_;
    std::string table_name_;
    std::vector<ColumnDefinition> columns_;
    bool if_not_exists_;
    bool done_ = false;
};

class PhysicalDropTable : public PhysicalOperator {
public:
    PhysicalDropTable(Catalog &catalog, const std::string &name, bool if_exists)
        : PhysicalOperator(PhysicalOperatorType::DROP_TABLE, {}),
          catalog_(catalog), table_name_(name), if_exists_(if_exists) {}

    void Init() override { done_ = false; }

    bool GetData(DataChunk &result) override {
        if (done_) return false;
        done_ = true;

        if (!catalog_.DropTable(table_name_) && !if_exists_) {
            throw CatalogException("Table '" + table_name_ + "' does not exist");
        }
        return false;
    }

private:
    Catalog &catalog_;
    std::string table_name_;
    bool if_exists_;
    bool done_ = false;
};

// ============================================================================
// Window
// ============================================================================

class PhysicalWindow : public PhysicalOperator {
public:
    PhysicalWindow(std::vector<BoundExprPtr> select_list, BoundExprPtr qualify,
                   std::vector<LogicalType> types)
        : PhysicalOperator(PhysicalOperatorType::TABLE_SCAN, std::move(types)),
          select_list_(std::move(select_list)), qualify_(std::move(qualify)) {}

    void SetRowLimit(idx_t n) override { row_limit_ = n; }

    void Init() override {
        for (auto &child : children) child->Init();
        setup_done_ = false;
        use_fallback_ = false;
        emit_part_ = 0;
        emit_pos_ = 0;
        rank_cur_ = 1;
        rank_prev_ = INVALID_INDEX;
        dense_cur_ = 1;
        dense_prev_ = INVALID_INDEX;
        input_ = InputBuf{};
        partitions_.clear();
        select_win_info_.clear();
        qualify_win_info_ = WinInfo{};
        qualify_has_win_ = false;
        qualify_const_ = nullptr;
        qualify_op_ = -1;
        qualify_win_on_left_ = true;
        fallback_rows_.clear();
        sort_win_ = nullptr;
        sort_single_col_ok_ = false;
        sort_col_ = INVALID_INDEX;
        sort_asc_ = true;
        sort_tid_ = LogicalTypeId::SQLNULL;
        sort_i32_.clear();
        sort_i64_.clear();
        sort_d_.clear();
        sort_s_.clear();
        partition_sorted_.clear();
    }

    bool GetData(DataChunk &result) override {
        if (!setup_done_) { Setup(); setup_done_ = true; }

        if (use_fallback_) {
            if (emit_pos_ >= fallback_rows_.size()) return false;
            result.Initialize(GetTypes());
            idx_t count = 0;
            while (emit_pos_ < fallback_rows_.size() && count < VECTOR_SIZE) {
                auto &row = fallback_rows_[emit_pos_];
                for (idx_t col = 0; col < row.size(); col++) {
                    result.SetValue(col, count, row[col]);
                }
                emit_pos_++;
                count++;
            }
            result.SetCardinality(count);
            return count > 0;
        }

        if (emit_part_ >= partitions_.size()) return false;

        if (result.ColumnCount() != GetTypes().size()) result.Initialize(GetTypes());
        else result.Reset();

        // Vectorised fast path: every output column is either a plain
        // COLUMN_REF or a positional window function (ROW_NUMBER / RANK /
        // DENSE_RANK with no qualify, no lag/lead, no aggregate-shape
        // window). The 10M-row sort+emit becomes a typed-array gather +
        // a positional sequence write, no Value boxing per output row.
        if (!qualify_has_win_ && CanUseVectorisedEmit()) {
            idx_t out = VectorisedEmit(result);
            return out > 0;
        }

        idx_t out = 0;
        while (emit_part_ < partitions_.size() && out < VECTOR_SIZE) {
            // Lazy sort: only pay sort cost for partitions we actually emit from.
            if (emit_pos_ == 0) SortOnePartition(emit_part_);
            auto &idxs = partitions_[emit_part_];
            while (emit_pos_ < idxs.size() && out < VECTOR_SIZE) {
                idx_t input_row = idxs[emit_pos_];

                if (qualify_has_win_ && !EvalQualify(emit_part_, emit_pos_)) {
                    emit_pos_++;
                    continue;
                }

                for (idx_t col = 0; col < select_list_.size(); col++) {
                    auto &expr = select_list_[col];
                    auto et = expr->GetExpressionType();
                    Value v;
                    if (et == BoundExpressionType::WINDOW) {
                        v = ComputeWindowAt(select_win_info_[col], emit_part_, emit_pos_);
                    } else if (et == BoundExpressionType::COLUMN_REF) {
                        auto &ref = static_cast<BoundColumnRef &>(*expr);
                        v = input_.Get(input_row, ref.column_index);
                    } else if (et == BoundExpressionType::CONSTANT) {
                        v = static_cast<BoundConstant &>(*expr).value;
                    } else {
                        // Generic expression: materialize row and evaluate.
                        DataChunk single;
                        auto in_types = children[0]->GetTypes();
                        single.Initialize(in_types);
                        for (idx_t c = 0; c < input_.types.size(); c++) {
                            single.SetValue(c, 0, input_.Get(input_row, c));
                        }
                        Vector res(expr->GetReturnType());
                        ExpressionExecutor::Execute(*expr, single, res, 1);
                        v = res.GetValue(0);
                    }
                    result.SetValue(col, out, v);
                }

                emit_pos_++;
                out++;
            }
            if (emit_pos_ >= idxs.size()) {
                emit_part_++;
                emit_pos_ = 0;
                rank_cur_ = 1;
                rank_prev_ = INVALID_INDEX;
                dense_cur_ = 1;
                dense_prev_ = INVALID_INDEX;
            }
        }

        result.SetCardinality(out);
        return out > 0;
    }

    // Returns true iff every select_list entry can be emitted via the
    // typed-vectorised path (no per-row Value boxing).
    bool CanUseVectorisedEmit() const {
        for (idx_t col = 0; col < select_list_.size(); col++) {
            auto &expr = select_list_[col];
            auto et = expr->GetExpressionType();
            if (et == BoundExpressionType::COLUMN_REF) continue;
            if (et == BoundExpressionType::WINDOW) {
                auto &info = select_win_info_[col];
                if (info.fn == WF_ROW_NUMBER) continue;
                // RANK / DENSE_RANK need to peek at the previous order-by
                // value — also vectorisable but slightly more careful.
                // With an ORDER BY whose first key is not a column, the
                // peers are not known here; ComputeWindowAt decides.
                if (info.fn == WF_RANK || info.fn == WF_DENSE_RANK) {
                    if (info.has_order && info.rank_order_col == INVALID_INDEX) return false;
                    continue;
                }
                return false;
            }
            return false;
        }
        return true;
    }

    // Typed-vectorised emit. For each output column:
    //   - COLUMN_REF: gather typed values from the input chunks via
    //     `idxs[emit_pos_..emit_pos_+count]`, write into the output's
    //     typed array directly. Validity bit copied per row.
    //   - ROW_NUMBER: write the running 1-based counter into the output
    //     int32 array.
    //   - RANK / DENSE_RANK: same as ROW_NUMBER but bumped only on
    //     tie-break of the order-by key.
    idx_t VectorisedEmit(DataChunk &result) {
        idx_t out = 0;
        idx_t num_cols = select_list_.size();

        // Pre-cache, per output column, the BoundColumnRef.column_index
        // or the WinFunc kind. Resolved at first call.
        if (vec_emit_cache_.empty()) {
            vec_emit_cache_.resize(num_cols);
            for (idx_t c = 0; c < num_cols; c++) {
                auto &expr = select_list_[c];
                if (expr->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                    vec_emit_cache_[c].is_col = true;
                    vec_emit_cache_[c].input_col =
                        static_cast<BoundColumnRef &>(*expr).column_index;
                } else {
                    vec_emit_cache_[c].is_col = false;
                    vec_emit_cache_[c].fn = select_win_info_[c].fn;
                    vec_emit_cache_[c].rank_order_col = select_win_info_[c].rank_order_col;
                    vec_emit_cache_[c].rank_order_tid = select_win_info_[c].rank_order_tid;
                }
            }
        }

        while (emit_part_ < partitions_.size() && out < VECTOR_SIZE) {
            if (emit_pos_ == 0) SortOnePartition(emit_part_);
            auto &idxs = partitions_[emit_part_];
            idx_t avail = idxs.size() - emit_pos_;
            idx_t take = std::min<idx_t>(avail, VECTOR_SIZE - out);
            if (take == 0) {
                emit_part_++;
                emit_pos_ = 0;
                rank_cur_ = 1;
                rank_prev_ = INVALID_INDEX;
                dense_cur_ = 1;
                dense_prev_ = INVALID_INDEX;
                continue;
            }

            for (idx_t c = 0; c < num_cols; c++) {
                auto &slot = vec_emit_cache_[c];
                auto &dst_vec = result.GetVector(c);
                if (slot.is_col) {
                    idx_t in_col = slot.input_col;
                    LogicalTypeId in_tid = input_.types[in_col];
                    if (in_tid == LogicalTypeId::BIGINT) {
                        auto *d = dst_vec.GetData<int64_t>();
                        for (idx_t r = 0; r < take; r++) {
                            idx_t row = idxs[emit_pos_ + r];
                            if (input_.IsNull(row, in_col))
                                dst_vec.GetValidity().SetInvalid(out + r);
                            else d[out + r] = input_.GetInt64(row, in_col);
                        }
                    } else if (in_tid == LogicalTypeId::INTEGER) {
                        auto *d = dst_vec.GetData<int32_t>();
                        for (idx_t r = 0; r < take; r++) {
                            idx_t row = idxs[emit_pos_ + r];
                            if (input_.IsNull(row, in_col))
                                dst_vec.GetValidity().SetInvalid(out + r);
                            else d[out + r] = input_.GetInt32(row, in_col);
                        }
                    } else if (in_tid == LogicalTypeId::DOUBLE) {
                        auto *d = dst_vec.GetData<double>();
                        for (idx_t r = 0; r < take; r++) {
                            idx_t row = idxs[emit_pos_ + r];
                            if (input_.IsNull(row, in_col))
                                dst_vec.GetValidity().SetInvalid(out + r);
                            else d[out + r] = input_.GetDouble(row, in_col);
                        }
                    } else if (in_tid == LogicalTypeId::VARCHAR) {
                        // Direct string_t write into the output vector,
                        // skipping Value::VARCHAR + dst_vec.SetValue boxing.
                        // Inline strings (<=12 bytes) get copied straight
                        // into the slot. Long strings allocate once via
                        // the dst vector's string buffer. 10M-row VARCHAR
                        // window emit drops from ~3 s to ~300 ms.
                        auto *dst = dst_vec.GetData<string_t>();
                        auto &str_buf = dst_vec.GetStringBuffer();
                        for (idx_t r = 0; r < take; r++) {
                            idx_t row = idxs[emit_pos_ + r];
                            if (input_.IsNull(row, in_col)) {
                                dst_vec.GetValidity().SetInvalid(out + r);
                                dst[out + r] = string_t("", 0);
                                continue;
                            }
                            const string_t &s = input_.GetStr(row, in_col);
                            uint32_t sz = s.GetSize();
                            if (sz <= string_t::INLINE_LENGTH) {
                                dst[out + r] = string_t(s.GetData(), sz);
                            } else {
                                const char *heap = str_buf.AddString(s.GetData(), sz);
                                dst[out + r] = string_t(heap, sz);
                            }
                        }
                    } else {
                        // Fallback for less-common types.
                        for (idx_t r = 0; r < take; r++) {
                            idx_t row = idxs[emit_pos_ + r];
                            dst_vec.SetValue(out + r, input_.Get(row, in_col));
                        }
                    }
                } else {
                    // Window function — typed sequence per partition.
                    // ROW_NUMBER / RANK / DENSE_RANK all return BIGINT
                    // (matches the binder's return-type assignment).
                    auto *d = dst_vec.GetData<int64_t>();
                    if (slot.fn == WF_ROW_NUMBER) {
                        for (idx_t r = 0; r < take; r++) {
                            d[out + r] = static_cast<int64_t>(emit_pos_ + r + 1);
                        }
                    } else if (slot.fn == WF_RANK || slot.fn == WF_DENSE_RANK) {
                        idx_t order_col = slot.rank_order_col;
                        LogicalTypeId otid = slot.rank_order_tid;
                        for (idx_t r = 0; r < take; r++) {
                            idx_t row = idxs[emit_pos_ + r];
                            // Compare order key vs previous; bump on change.
                            bool same = false;
                            if (rank_prev_ != INVALID_INDEX) {
                                // No ORDER BY (CanUseVectorisedEmit admits no
                                // other case without an order column): every
                                // row of the partition is a peer, so rank()
                                // over () is 1. This read column INVALID_INDEX
                                // (libzugodb Stage 7, #11).
                                if (order_col == INVALID_INDEX)
                                    same = true;
                                else if (otid == LogicalTypeId::BIGINT)
                                    same = input_.GetInt64(rank_prev_, order_col) == input_.GetInt64(row, order_col);
                                else if (otid == LogicalTypeId::INTEGER)
                                    same = input_.GetInt32(rank_prev_, order_col) == input_.GetInt32(row, order_col);
                                else if (otid == LogicalTypeId::DOUBLE)
                                    same = SqlEq(input_.GetDouble(rank_prev_, order_col), input_.GetDouble(row, order_col));
                                else
                                    same = input_.Get(rank_prev_, order_col) == input_.Get(row, order_col);
                            }
                            if (slot.fn == WF_RANK) {
                                if (!same) rank_cur_ = static_cast<idx_t>(emit_pos_ + r + 1);
                                d[out + r] = static_cast<int64_t>(rank_cur_);
                            } else {
                                if (!same) dense_cur_++;
                                if (rank_prev_ == INVALID_INDEX) dense_cur_ = 1;
                                d[out + r] = static_cast<int64_t>(dense_cur_);
                            }
                            rank_prev_ = row;
                        }
                    }
                }
            }

            emit_pos_ += take;
            out += take;
            if (emit_pos_ >= idxs.size()) {
                emit_part_++;
                emit_pos_ = 0;
                rank_cur_ = 1;
                rank_prev_ = INVALID_INDEX;
                dense_cur_ = 1;
                dense_prev_ = INVALID_INDEX;
            }
        }
        result.SetCardinality(out);
        return out;
    }

private:
    // Walk an expression tree, marking column indices that are referenced.
    static void CollectRefs(const BoundExpression &e, std::vector<bool> &used) {
        switch (e.GetExpressionType()) {
        case BoundExpressionType::COLUMN_REF: {
            auto &r = static_cast<const BoundColumnRef &>(e);
            if (r.column_index < used.size()) used[r.column_index] = true;
            break;
        }
        case BoundExpressionType::WINDOW: {
            auto &w = static_cast<const BoundWindowExpression &>(e);
            for (auto &p : w.partition_by) CollectRefs(*p, used);
            for (auto &o : w.order_by) CollectRefs(*o.expression, used);
            for (auto &a : w.arguments) CollectRefs(*a, used);
            break;
        }
        case BoundExpressionType::COMPARISON: {
            auto &c = static_cast<const BoundComparison &>(e);
            CollectRefs(*c.left, used);
            CollectRefs(*c.right, used);
            break;
        }
        case BoundExpressionType::CONJUNCTION: {
            auto &c = static_cast<const BoundConjunction &>(e);
            CollectRefs(*c.left, used);
            CollectRefs(*c.right, used);
            break;
        }
        case BoundExpressionType::NEGATION: {
            auto &n = static_cast<const BoundNegation &>(e);
            CollectRefs(*n.child, used);
            break;
        }
        case BoundExpressionType::IS_NULL: {
            auto &n = static_cast<const BoundIsNull &>(e);
            CollectRefs(*n.child, used);
            break;
        }
        case BoundExpressionType::ARITHMETIC: {
            auto &a = static_cast<const BoundArithmetic &>(e);
            CollectRefs(*a.left, used);
            CollectRefs(*a.right, used);
            break;
        }
        case BoundExpressionType::FUNCTION: {
            auto &f = static_cast<const BoundFunction &>(e);
            for (auto &a : f.arguments) CollectRefs(*a, used);
            break;
        }
        case BoundExpressionType::UNARY_MINUS: {
            auto &u = static_cast<const BoundUnaryMinus &>(e);
            CollectRefs(*u.child, used);
            break;
        }
        case BoundExpressionType::CAST: {
            auto &c = static_cast<const BoundCast &>(e);
            CollectRefs(*c.child, used);
            break;
        }
        case BoundExpressionType::CONSTANT:
        case BoundExpressionType::STAR:
            // No column refs.
            break;
        default:
            // SUBQUERY or unknown - be conservative: mark all needed.
            for (idx_t i = 0; i < used.size(); i++) used[i] = true;
            break;
        }
    }

    // Input buffer: stores chunks as-is + a global row -> (chunk,pos) lookup.
    // This avoids the O(N*C) Value allocation that dominated the old path.
    struct InputBuf {
        std::vector<DataChunk> chunks;
        std::vector<uint32_t> chunk_of; // global row -> chunk idx
        std::vector<uint32_t> pos_of;   // global row -> row within chunk
        idx_t total = 0;
        std::vector<LogicalTypeId> types;

        idx_t size() const { return total; }

        Value Get(idx_t row, idx_t col) const {
            return chunks[chunk_of[row]].GetValue(col, pos_of[row]);
        }
        bool IsNull(idx_t row, idx_t col) const {
            auto &v = chunks[chunk_of[row]].GetVector(col);
            return !v.GetValidity().RowIsValid(pos_of[row]);
        }
        int32_t GetInt32(idx_t row, idx_t col) const {
            auto &v = chunks[chunk_of[row]].GetVector(col);
            return reinterpret_cast<const int32_t *>(v.GetData())[pos_of[row]];
        }
        int64_t GetInt64(idx_t row, idx_t col) const {
            auto &v = chunks[chunk_of[row]].GetVector(col);
            return reinterpret_cast<const int64_t *>(v.GetData())[pos_of[row]];
        }
        double GetDouble(idx_t row, idx_t col) const {
            auto &v = chunks[chunk_of[row]].GetVector(col);
            return reinterpret_cast<const double *>(v.GetData())[pos_of[row]];
        }
        const string_t &GetStr(idx_t row, idx_t col) const {
            auto &v = chunks[chunk_of[row]].GetVector(col);
            return reinterpret_cast<const string_t *>(v.GetData())[pos_of[row]];
        }
        // Read int64 from any integer column (INTEGER or BIGINT); 0 if other.
        int64_t ReadI64(idx_t row, idx_t col) const {
            auto tid = types[col];
            if (tid == LogicalTypeId::BIGINT) return GetInt64(row, col);
            if (tid == LogicalTypeId::INTEGER) return GetInt32(row, col);
            return 0;
        }
    };

    // Per-window metadata precomputed during Setup.
    enum WinFunc {
        WF_ROW_NUMBER, WF_RANK, WF_DENSE_RANK, WF_NTILE,
        WF_LAG, WF_LEAD, WF_FIRST_VALUE, WF_LAST_VALUE,
        WF_SUM, WF_COUNT, WF_AVG, WF_MIN, WF_MAX, WF_UNKNOWN
    };

    static WinFunc ResolveWinFunc(const std::string &name) {
        if (name == "ROW_NUMBER") return WF_ROW_NUMBER;
        if (name == "RANK") return WF_RANK;
        if (name == "DENSE_RANK") return WF_DENSE_RANK;
        if (name == "NTILE") return WF_NTILE;
        if (name == "LAG") return WF_LAG;
        if (name == "LEAD") return WF_LEAD;
        if (name == "FIRST_VALUE") return WF_FIRST_VALUE;
        if (name == "LAST_VALUE") return WF_LAST_VALUE;
        if (name == "SUM") return WF_SUM;
        if (name == "COUNT") return WF_COUNT;
        if (name == "AVG") return WF_AVG;
        if (name == "MIN") return WF_MIN;
        if (name == "MAX") return WF_MAX;
        return WF_UNKNOWN;
    }

    struct WinInfo {
        WinFunc fn = WF_UNKNOWN;
        idx_t arg_col = INVALID_INDEX;
        LogicalTypeId arg_tid = LogicalTypeId::SQLNULL;
        int64_t lag_offset = 1;
        int64_t ntile_buckets = 1;
        idx_t rank_order_col = INVALID_INDEX;
        LogicalTypeId rank_order_tid = LogicalTypeId::SQLNULL;
        // When the window has an ORDER BY, SUM/COUNT/AVG/MIN/MAX use the
        // SQL default running frame (RANGE UNBOUNDED PRECEDING..CURRENT
        // ROW) instead of the whole-partition aggregate.
        bool has_order = false;
        // Precomputed Value per partition (only for aggregate-shape windows
        // with NO order — the whole-partition aggregate).
        std::vector<Value> part_value;
    };

    struct VecEmitSlot {
        bool is_col = false;
        idx_t input_col = INVALID_INDEX;
        WinFunc fn = WF_UNKNOWN;
        idx_t rank_order_col = INVALID_INDEX;
        LogicalTypeId rank_order_tid = LogicalTypeId::SQLNULL;
    };
    std::vector<VecEmitSlot> vec_emit_cache_;

    // Reads entire child input into InputBuf with typed raw Vector storage.
    void ReadInput() {
        idx_t num_cols = children[0]->GetTypes().size();
        input_.types.clear();
        input_.types.reserve(num_cols);
        for (auto &t : children[0]->GetTypes()) input_.types.push_back(t.id());
        input_.chunks.clear();
        input_.chunks.reserve(16);
        input_.total = 0;
        while (true) {
            DataChunk ch;
            ch.Initialize(children[0]->GetTypes());
            if (!children[0]->GetData(ch)) break;
            idx_t cnt = ch.size();
            if (cnt == 0) continue;
            input_.total += cnt;
            input_.chunks.push_back(std::move(ch));
        }
        if (input_.total == 0) return;
        input_.chunk_of.resize(input_.total);
        input_.pos_of.resize(input_.total);
        idx_t gi = 0;
        for (idx_t ci = 0; ci < input_.chunks.size(); ci++) {
            idx_t sz = input_.chunks[ci].size();
            for (idx_t ri = 0; ri < sz; ri++) {
                input_.chunk_of[gi] = static_cast<uint32_t>(ci);
                input_.pos_of[gi] = static_cast<uint32_t>(ri);
                gi++;
            }
        }
    }

    // True if two window expressions share the same PARTITION BY and ORDER BY
    // (by column reference and direction).
    static bool SamePartitionAndOrder(const BoundWindowExpression &a,
                                       const BoundWindowExpression &b) {
        if (a.partition_by.size() != b.partition_by.size()) return false;
        if (a.order_by.size() != b.order_by.size()) return false;
        for (idx_t i = 0; i < a.partition_by.size(); i++) {
            auto *ea = a.partition_by[i].get();
            auto *eb = b.partition_by[i].get();
            if (ea->GetExpressionType() != BoundExpressionType::COLUMN_REF) return false;
            if (eb->GetExpressionType() != BoundExpressionType::COLUMN_REF) return false;
            if (static_cast<const BoundColumnRef *>(ea)->column_index !=
                static_cast<const BoundColumnRef *>(eb)->column_index) return false;
        }
        for (idx_t i = 0; i < a.order_by.size(); i++) {
            auto *ea = a.order_by[i].expression.get();
            auto *eb = b.order_by[i].expression.get();
            if (ea->GetExpressionType() != BoundExpressionType::COLUMN_REF) return false;
            if (eb->GetExpressionType() != BoundExpressionType::COLUMN_REF) return false;
            if (static_cast<const BoundColumnRef *>(ea)->column_index !=
                static_cast<const BoundColumnRef *>(eb)->column_index) return false;
            if (a.order_by[i].ascending != b.order_by[i].ascending) return false;
        }
        return true;
    }

    // Build partitions_ using the reference window's PARTITION BY.
    void BuildPartitions(BoundWindowExpression &win) {
        idx_t n = input_.total;
        partitions_.clear();
        if (win.partition_by.empty()) {
            partitions_.emplace_back();
            partitions_[0].reserve(n);
            for (idx_t i = 0; i < n; i++) partitions_[0].push_back(i);
            return;
        }
        bool single_col = (win.partition_by.size() == 1 &&
            win.partition_by[0]->GetExpressionType() == BoundExpressionType::COLUMN_REF);
        LogicalTypeId tid = LogicalTypeId::SQLNULL;
        idx_t part_col = INVALID_INDEX;
        if (single_col) {
            auto &ref = static_cast<BoundColumnRef &>(*win.partition_by[0]);
            part_col = ref.column_index;
            if (part_col < input_.types.size()) tid = input_.types[part_col];
        }
        if (single_col && (tid == LogicalTypeId::INTEGER || tid == LogicalTypeId::BIGINT)) {
            std::unordered_map<int64_t, idx_t> k2p;
            k2p.reserve(n / 4 + 16);
            // NULLs are one partition of their own; the raw value of a NULL
            // row put it in the partition of 0, or of whatever was there.
            idx_t null_pi = INVALID_INDEX;
            for (idx_t i = 0; i < n; i++) {
                if (input_.IsNull(i, part_col)) {
                    if (null_pi == INVALID_INDEX) { null_pi = partitions_.size(); partitions_.emplace_back(); }
                    partitions_[null_pi].push_back(i);
                    continue;
                }
                int64_t k = input_.ReadI64(i, part_col);
                auto it = k2p.find(k);
                idx_t pi;
                if (it == k2p.end()) { pi = partitions_.size(); partitions_.emplace_back(); k2p.emplace(k, pi); }
                else pi = it->second;
                partitions_[pi].push_back(i);
            }
        } else if (single_col && tid == LogicalTypeId::VARCHAR) {
            // Linear-cache fast path - most real VARCHAR PARTITION BY is
            // low-cardinality (~10s of categories). Keep a small inline array
            // and do memcmp against it - much faster than std::unordered_map's
            // string hashing + allocation per row.
            struct Entry { const char *data; uint32_t len; idx_t pi; };
            std::vector<Entry> cache;
            cache.reserve(64);
            std::unordered_map<std::string, idx_t> overflow; // used if cardinality > cache cap
            const idx_t cache_cap = 256;
            idx_t null_pi = INVALID_INDEX; // NULL is not '' (see above)
            for (idx_t i = 0; i < n; i++) {
                if (input_.IsNull(i, part_col)) {
                    if (null_pi == INVALID_INDEX) { null_pi = partitions_.size(); partitions_.emplace_back(); }
                    partitions_[null_pi].push_back(i);
                    continue;
                }
                auto &s = input_.GetStr(i, part_col);
                const char *d = s.GetData();
                uint32_t l = s.GetSize();
                idx_t pi = INVALID_INDEX;
                if (cache.size() < cache_cap) {
                    for (auto &e : cache) {
                        if (e.len == l && memcmp(e.data, d, l) == 0) { pi = e.pi; break; }
                    }
                    if (pi == INVALID_INDEX) {
                        pi = partitions_.size();
                        partitions_.emplace_back();
                        cache.push_back({d, l, pi});
                    }
                } else {
                    std::string key(d, l);
                    auto it = overflow.find(key);
                    if (it == overflow.end()) {
                        pi = partitions_.size();
                        partitions_.emplace_back();
                        overflow.emplace(std::move(key), pi);
                    } else pi = it->second;
                }
                partitions_[pi].push_back(i);
            }
        } else {
            // Generic path: PARTITION BY may contain expressions
            // (a%2, CASE.., LENGTH(s)), not just plain columns.
            // Previously only COLUMN_REF keys were read — every other
            // expression contributed nothing to the key, collapsing all
            // rows into a single partition (PARTITION BY silently
            // ignored). Now each partition expression is evaluated per
            // row via a reusable single-row chunk.
            auto in_types = children[0]->GetTypes();
            DataChunk single;
            single.Initialize(in_types);
            single.SetCardinality(1);
            bool need_eval = false;
            for (auto &p : win.partition_by) {
                if (p->GetExpressionType() != BoundExpressionType::COLUMN_REF) {
                    need_eval = true; break;
                }
            }
            std::unordered_map<std::string, idx_t> k2p;
            k2p.reserve(n / 4 + 16);
            for (idx_t i = 0; i < n; i++) {
                if (need_eval) {
                    for (idx_t c = 0; c < input_.types.size(); c++)
                        single.SetValue(c, 0, input_.Get(i, c));
                }
                std::string key;
                for (auto &p : win.partition_by) {
                    Value v;
                    if (p->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                        v = input_.Get(i, static_cast<BoundColumnRef &>(*p).column_index);
                    } else {
                        Vector res(p->GetReturnType());
                        ExpressionExecutor::Execute(*p, single, res, 1);
                        v = res.GetValue(0);
                    }
                    AppendKeyValue(key, v);
                }
                auto it = k2p.find(key);
                idx_t pi;
                if (it == k2p.end()) { pi = partitions_.size(); partitions_.emplace_back(); k2p.emplace(std::move(key), pi); }
                else pi = it->second;
                partitions_[pi].push_back(i);
            }
        }
    }

    // Extract sort-key data once at setup; actual sort happens lazily per
    // partition during emit (SortOnePartition). Critical for LIMIT queries
    // that emit from only a handful of partitions.
    void PrepareSortKeys(BoundWindowExpression &win) {
        sort_win_ = nullptr;
        sort_single_col_ok_ = false;
        if (win.order_by.empty()) return;
        sort_win_ = &win;
        bool single_ord = (win.order_by.size() == 1 &&
            win.order_by[0].expression->GetExpressionType() == BoundExpressionType::COLUMN_REF);
        if (!single_ord) return;
        auto &ref = static_cast<BoundColumnRef &>(*win.order_by[0].expression);
        sort_col_ = ref.column_index;
        sort_asc_ = win.order_by[0].ascending;
        sort_tid_ = (sort_col_ < input_.types.size()) ? input_.types[sort_col_] : LogicalTypeId::SQLNULL;
        idx_t n = input_.total;
        // A NULL key's bytes sorted as a value; the Value path below puts
        // NULLs last, as DuckDB does (#38).
        for (idx_t i = 0; i < n; i++) {
            if (input_.IsNull(i, sort_col_)) return;
        }
        if (sort_tid_ == LogicalTypeId::INTEGER) {
            sort_i32_.resize(n);
            for (idx_t i = 0; i < n; i++) sort_i32_[i] = input_.GetInt32(i, sort_col_);
            sort_single_col_ok_ = true;
        } else if (sort_tid_ == LogicalTypeId::BIGINT) {
            sort_i64_.resize(n);
            for (idx_t i = 0; i < n; i++) sort_i64_[i] = input_.GetInt64(i, sort_col_);
            sort_single_col_ok_ = true;
        } else if (sort_tid_ == LogicalTypeId::DOUBLE) {
            sort_d_.resize(n);
            for (idx_t i = 0; i < n; i++) sort_d_[i] = input_.GetDouble(i, sort_col_);
            sort_single_col_ok_ = true;
        } else if (sort_tid_ == LogicalTypeId::VARCHAR) {
            sort_s_.resize(n);
            for (idx_t i = 0; i < n; i++) sort_s_[i] = input_.GetStr(i, sort_col_);
            sort_single_col_ok_ = true;
        }
    }

    void SortOnePartition(idx_t p) {
        if (p >= partition_sorted_.size() || partition_sorted_[p]) return;
        auto &idxs = partitions_[p];
        if (!sort_win_) { partition_sorted_[p] = true; return; }
        // Use partial_sort when LIMIT is small and we're sorting the first (or only)
        // partition - takes only the top-K rows in order, O(N log K) vs O(N log N).
        // Safe for ROW_NUMBER/NTILE/LAG/LEAD since consumer LIMIT stops the emit.
        idx_t partial_k = 0;
        if (row_limit_ > 0 && row_limit_ < idxs.size()) {
            // Only apply to the first partition - later partitions might contribute rows
            // if earlier ones don't fill the limit; conservatively sort later ones fully.
            if (p == 0) partial_k = row_limit_;
        }
        auto do_sort = [&](auto cmp) {
            if (partial_k > 0) {
                std::partial_sort(idxs.begin(), idxs.begin() + partial_k, idxs.end(), cmp);
            } else {
                std::sort(idxs.begin(), idxs.end(), cmp);
            }
        };
        if (sort_single_col_ok_) {
            if (sort_tid_ == LogicalTypeId::INTEGER) {
                bool asc = sort_asc_;
                auto *keys = sort_i32_.data();
                do_sort([keys, asc](idx_t a, idx_t b) {
                    return asc ? SqlLess(keys[a], keys[b]) : SqlGreater(keys[a], keys[b]);
                });
            } else if (sort_tid_ == LogicalTypeId::BIGINT) {
                bool asc = sort_asc_;
                auto *keys = sort_i64_.data();
                do_sort([keys, asc](idx_t a, idx_t b) {
                    return asc ? SqlLess(keys[a], keys[b]) : SqlGreater(keys[a], keys[b]);
                });
            } else if (sort_tid_ == LogicalTypeId::DOUBLE) {
                bool asc = sort_asc_;
                auto *keys = sort_d_.data();
                do_sort([keys, asc](idx_t a, idx_t b) {
                    return asc ? SqlLess(keys[a], keys[b]) : SqlGreater(keys[a], keys[b]);
                });
            } else if (sort_tid_ == LogicalTypeId::VARCHAR) {
                bool asc = sort_asc_;
                auto *keys = sort_s_.data();
                do_sort([keys, asc](idx_t a, idx_t b) {
                    auto &sa = keys[a]; auto &sb = keys[b];
                    uint32_t la = sa.GetSize(), lb = sb.GetSize();
                    uint32_t m = la < lb ? la : lb;
                    int c = memcmp(sa.GetData(), sb.GetData(), m);
                    if (c == 0) c = (la < lb) ? -1 : (la > lb) ? 1 : 0;
                    return asc ? (c < 0) : (c > 0);
                });
            }
        } else {
            auto &win = *sort_win_;
            std::sort(idxs.begin(), idxs.end(), [&](idx_t a, idx_t b) {
                for (auto &ord : win.order_by) {
                    if (ord.expression->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                        auto &ref = static_cast<BoundColumnRef &>(*ord.expression);
                        auto va = input_.Get(a, ref.column_index);
                        auto vb = input_.Get(b, ref.column_index);
                        if (va.IsNull() && vb.IsNull()) continue;
                        if (va.IsNull() || vb.IsNull()) return vb.IsNull();
                        if (va < vb) return ord.ascending;
                        if (vb < va) return !ord.ascending;
                    }
                }
                return false;
            });
        }
        partition_sorted_[p] = true;
    }

    // Resolve a window expression into WinInfo and precompute per-partition
    // values for aggregate-shape windows (SUM/COUNT/AVG/MIN/MAX/FIRST_VALUE/LAST_VALUE).
    void CompileWindow(BoundWindowExpression &win, WinInfo &info) {
        info.fn = ResolveWinFunc(win.function_name);
        info.arg_col = INVALID_INDEX;
        info.arg_tid = LogicalTypeId::SQLNULL;
        if (!win.arguments.empty() &&
            win.arguments[0]->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
            info.arg_col = static_cast<BoundColumnRef &>(*win.arguments[0]).column_index;
            if (info.arg_col < input_.types.size()) info.arg_tid = input_.types[info.arg_col];
        }
        info.lag_offset = 1;
        if (win.arguments.size() > 1 &&
            win.arguments[1]->GetExpressionType() == BoundExpressionType::CONSTANT) {
            auto &c = static_cast<BoundConstant &>(*win.arguments[1]);
            auto ctid = c.value.type().id();
            if (ctid == LogicalTypeId::INTEGER) info.lag_offset = c.value.GetValue<int32_t>();
            else if (ctid == LogicalTypeId::BIGINT) info.lag_offset = c.value.GetValue<int64_t>();
        }
        info.ntile_buckets = 1;
        if (info.fn == WF_NTILE && !win.arguments.empty() &&
            win.arguments[0]->GetExpressionType() == BoundExpressionType::CONSTANT) {
            auto &c = static_cast<BoundConstant &>(*win.arguments[0]);
            auto ctid = c.value.type().id();
            if (ctid == LogicalTypeId::INTEGER) info.ntile_buckets = c.value.GetValue<int32_t>();
            else if (ctid == LogicalTypeId::BIGINT) info.ntile_buckets = c.value.GetValue<int64_t>();
            // DuckDB's error; a count below 1 was taken as 1 (Stage 9).
            if (info.ntile_buckets <= 0) {
                throw InvalidInputException("Argument for ntile must be greater than zero");
            }
        }
        info.rank_order_col = INVALID_INDEX;
        info.rank_order_tid = LogicalTypeId::SQLNULL;
        if (!win.order_by.empty() &&
            win.order_by[0].expression->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
            info.rank_order_col = static_cast<BoundColumnRef &>(*win.order_by[0].expression).column_index;
            if (info.rank_order_col < input_.types.size())
                info.rank_order_tid = input_.types[info.rank_order_col];
        }

        // ORDER BY present => running frame (computed per-row at emit
        // time in ComputeWindowAt). No ORDER BY => whole-partition
        // aggregate, precomputed once here.
        info.has_order = !win.order_by.empty();
        if ((info.fn == WF_SUM || info.fn == WF_COUNT || info.fn == WF_AVG ||
             info.fn == WF_MIN || info.fn == WF_MAX) && !info.has_order) {
            info.part_value.resize(partitions_.size());
            for (idx_t p = 0; p < partitions_.size(); p++) {
                info.part_value[p] = ComputePartitionAggregate(info, partitions_[p]);
            }
        }
    }

    // Aggregate over the first `limit` indices of `indices` (INVALID_INDEX
    // = all). The running-frame path passes the peer-group end so the
    // value is the RANGE UNBOUNDED PRECEDING..CURRENT ROW total.
    Value ComputePartitionAggregate(const WinInfo &info, const std::vector<idx_t> &indices,
                                    idx_t limit = INVALID_INDEX) {
        idx_t ps = indices.size();
        if (limit != INVALID_INDEX && limit < ps) ps = limit;
        if (ps == 0) return Value();
        if (info.fn == WF_FIRST_VALUE) {
            if (info.arg_col == INVALID_INDEX) return Value();
            return input_.Get(indices[0], info.arg_col);
        }
        if (info.fn == WF_LAST_VALUE) {
            if (info.arg_col == INVALID_INDEX) return Value();
            return input_.Get(indices[ps - 1], info.arg_col);
        }
        double sum = 0;
        int64_t count = 0;
        double min_d = 0, max_d = 0;
        bool has_mm = false;
        if (info.arg_col != INVALID_INDEX &&
            (info.arg_tid == LogicalTypeId::INTEGER || info.arg_tid == LogicalTypeId::BIGINT ||
             info.arg_tid == LogicalTypeId::SMALLINT || info.arg_tid == LogicalTypeId::TINYINT)) {
            // Integers in 64 bits, exactly: through a double, a sum or a
            // bound past 2^53 lost its low digits, and a sum of NULLs was 0
            // (Stage 8, #12). A sum past BIGINT is DuckDB's HUGEINT, which
            // the engine does not have: refused.
            int64_t isum = 0, imin = 0, imax = 0;
            for (idx_t k = 0; k < ps; k++) {
                idx_t idx = indices[k];
                if (input_.IsNull(idx, info.arg_col)) continue;
                int64_t d;
                if (info.arg_tid == LogicalTypeId::INTEGER) d = input_.GetInt32(idx, info.arg_col);
                else if (info.arg_tid == LogicalTypeId::BIGINT) d = input_.GetInt64(idx, info.arg_col);
                else {
                    auto v = input_.Get(idx, info.arg_col);
                    d = info.arg_tid == LogicalTypeId::SMALLINT ? v.GetValue<int16_t>() : v.GetValue<int8_t>();
                }
                if ((d > 0 && isum > std::numeric_limits<int64_t>::max() - d) ||
                    (d < 0 && isum < std::numeric_limits<int64_t>::min() - d)) {
                    if (info.fn == WF_SUM || info.fn == WF_AVG)
                        throw OutOfRangeException("Window " + std::string(info.fn == WF_SUM ? "SUM" : "AVG") +
                                                  " is out of the range of BIGINT");
                }
                isum += d;
                if (count == 0 || d < imin) imin = d;
                if (count == 0 || d > imax) imax = d;
                count++;
            }
            if (info.fn == WF_COUNT) return Value::BIGINT(count);
            if (count == 0) return Value();
            if (info.fn == WF_SUM) return Value::BIGINT(isum);
            if (info.fn == WF_AVG) return Value::DOUBLE(static_cast<double>(isum) / static_cast<double>(count));
            int64_t m = info.fn == WF_MIN ? imin : imax;
            if (info.fn == WF_MIN || info.fn == WF_MAX) {
                switch (info.arg_tid) {
                case LogicalTypeId::TINYINT: return Value::TINYINT(static_cast<int8_t>(m));
                case LogicalTypeId::SMALLINT: return Value::SMALLINT(static_cast<int16_t>(m));
                case LogicalTypeId::INTEGER: return Value::INTEGER(static_cast<int32_t>(m));
                default: return Value::BIGINT(m);
                }
            }
            return Value();
        }
        if (info.arg_col == INVALID_INDEX) {
            count = static_cast<int64_t>(ps);
        } else if (info.arg_tid == LogicalTypeId::DOUBLE) {
            for (idx_t k = 0; k < ps; k++) { idx_t idx = indices[k]; if (!input_.IsNull(idx, info.arg_col)) {
                double d = input_.GetDouble(idx, info.arg_col);
                count++; sum += d;
                // NaN is the largest, and the last of equal values wins,
                // as DuckDB's MIN and MAX (#42).
                if (!has_mm) { min_d = max_d = d; has_mm = true; }
                else { if (MinReplaces(min_d, d)) min_d = d; if (MaxReplaces(max_d, d)) max_d = d; }
            } }
        } else {
            Value min_v, max_v;
            for (idx_t k = 0; k < ps; k++) {
                idx_t idx = indices[k];
                auto v = input_.Get(idx, info.arg_col);
                if (!v.IsNull()) {
                    count++;
                    auto tid = v.type().id();
                    double d = 0;
                    if (tid == LogicalTypeId::INTEGER) d = v.GetValue<int32_t>();
                    else if (tid == LogicalTypeId::BIGINT) d = static_cast<double>(v.GetValue<int64_t>());
                    else if (tid == LogicalTypeId::DOUBLE) d = v.GetValue<double>();
                    else if (tid == LogicalTypeId::FLOAT) d = v.GetValue<float>();
                    sum += d;
                    // The last of equal values wins, as DuckDB's (#42).
                    if (!has_mm || !(min_v < v)) min_v = v;
                    if (!has_mm || !(v < max_v)) max_v = v;
                    has_mm = true;
                }
            }
            if (info.fn == WF_COUNT) return Value::BIGINT(count);
            if (info.fn == WF_SUM) {
                if (count == 0) return Value();
                if (info.arg_tid == LogicalTypeId::DOUBLE || info.arg_tid == LogicalTypeId::FLOAT)
                    return Value::DOUBLE(sum);
                return Value::BIGINT(static_cast<int64_t>(sum));
            }
            if (info.fn == WF_AVG) return count > 0 ? Value::DOUBLE(sum / count) : Value();
            if (info.fn == WF_MIN) return has_mm ? min_v : Value();
            if (info.fn == WF_MAX) return has_mm ? max_v : Value();
            return Value();
        }
        if (info.fn == WF_COUNT) return Value::BIGINT(count);
        if (info.fn == WF_SUM) {
            if (count == 0) return Value();
            // Return type matches argument's numeric type (DOUBLE for DOUBLE, BIGINT otherwise).
            if (info.arg_tid == LogicalTypeId::DOUBLE) return Value::DOUBLE(sum);
            return Value::BIGINT(static_cast<int64_t>(sum));
        }
        if (info.fn == WF_AVG) return count > 0 ? Value::DOUBLE(sum / count) : Value();
        if (info.fn == WF_MIN) {
            if (!has_mm) return Value();
            if (info.arg_tid == LogicalTypeId::INTEGER) return Value::INTEGER(static_cast<int32_t>(min_d));
            if (info.arg_tid == LogicalTypeId::BIGINT) return Value::BIGINT(static_cast<int64_t>(min_d));
            return Value::DOUBLE(min_d);
        }
        if (info.fn == WF_MAX) {
            if (!has_mm) return Value();
            if (info.arg_tid == LogicalTypeId::INTEGER) return Value::INTEGER(static_cast<int32_t>(max_d));
            if (info.arg_tid == LogicalTypeId::BIGINT) return Value::BIGINT(static_cast<int64_t>(max_d));
            return Value::DOUBLE(max_d);
        }
        return Value();
    }

    // Compute the window value for one row during streaming emit.
    Value ComputeWindowAt(const WinInfo &info, idx_t part_idx, idx_t pos) {
        auto &indices = partitions_[part_idx];
        idx_t ps = indices.size();
        switch (info.fn) {
        case WF_ROW_NUMBER:
            return Value::BIGINT(static_cast<int64_t>(pos + 1));
        case WF_RANK: {
            if (pos == 0) { rank_cur_ = 1; rank_prev_ = indices[0]; return Value::BIGINT(1); }
            bool same = OrderEqual(info, rank_prev_, indices[pos]);
            if (!same) rank_cur_ = static_cast<int64_t>(pos + 1);
            rank_prev_ = indices[pos];
            return Value::BIGINT(rank_cur_);
        }
        case WF_DENSE_RANK: {
            if (pos == 0) { dense_cur_ = 1; dense_prev_ = indices[0]; return Value::BIGINT(1); }
            bool same = OrderEqual(info, dense_prev_, indices[pos]);
            if (!same) dense_cur_++;
            dense_prev_ = indices[pos];
            return Value::BIGINT(dense_cur_);
        }
        case WF_NTILE:
            return Value::BIGINT(NtileBucket(pos, ps, info.ntile_buckets));
        case WF_LAG: {
            int64_t target = static_cast<int64_t>(pos) - info.lag_offset;
            if (target >= 0 && target < static_cast<int64_t>(ps) && info.arg_col != INVALID_INDEX)
                return input_.Get(indices[static_cast<idx_t>(target)], info.arg_col);
            return Value();
        }
        case WF_LEAD: {
            int64_t target = static_cast<int64_t>(pos) + info.lag_offset;
            if (target >= 0 && target < static_cast<int64_t>(ps) && info.arg_col != INVALID_INDEX)
                return input_.Get(indices[static_cast<idx_t>(target)], info.arg_col);
            return Value();
        }
        case WF_FIRST_VALUE:
            if (info.arg_col == INVALID_INDEX || ps == 0) return Value();
            return input_.Get(indices[0], info.arg_col);
        case WF_LAST_VALUE:
            if (info.arg_col == INVALID_INDEX || ps == 0) return Value();
            return input_.Get(indices[ps - 1], info.arg_col);
        case WF_SUM:
        case WF_COUNT:
        case WF_AVG:
        case WF_MIN:
        case WF_MAX:
            if (info.has_order) {
                // Running frame: aggregate rows [0 .. peer-end of pos].
                // The partition is sorted; peers share the same order key
                // (RANGE semantics). Without a usable single-column order
                // key (rank_order_col == INVALID), fall back to ROWS
                // (current row only) which coincides with RANGE when
                // order keys are distinct.
                idx_t peer_end = pos;
                if (info.rank_order_col != INVALID_INDEX) {
                    while (peer_end + 1 < ps &&
                           OrderEqual(info, indices[pos], indices[peer_end + 1])) {
                        peer_end++;
                    }
                }
                return ComputePartitionAggregate(info, indices, peer_end + 1);
            }
            return (part_idx < info.part_value.size()) ? info.part_value[part_idx] : Value();
        default:
            return Value();
        }
    }

    bool OrderEqual(const WinInfo &info, idx_t a, idx_t b) {
        // Without an ORDER BY every row of a partition is a peer.
        if (info.rank_order_col == INVALID_INDEX) return !info.has_order;
        bool na = input_.IsNull(a, info.rank_order_col), nb = input_.IsNull(b, info.rank_order_col);
        if (na || nb) return na && nb;
        switch (info.rank_order_tid) {
        case LogicalTypeId::INTEGER:
            return input_.GetInt32(a, info.rank_order_col) == input_.GetInt32(b, info.rank_order_col);
        case LogicalTypeId::BIGINT:
            return input_.GetInt64(a, info.rank_order_col) == input_.GetInt64(b, info.rank_order_col);
        case LogicalTypeId::DOUBLE:
            return SqlEq(input_.GetDouble(a, info.rank_order_col), input_.GetDouble(b, info.rank_order_col));
        case LogicalTypeId::VARCHAR: {
            auto &sa = input_.GetStr(a, info.rank_order_col);
            auto &sb = input_.GetStr(b, info.rank_order_col);
            if (sa.GetSize() != sb.GetSize()) return false;
            return memcmp(sa.GetData(), sb.GetData(), sa.GetSize()) == 0;
        }
        default: {
            auto va = input_.Get(a, info.rank_order_col);
            auto vb = input_.Get(b, info.rank_order_col);
            return va == vb;
        }
        }
    }

    bool EvalQualify(idx_t part_idx, idx_t pos) {
        Value wv = ComputeWindowAt(qualify_win_info_, part_idx, pos);
        if (wv.IsNull() || !qualify_const_) return false;
        auto &cv = qualify_const_->value;
        auto ctid = cv.type().id();
        bool fast_int = (ctid == LogicalTypeId::INTEGER || ctid == LogicalTypeId::BIGINT);
        if (fast_int && wv.type().id() == LogicalTypeId::BIGINT) {
            int64_t cl = (ctid == LogicalTypeId::INTEGER) ? cv.GetValue<int32_t>() : cv.GetValue<int64_t>();
            int64_t wl = wv.GetValue<int64_t>();
            int64_t lhs = qualify_win_on_left_ ? wl : cl;
            int64_t rhs = qualify_win_on_left_ ? cl : wl;
            switch (qualify_op_) {
            case 0: return lhs == rhs;
            case 1: return lhs < rhs;
            case 2: return lhs <= rhs;
            case 3: return lhs > rhs;
            case 4: return lhs >= rhs;
            case 5: return lhs != rhs;
            default: return false;
            }
        }
        const Value &lhs = qualify_win_on_left_ ? wv : cv;
        const Value &rhs = qualify_win_on_left_ ? cv : wv;
        switch (qualify_op_) {
        case 0: return !(lhs < rhs) && !(rhs < lhs);
        case 1: return lhs < rhs;
        case 2: return lhs <= rhs;
        case 3: return lhs > rhs;
        case 4: return lhs >= rhs;
        case 5: return lhs != rhs;
        default: return false;
        }
    }

    void Setup() {
#if 0 // ZUGODB_DBG_WINDOW
        auto _dbg_t0 = std::chrono::high_resolution_clock::now();
        auto _dbg_step = [&](const char *what) {
            auto t = std::chrono::high_resolution_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::microseconds>(t - _dbg_t0).count() / 1000.0;
            fprintf(stderr, "[win] %-26s %.1f ms\n", what, ms);
            _dbg_t0 = t;
        };
#define DBG(s) _dbg_step(s)
#else
#define DBG(s) ((void)0)
#endif
        // Column pruning: tell the child which columns SELECT and QUALIFY
        // reference.
        idx_t num_cols = children[0]->GetTypes().size();
        std::vector<bool> needed(num_cols, false);
        for (auto &s : select_list_) CollectRefs(*s, needed);
        if (qualify_) CollectRefs(*qualify_, needed);
        bool any = false;
        for (bool b : needed) if (b) { any = true; break; }
        if (any) {
            children[0]->SetNeededOutputs(needed);
        }
        ReadInput();
        DBG("ReadInput");
        if (input_.total == 0) return;

        // Collect all window expressions (from SELECT and QUALIFY).
        std::vector<BoundWindowExpression *> wins;
        for (auto &s : select_list_) {
            if (s->GetExpressionType() == BoundExpressionType::WINDOW)
                wins.push_back(static_cast<BoundWindowExpression *>(s.get()));
        }
        BoundWindowExpression *qwin = nullptr;
        if (qualify_ && qualify_->GetExpressionType() == BoundExpressionType::COMPARISON) {
            auto &cmp = static_cast<BoundComparison &>(*qualify_);
            if (cmp.left->GetExpressionType() == BoundExpressionType::WINDOW) {
                qwin = static_cast<BoundWindowExpression *>(cmp.left.get());
                qualify_win_on_left_ = true;
                if (cmp.right->GetExpressionType() == BoundExpressionType::CONSTANT)
                    qualify_const_ = static_cast<BoundConstant *>(cmp.right.get());
            } else if (cmp.right->GetExpressionType() == BoundExpressionType::WINDOW) {
                qwin = static_cast<BoundWindowExpression *>(cmp.right.get());
                qualify_win_on_left_ = false;
                if (cmp.left->GetExpressionType() == BoundExpressionType::CONSTANT)
                    qualify_const_ = static_cast<BoundConstant *>(cmp.left.get());
            }
            if (qwin) {
                wins.push_back(qwin);
                const std::string &op = cmp.op;
                if (op == "=") qualify_op_ = 0;
                else if (op == "<") qualify_op_ = 1;
                else if (op == "<=") qualify_op_ = 2;
                else if (op == ">") qualify_op_ = 3;
                else if (op == ">=") qualify_op_ = 4;
                else if (op == "!=" || op == "<>") qualify_op_ = 5;
                else qualify_op_ = -1;
                qualify_has_win_ = true;
            }
        }

        if (wins.empty()) {
            // No windows at all - trivial case. Single partition, no sort.
            partitions_.emplace_back();
            partitions_[0].reserve(input_.total);
            for (idx_t i = 0; i < input_.total; i++) partitions_[0].push_back(i);
            select_win_info_.resize(select_list_.size());
            return;
        }

        // All windows must share PARTITION BY + ORDER BY. If not, fall back
        // to the generic per-column compute (builds full output once).
        auto *ref_win = wins.front();
        for (idx_t i = 1; i < wins.size(); i++) {
            if (!SamePartitionAndOrder(*ref_win, *wins[i])) {
                use_fallback_ = true;
                break;
            }
        }

        if (use_fallback_) {
            BuildFallbackRows();
            return;
        }

        BuildPartitions(*ref_win); DBG("BuildPartitions");
        partition_sorted_.assign(partitions_.size(), false);
        PrepareSortKeys(*ref_win); DBG("PrepareSortKeys");

        select_win_info_.resize(select_list_.size());
        for (idx_t col = 0; col < select_list_.size(); col++) {
            if (select_list_[col]->GetExpressionType() == BoundExpressionType::WINDOW) {
                auto &w = static_cast<BoundWindowExpression &>(*select_list_[col]);
                CompileWindow(w, select_win_info_[col]);
            }
        }
        if (qwin) CompileWindow(*qwin, qualify_win_info_);

        // Fast path: QUALIFY ROW_NUMBER() OVER (... ORDER BY X) = 1.
        // Just reduce each partition to its argmin/argmax row - no sort.
        TryTop1Qualify(ref_win, qwin);

        // Eager parallel per-partition sort.
        // Lazy sort serialised the cost of N partitions × M rows into the
        // single-threaded emit loop. With multiple partitions (PARTITION BY
        // region — typically 3-50) and a meaningful row count, sorting
        // them concurrently before emit cuts the total wall time
        // proportionally to min(num_partitions, threads).
        if (input_.total >= 200000 && partitions_.size() >= 2 &&
            !use_fallback_) {
            unsigned int nthreads = HWThreads();
            if (nthreads > 8) nthreads = 8;
            if (nthreads > partitions_.size()) nthreads = static_cast<unsigned int>(partitions_.size());
            if (nthreads > 1) {
                std::atomic<idx_t> next{0};
                zugodb::WorkerGroup ts;
                ts.reserve(nthreads);
                for (unsigned int t = 0; t < nthreads; t++) {
                    ts.Spawn([&] {
                        while (true) {
                            if (ts.Failed()) return;
                            idx_t p = next.fetch_add(1, std::memory_order_relaxed);
                            if (p >= partitions_.size()) return;
                            SortOnePartition(p);
                        }
                    });
                }
                ts.Join();
            } else {
                for (idx_t p = 0; p < partitions_.size(); p++) SortOnePartition(p);
            }
        }
        DBG("ParallelSort");
#undef DBG
    }

    void TryTop1Qualify(BoundWindowExpression *ref_win, BoundWindowExpression *qwin) {
        if (!qwin) return;
        if (qualify_win_info_.fn != WF_ROW_NUMBER) return;
        if (qualify_op_ != 0) return;           // must be =
        if (!qualify_const_) return;
        if (!qualify_win_on_left_) return;      // only rn = K form
        auto ctid = qualify_const_->value.type().id();
        int64_t k;
        if (ctid == LogicalTypeId::INTEGER) k = qualify_const_->value.GetValue<int32_t>();
        else if (ctid == LogicalTypeId::BIGINT) k = qualify_const_->value.GetValue<int64_t>();
        else return;
        if (k != 1) return;
        if (ref_win->order_by.empty()) return;
        if (ref_win->order_by[0].expression->GetExpressionType() != BoundExpressionType::COLUMN_REF) return;

        auto &ord_ref = static_cast<BoundColumnRef &>(*ref_win->order_by[0].expression);
        idx_t col = ord_ref.column_index;
        LogicalTypeId tid = (col < input_.types.size()) ? input_.types[col] : LogicalTypeId::SQLNULL;
        bool asc = ref_win->order_by[0].ascending;

        // Reduce each partition to a single winner row - the argmin (asc) or argmax (desc).
        // A NULL key sorts last, so it is the winner only of a partition
        // of NULLs; this path read its bytes as a value (#38).
        for (auto &idxs : partitions_) {
            for (auto x : idxs) {
                if (input_.IsNull(x, col)) return;
            }
        }
        auto pick_one = [&](std::vector<idx_t> &idxs) {
            if (idxs.empty()) return;
            idx_t best = idxs[0];
            if (tid == LogicalTypeId::INTEGER) {
                int32_t bk = input_.GetInt32(best, col);
                for (idx_t j = 1; j < idxs.size(); j++) {
                    int32_t v = input_.GetInt32(idxs[j], col);
                    if ((asc && SqlLess(v, bk)) || (!asc && SqlGreater(v, bk))) { bk = v; best = idxs[j]; }
                }
            } else if (tid == LogicalTypeId::BIGINT) {
                int64_t bk = input_.GetInt64(best, col);
                for (idx_t j = 1; j < idxs.size(); j++) {
                    int64_t v = input_.GetInt64(idxs[j], col);
                    if ((asc && SqlLess(v, bk)) || (!asc && SqlGreater(v, bk))) { bk = v; best = idxs[j]; }
                }
            } else if (tid == LogicalTypeId::DOUBLE) {
                double bk = input_.GetDouble(best, col);
                for (idx_t j = 1; j < idxs.size(); j++) {
                    double v = input_.GetDouble(idxs[j], col);
                    if ((asc && SqlLess(v, bk)) || (!asc && SqlGreater(v, bk))) { bk = v; best = idxs[j]; }
                }
            } else if (tid == LogicalTypeId::VARCHAR) {
                auto bs = input_.GetStr(best, col);
                for (idx_t j = 1; j < idxs.size(); j++) {
                    auto v = input_.GetStr(idxs[j], col);
                    uint32_t la = v.GetSize(), lb = bs.GetSize();
                    uint32_t m = la < lb ? la : lb;
                    int c = memcmp(v.GetData(), bs.GetData(), m);
                    if (c == 0) c = (la < lb) ? -1 : (la > lb) ? 1 : 0;
                    if ((asc && c < 0) || (!asc && c > 0)) { bs = v; best = idxs[j]; }
                }
            } else {
                return;
            }
            idxs.clear();
            idxs.push_back(best);
        };
        for (auto &idxs : partitions_) pick_one(idxs);
        // Each partition now has size 1 - subsequent lazy sort is a no-op,
        // and the emit loop only produces one row per partition. QUALIFY ROW_NUMBER=1
        // evaluates true at pos 0, so all rows pass.
        for (idx_t p = 0; p < partition_sorted_.size(); p++) partition_sorted_[p] = true;
    }

    // Fallback path: multi-window query with different partition/order.
    // Build fallback_rows_ using per-column compute (old logic).
    void BuildFallbackRows() {
        idx_t n = input_.total;
        idx_t num_output_cols = select_list_.size();
        std::vector<std::vector<Value>> output(n);
        for (auto &row : output) row.resize(num_output_cols);
        for (idx_t col = 0; col < num_output_cols; col++) {
            auto &expr = select_list_[col];
            auto et = expr->GetExpressionType();
            if (et == BoundExpressionType::WINDOW) {
                auto &win = static_cast<BoundWindowExpression &>(*expr);
                ComputeWindowColumn(win, input_, output, col);
            } else if (et == BoundExpressionType::COLUMN_REF) {
                auto &ref = static_cast<BoundColumnRef &>(*expr);
                for (idx_t i = 0; i < n; i++) output[i][col] = input_.Get(i, ref.column_index);
            } else if (et == BoundExpressionType::CONSTANT) {
                auto &c = static_cast<BoundConstant &>(*expr);
                for (idx_t i = 0; i < n; i++) output[i][col] = c.value;
            } else {
                auto in_types = children[0]->GetTypes();
                for (idx_t i = 0; i < n; i++) {
                    DataChunk single;
                    single.Initialize(in_types);
                    for (idx_t c = 0; c < input_.types.size(); c++)
                        single.SetValue(c, 0, input_.Get(i, c));
                    Vector res(expr->GetReturnType());
                    ExpressionExecutor::Execute(*expr, single, res, 1);
                    output[i][col] = res.GetValue(0);
                }
            }
        }
        if (qualify_ && qualify_->GetExpressionType() == BoundExpressionType::COMPARISON) {
            auto &cmp = static_cast<BoundComparison &>(*qualify_);
            BoundWindowExpression *qwin = nullptr;
            BoundConstant *qcon = nullptr;
            bool win_left = true;
            if (cmp.left->GetExpressionType() == BoundExpressionType::WINDOW) {
                qwin = static_cast<BoundWindowExpression *>(cmp.left.get());
                if (cmp.right->GetExpressionType() == BoundExpressionType::CONSTANT)
                    qcon = static_cast<BoundConstant *>(cmp.right.get());
            } else if (cmp.right->GetExpressionType() == BoundExpressionType::WINDOW) {
                qwin = static_cast<BoundWindowExpression *>(cmp.right.get());
                win_left = false;
                if (cmp.left->GetExpressionType() == BoundExpressionType::CONSTANT)
                    qcon = static_cast<BoundConstant *>(cmp.left.get());
            }
            if (qwin) {
                idx_t qual_col = num_output_cols;
                for (auto &row : output) row.push_back(Value());
                ComputeWindowColumn(*qwin, input_, output, qual_col);
                const std::string &op = cmp.op;
                for (idx_t i = 0; i < n; i++) {
                    auto &wv = output[i][qual_col];
                    if (wv.IsNull() || !qcon) continue;
                    auto &cv = qcon->value;
                    const Value &lhs = win_left ? wv : cv;
                    const Value &rhs = win_left ? cv : wv;
                    bool pass = false;
                    if (op == "=") pass = !(lhs < rhs) && !(rhs < lhs);
                    else if (op == "<") pass = lhs < rhs;
                    else if (op == "<=") pass = lhs <= rhs;
                    else if (op == ">") pass = lhs > rhs;
                    else if (op == ">=") pass = lhs >= rhs;
                    else if (op == "!=" || op == "<>") pass = lhs != rhs;
                    if (pass) {
                        auto row = std::move(output[i]);
                        row.resize(num_output_cols);
                        fallback_rows_.push_back(std::move(row));
                    }
                }
                return;
            }
        }
        fallback_rows_ = std::move(output);
    }

    void ComputeWindowColumn(BoundWindowExpression &win,
                              const InputBuf &input,
                              std::vector<std::vector<Value>> &output,
                              idx_t out_col) {
        idx_t n = input.size();
        if (n == 0) return;
        WinFunc fn = ResolveWinFunc(win.function_name);

        // Pre-resolve arg / ORDER BY column indices once.
        idx_t arg_col = INVALID_INDEX;
        if (!win.arguments.empty() &&
            win.arguments[0]->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
            arg_col = static_cast<BoundColumnRef &>(*win.arguments[0]).column_index;
        }
        int64_t lag_offset = 1;
        if (win.arguments.size() > 1 &&
            win.arguments[1]->GetExpressionType() == BoundExpressionType::CONSTANT) {
            auto &c = static_cast<BoundConstant &>(*win.arguments[1]);
            auto ctid = c.value.type().id();
            if (ctid == LogicalTypeId::INTEGER) lag_offset = c.value.GetValue<int32_t>();
            else if (ctid == LogicalTypeId::BIGINT) lag_offset = c.value.GetValue<int64_t>();
        }
        int64_t ntile_buckets = 1;
        if (fn == WF_NTILE && !win.arguments.empty() &&
            win.arguments[0]->GetExpressionType() == BoundExpressionType::CONSTANT) {
            auto &c = static_cast<BoundConstant &>(*win.arguments[0]);
            auto ctid = c.value.type().id();
            if (ctid == LogicalTypeId::INTEGER) ntile_buckets = c.value.GetValue<int32_t>();
            else if (ctid == LogicalTypeId::BIGINT) ntile_buckets = c.value.GetValue<int64_t>();
            // DuckDB's error; a count below 1 was taken as 1 (Stage 9).
            if (ntile_buckets <= 0) {
                throw InvalidInputException("Argument for ntile must be greater than zero");
            }
        }
        idx_t rank_order_col = INVALID_INDEX;
        LogicalTypeId rank_order_tid = LogicalTypeId::SQLNULL;
        if (!win.order_by.empty() &&
            win.order_by[0].expression->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
            rank_order_col = static_cast<BoundColumnRef &>(*win.order_by[0].expression).column_index;
            if (rank_order_col < input.types.size()) rank_order_tid = input.types[rank_order_col];
        }

        // Build partition groups. Typed fast paths for single-column int/varchar PARTITION BY.
        std::vector<std::vector<idx_t>> partitions_list;

        LogicalTypeId part_tid = LogicalTypeId::SQLNULL;
        idx_t part_col = INVALID_INDEX;
        bool single_col_partition = (win.partition_by.size() == 1 &&
            win.partition_by[0]->GetExpressionType() == BoundExpressionType::COLUMN_REF);
        if (single_col_partition) {
            auto &ref = static_cast<BoundColumnRef &>(*win.partition_by[0]);
            part_col = ref.column_index;
            if (part_col < input.types.size()) part_tid = input.types[part_col];
        }

        if (win.partition_by.empty()) {
            partitions_list.emplace_back();
            partitions_list[0].reserve(n);
            for (idx_t i = 0; i < n; i++) partitions_list[0].push_back(i);
        } else if (single_col_partition &&
                   (part_tid == LogicalTypeId::INTEGER || part_tid == LogicalTypeId::BIGINT)) {
            // Raw int64 key - no Value alloc, no hash on strings.
            std::unordered_map<int64_t, idx_t> key_to_part;
            key_to_part.reserve(n / 4 + 16);
            idx_t null_part = INVALID_INDEX; // NULLs, a partition of their own
            for (idx_t i = 0; i < n; i++) {
                if (input.IsNull(i, part_col)) {
                    if (null_part == INVALID_INDEX) { null_part = partitions_list.size(); partitions_list.emplace_back(); }
                    partitions_list[null_part].push_back(i);
                    continue;
                }
                int64_t k = input.ReadI64(i, part_col);
                auto it = key_to_part.find(k);
                idx_t p_idx;
                if (it == key_to_part.end()) {
                    p_idx = partitions_list.size();
                    partitions_list.emplace_back();
                    key_to_part.emplace(k, p_idx);
                } else {
                    p_idx = it->second;
                }
                partitions_list[p_idx].push_back(i);
            }
        } else if (single_col_partition && part_tid == LogicalTypeId::VARCHAR) {
            // Raw string_t read - no Value/ToString alloc.
            std::unordered_map<std::string, idx_t> key_to_part;
            key_to_part.reserve(n / 4 + 16);
            idx_t null_part = INVALID_INDEX; // NULL is not ''
            for (idx_t i = 0; i < n; i++) {
                if (input.IsNull(i, part_col)) {
                    if (null_part == INVALID_INDEX) { null_part = partitions_list.size(); partitions_list.emplace_back(); }
                    partitions_list[null_part].push_back(i);
                    continue;
                }
                auto &s = input.GetStr(i, part_col);
                auto it = key_to_part.find(std::string(s.GetData(), s.GetSize()));
                idx_t p_idx;
                if (it == key_to_part.end()) {
                    p_idx = partitions_list.size();
                    partitions_list.emplace_back();
                    key_to_part.emplace(std::string(s.GetData(), s.GetSize()), p_idx);
                } else {
                    p_idx = it->second;
                }
                partitions_list[p_idx].push_back(i);
            }
        } else {
            // Generic: Value::ToString() keyed map (slow but handles multi-col / exotic types).
            std::unordered_map<std::string, idx_t> key_to_part;
            key_to_part.reserve(n / 4 + 16);
            // A PARTITION BY expression other than a column is evaluated
            // per row; it added nothing to the key, so the rows were one
            // partition (Stage 10).
            bool need_eval = false;
            for (auto &p : win.partition_by) {
                if (p->GetExpressionType() != BoundExpressionType::COLUMN_REF) need_eval = true;
            }
            DataChunk single;
            if (need_eval) {
                single.Initialize(children[0]->GetTypes());
                single.SetCardinality(1);
            }
            for (idx_t i = 0; i < n; i++) {
                std::string key;
                if (need_eval) {
                    for (idx_t c = 0; c < input.types.size(); c++) single.SetValue(c, 0, input.Get(i, c));
                }
                for (auto &p : win.partition_by) {
                    if (p->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                        auto &ref = static_cast<BoundColumnRef &>(*p);
                        AppendKeyValue(key, input.Get(i, ref.column_index));
                    } else {
                        Vector res(p->GetReturnType(), 1);
                        ExpressionExecutor::Execute(*p, single, res, 1);
                        AppendKeyValue(key, res.GetValue(0));
                    }
                }
                auto it = key_to_part.find(key);
                idx_t p_idx;
                if (it == key_to_part.end()) {
                    p_idx = partitions_list.size();
                    partitions_list.emplace_back();
                    key_to_part.emplace(std::move(key), p_idx);
                } else {
                    p_idx = it->second;
                }
                partitions_list[p_idx].push_back(i);
            }
        }

        // Typed sort comparator for single-column ORDER BY (common case).
        // Falls back to Value comparisons for multi-col / exotic types.
        auto sort_partitions = [&]() {
            if (win.order_by.empty()) return;
            bool single_ord = (win.order_by.size() == 1 &&
                win.order_by[0].expression->GetExpressionType() == BoundExpressionType::COLUMN_REF);
            if (single_ord) {
                auto &ref = static_cast<BoundColumnRef &>(*win.order_by[0].expression);
                idx_t col = ref.column_index;
                bool asc = win.order_by[0].ascending;
                auto tid = (col < input.types.size()) ? input.types[col] : LogicalTypeId::SQLNULL;
                if (tid == LogicalTypeId::INTEGER) {
                    for (auto &idxs : partitions_list) {
                        std::sort(idxs.begin(), idxs.end(), [&](idx_t a, idx_t b) {
                            // NULLs last, after ASC and DESC alike, as
                            // DuckDB's default; their bytes were compared
                            // as values (#38).
                            bool na = input.IsNull(a, col), nb = input.IsNull(b, col);
                            if (na || nb) return !na && nb;
                            auto va = input.GetInt32(a, col);
                            auto vb = input.GetInt32(b, col);
                            return asc ? SqlLess(va, vb) : SqlGreater(va, vb);
                        });
                    }
                    return;
                }
                if (tid == LogicalTypeId::BIGINT) {
                    for (auto &idxs : partitions_list) {
                        std::sort(idxs.begin(), idxs.end(), [&](idx_t a, idx_t b) {
                            // NULLs last, after ASC and DESC alike, as
                            // DuckDB's default; their bytes were compared
                            // as values (#38).
                            bool na = input.IsNull(a, col), nb = input.IsNull(b, col);
                            if (na || nb) return !na && nb;
                            auto va = input.GetInt64(a, col);
                            auto vb = input.GetInt64(b, col);
                            return asc ? SqlLess(va, vb) : SqlGreater(va, vb);
                        });
                    }
                    return;
                }
                if (tid == LogicalTypeId::DOUBLE) {
                    for (auto &idxs : partitions_list) {
                        std::sort(idxs.begin(), idxs.end(), [&](idx_t a, idx_t b) {
                            // NULLs last, after ASC and DESC alike, as
                            // DuckDB's default; their bytes were compared
                            // as values (#38).
                            bool na = input.IsNull(a, col), nb = input.IsNull(b, col);
                            if (na || nb) return !na && nb;
                            auto va = input.GetDouble(a, col);
                            auto vb = input.GetDouble(b, col);
                            return asc ? SqlLess(va, vb) : SqlGreater(va, vb);
                        });
                    }
                    return;
                }
                if (tid == LogicalTypeId::VARCHAR) {
                    for (auto &idxs : partitions_list) {
                        std::sort(idxs.begin(), idxs.end(), [&](idx_t a, idx_t b) {
                            // NULLs last, after ASC and DESC alike, as
                            // DuckDB's default; their bytes were compared
                            // as values (#38).
                            bool na = input.IsNull(a, col), nb = input.IsNull(b, col);
                            if (na || nb) return !na && nb;
                            auto &sa = input.GetStr(a, col);
                            auto &sb = input.GetStr(b, col);
                            uint32_t la = sa.GetSize(), lb = sb.GetSize();
                            uint32_t m = la < lb ? la : lb;
                            int c = memcmp(sa.GetData(), sb.GetData(), m);
                            if (c == 0) c = (la < lb) ? -1 : (la > lb) ? 1 : 0;
                            return asc ? (c < 0) : (c > 0);
                        });
                    }
                    return;
                }
            }
            // Generic Value-based fallback.
            for (auto &idxs : partitions_list) {
                std::sort(idxs.begin(), idxs.end(), [&](idx_t a, idx_t b) {
                    for (auto &ord : win.order_by) {
                        if (ord.expression->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                            auto &ref = static_cast<BoundColumnRef &>(*ord.expression);
                            auto va = input.Get(a, ref.column_index);
                            auto vb = input.Get(b, ref.column_index);
                            if (va.IsNull() && vb.IsNull()) continue;
                            if (va.IsNull() || vb.IsNull()) return vb.IsNull();
                            if (va < vb) return ord.ascending;
                            if (vb < va) return !ord.ascending;
                        }
                    }
                    return false;
                });
            }
        };
        sort_partitions();

        // Typed equality for RANK/DENSE_RANK tie detection (reads raw data).
        auto order_equal = [&](idx_t a, idx_t b) -> bool {
            // Without an ORDER BY every row of a partition is a peer.
            if (rank_order_col == INVALID_INDEX) return win.order_by.empty();
            bool na = input.IsNull(a, rank_order_col), nb = input.IsNull(b, rank_order_col);
            if (na || nb) return na && nb;
            switch (rank_order_tid) {
            case LogicalTypeId::INTEGER:
                return input.GetInt32(a, rank_order_col) == input.GetInt32(b, rank_order_col);
            case LogicalTypeId::BIGINT:
                return input.GetInt64(a, rank_order_col) == input.GetInt64(b, rank_order_col);
            case LogicalTypeId::DOUBLE:
                return SqlEq(input.GetDouble(a, rank_order_col), input.GetDouble(b, rank_order_col));
            case LogicalTypeId::VARCHAR: {
                auto &sa = input.GetStr(a, rank_order_col);
                auto &sb = input.GetStr(b, rank_order_col);
                if (sa.GetSize() != sb.GetSize()) return false;
                return memcmp(sa.GetData(), sb.GetData(), sa.GetSize()) == 0;
            }
            default: {
                auto va = input.Get(a, rank_order_col);
                auto vb = input.Get(b, rank_order_col);
                return va == vb;
            }
            }
        };

        // Per-function O(n) computation. Function kind resolved once, outside the row loop.
        for (auto &indices : partitions_list) {
            idx_t ps = indices.size();
            if (ps == 0) continue;

            switch (fn) {
            case WF_ROW_NUMBER:
                for (idx_t pos = 0; pos < ps; pos++)
                    output[indices[pos]][out_col] = Value::BIGINT(static_cast<int64_t>(pos + 1));
                break;

            case WF_RANK: {
                int64_t cur_rank = 1;
                output[indices[0]][out_col] = Value::BIGINT(1);
                for (idx_t pos = 1; pos < ps; pos++) {
                    if (!order_equal(indices[pos], indices[pos - 1]))
                        cur_rank = static_cast<int64_t>(pos + 1);
                    output[indices[pos]][out_col] = Value::BIGINT(cur_rank);
                }
                break;
            }

            case WF_DENSE_RANK: {
                int64_t cur_rank = 1;
                output[indices[0]][out_col] = Value::BIGINT(1);
                for (idx_t pos = 1; pos < ps; pos++) {
                    if (!order_equal(indices[pos], indices[pos - 1])) cur_rank++;
                    output[indices[pos]][out_col] = Value::BIGINT(cur_rank);
                }
                break;
            }

            case WF_NTILE:
                for (idx_t pos = 0; pos < ps; pos++) {
                    int64_t bucket = NtileBucket(pos, ps, ntile_buckets);
                    output[indices[pos]][out_col] = Value::BIGINT(bucket);
                }
                break;

            case WF_LAG:
                for (idx_t pos = 0; pos < ps; pos++) {
                    int64_t target = static_cast<int64_t>(pos) - lag_offset;
                    if (target >= 0 && target < static_cast<int64_t>(ps) && arg_col != INVALID_INDEX) {
                        output[indices[pos]][out_col] =
                            input.Get(indices[static_cast<idx_t>(target)], arg_col);
                    } // else stays default NULL Value
                }
                break;

            case WF_LEAD:
                for (idx_t pos = 0; pos < ps; pos++) {
                    int64_t target = static_cast<int64_t>(pos) + lag_offset;
                    if (target >= 0 && target < static_cast<int64_t>(ps) && arg_col != INVALID_INDEX) {
                        output[indices[pos]][out_col] =
                            input.Get(indices[static_cast<idx_t>(target)], arg_col);
                    }
                }
                break;

            case WF_FIRST_VALUE: {
                Value v;
                if (arg_col != INVALID_INDEX) v = input.Get(indices[0], arg_col);
                for (idx_t pos = 0; pos < ps; pos++) output[indices[pos]][out_col] = v;
                break;
            }

            case WF_LAST_VALUE: {
                Value v;
                if (arg_col != INVALID_INDEX) v = input.Get(indices[ps - 1], arg_col);
                for (idx_t pos = 0; pos < ps; pos++) output[indices[pos]][out_col] = v;
                break;
            }

            case WF_SUM:
            case WF_COUNT:
            case WF_AVG:
            case WF_MIN:
            case WF_MAX: {
                // Aggregate over the whole partition, broadcast to rows:
                // the same computation as the main path's (exact integer
                // sums, NULL for no value; Stage 8, #12).
                WinInfo wi;
                wi.fn = fn;
                wi.arg_col = arg_col;
                wi.arg_tid = (arg_col != INVALID_INDEX && arg_col < input.types.size())
                                 ? input.types[arg_col] : LogicalTypeId::SQLNULL;
                Value result = ComputePartitionAggregate(wi, indices);
                for (auto idx : indices) output[idx][out_col] = result;
                break;
            }

            case WF_UNKNOWN:
                break;
            }
        }
    }

    std::vector<BoundExprPtr> select_list_;
    BoundExprPtr qualify_;

    // Streaming state.
    InputBuf input_;
    std::vector<std::vector<idx_t>> partitions_;
    std::vector<WinInfo> select_win_info_;
    WinInfo qualify_win_info_;
    bool qualify_has_win_ = false;
    BoundConstant *qualify_const_ = nullptr;
    int qualify_op_ = -1;             // 0=EQ 1=LT 2=LE 3=GT 4=GE 5=NE
    bool qualify_win_on_left_ = true;
    idx_t emit_part_ = 0;
    idx_t emit_pos_ = 0;
    bool setup_done_ = false;
    // Lazy-sort state - sort each partition on-demand at emit time.
    BoundWindowExpression *sort_win_ = nullptr;
    bool sort_single_col_ok_ = false;
    idx_t sort_col_ = INVALID_INDEX;
    bool sort_asc_ = true;
    LogicalTypeId sort_tid_ = LogicalTypeId::SQLNULL;
    std::vector<int32_t> sort_i32_;
    std::vector<int64_t> sort_i64_;
    std::vector<double> sort_d_;
    std::vector<string_t> sort_s_;
    // One byte per partition, not std::vector<bool>: Setup() sorts
    // partitions on several workers, and each sets its own flag, so the
    // flags must not share a word.
    std::vector<uint8_t> partition_sorted_;
    // Streaming RANK/DENSE_RANK state, reset per partition.
    int64_t rank_cur_ = 1;
    idx_t rank_prev_ = INVALID_INDEX;
    int64_t dense_cur_ = 1;
    idx_t dense_prev_ = INVALID_INDEX;
    // Fallback path (multi-window, different partition/order).
    bool use_fallback_ = false;
    std::vector<std::vector<Value>> fallback_rows_;
    // LIMIT pushdown - 0 means unlimited (full sort).
    idx_t row_limit_ = 0;
};

// ============================================================================
// Distinct
// ============================================================================

class PhysicalDistinct : public PhysicalOperator {
public:
    explicit PhysicalDistinct(std::vector<LogicalType> types)
        : PhysicalOperator(PhysicalOperatorType::TABLE_SCAN, std::move(types)) {}

    void Init() override {
        for (auto &child : children) child->Init();
        seen_.clear();
    }

    bool GetData(DataChunk &result) override {
        while (true) {
            DataChunk input;
            input.Initialize(GetTypes());
            if (!children[0]->GetData(input)) return false;

            result.Initialize(GetTypes());
            idx_t result_count = 0;

            for (idx_t i = 0; i < input.size(); i++) {
                // Build a key from all column values.
                std::string key;
                for (idx_t col = 0; col < input.ColumnCount(); col++) {
                    AppendKeyVector(key, input.GetVector(col), i);
                }
                if (seen_.insert(key).second) {
                    for (idx_t col = 0; col < input.ColumnCount(); col++) {
                        result.SetValue(col, result_count, input.GetValue(col, i));
                    }
                    result_count++;
                }
            }

            if (result_count > 0) {
                result.SetCardinality(result_count);
                return true;
            }
        }
    }

private:
    std::unordered_set<std::string> seen_;
};

// ============================================================================
// Hash Aggregate
// ============================================================================

class PhysicalHashAggregate : public PhysicalOperator {
public:
    PhysicalHashAggregate(std::vector<BoundExprPtr> groups,
                          std::vector<BoundExprPtr> aggregates,
                          std::vector<LogicalType> result_types)
        : PhysicalOperator(PhysicalOperatorType::TABLE_SCAN, std::move(result_types)),
          groups_(std::move(groups)), aggregates_(std::move(aggregates)) {}

    // TopN pushdown: when an upstream OrderBy(simple_col_ref) → Limit(K) is
    // present, OrderBy/Projection forwards (col_idx, ascending, limit) to us.
    // The direct-emit path then keeps a bounded heap of size `limit` instead
    // of materializing every group. col_idx is in our OUTPUT slot space:
    // 0..groups-1 → group cols, groups..groups+aggs-1 → agg cols.
    void SetTopNHint(idx_t col_idx, bool ascending, idx_t limit) override {
        topn_col_idx_ = col_idx;
        topn_ascending_ = ascending;
        topn_limit_ = limit;
        topn_active_ = true;
    }

    // Bare LIMIT (no ORDER BY) — no row ordering required, just emit the
    // first N. Used by emit paths that would otherwise materialize every
    // group (a wide-VARCHAR GROUP BY: 80M string copies before LIMIT 10 truncates).
    void SetRowLimit(idx_t n) override { row_limit_hint_ = n; }

    void Init() override {
        for (auto &child : children) child->Init();
        computed_ = false;
        emit_pos_ = 0;
    }

    bool GetData(DataChunk &result) override {
        if (!computed_) {
            ComputeAggregates();
            computed_ = true;
        }

        if (emit_pos_ >= result_rows_.size()) return false;

        result.Initialize(GetTypes());
        idx_t count = 0;
        while (emit_pos_ < result_rows_.size() && count < VECTOR_SIZE) {
            auto &row = result_rows_[emit_pos_];
            for (idx_t col = 0; col < row.size(); col++) {
                result.SetValue(col, count, row[col]);
            }
            emit_pos_++;
            count++;
        }
        result.SetCardinality(count);
        return count > 0;
    }

private:
    // Extras: rarely-used per-AggState fields bundled behind a single
    // unique_ptr. STDDEV/MEDIAN/STRING_AGG/COUNT(DISTINCT)/BOOL_AND/OR all
    // populate fields here only when the relevant aggregate fires. For
    // common queries (COUNT*, SUM, AVG, numeric MIN/MAX) this stays nullptr,
    // saving ~140B per AggState.
    struct AggExtras {
        double sum_sq = 0;          // for STDDEV/VARIANCE
        std::vector<double> values; // for MEDIAN
        std::string str_agg;        // for STRING_AGG
        std::string str_delim;
        bool str_started = false;
        bool bool_and = true;       // for BOOL_AND
        bool bool_or = false;       // for BOOL_OR
        std::unordered_set<std::string> distinct_set; // for COUNT(DISTINCT VARCHAR / mixed)
        zugodb::SimpleI64Set distinct_int_set; // INT/BIGINT fast path
    };

    struct AggState {
        int64_t count = 0;
        double sum = 0;
        bool has_min = false;
        // Lazy: only allocated for VARCHAR MIN. Numeric MIN uses sum_min.
        std::unique_ptr<Value> min_val_ptr;
        double sum_min = 0;         // numeric min for fast comparison
        bool has_max = false;
        std::unique_ptr<Value> max_val_ptr;
        double sum_max = 0;         // numeric max for fast comparison
        std::unique_ptr<AggExtras> extras_ptr;

        // Lazy create extras on first use of any uncommon field.
        AggExtras &extras() {
            if (!extras_ptr) extras_ptr = std::make_unique<AggExtras>();
            return *extras_ptr;
        }
        // Field accessors mirror old direct access (so reads on never-used
        // states return defaults without allocation).
        double sum_sq() const { return extras_ptr ? extras_ptr->sum_sq : 0.0; }
        bool str_started() const { return extras_ptr ? extras_ptr->str_started : false; }
        bool bool_and_v() const { return extras_ptr ? extras_ptr->bool_and : true; }
        bool bool_or_v() const { return extras_ptr ? extras_ptr->bool_or : false; }
        const std::string &str_agg_const() const {
            static const std::string empty;
            return extras_ptr ? extras_ptr->str_agg : empty;
        }
        bool has_extras() const { return (bool)extras_ptr; }

        // Helpers for MIN/MAX
        Value &min_val() {
            if (!min_val_ptr) min_val_ptr = std::make_unique<Value>();
            return *min_val_ptr;
        }
        Value &max_val() {
            if (!max_val_ptr) max_val_ptr = std::make_unique<Value>();
            return *max_val_ptr;
        }
        bool min_val_is_null() const { return !min_val_ptr || min_val_ptr->IsNull(); }
        bool max_val_is_null() const { return !max_val_ptr || max_val_ptr->IsNull(); }
        Value min_val_or_null() const { return min_val_ptr ? *min_val_ptr : Value(); }
        Value max_val_or_null() const { return max_val_ptr ? *max_val_ptr : Value(); }
        void set_min_from(const AggState &s) {
            if (s.min_val_ptr) min_val_ptr = std::make_unique<Value>(*s.min_val_ptr);
        }
        void set_max_from(const AggState &s) {
            if (s.max_val_ptr) max_val_ptr = std::make_unique<Value>(*s.max_val_ptr);
        }
    };

    struct AggInfo {
        std::string name;
        idx_t col_idx;        // column index if simple column ref, else INVALID_INDEX
        bool is_count_star;
        bool is_distinct;
        // Algebraic collapse: when the agg is SUM(col +/- const), share the
        // base SUM(col) + COUNT_NON_NULL(col) scan and synthesize the per-
        // agg result as `state.sum + offset * state.count` at emit. A query
        // with 90 SUM(col + N) calls collapses to ONE scan instead
        // of 90× per-row Value boxing in the generic agg path.
        bool sum_with_offset = false;
        double sum_offset = 0.0;
        // Dedup: when multiple aggs target the same (col, name), only the
        // first one (primary) runs the scan; the rest set `primary_idx`
        // pointing to it and skip the inner loop. Emit reads from the
        // primary's AggState. Drops 90 redundant column scans to 1.
        idx_t primary_idx = INVALID_INDEX;
        // STRLEN/LENGTH(col_ref): when the agg arg is a scalar function
        // computing a VARCHAR's byte length, we accumulate string sizes
        // directly inside the parquet generic GROUP BY hot loop (e.g.
        // AVG(STRLEN(col))) without falling through to the per-row
        // ExpressionExecutor path. col_idx stays INVALID_INDEX so the
        // numeric fast paths still bypass this agg correctly; the
        // strlen-aware paths use strlen_col_idx instead.
        bool strlen_of_col = false;
        idx_t strlen_col_idx = INVALID_INDEX;
    };

    // Fused JOIN+aggregate hot-loop - defined out-of-class (needs full PhysicalHashJoin).
    // Returns true if it handled the computation (result_rows_ populated).
    bool TryComputeFusedJoinAggregate(PhysicalHashJoin *hj,
                                       const std::vector<AggInfo> &agg_infos,
                                       const std::vector<idx_t> &group_col_indices);

    // Fast path: read a double from a vector at index without Value allocation.
    static double ReadDouble(const Vector &vec, idx_t row) {
        auto tid = vec.GetType().id();
        auto *validity = &vec.GetValidity();
        if (!validity->RowIsValid(row)) return 0.0;
        switch (tid) {
        case LogicalTypeId::INTEGER:
        case LogicalTypeId::DATE:
            return static_cast<double>(reinterpret_cast<const int32_t *>(vec.GetData())[row]);
        case LogicalTypeId::BIGINT:
        case LogicalTypeId::TIMESTAMP:
        case LogicalTypeId::TIMESTAMP_TZ:
        case LogicalTypeId::TIME:
            return static_cast<double>(reinterpret_cast<const int64_t *>(vec.GetData())[row]);
        case LogicalTypeId::DOUBLE:  return reinterpret_cast<const double *>(vec.GetData())[row];
        case LogicalTypeId::FLOAT:   return static_cast<double>(reinterpret_cast<const float *>(vec.GetData())[row]);
        // These read as 0 before (Stage 8, #12): MIN and MAX of a BOOLEAN
        // were false, a SUM of SMALLINT 0.
        case LogicalTypeId::BOOLEAN: return reinterpret_cast<const bool *>(vec.GetData())[row] ? 1.0 : 0.0;
        case LogicalTypeId::TINYINT: return static_cast<double>(reinterpret_cast<const int8_t *>(vec.GetData())[row]);
        case LogicalTypeId::SMALLINT: return static_cast<double>(reinterpret_cast<const int16_t *>(vec.GetData())[row]);
        default: return 0.0;
        }
    }

    // Whether `val` replaces the MIN (MAX) state. A FLOAT or DOUBLE in
    // DuckDB's order, NaN the largest and the last of equal values winning
    // (min over 0.0 then -0.0 is -0.0), as DuckDB's MIN and MAX (#42); any
    // other type, read as a double, as before.
    static bool IsFloating(LogicalTypeId t) {
        return t == LogicalTypeId::FLOAT || t == LogicalTypeId::DOUBLE;
    }
    static bool IsFloating(const Vector &v) { return IsFloating(v.GetType().id()); }
    static bool IsFloating(const Value &v) { return IsFloating(v.type().id()); }
    static bool MinWins(const AggState &s, double val, bool floating) {
        return !s.has_min || (floating ? MinReplaces(s.sum_min, val) : val < s.sum_min);
    }
    static bool MaxWins(const AggState &s, double val, bool floating) {
        return !s.has_max || (floating ? MaxReplaces(s.sum_max, val) : val > s.sum_max);
    }

    // Fast path: build a group key string from vector data directly, one
    // tagged part per column (group_key.hpp); "NULL|" and '|'-joined text
    // made a NULL and the text 'NULL', and ('a|', 'b') and ('a', '|b'), one
    // group.
    static void AppendGroupKey(std::string &key, const Vector &vec, idx_t row) {
        AppendKeyVector(key, vec, row);
    }

    void ComputeAggregates() {
        // Process chunks directly - no intermediate row materialization.
        // ankerl maps are 2-3× faster than std::unordered_map for the
        // 5M+ unique keys produced by a high-card multi-col GROUP BY.
        ankerl::unordered_dense::map<std::string, std::vector<AggState>> group_states;
        ankerl::unordered_dense::map<std::string, std::vector<Value>> group_keys;
        std::vector<std::string> group_order;
        // Int-only fast path: when the FUSED GENERIC parquet branch sees
        // GROUP BY columns that are all integral (no VARCHAR), the per-
        // thread `pkey` is content-deterministic across threads, so the
        // std::string content_key the generic path builds is unnecessary
        // work. The 5.7 M std::string allocations + 4 std::string-keyed
        // map insertions per pkey were ~25 s of wall on this code.
        // When `int_only_active` is set, run_merge and the result emit
        // loop iterate the uint64-keyed map below instead of the
        // std::string-keyed maps above.
        struct MergeSrc { int t; uint64_t pkey; const std::string *sk_raw; };
        // Fused per-group record: kv + states + cross-thread merge srcs
        // share one map slot so each pkey lookup probes ankerl once
        // instead of three times (group_keys/states/src_map).
        struct PerGroupRec {
            std::vector<Value> kv;
            std::vector<AggState> states;
            std::vector<MergeSrc> srcs;
        };
        ankerl::unordered_dense::map<uint64_t, PerGroupRec> group_recs_u64;
        std::vector<uint64_t> group_order_u64;
        bool int_only_active = false;

        idx_t num_aggs = aggregates_.size();

        // Pre-resolve aggregate argument column indices for fast path.
        std::vector<AggInfo> agg_infos(num_aggs);
        for (idx_t a = 0; a < num_aggs; a++) {
            auto &agg_expr = static_cast<BoundFunction &>(*aggregates_[a]);
            agg_infos[a].name = StringUtil::Upper(agg_expr.function_name);
            agg_infos[a].is_count_star = agg_expr.arguments.empty();
            agg_infos[a].is_distinct = agg_expr.is_distinct;
            agg_infos[a].col_idx = INVALID_INDEX;
            if (!agg_expr.arguments.empty()) {
                auto &arg = *agg_expr.arguments[0];
                if (arg.GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                    agg_infos[a].col_idx = static_cast<BoundColumnRef &>(arg).column_index;
                } else if (arg.GetExpressionType() == BoundExpressionType::FUNCTION) {
                    // SUM/AVG/MIN/MAX/COUNT(LENGTH(col)) or STRLEN(col):
                    // accumulate the byte length of each row's string. The
                    // GROUP BY hot loop reads sizes directly from the parquet
                    // VARCHAR column without per-row ExpressionExecutor cost.
                    auto &fn = static_cast<BoundFunction &>(arg);
                    auto fn_name = StringUtil::Upper(fn.function_name);
                    if ((fn_name == "LENGTH" || fn_name == "STRLEN") &&
                        fn.arguments.size() == 1 &&
                        fn.arguments[0]->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                        agg_infos[a].strlen_of_col = true;
                        agg_infos[a].strlen_col_idx =
                            static_cast<BoundColumnRef &>(*fn.arguments[0]).column_index;
                    }
                }
                if (!agg_infos[a].strlen_of_col &&
                    agg_infos[a].name == "SUM" && !agg_infos[a].is_distinct &&
                    arg.GetExpressionType() == BoundExpressionType::ARITHMETIC) {
                    // Detect SUM(col +/- const): share the base scan and
                    // derive each result at emit. Accept (col OP const) or
                    // (const + col); subtract is only valid as (col - const).
                    auto &ar = static_cast<BoundArithmetic &>(arg);
                    const BoundExpression *col_side = nullptr;
                    const BoundExpression *con_side = nullptr;
                    bool col_on_left = false;
                    if (ar.left->GetExpressionType() == BoundExpressionType::COLUMN_REF &&
                        ar.right->GetExpressionType() == BoundExpressionType::CONSTANT) {
                        col_side = ar.left.get(); con_side = ar.right.get();
                        col_on_left = true;
                    } else if (ar.left->GetExpressionType() == BoundExpressionType::CONSTANT &&
                               ar.right->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                        col_side = ar.right.get(); con_side = ar.left.get();
                        col_on_left = false;
                    }
                    // Only over floating point: integer col + const raises
                    // on a row that overflows, which sum + count * const
                    // never sees (SUM(x + 1) of 2147483647 gave
                    // 2147483648; DuckDB's error, #43).
                    auto ar_type = ar.GetReturnType().id();
                    bool ar_floating = ar_type == LogicalTypeId::DOUBLE ||
                                       ar_type == LogicalTypeId::FLOAT;
                    if (ar_floating && col_side && con_side &&
                        (ar.op == "+" || (ar.op == "-" && col_on_left))) {
                        const auto &con = static_cast<const BoundConstant &>(*con_side);
                        if (!con.value.IsNull()) {
                            double off = 0.0;
                            bool ok = true;
                            try {
                                switch (con.value.type().id()) {
                                case LogicalTypeId::TINYINT:  off = (double)con.value.GetValue<int8_t>(); break;
                                case LogicalTypeId::SMALLINT: off = (double)con.value.GetValue<int16_t>(); break;
                                case LogicalTypeId::INTEGER:  off = (double)con.value.GetValue<int32_t>(); break;
                                case LogicalTypeId::BIGINT:   off = (double)con.value.GetValue<int64_t>(); break;
                                case LogicalTypeId::FLOAT:    off = (double)con.value.GetValue<float>(); break;
                                case LogicalTypeId::DOUBLE:   off = con.value.GetValue<double>(); break;
                                default: ok = false; break;
                                }
                            } catch (...) { ok = false; }
                            if (ok) {
                                if (ar.op == "-") off = -off;
                                agg_infos[a].col_idx =
                                    static_cast<const BoundColumnRef &>(*col_side).column_index;
                                agg_infos[a].sum_with_offset = true;
                                agg_infos[a].sum_offset = off;
                            }
                        }
                    }
                }
            }
        }
        // Dedup: same (col, name, !is_distinct) → only the primary runs the
        // scan. A query with 90 SUMs over the same column would otherwise
        // make the FUSED PARQUET FAST PATH iterate the typed-numeric inner loop 90×
        // per row group. SUM with offsets shares one base scan because
        // emit synthesizes `sum + offset * count` per agg.
        for (idx_t a = 0; a < num_aggs; a++) {
            auto &info = agg_infos[a];
            if (info.is_count_star) continue;
            if (info.col_idx == INVALID_INDEX) continue;
            if (info.is_distinct) continue;
            if (info.name != "SUM" && info.name != "COUNT" &&
                info.name != "AVG" && info.name != "MIN" && info.name != "MAX") continue;
            for (idx_t b = 0; b < a; b++) {
                auto &binfo = agg_infos[b];
                if (binfo.is_count_star) continue;
                if (binfo.col_idx != info.col_idx) continue;
                if (binfo.is_distinct) continue;
                if (binfo.name != info.name) continue;
                info.primary_idx = b;
                break;
            }
        }

        // Pre-resolve group-by column indices.
        std::vector<idx_t> group_col_indices;
        for (auto &g : groups_) {
            if (g->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                group_col_indices.push_back(static_cast<BoundColumnRef &>(*g).column_index);
            }
        }

        // === FAST PATH: fused JOIN + GROUP BY ===
        if (auto *hj = AsHashJoin(children[0].get())) {
            if (TryComputeFusedJoinAggregate(hj, agg_infos, group_col_indices)) return;
        }

        DataChunk chunk;
        chunk.Initialize(children[0]->GetTypes());
        std::string key;
        idx_t total_rows_processed = 0;

        // === FAST PATH: COUNT(*) with no GROUP BY - just count rows ===
        bool is_simple_count_star = (group_col_indices.empty() && num_aggs == 1 &&
                                     agg_infos[0].is_count_star && agg_infos[0].name == "COUNT");
        // FAST PATH: all COUNT/SUM/AVG on simple columns, no GROUP BY
        bool is_simple_no_group = group_col_indices.empty();
        bool all_simple_aggs = is_simple_no_group;
        for (idx_t a = 0; a < num_aggs && all_simple_aggs; a++) {
            auto &info = agg_infos[a];
            if (info.name == "COUNT" && info.is_count_star) continue;
            if ((info.name == "COUNT" || info.name == "SUM" || info.name == "AVG" ||
                 info.name == "MIN" || info.name == "MAX") && info.col_idx != INVALID_INDEX && !info.is_distinct) continue;
            all_simple_aggs = false;
        }

        // === FAST PATH: single-column GROUP BY with simple aggregates ===
        bool single_group_fast = (group_col_indices.size() == 1 && all_simple_aggs == false &&
                                  num_aggs > 0);
        if (single_group_fast) {
            // Re-check: all aggs must be simple COUNT/SUM/AVG/MIN/MAX on direct columns
            single_group_fast = true;
            for (idx_t a = 0; a < num_aggs; a++) {
                auto &info = agg_infos[a];
                if (info.is_count_star) continue;
                if ((info.name == "COUNT" || info.name == "SUM" || info.name == "AVG" ||
                     info.name == "MIN" || info.name == "MAX") &&
                    info.col_idx != INVALID_INDEX && !info.is_distinct) continue;
                single_group_fast = false;
                break;
            }
        }

        if (single_group_fast) {
            idx_t group_col = group_col_indices[0];

            {
                // Column pruning: tell file scan or join to only populate needed columns.
                std::vector<bool> needed(children[0]->GetTypes().size(), false);
                needed[group_col] = true;
                for (auto &info : agg_infos) {
                    if (info.col_idx != INVALID_INDEX && info.col_idx < needed.size()) {
                        needed[info.col_idx] = true;
                    }
                }
                children[0]->SetNeededOutputs(needed);

            // The NULL group of the chunk loop below, when there is one.
            std::unique_ptr<std::vector<AggState>> null_group;

            // Direct-pointer cache for VARCHAR group keys (low-cardinality optimization).
            // Reserve enough for typical cardinality - pointers remain stable.
            std::vector<std::string> str_cache_keys;
            std::vector<std::vector<AggState>> str_cache_states;
            str_cache_keys.reserve(256);
            str_cache_states.reserve(256);

            // Two hash maps based on group column type - avoid string conversion.
            // unordered_dense is ~2× faster than std::unordered_map for the hot
            // INT-keyed GROUP BY path (flat open-addressing vs chained buckets).
            ankerl::unordered_dense::map<int64_t, std::vector<AggState>> int_groups;
            std::unordered_map<std::string, std::vector<AggState>> str_groups;
            std::vector<int64_t> int_order;
            std::vector<std::string> str_order;
            ankerl::unordered_dense::map<int64_t, Value> int_keys;
            std::unordered_map<std::string, Value> str_keys;


            while (children[0]->GetData(chunk)) {
                idx_t chunk_size = chunk.size();
                auto &gvec = chunk.GetVector(group_col);
                auto gtid = gvec.GetType().id();
                bool is_int = (gtid == LogicalTypeId::INTEGER || gtid == LogicalTypeId::BIGINT);
                bool is_str = (gtid == LogicalTypeId::VARCHAR);
                auto &gvalid = gvec.GetValidity();

                for (idx_t i = 0; i < chunk_size; i++) {
                    std::vector<AggState> *states_ptr = nullptr;

                    if ((is_int || is_str) && !gvalid.RowIsValid(i)) {
                        // The NULL group. The raw value of a NULL key put
                        // its rows in the group of 0 or '', or of whatever
                        // the slot held.
                        if (!null_group) null_group = std::make_unique<std::vector<AggState>>(num_aggs);
                        states_ptr = null_group.get();
                    } else if (is_int) {
                        int64_t k = (gtid == LogicalTypeId::BIGINT)
                            ? reinterpret_cast<const int64_t *>(gvec.GetData())[i]
                            : static_cast<int64_t>(reinterpret_cast<const int32_t *>(gvec.GetData())[i]);
                        auto it = int_groups.find(k);
                        if (it == int_groups.end()) {
                            int_order.push_back(k);
                            int_keys[k] = chunk.GetValue(group_col, i);
                            it = int_groups.emplace(k, std::vector<AggState>(num_aggs)).first;
                        }
                        states_ptr = &it->second;
                    } else if (is_str) {
                        auto &s = reinterpret_cast<const string_t *>(gvec.GetData())[i];
                        const char *s_data = s.GetData();
                        uint32_t s_len = s.GetSize();
                        // Linear scan with direct pointer cache - no map lookup on hit.
                        states_ptr = nullptr;
                        for (idx_t oi = 0; oi < str_cache_keys.size(); oi++) {
                            const auto &ck = str_cache_keys[oi];
                            if (ck.size() == s_len && memcmp(ck.data(), s_data, s_len) == 0) {
                                states_ptr = &str_cache_states[oi];
                                break;
                            }
                        }
                        if (!states_ptr) {
                            // New key - add to cache and map.
                            std::string k(s_data, s_len);
                            str_cache_keys.push_back(k);
                            str_cache_states.emplace_back(num_aggs);
                            str_order.push_back(k);
                            str_keys[k] = chunk.GetValue(group_col, i);
                            states_ptr = &str_cache_states.back();
                        }
                    } else {
                        // The group key of any other type (group_key.hpp):
                        // its text kept "nan" from "-nan" and "-0.0" from
                        // "0.0" (#42).
                        std::string k;
                        AppendKeyVector(k, gvec, i);
                        auto it = str_groups.find(k);
                        if (it == str_groups.end()) {
                            str_order.push_back(k);
                            str_keys[k] = chunk.GetValue(group_col, i);
                            it = str_groups.emplace(std::move(k), std::vector<AggState>(num_aggs)).first;
                        }
                        states_ptr = &it->second;
                    }

                    auto &states = *states_ptr;
                    for (idx_t a = 0; a < num_aggs; a++) {
                        auto &state = states[a];
                        auto &info = agg_infos[a];
                        if (info.name == "COUNT" && info.is_count_star) {
                            state.count++;
                        } else if (info.col_idx != INVALID_INDEX) {
                            auto &vec = chunk.GetVector(info.col_idx);
                            if (vec.GetValidity().RowIsValid(i)) {
                                if (info.name == "COUNT") {
                                    state.count++;
                                } else {
                                    double val = ReadDouble(vec, i);
                                    if (info.name == "SUM" || info.name == "AVG") {
                                        state.count++;
                                        state.sum += val;
                                    } else if (info.name == "MIN") {
                                        if (MinWins(state, val, IsFloating(vec))) {
                                            state.sum_min = val; state.has_min = true;
                                            state.min_val() = chunk.GetValue(info.col_idx, i);
                                        }
                                    } else if (info.name == "MAX") {
                                        if (MaxWins(state, val, IsFloating(vec))) {
                                            state.sum_max = val; state.has_max = true;
                                            state.max_val() = chunk.GetValue(info.col_idx, i);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                total_rows_processed += chunk_size;
            }

            // Move cache states into str_groups for result building.
            for (idx_t oi = 0; oi < str_cache_keys.size(); oi++) {
                str_groups[str_cache_keys[oi]] = std::move(str_cache_states[oi]);
            }
            for (auto k : int_order) {
                std::string sk = std::to_string(k);
                group_states[sk] = std::move(int_groups[k]);
                group_keys[sk] = {int_keys[k]};
                group_order.push_back(sk);
            }
            for (auto &k : str_order) {
                group_states[k] = std::move(str_groups[k]);
                group_keys[k] = {str_keys[k]};
                group_order.push_back(k);
            }
            if (null_group) {
                // A key no group of a value holds: those are digits, or the
                // text itself, so extend it until it is free.
                std::string sk("\0NULL", 5);
                while (group_states.count(sk)) sk.push_back('\0');
                group_states[sk] = std::move(*null_group);
                group_keys[sk] = {Value()};
                group_order.push_back(sk);
            }
            } // end of the column-pruned scope
        } else if (is_simple_count_star) {
            group_order.push_back("");
            group_states[""].resize(1);
            group_keys[""] = {};
            auto &state = group_states[""][0];
            // Count chunk sizes.
            while (children[0]->GetData(chunk)) {
                state.count += static_cast<int64_t>(chunk.size());
                total_rows_processed += chunk.size();
            }
        } else if (all_simple_aggs && is_simple_no_group) {
            // Fast: no GROUP BY, simple aggregates - process vectors directly, no key building.
            // Column pruning: only parse columns we aggregate over.
            {
                std::vector<bool> needed(children[0]->GetTypes().size(), false);
                bool any = false;
                for (auto &info : agg_infos) {
                    if (info.col_idx != INVALID_INDEX && info.col_idx < needed.size()) {
                        needed[info.col_idx] = true; any = true;
                    }
                }
                if (any) {
                    children[0]->SetNeededOutputs(needed);
                }
            }

            group_order.push_back("");
            group_states[""].resize(num_aggs);
            group_keys[""] = {};
            auto &states = group_states[""];

            while (children[0]->GetData(chunk)) {
                idx_t chunk_size = chunk.size();
                for (idx_t a = 0; a < num_aggs; a++) {
                    auto &state = states[a];
                    auto &info = agg_infos[a];
                    if (info.name == "COUNT" && info.is_count_star) {
                        state.count += static_cast<int64_t>(chunk_size);
                    } else if (info.col_idx != INVALID_INDEX) {
                        auto &vec = chunk.GetVector(info.col_idx);
                        auto *validity = &vec.GetValidity();
                        bool all_valid = (validity->GetData() == nullptr); // nullptr = all valid
                        if (info.name == "COUNT") {
                            if (all_valid) { state.count += static_cast<int64_t>(chunk_size); }
                            else { for (idx_t i = 0; i < chunk_size; i++) { if (validity->RowIsValid(i)) state.count++; } }
                        } else if (info.name == "SUM" || info.name == "AVG") {
                            auto tid = vec.GetType().id();
                            if (tid == LogicalTypeId::BIGINT && all_valid) {
                                auto *arr = reinterpret_cast<const int64_t *>(vec.GetData());
                                for (idx_t i = 0; i < chunk_size; i++) state.sum += static_cast<double>(arr[i]);
                                state.count += static_cast<int64_t>(chunk_size);
                            } else if (tid == LogicalTypeId::DOUBLE && all_valid) {
                                auto *arr = reinterpret_cast<const double *>(vec.GetData());
                                for (idx_t i = 0; i < chunk_size; i++) state.sum += arr[i];
                                state.count += static_cast<int64_t>(chunk_size);
                            } else if (tid == LogicalTypeId::INTEGER && all_valid) {
                                auto *arr = reinterpret_cast<const int32_t *>(vec.GetData());
                                for (idx_t i = 0; i < chunk_size; i++) state.sum += static_cast<double>(arr[i]);
                                state.count += static_cast<int64_t>(chunk_size);
                            } else {
                                for (idx_t i = 0; i < chunk_size; i++) {
                                    if (all_valid || validity->RowIsValid(i)) {
                                        state.sum += ReadDouble(vec, i);
                                        state.count++;
                                    }
                                }
                            }
                        } else if (info.name == "MIN" || info.name == "MAX") {
                            // VARCHAR columns must compare lexicographically.
                            // ReadDouble returns 0 for every VARCHAR cell, so
                            // every row compared equal and the last row "won",
                            // silently giving MIN('zebra','apple','mango') =
                            // 'zebra'. Use Value::operator< for VARCHAR.
                            bool is_varchar = vec.GetType().id() == LogicalTypeId::VARCHAR || vec.GetType().id() == LogicalTypeId::BLOB;
                            for (idx_t i = 0; i < chunk_size; i++) {
                                if (all_valid || validity->RowIsValid(i)) {
                                    if (is_varchar) {
                                        auto cv = chunk.GetValue(info.col_idx, i);
                                        if (info.name == "MIN") {
                                            if (!state.has_min || cv < state.min_val()) {
                                                state.min_val() = cv;
                                                state.has_min = true;
                                            }
                                        } else {
                                            if (!state.has_max || state.max_val() < cv) {
                                                state.max_val() = cv;
                                                state.has_max = true;
                                            }
                                        }
                                    } else {
                                        double val = ReadDouble(vec, i);
                                        if (info.name == "MIN") {
                                            if (MinWins(state, val, IsFloating(vec))) {
                                                state.sum_min = val; state.has_min = true;
                                                state.min_val() = chunk.GetValue(info.col_idx, i);
                                            }
                                        } else {
                                            if (MaxWins(state, val, IsFloating(vec))) {
                                                state.sum_max = val; state.has_max = true;
                                                state.max_val() = chunk.GetValue(info.col_idx, i);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                total_rows_processed += chunk_size;
            }
        } else {

        // Compact COUNT(*)-only multi-column GROUP BY. The generic chunk-
        // loop below keeps ~400 bytes of per-group state (three string-keyed
        // maps + a heap AggState vector + a Value vector); a GROUP BY with
        // tens of millions of distinct groups exhausts RAM. When every
        // aggregate is COUNT(*), only an int64 counter matters per group, so
        // route to a binary-keyed counter that costs ~60 bytes per group.
        if (!group_col_indices.empty() &&
            group_col_indices.size() == groups_.size() && num_aggs >= 1) {
            bool count_only = true;
            for (idx_t a = 0; a < num_aggs && count_only; a++)
                if (!agg_infos[a].is_count_star) count_only = false;
            // Every group column must be a type the binary key encoder
            // supports (the integral family, date/time/timestamp,
            // varchar/blob). Not FLOAT or DOUBLE: their raw bytes kept
            // -0.0 from 0.0 and one NaN from another, and the key's bytes,
            // not the first row's, were the value shown (#42).
            const auto &ct = children[0]->GetTypes();
            for (idx_t gc : group_col_indices) {
                if (gc >= ct.size()) { count_only = false; break; }
                switch (ct[gc].id()) {
                case LogicalTypeId::BOOLEAN:
                case LogicalTypeId::TINYINT:  case LogicalTypeId::UTINYINT:
                case LogicalTypeId::SMALLINT: case LogicalTypeId::USMALLINT:
                case LogicalTypeId::INTEGER:  case LogicalTypeId::UINTEGER:
                case LogicalTypeId::BIGINT:   case LogicalTypeId::UBIGINT:
                case LogicalTypeId::HUGEINT:
                case LogicalTypeId::DATE:     case LogicalTypeId::TIME:
                case LogicalTypeId::TIMESTAMP:
                case LogicalTypeId::TIMESTAMP_TZ:
                case LogicalTypeId::VARCHAR:  case LogicalTypeId::BLOB:
                    break;
                default:
                    count_only = false; break;
                }
                if (!count_only) break;
            }
            if (count_only) {
                // An upstream ORDER BY <count column> LIMIT forwards a TopN
                // hint whose col_idx lands in the aggregate-column range
                // (every aggregate here is COUNT(*), so ordering by any of
                // them is ordering by the group count). Pass it down so the
                // helper emits only the top-K rows.
                bool topn_count_order =
                    topn_active_ &&
                    topn_col_idx_ >= group_col_indices.size() &&
                    topn_col_idx_ < group_col_indices.size() + num_aggs;
                result_rows_ = zugodb::RunCountGroupAggregate(
                    children[0].get(), group_col_indices, num_aggs,
                    topn_active_, topn_count_order, topn_ascending_,
                    topn_limit_);
                return;
            }
        }

        // Generic chunk-loop fallback (e.g. a PhysicalProjection wraps the
        // parquet scan with a derived GROUP BY key and breaks FUSED PARQUET
        // dispatch). Every input row must be aggregated — no group cap: a
        // cap silently under-counts and, because group arrival order varies
        // across runs, makes the result non-deterministic.
        while (children[0]->GetData(chunk)) {
            idx_t chunk_size = chunk.size();
            for (idx_t i = 0; i < chunk_size; i++) {
                // Build group key directly from vectors.
                key.clear();
                std::vector<Value> key_vals;
                for (auto gc : group_col_indices) {
                    AppendGroupKey(key, chunk.GetVector(gc), i);
                    key_vals.push_back(chunk.GetValue(gc, i));
                }

                // try_emplace: single probe vs find+operator[] (which is two).
                auto [it, inserted] = group_states.try_emplace(
                    key, std::vector<AggState>(num_aggs));
                if (inserted) {
                    group_keys[key] = key_vals;
                    group_order.push_back(key);
                }

                auto &states = it->second;

                // Update aggregates - read directly from vectors.
                for (idx_t a = 0; a < num_aggs; a++) {
                    auto &state = states[a];
                    auto &info = agg_infos[a];

                    if (info.name == "COUNT") {
                        if (info.is_count_star) {
                            state.count++;
                        } else if (info.col_idx != INVALID_INDEX &&
                                   chunk.GetVector(info.col_idx).GetValidity().RowIsValid(i)) {
                            if (info.is_distinct) {
                                // The group key's encoding: every NaN one
                                // value, -0.0 and 0.0 one (#42).
                                std::string dk;
                                AppendKeyVector(dk, chunk.GetVector(info.col_idx), i);
                                if (state.extras().distinct_set.insert(std::move(dk)).second)
                                    state.count++;
                            } else {
                                state.count++;
                            }
                        } else if (info.col_idx == INVALID_INDEX) {
                            // Complex argument - e.g. COUNT(CASE WHEN ... THEN 1 END).
                            // Evaluate the expression per row; count non-null results.
                            auto &agg_expr = static_cast<BoundFunction &>(*aggregates_[a]);
                            if (!agg_expr.arguments.empty()) {
                                DataChunk row_chunk;
                                row_chunk.Initialize(children[0]->GetTypes());
                                for (idx_t c = 0; c < chunk.ColumnCount(); c++)
                                    row_chunk.SetValue(c, 0, chunk.GetValue(c, i));
                                row_chunk.SetCardinality(1);
                                Vector res(agg_expr.arguments[0]->GetReturnType());
                                ExpressionExecutor::Execute(*agg_expr.arguments[0], row_chunk, res, 1);
                                Value arg_val = res.GetValue(0);
                                if (!arg_val.IsNull()) {
                                    if (info.is_distinct) {
                                        std::string dk;
                                        AppendKeyValue(dk, arg_val);
                                        if (state.extras().distinct_set.insert(std::move(dk)).second)
                                            state.count++;
                                    } else {
                                        state.count++;
                                    }
                                }
                            }
                        }
                    } else if (info.col_idx == INVALID_INDEX) {
                        // Complex argument for SUM / AVG / MIN / MAX / etc —
                        // arg is an expression (CASE, arithmetic, function
                        // call), not a plain column ref. Evaluate it per row
                        // and route to the same per-aggregate branches below.
                        // NULL result means absent — agg skips this row.
                        // Without this branch SUM(CASE WHEN ... THEN x END) and
                        // the FILTER-rewrite path silently returned 0.
                        auto &agg_expr = static_cast<BoundFunction &>(*aggregates_[a]);
                        if (agg_expr.arguments.empty()) {
                            // nothing to evaluate — leave state untouched
                        } else {
                            DataChunk row_chunk;
                            row_chunk.Initialize(children[0]->GetTypes());
                            for (idx_t c = 0; c < chunk.ColumnCount(); c++)
                                row_chunk.SetValue(c, 0, chunk.GetValue(c, i));
                            row_chunk.SetCardinality(1);
                            Vector res(agg_expr.arguments[0]->GetReturnType());
                            ExpressionExecutor::Execute(*agg_expr.arguments[0], row_chunk, res, 1);
                            Value arg_val = res.GetValue(0);
                            if (!arg_val.IsNull()) {
                                double val = 0.0;
                                bool numeric = true;
                                try {
                                    switch (arg_val.type().id()) {
                                    case LogicalTypeId::BOOLEAN:  val = arg_val.GetValue<bool>() ? 1.0 : 0.0; break;
                                    case LogicalTypeId::TINYINT:  val = arg_val.GetValue<int8_t>(); break;
                                    case LogicalTypeId::SMALLINT: val = arg_val.GetValue<int16_t>(); break;
                                    case LogicalTypeId::INTEGER:  val = arg_val.GetValue<int32_t>(); break;
                                    case LogicalTypeId::BIGINT:   val = static_cast<double>(arg_val.GetValue<int64_t>()); break;
                                    case LogicalTypeId::FLOAT:    val = arg_val.GetValue<float>(); break;
                                    case LogicalTypeId::DOUBLE:   val = arg_val.GetValue<double>(); break;
                                    default: numeric = false; break;
                                    }
                                } catch (...) { numeric = false; }
                                if (numeric) {
                                    if (info.name == "SUM" || info.name == "AVG") {
                                        state.count++;
                                        state.sum += val;
                                    } else if (info.name == "MIN") {
                                        if (MinWins(state, val, IsFloating(arg_val))) {
                                            state.sum_min = val;
                                            state.has_min = true;
                                            state.min_val() = arg_val;
                                        }
                                    } else if (info.name == "MAX") {
                                        if (MaxWins(state, val, IsFloating(arg_val))) {
                                            state.sum_max = val;
                                            state.has_max = true;
                                            state.max_val() = arg_val;
                                        }
                                    } else if (info.name == "STDDEV" || info.name == "STDDEV_SAMP" ||
                                               info.name == "STDDEV_POP" || info.name == "VARIANCE" ||
                                               info.name == "VAR_SAMP" || info.name == "VAR_POP") {
                                        state.count++;
                                        state.sum += val;
                                        state.extras().sum_sq += val * val;
                                    } else if (info.name == "MEDIAN") {
                                        state.extras().values.push_back(val);
                                    } else if (info.name == "BOOL_AND") {
                                        state.count++;
                                        auto &x = state.extras();
                                        x.bool_and = x.bool_and && (val != 0.0);
                                    } else if (info.name == "BOOL_OR") {
                                        state.count++;
                                        auto &x = state.extras();
                                        x.bool_or = x.bool_or || (val != 0.0);
                                    } else {
                                        // STRING_AGG and the rest were skipped,
                                        // so their result was NULL (Stage 8).
                                        throw NotImplementedException("Aggregate " + info.name +
                                                                      " of an expression is not supported");
                                    }
                                } else {
                                    // A VARCHAR, DATE, ... expression was
                                    // skipped: MIN('hello') was NULL (Stage 8).
                                    throw NotImplementedException("Aggregate " + info.name + " of a " +
                                                                  arg_val.type().ToString() +
                                                                  " expression is not supported");
                                }
                            }
                        }
                    } else if (info.col_idx != INVALID_INDEX) {
                        auto &vec = chunk.GetVector(info.col_idx);
                        if (vec.GetValidity().RowIsValid(i)) {
                            // For VARCHAR columns, MIN/MAX must compare strings
                            // lexicographically, not via ReadDouble (which
                            // returns 0.0 for VARCHAR — all rows then compare
                            // equal and the last row "wins", giving silently
                            // wrong results like MIN('zebra','apple','mango')
                            // = 'zebra'). Use Value::operator< for VARCHAR.
                            bool is_varchar = vec.GetType().id() == LogicalTypeId::VARCHAR || vec.GetType().id() == LogicalTypeId::BLOB;
                            double val = is_varchar ? 0.0 : ReadDouble(vec, i);
                            if (info.name == "SUM" || info.name == "AVG") {
                                state.count++;
                                state.sum += val;
                            } else if (info.name == "MIN") {
                                if (is_varchar) {
                                    auto cv = chunk.GetValue(info.col_idx, i);
                                    if (!state.has_min || cv < state.min_val()) {
                                        state.min_val() = cv;
                                        state.has_min = true;
                                    }
                                } else if (MinWins(state, val, IsFloating(vec))) {
                                    state.sum_min = val;
                                    state.has_min = true;
                                    state.min_val() = chunk.GetValue(info.col_idx, i);
                                }
                            } else if (info.name == "MAX") {
                                if (is_varchar) {
                                    auto cv = chunk.GetValue(info.col_idx, i);
                                    if (!state.has_max || state.max_val() < cv) {
                                        state.max_val() = cv;
                                        state.has_max = true;
                                    }
                                } else if (MaxWins(state, val, IsFloating(vec))) {
                                    state.sum_max = val;
                                    state.has_max = true;
                                    state.max_val() = chunk.GetValue(info.col_idx, i);
                                }
                            } else if (info.name == "STDDEV" || info.name == "STDDEV_SAMP" ||
                                       info.name == "STDDEV_POP" || info.name == "VARIANCE" ||
                                       info.name == "VAR_SAMP" || info.name == "VAR_POP") {
                                state.count++;
                                state.sum += val;
                                state.extras().sum_sq += val * val;
                            } else if (info.name == "MEDIAN") {
                                state.extras().values.push_back(val);
                            } else if (info.name == "BOOL_AND") {
                                state.count++;
                                auto &x = state.extras();
                                x.bool_and = x.bool_and && (val != 0.0);
                            } else if (info.name == "BOOL_OR") {
                                state.count++;
                                auto &x = state.extras();
                                x.bool_or = x.bool_or || (val != 0.0);
                            } else if (info.name == "STRING_AGG" || info.name == "LISTAGG" || info.name == "GROUP_CONCAT") {
                                auto v = chunk.GetValue(info.col_idx, i);
                                if (!v.IsNull()) {
                                    auto &x = state.extras();
                                    if (!x.str_started) {
                                        // The separator, a string constant
                                        // (the binder's check), else ','.
                                        auto &fx = static_cast<BoundFunction &>(*aggregates_[a]);
                                        x.str_delim = fx.arguments.size() > 1
                                            ? static_cast<BoundConstant &>(*fx.arguments[1]).value.ToString()
                                            : std::string(",");
                                    }
                                    if (x.str_started) x.str_agg += x.str_delim;
                                    x.str_agg += v.ToString();
                                    x.str_started = true;
                                }
                            }
                        }
                    } else {
                        // Complex expression - fallback to Value path.
                        auto &agg_expr = static_cast<BoundFunction &>(*aggregates_[a]);
                        Value arg_val;
                        if (!agg_expr.arguments.empty()) {
                            DataChunk row_chunk;
                            row_chunk.Initialize(children[0]->GetTypes());
                            for (idx_t c = 0; c < chunk.ColumnCount(); c++)
                                row_chunk.SetValue(c, 0, chunk.GetValue(c, i));
                            row_chunk.SetCardinality(1);
                            Vector res(agg_expr.arguments[0]->GetReturnType());
                            ExpressionExecutor::Execute(*agg_expr.arguments[0], row_chunk, res, 1);
                            arg_val = res.GetValue(0);
                        }
                        // Only COUNT, SUM and AVG are computed here; any
                        // other aggregate of an expression gave NULL, and
                        // a non-DOUBLE value was read as a double (Stage
                        // 8, #12).
                        if (info.name != "COUNT" && info.name != "SUM" && info.name != "AVG") {
                            throw NotImplementedException("Aggregate " + info.name +
                                                          " of this expression is not supported");
                        }
                        if (info.name == "COUNT" && !arg_val.IsNull()) state.count++;
                        else if ((info.name == "SUM" || info.name == "AVG") && !arg_val.IsNull()) {
                            double d;
                            switch (arg_val.type().id()) {
                            case LogicalTypeId::DOUBLE: d = arg_val.GetValue<double>(); break;
                            case LogicalTypeId::FLOAT: d = arg_val.GetValue<float>(); break;
                            case LogicalTypeId::INTEGER: d = arg_val.GetValue<int32_t>(); break;
                            case LogicalTypeId::SMALLINT: d = arg_val.GetValue<int16_t>(); break;
                            case LogicalTypeId::TINYINT: d = arg_val.GetValue<int8_t>(); break;
                            default:
                                throw NotImplementedException("Aggregate " + info.name + " of a " +
                                                              arg_val.type().ToString() +
                                                              " expression is not supported");
                            }
                            state.count++;
                            state.sum += d;
                        }
                    }
                }
            }
            total_rows_processed += chunk_size;
        }
        }  // close the `} else {` block opened above the slow chunk-loop
        // Pre-resolve emit descriptors once per query. The big per-agg
        // dispatch switch lives in agg_emit_helpers.cpp so future emit-
        // loop changes don't shift this TU's .text section
        // (see feedback_text_icache_shift.md).
        std::vector<EmitAggDesc> emit_descs(num_aggs);
        for (idx_t a = 0; a < num_aggs; a++) {
            auto &agg_expr = static_cast<BoundFunction &>(*aggregates_[a]);
            emit_descs[a].kind = ResolveEmitAggKind(StringUtil::Upper(agg_expr.function_name));
            emit_descs[a].return_type_id = agg_expr.GetReturnType().id();
            emit_descs[a].sum_with_offset = agg_infos[a].sum_with_offset;
            emit_descs[a].sum_offset = agg_infos[a].sum_offset;
            emit_descs[a].arg_float = !agg_expr.arguments.empty() &&
                agg_expr.arguments[0]->GetReturnType().id() == LogicalTypeId::FLOAT;
        }

        auto build_view = [](const AggState &state, EmitAggView &view) {
            view.count = state.count;
            view.sum = state.sum;
            view.sum_sq = state.sum_sq();
            view.has_min = state.has_min;
            view.sum_min = state.sum_min;
            view.min_val_ptr = state.min_val_is_null() ? nullptr : state.min_val_ptr.get();
            view.has_max = state.has_max;
            view.sum_max = state.sum_max;
            view.max_val_ptr = state.max_val_is_null() ? nullptr : state.max_val_ptr.get();
            view.str_started = state.str_started();
            view.str_agg = &state.str_agg_const();
            view.values = state.extras_ptr ? &state.extras_ptr->values : nullptr;
            view.bool_and_v = state.bool_and_v();
            view.bool_or_v = state.bool_or_v();
        };

        if (int_only_active) {
            // uint64-keyed emit: one map probe per group instead of two
            // (kv + states share one PerGroupRec slot). std::string-keyed
            // group_states/group_keys/group_order stay empty in this path.
            result_rows_.reserve(group_order_u64.size());
            for (uint64_t pkey : group_order_u64) {
                auto &rec = group_recs_u64.find(pkey)->second;

                std::vector<Value> result_row;
                result_row.reserve(rec.kv.size() + num_aggs);
                for (auto &v : rec.kv) result_row.push_back(v);

                for (idx_t a = 0; a < num_aggs; a++) {
                    idx_t state_idx = (agg_infos[a].primary_idx != INVALID_INDEX)
                                          ? agg_infos[a].primary_idx : a;
                    EmitAggView view;
                    build_view(rec.states[state_idx], view);
                    EmitAggValue(emit_descs[a], view, result_row);
                }

                result_rows_.push_back(std::move(result_row));
            }
        } else {
            result_rows_.reserve(group_order.size());
            for (auto &gk : group_order) {
                auto &key_vals = group_keys.find(gk)->second;
                auto &states = group_states.find(gk)->second;

                std::vector<Value> result_row;
                result_row.reserve(key_vals.size() + num_aggs);
                for (auto &v : key_vals) result_row.push_back(v);

                for (idx_t a = 0; a < num_aggs; a++) {
                    // Dedup: when primary_idx is set, emit reads from the
                    // primary's state (the only one populated by the scan).
                    idx_t state_idx = (agg_infos[a].primary_idx != INVALID_INDEX)
                                          ? agg_infos[a].primary_idx : a;
                    EmitAggView view;
                    build_view(states[state_idx], view);
                    EmitAggValue(emit_descs[a], view, result_row);
                }

                result_rows_.push_back(std::move(result_row));
            }
        }

        // Handle no-group aggregation (e.g., SELECT COUNT(*) FROM t).
        if (groups_.empty() && result_rows_.empty()) {
            std::vector<Value> row;
            // Use default states (count=0, etc.)
            std::vector<AggState> default_states(num_aggs);

            // We still need to process all rows for no-group aggs.
            // They were already processed above, but if there are no rows
            // and no groups, we need a single result row with default values.
            for (idx_t a = 0; a < num_aggs; a++) {
                auto &agg_expr = static_cast<BoundFunction &>(*aggregates_[a]);
                auto name = StringUtil::Upper(agg_expr.function_name);
                if (name == "COUNT") {
                    row.push_back(Value::BIGINT(0));
                } else {
                    row.push_back(Value()); // NULL
                }
            }
            result_rows_.push_back(std::move(row));
        }
    }

    std::vector<BoundExprPtr> groups_;
    std::vector<BoundExprPtr> aggregates_;
    std::vector<std::vector<Value>> result_rows_;
    bool computed_ = false;
    idx_t emit_pos_ = 0;
    // TopN pushdown state (see SetTopNHint comment).
    bool topn_active_ = false;
    idx_t topn_col_idx_ = 0;
    bool topn_ascending_ = true;
    idx_t topn_limit_ = 0;
    // Bare LIMIT propagated from PhysicalLimit when no ORDER BY exists.
    idx_t row_limit_hint_ = 0;
};

// ============================================================================
// Hash Join
// ============================================================================

class PhysicalHashJoin : public PhysicalOperator {
public:
    PhysicalHashJoin(JoinType join_type, BoundExprPtr condition,
                     std::vector<LogicalType> result_types,
                     idx_t left_col_count, idx_t right_col_count)
        : PhysicalOperator(PhysicalOperatorType::TABLE_SCAN, std::move(result_types)),
          join_type_(join_type), condition_(std::move(condition)),
          left_col_count_(left_col_count), right_col_count_(right_col_count) {}

    // Projection pushdown: tell join which output columns are needed.
    void SetNeededOutputs(const std::vector<bool> &mask) override { needed_cols_ = mask; }

    void Init() override {
        for (auto &child : children) child->Init();
        built_ = false;
        emit_pos_ = 0;
        probe_done_ = false;
        probe_chunk_pos_ = 0;
        match_pos_ = 0;
        current_match_list_ = nullptr;
        current_probe_row_ = 0;
    }

    bool GetData(DataChunk &result) override {
        if (!built_) {
            BuildHashTable();
            built_ = true;
        }

        // CROSS join uses the legacy result_rows_ path.
        if (join_type_ == JoinType::CROSS) {
            if (emit_pos_ >= result_rows_.size()) return false;
            result.Initialize(GetTypes());
            idx_t count = 0;
            while (emit_pos_ < result_rows_.size() && count < VECTOR_SIZE) {
                auto &row = result_rows_[emit_pos_];
                for (idx_t col = 0; col < row.size(); col++) result.SetValue(col, count, row[col]);
                emit_pos_++;
                count++;
            }
            result.SetCardinality(count);
            return count > 0;
        }

        // Streaming probe: produce one chunk at a time.
        // Only initialize if chunk isn't already set up (reuse across calls).
        if (result.ColumnCount() != GetTypes().size()) {
            result.Initialize(GetTypes());
        } else {
            result.Reset();
        }
        idx_t out = 0;

        auto emit = [&](idx_t build_idx) {
            // Compound-ON: re-check residual conjuncts; skip non-matching
            // pairs without marking the build row matched.
            if (!residual_preds_.empty() && !ResidualPasses(build_idx)) {
                return;
            }
            current_probe_matched_ = true;
            idx_t col = 0;
            auto set_if_needed = [&](const Value &v) {
                if (needed_cols_.empty() || (col < needed_cols_.size() && needed_cols_[col])) {
                    result.SetValue(col, out, v);
                }
                col++;
            };
            if (build_is_right_) {
                for (idx_t c = 0; c < probe_chunk_.ColumnCount(); c++) {
                    if (needed_cols_.empty() || (col < needed_cols_.size() && needed_cols_[col])) {
                        result.SetValue(col, out, probe_chunk_.GetValue(c, current_probe_row_));
                    }
                    col++;
                }
                for (auto &v : build_rows_[build_idx]) set_if_needed(v);
            } else {
                for (auto &v : build_rows_[build_idx]) set_if_needed(v);
                for (idx_t c = 0; c < probe_chunk_.ColumnCount(); c++) {
                    if (needed_cols_.empty() || (col < needed_cols_.size() && needed_cols_[col])) {
                        result.SetValue(col, out, probe_chunk_.GetValue(c, current_probe_row_));
                    }
                    col++;
                }
            }
            out++;
            build_matched_[build_idx] = true;
        };

        while (out < VECTOR_SIZE) {
            // Emit remaining matches from current probe row.
            if (current_match_list_) {
                while (out < VECTOR_SIZE && match_pos_ < current_match_list_->size()) {
                    emit((*current_match_list_)[match_pos_]);
                    match_pos_++;
                }
                if (match_pos_ >= current_match_list_->size()) {
                    // Compound-ON: hash key matched but every candidate
                    // failed the residual. For a probe-side outer join,
                    // the probe row still needs its NULL-padded output.
                    if (!residual_preds_.empty() && !current_probe_matched_ &&
                        out < VECTOR_SIZE) {
                        bool want = false;
                        if (!build_is_right_ &&
                            (join_type_ == JoinType::RIGHT || join_type_ == JoinType::FULL)) want = true;
                        if (build_is_right_ &&
                            (join_type_ == JoinType::LEFT || join_type_ == JoinType::FULL)) want = true;
                        if (want) {
                            idx_t col = 0;
                            if (build_is_right_) {
                                for (idx_t c = 0; c < probe_chunk_.ColumnCount(); c++)
                                    result.SetValue(col++, out, probe_chunk_.GetValue(c, current_probe_row_));
                                for (idx_t c = 0; c < right_col_count_; c++)
                                    result.SetValue(col++, out, Value());
                            } else {
                                for (idx_t c = 0; c < left_col_count_; c++)
                                    result.SetValue(col++, out, Value());
                                for (idx_t c = 0; c < probe_chunk_.ColumnCount(); c++)
                                    result.SetValue(col++, out, probe_chunk_.GetValue(c, current_probe_row_));
                            }
                            out++;
                        }
                    }
                    current_match_list_ = nullptr;
                    probe_chunk_pos_++;
                    match_pos_ = 0;
                }
                if (out >= VECTOR_SIZE) break;
            }

            // Process next probe row in current chunk.
            if (probe_chunk_pos_ < probe_chunk_.size()) {
                current_probe_row_ = probe_chunk_pos_;
                auto &key_vec = probe_chunk_.GetVector(probe_join_col_);
                std::vector<idx_t> *match_list = nullptr;

                if (nested_loop_) {
                    match_list = &all_build_rows_;
                } else if (use_i64_hash_) {
                    // Typed int path - read directly from the Vector data.
                    // NULL-in-probe-key never matches in equi-join semantics.
                    const bool null_probe = !key_vec.GetValidity().RowIsValid(current_probe_row_);
                    if (!null_probe) {
                        int64_t k = 0;
                        switch (key_vec.GetType().id()) {
                        case LogicalTypeId::TINYINT:
                            k = reinterpret_cast<const int8_t *>(key_vec.GetData())[current_probe_row_];
                            break;
                        case LogicalTypeId::SMALLINT:
                            k = reinterpret_cast<const int16_t *>(key_vec.GetData())[current_probe_row_];
                            break;
                        case LogicalTypeId::INTEGER:
                            k = reinterpret_cast<const int32_t *>(key_vec.GetData())[current_probe_row_];
                            break;
                        case LogicalTypeId::BIGINT:
                        case LogicalTypeId::HUGEINT:
                            k = reinterpret_cast<const int64_t *>(key_vec.GetData())[current_probe_row_];
                            break;
                        default:
                            k = probe_chunk_.GetValue(probe_join_col_, current_probe_row_).GetValue<int64_t>();
                        }
                        if (use_linear_cache_) {
                            for (idx_t ki = 0; ki < build_key_cache_i64_.size(); ki++) {
                                if (build_key_cache_i64_[ki] == k) {
                                    match_list = build_match_cache_[ki];
                                    break;
                                }
                            }
                        } else {
                            auto it = hash_table_i64_.find(k);
                            if (it != hash_table_i64_.end()) match_list = &it->second;
                        }
                    }
                    // match_list stays nullptr when null_probe - falls through
                    // to the usual "no match" logic below.
                } else if (!key_vec.GetValidity().RowIsValid(current_probe_row_)) {
                    // A NULL probe key matches nothing.
                } else if (use_linear_cache_ && key_vec.GetType().id() == LogicalTypeId::VARCHAR) {
                    // Ultra-fast: direct bytes from string_t, linear memcmp.
                    auto &s = reinterpret_cast<const string_t *>(key_vec.GetData())[current_probe_row_];
                    const char *d = s.GetData();
                    uint32_t l = s.GetSize();
                    for (idx_t ki = 0; ki < build_key_cache_.size(); ki++) {
                        const auto &k = build_key_cache_[ki];
                        if (k.size() == l && memcmp(k.data(), d, l) == 0) {
                            match_list = build_match_cache_[ki];
                            break;
                        }
                    }
                } else {
                    std::string key;
                    if (key_vec.GetType().id() == LogicalTypeId::VARCHAR) {
                        auto &s = reinterpret_cast<const string_t *>(key_vec.GetData())[current_probe_row_];
                        key.assign(s.GetData(), s.GetSize());
                    } else {
                        key = JoinKey(probe_chunk_.GetValue(probe_join_col_, current_probe_row_));
                    }
                    auto it = hash_table_.find(key);
                    if (it != hash_table_.end()) match_list = &it->second;
                }

                if (match_list && !match_list->empty()) {
                    current_match_list_ = match_list;
                    match_pos_ = 0;
                    current_probe_matched_ = false; // reset for compound-ON tracking
                } else {
                    // Outer join: emit probe row with NULLs for build side.
                    bool want = false;
                    if (!build_is_right_ && (join_type_ == JoinType::RIGHT || join_type_ == JoinType::FULL)) want = true;
                    if (build_is_right_ && (join_type_ == JoinType::LEFT || join_type_ == JoinType::FULL)) want = true;
                    if (want) {
                        idx_t col = 0;
                        if (build_is_right_) {
                            for (idx_t c = 0; c < probe_chunk_.ColumnCount(); c++)
                                result.SetValue(col++, out, probe_chunk_.GetValue(c, current_probe_row_));
                            for (idx_t c = 0; c < right_col_count_; c++) result.SetValue(col++, out, Value());
                        } else {
                            for (idx_t c = 0; c < left_col_count_; c++) result.SetValue(col++, out, Value());
                            for (idx_t c = 0; c < probe_chunk_.ColumnCount(); c++)
                                result.SetValue(col++, out, probe_chunk_.GetValue(c, current_probe_row_));
                        }
                        out++;
                    }
                    probe_chunk_pos_++;
                }
                continue;
            }

            // Current chunk exhausted - fetch next.
            if (probe_done_) break;
            probe_chunk_.Initialize(probe_child_->GetTypes());
            if (!probe_child_->GetData(probe_chunk_)) {
                probe_done_ = true;
                break;
            }
            probe_chunk_pos_ = 0;
        }

        // After probe is fully done, emit unmatched build rows for outer joins.
        if (probe_done_ && out < VECTOR_SIZE) {
            bool want = false;
            if (!build_is_right_ && (join_type_ == JoinType::LEFT || join_type_ == JoinType::FULL)) want = true;
            if (build_is_right_ && (join_type_ == JoinType::RIGHT || join_type_ == JoinType::FULL)) want = true;
            if (want) {
                while (out < VECTOR_SIZE && outer_emit_pos_ < build_rows_.size()) {
                    if (!build_matched_[outer_emit_pos_]) {
                        idx_t col = 0;
                        if (build_is_right_) {
                            for (idx_t c = 0; c < left_col_count_; c++) result.SetValue(col++, out, Value());
                            for (auto &v : build_rows_[outer_emit_pos_]) result.SetValue(col++, out, v);
                        } else {
                            for (auto &v : build_rows_[outer_emit_pos_]) result.SetValue(col++, out, v);
                            for (idx_t c = 0; c < right_col_count_; c++) result.SetValue(col++, out, Value());
                        }
                        out++;
                    }
                    outer_emit_pos_++;
                }
            }
        }

        result.SetCardinality(out);
        return out > 0;
    }

    // --- Fuse interface: used by PhysicalHashAggregate to bypass materialization. ---
    void FuseBuild() { if (!built_) { BuildHashTable(); built_ = true; } }
    // The fused path probes the hash table alone: only for an inner join
    // whose whole condition is one column = column equality.
    bool FuseIsInner() const { return join_type_ == JoinType::INNER && IsSingleEquiCondition(); }
    bool FuseBuildIsRight() const { return build_is_right_; }
    PhysicalOperator *FuseProbeChild() { return probe_child_; }
    const std::vector<std::vector<Value>> &FuseBuildRows() const { return build_rows_; }
    const std::unordered_map<std::string, std::vector<idx_t>> &FuseHashTable() const { return hash_table_; }
    bool FuseUseI64Hash() const { return use_i64_hash_; }
    // The key of a value in hash_table_ (float_keys_).
    std::string JoinKey(const Value &v) const {
        // A NULL matches nothing: no build key is one byte long.
        if (v.IsNull()) return std::string(1, '\0');
        if (!float_keys_) return v.ToString();
        std::string key;
        AppendKeyRaw(key, CanonicalKey(v.GetValue<double>()));
        return key;
    }
    const std::unordered_map<int64_t, std::vector<idx_t>> &FuseHashTableI64() const { return hash_table_i64_; }
    const std::vector<int64_t> &FuseBuildKeyCacheI64() const { return build_key_cache_i64_; }
    bool FuseUseLinearCache() const { return use_linear_cache_; }
    const std::vector<std::string> &FuseBuildKeyCache() const { return build_key_cache_; }
    const std::vector<std::vector<idx_t>*> &FuseBuildMatchCache() const { return build_match_cache_; }
    idx_t FuseProbeJoinCol() const { return probe_join_col_; }
    idx_t FuseLeftColCount() const { return left_col_count_; }

private:
    void BuildHashTable() {
        DataChunk chunk;

        // Push projection down to BOTH children before materializing. Without
        // this, the build side parses every column even if the consumer only
        // wants one or two - wasteful for wide tables. We derive the join
        // columns from the AST and union them with needed_cols_ (if any).
        if (!needed_cols_.empty() && IsSingleEquiCondition()) {
            auto &cmp = static_cast<BoundComparison &>(*condition_);
            idx_t left_key = 0, right_key = 0;
            // Classify each side of the equi-join as LEFT- or RIGHT-table
            // relative based on its combined column index. A user-written
            // `b.k = s.k` may put the right-table column first (cmp.left),
            // so we can't assume cmp.left == LEFT table.
            auto classify = [&](BoundExpression *e, idx_t &local) -> int {
                if (!e || e->GetExpressionType() != BoundExpressionType::COLUMN_REF) return -1;
                idx_t combined = static_cast<BoundColumnRef &>(*e).column_index;
                if (combined >= left_col_count_) { local = combined - left_col_count_; return 1; }
                local = combined; return 0;
            };
            idx_t la = 0, ra = 0;
            int ls = classify(cmp.left.get(), la);
            int rs = classify(cmp.right.get(), ra);
            if (ls == 0 && rs == 1)      { left_key = la; right_key = ra; }
            else if (ls == 1 && rs == 0) { left_key = ra; right_key = la; }

            // Left child: keep the join column + any left-side output columns needed.
            if (!children.empty() && children[0]) {
                idx_t left_n = children[0]->GetTypes().size();
                std::vector<bool> left_need(left_n, false);
                if (left_key < left_n) left_need[left_key] = true;
                for (idx_t i = 0; i < left_col_count_ && i < needed_cols_.size(); i++) {
                    if (needed_cols_[i] && i < left_n) left_need[i] = true;
                }
                children[0]->SetNeededOutputs(left_need);
            }
            // Right child: keep the join column + any right-side output columns.
            if (children.size() > 1 && children[1]) {
                idx_t right_n = children[1]->GetTypes().size();
                std::vector<bool> right_need(right_n, false);
                if (right_key < right_n) right_need[right_key] = true;
                for (idx_t out_i = left_col_count_; out_i < needed_cols_.size(); out_i++) {
                    if (needed_cols_[out_i]) {
                        idx_t right_i = out_i - left_col_count_;
                        if (right_i < right_n) right_need[right_i] = true;
                    }
                }
                children[1]->SetNeededOutputs(right_need);
            }
        }

        // CROSS JOIN: materialize both sides, emit all pairs into result_rows_ (legacy path).
        if (join_type_ == JoinType::CROSS) {
            std::vector<std::vector<Value>> l_rows, r_rows;
            while (true) {
                chunk.Initialize(children[0]->GetTypes());
                if (!children[0]->GetData(chunk)) break;
                for (idx_t i = 0; i < chunk.size(); i++) {
                    std::vector<Value> row;
                    for (idx_t c = 0; c < chunk.ColumnCount(); c++)
                        row.push_back(chunk.GetValue(c, i));
                    l_rows.push_back(std::move(row));
                }
            }
            while (true) {
                chunk.Initialize(children[1]->GetTypes());
                if (!children[1]->GetData(chunk)) break;
                for (idx_t i = 0; i < chunk.size(); i++) {
                    std::vector<Value> row;
                    for (idx_t c = 0; c < chunk.ColumnCount(); c++)
                        row.push_back(chunk.GetValue(c, i));
                    r_rows.push_back(std::move(row));
                }
            }
            for (auto &lr : l_rows) {
                for (auto &rr : r_rows) {
                    std::vector<Value> combined;
                    for (auto &v : lr) combined.push_back(v);
                    for (auto &v : rr) combined.push_back(v);
                    result_rows_.push_back(std::move(combined));
                }
            }
            return;
        }

        // Pick the build side: materialise LEFT with a size guard, and if
        // it exceeds the threshold, bail and use RIGHT instead.
        static constexpr idx_t SMALL_SIDE_THRESHOLD = 100000;
        std::vector<std::vector<Value>> build_rows;
        bool build_is_right = false;
        // Try to materialize LEFT side with a size guard.
        // If LEFT exceeds threshold, bail and use RIGHT as build side instead.
        std::vector<std::vector<Value>> left_rows;
        bool left_too_big = false;
        while (!left_too_big) {
            chunk.Initialize(children[0]->GetTypes());
            if (!children[0]->GetData(chunk)) break;
            for (idx_t i = 0; i < chunk.size(); i++) {
                std::vector<Value> row;
                for (idx_t col = 0; col < chunk.ColumnCount(); col++)
                    row.push_back(chunk.GetValue(col, i));
                left_rows.push_back(std::move(row));
                if (left_rows.size() > SMALL_SIDE_THRESHOLD) {
                    left_too_big = true;
                    break;
                }
            }
        }

        if (left_too_big) {
            children[0]->Init(); // reset left scan for the probe phase
            build_is_right = true;
            while (true) {
                chunk.Initialize(children[1]->GetTypes());
                if (!children[1]->GetData(chunk)) break;
                for (idx_t i = 0; i < chunk.size(); i++) {
                    std::vector<Value> row;
                    for (idx_t col = 0; col < chunk.ColumnCount(); col++)
                        row.push_back(chunk.GetValue(col, i));
                    build_rows.push_back(std::move(row));
                }
            }
            left_rows.clear();
        } else {
            build_rows = std::move(left_rows);
        }

        // Extract join column indices from condition. See the note in the
        // projection-pushdown block above: the predicate may be written with
        // the right table's column first (e.g. `ON b.k = s.k`), so each side
        // of the comparison is classified by its combined column index.
        idx_t left_join_col = 0, right_join_col = 0;
        residual_preds_.clear();
        nested_loop_ = false;
        auto classify = [&](BoundExpression *e, idx_t &local) -> int {
            if (!e || e->GetExpressionType() != BoundExpressionType::COLUMN_REF) return -1;
            idx_t combined = static_cast<BoundColumnRef &>(*e).column_index;
            if (combined >= left_col_count_) { local = combined - left_col_count_; return 1; }
            local = combined; return 0;
        };
        // Try to use a single COLUMN_REF=COLUMN_REF comparison as the hash
        // key. Returns true if the comparison was a usable equi-key.
        auto try_equi_key = [&](BoundExpression *cond) -> bool {
            if (!cond || cond->GetExpressionType() != BoundExpressionType::COMPARISON)
                return false;
            auto &cmp = static_cast<BoundComparison &>(*cond);
            if (cmp.op != "=") return false;
            idx_t la = 0, ra = 0;
            int ls = classify(cmp.left.get(), la);
            int rs = classify(cmp.right.get(), ra);
            if (ls == 0 && rs == 1) { left_join_col = la; right_join_col = ra; return true; }
            if (ls == 1 && rs == 0) { left_join_col = ra; right_join_col = la; return true; }
            return false;
        };
        if (condition_ && condition_->GetExpressionType() == BoundExpressionType::COMPARISON) {
            try_equi_key(condition_.get());
        } else if (condition_ &&
                   condition_->GetExpressionType() == BoundExpressionType::CONJUNCTION) {
            // Compound ON (`a.x=b.x AND a.y=b.y AND ...`). Flatten the
            // AND-tree, use the first equi-comparison as the hash key,
            // and keep every other conjunct as a residual predicate to
            // re-check per matched pair. Previously every conjunct after
            // the first was silently dropped — multi-condition joins
            // returned wrong rows.
            std::vector<BoundExpression *> conjs;
            std::function<void(BoundExpression *)> flatten = [&](BoundExpression *e) {
                if (!e) return;
                if (e->GetExpressionType() == BoundExpressionType::CONJUNCTION) {
                    auto &cj = static_cast<BoundConjunction &>(*e);
                    if (cj.op == "AND") {
                        flatten(cj.left.get());
                        flatten(cj.right.get());
                        return;
                    }
                }
                conjs.push_back(e);
            };
            flatten(condition_.get());
            bool key_set = false;
            for (auto *c : conjs) {
                if (!key_set && try_equi_key(c)) { key_set = true; continue; }
                residual_preds_.push_back(c);
            }
            nested_loop_ = !key_set;
        } else {
            nested_loop_ = true;
        }
        if (condition_ && condition_->GetExpressionType() == BoundExpressionType::COMPARISON &&
            !IsSingleEquiCondition()) {
            nested_loop_ = true;
        }
        if (nested_loop_) {
            // No equality between a column of each side (`ON i < k`, `ON
            // TRUE`, `ON i = 1`, `ON a OR b`): every build row is a
            // candidate for every probe row, and the whole condition is
            // checked per pair. Before, the hash key defaulted to column 0
            // of each side and the condition was dropped (Stage 8, #12).
            residual_preds_.clear();
            if (condition_) {
                std::function<void(BoundExpression *)> flatten_all = [&](BoundExpression *e) {
                    if (e->GetExpressionType() == BoundExpressionType::CONJUNCTION) {
                        auto &cj = static_cast<BoundConjunction &>(*e);
                        if (cj.op == "AND" && cj.left && cj.right) {
                            flatten_all(cj.left.get());
                            flatten_all(cj.right.get());
                            return;
                        }
                    }
                    residual_preds_.push_back(e);
                };
                flatten_all(condition_.get());
            }
        }
        // Combined [left][right] schema for residual evaluation.
        if (!residual_preds_.empty()) {
            combined_types_.clear();
            for (auto &t : children[0]->GetTypes()) combined_types_.push_back(t);
            for (auto &t : children[1]->GetTypes()) combined_types_.push_back(t);
        }

        // Store state for streaming probe.
        build_rows_ = std::move(build_rows);
        build_is_right_ = build_is_right;
        probe_join_col_ = build_is_right ? left_join_col : right_join_col;
        probe_child_ = build_is_right ? children[0].get() : children[1].get();
        build_matched_.assign(build_rows_.size(), false);
        hash_table_.clear();
        hash_table_i64_.clear();
        build_key_cache_.clear();
        build_match_cache_.clear();
        build_key_cache_i64_.clear();
        idx_t build_join_col = build_is_right ? right_join_col : left_join_col;

        // Detect integer-typed join: look at the first build row's value type AND
        // the probe-side column type. If both are integer-family (TINYINT /
        // SMALLINT / INTEGER / BIGINT / HUGEINT treated as signed 64-bit),
        // use the typed hash path.
        auto is_int_tid = [](LogicalTypeId t) {
            return t == LogicalTypeId::TINYINT || t == LogicalTypeId::SMALLINT ||
                   t == LogicalTypeId::INTEGER || t == LogicalTypeId::BIGINT ||
                   t == LogicalTypeId::HUGEINT;
        };
        LogicalTypeId build_key_type = LogicalTypeId::INVALID;
        if (!build_rows_.empty()) {
            build_key_type = build_rows_[0][build_join_col].type().id();
        }
        probe_key_type_ = LogicalTypeId::INVALID;
        if (probe_child_ && probe_join_col_ < probe_child_->GetTypes().size()) {
            probe_key_type_ = probe_child_->GetTypes()[probe_join_col_].id();
        }
        use_i64_hash_ = is_int_tid(build_key_type) && is_int_tid(probe_key_type_);
        auto is_float_tid = [](LogicalTypeId t) {
            return t == LogicalTypeId::FLOAT || t == LogicalTypeId::DOUBLE;
        };
        auto is_num_tid = [&](LogicalTypeId t) { return is_int_tid(t) || is_float_tid(t); };
        float_keys_ = (is_float_tid(build_key_type) || is_float_tid(probe_key_type_)) &&
                      (build_key_type == LogicalTypeId::INVALID || is_num_tid(build_key_type)) &&
                      is_num_tid(probe_key_type_);

        if (nested_loop_) {
            use_i64_hash_ = false;
            all_build_rows_.resize(build_rows_.size());
            for (idx_t i = 0; i < build_rows_.size(); i++) all_build_rows_[i] = i;
        } else if (use_i64_hash_) {
            // Typed int path - no string allocation per key. Note the build
            // side stores Values (union over all int widths); we have to
            // coerce on the stored type, not blindly call GetValue<int64_t>
            // (that slot is only populated for BIGINT).
            auto coerce_to_i64 = [](const Value &v) -> int64_t {
                switch (v.type().id()) {
                case LogicalTypeId::TINYINT:  return v.GetValue<int8_t>();
                case LogicalTypeId::SMALLINT: return v.GetValue<int16_t>();
                case LogicalTypeId::INTEGER:  return v.GetValue<int32_t>();
                case LogicalTypeId::BIGINT:   return v.GetValue<int64_t>();
                case LogicalTypeId::HUGEINT: {
                    auto h = v.GetValue<hugeint_t>();
                    return static_cast<int64_t>(h.lower);
                }
                default: return 0;
                }
            };
            for (idx_t i = 0; i < build_rows_.size(); i++) {
                if (build_rows_[i][build_join_col].IsNull()) continue; // NULL never matches in equi-join
                int64_t k = coerce_to_i64(build_rows_[i][build_join_col]);
                hash_table_i64_[k].push_back(i);
            }
            // Reserve the bucket count so probe map lookups don't rehash.
            hash_table_i64_.reserve(build_rows_.size() * 2);
        } else {
            for (idx_t i = 0; i < build_rows_.size(); i++) {
                // NULL never matches in an equi-join (`ToString` of NULL is
                // "NULL", which would match the string 'NULL').
                if (build_rows_[i][build_join_col].IsNull()) continue;
                auto key = JoinKey(build_rows_[i][build_join_col]);
                hash_table_[key].push_back(i);
            }
        }

        // Build a linear cache for fast low-cardinality probe lookups - faster
        // than a hash map for dimension tables with <256 unique keys. Applies
        // to both the int64 and string paths.
        use_linear_cache_ = !nested_loop_ && build_rows_.size() <= 256;
        if (use_linear_cache_) {
            if (use_i64_hash_) {
                for (auto &kv : hash_table_i64_) {
                    build_key_cache_i64_.push_back(kv.first);
                    build_match_cache_.push_back(&kv.second);
                }
            } else {
                for (auto &kv : hash_table_) {
                    build_key_cache_.push_back(kv.first);
                    build_match_cache_.push_back(&kv.second);
                }
            }
        }

        // Push projection down to probe child: only the join column + columns
        // that appear in the output and are needed by the caller.
        if (!needed_cols_.empty() && !nested_loop_ && residual_preds_.empty()) {
            idx_t probe_col_count = probe_child_->GetTypes().size();
            std::vector<bool> probe_needed(probe_col_count, false);
            probe_needed[probe_join_col_] = true;
            // Map each output column -> probe column if applicable.
            for (idx_t out_col = 0; out_col < needed_cols_.size() && needed_cols_[out_col]; out_col++) {}
            for (idx_t out_col = 0; out_col < needed_cols_.size(); out_col++) {
                if (!needed_cols_[out_col]) continue;
                idx_t probe_col = INVALID_INDEX;
                if (build_is_right_) {
                    // output = [probe_cols..., build_cols...]
                    if (out_col < probe_col_count) probe_col = out_col;
                } else {
                    // output = [build_cols..., probe_cols...]
                    if (out_col >= left_col_count_) probe_col = out_col - left_col_count_;
                }
                if (probe_col != INVALID_INDEX && probe_col < probe_col_count) {
                    probe_needed[probe_col] = true;
                }
            }
            probe_child_->SetNeededOutputs(probe_needed);
        }
    }

private:
    // Whether the condition is exactly one `=` between a column of the left
    // side and a column of the right side.
    bool IsSingleEquiCondition() const {
        if (!condition_ || condition_->GetExpressionType() != BoundExpressionType::COMPARISON) return false;
        auto &cmp = static_cast<const BoundComparison &>(*condition_);
        if (cmp.op != "=") return false;
        auto side = [&](const BoundExpression *e) -> int {
            if (!e || e->GetExpressionType() != BoundExpressionType::COLUMN_REF) return -1;
            return static_cast<const BoundColumnRef &>(*e).column_index >= left_col_count_ ? 1 : 0;
        };
        int ls = side(cmp.left.get()), rs = side(cmp.right.get());
        return (ls == 0 && rs == 1) || (ls == 1 && rs == 0);
    }

    // Member state for streaming probe.
    bool nested_loop_ = false;
    std::vector<idx_t> all_build_rows_;
    bool build_is_right_ = false;
    std::vector<std::vector<Value>> build_rows_;
    std::unordered_map<std::string, std::vector<idx_t>> hash_table_;
    // Typed int64 hash table - used when BOTH join columns are integer types.
    // Skips the Value::ToString() allocation per key (build + probe) and the
    // string-hash work. ~30-50% faster for integer joins on ≥100k rows.
    std::unordered_map<int64_t, std::vector<idx_t>> hash_table_i64_;
    bool use_i64_hash_ = false;
    LogicalTypeId probe_key_type_ = LogicalTypeId::INVALID;
    // A FLOAT or DOUBLE key on either side: keys are the value as a double
    // in canonical bits (JoinKey), so every NaN joins every NaN and -0.0
    // joins 0.0, as DuckDB's (#42). Their text kept "nan" from "-nan" and
    // "-0.0" from "0.0", and 1 from 1.0.
    bool float_keys_ = false;
    // Linear cache for low-cardinality probes - avoids map/string hash overhead.
    bool use_linear_cache_ = false;
    std::vector<std::string> build_key_cache_;
    std::vector<std::vector<idx_t>*> build_match_cache_;
    // Parallel int64 linear cache - same idea, typed.
    std::vector<int64_t> build_key_cache_i64_;
    std::vector<bool> build_matched_;
    idx_t probe_join_col_ = 0;
    PhysicalOperator *probe_child_ = nullptr;
    DataChunk probe_chunk_;
    idx_t probe_chunk_pos_ = 0;
    idx_t current_probe_row_ = 0;
    const std::vector<idx_t> *current_match_list_ = nullptr;
    idx_t match_pos_ = 0;
    bool probe_done_ = false;
    idx_t outer_emit_pos_ = 0;

    JoinType join_type_;
    BoundExprPtr condition_;
    idx_t left_col_count_;
    idx_t right_col_count_;
    std::vector<std::vector<Value>> result_rows_; // only for CROSS
    bool built_ = false;
    idx_t emit_pos_ = 0;
    std::vector<bool> needed_cols_;
    // Compound-ON residual predicates (every AND-conjunct that isn't the
    // hash equi-key). Non-owning pointers into condition_'s tree.
    std::vector<BoundExpression *> residual_preds_;
    std::vector<LogicalType> combined_types_;   // [left][right] schema
    bool current_probe_matched_ = false;        // any residual-passing match this probe row

    // Evaluate the residual predicates against the combined [left][right]
    // row formed from build_rows_[build_idx] and the current probe row.
    // Returns true when all residuals hold (or none exist).
    bool ResidualPasses(idx_t build_idx) {
        if (residual_preds_.empty()) return true;
        DataChunk row;
        row.Initialize(combined_types_);
        row.SetCardinality(1);
        // Combined canonical order is always [left cols][right cols].
        const std::vector<Value> *left_row;
        // probe row values, in probe-schema order.
        auto probe_val = [&](idx_t c) {
            return probe_chunk_.GetValue(c, current_probe_row_);
        };
        if (build_is_right_) {
            // build = right, probe = left.
            for (idx_t c = 0; c < left_col_count_; c++) row.SetValue(c, 0, probe_val(c));
            auto &br = build_rows_[build_idx];
            for (idx_t c = 0; c < br.size(); c++) row.SetValue(left_col_count_ + c, 0, br[c]);
        } else {
            // build = left, probe = right.
            auto &br = build_rows_[build_idx];
            for (idx_t c = 0; c < br.size(); c++) row.SetValue(c, 0, br[c]);
            for (idx_t c = 0; c < right_col_count_; c++)
                row.SetValue(left_col_count_ + c, 0, probe_val(c));
        }
        (void)left_row;
        for (auto *pred : residual_preds_) {
            Vector res(LogicalType::BOOLEAN(), 1);
            ExpressionExecutor::Execute(*pred, row, res, 1);
            auto v = res.GetValue(0);
            if (v.IsNull() || !v.GetValue<bool>()) return false;
        }
        return true;
    }
};

static PhysicalHashJoin *AsHashJoin(PhysicalOperator *op) {
    return dynamic_cast<PhysicalHashJoin *>(op);
}

// Fused JOIN+aggregate hot path - streams probe chunks, looks up build side,
// updates aggregate state directly. Cuts 2-4x off large-fact × small-dim patterns
// (e.g. `sales JOIN regions GROUP BY manager`).
bool PhysicalHashAggregate::TryComputeFusedJoinAggregate(
    PhysicalHashJoin *hj,
    const std::vector<AggInfo> &agg_infos,
    const std::vector<idx_t> &group_col_indices) {
    if (!hj->FuseIsInner()) return false;
    idx_t num_aggs = agg_infos.size();
    const auto &join_types = hj->GetTypes();
    for (auto &info : agg_infos) {
        if (info.name == "COUNT" && info.is_count_star) continue;
        if (info.name == "COUNT" && info.col_idx != INVALID_INDEX && !info.is_distinct) continue;
        // SUM, AVG, MIN and MAX of an INTEGER or a BIGINT only. A FLOAT,
        // SMALLINT or TINYINT argument on the probe side was read as 0,
        // and a DOUBLE's MIN and MAX were IEEE's; the generic path below
        // computes them, NaN as DuckDB orders it (#42).
        if ((info.name == "SUM" || info.name == "AVG" ||
             info.name == "MIN" || info.name == "MAX") &&
            info.col_idx != INVALID_INDEX && !info.is_distinct &&
            info.col_idx < join_types.size() &&
            (join_types[info.col_idx].id() == LogicalTypeId::INTEGER ||
             join_types[info.col_idx].id() == LogicalTypeId::BIGINT)) continue;
        return false;
    }

    hj->FuseBuild();
    auto *probe = hj->FuseProbeChild();
    const auto &build_rows = hj->FuseBuildRows();
    const auto &hash_tab = hj->FuseHashTable();
    const auto &hash_tab_i64 = hj->FuseHashTableI64();
    bool use_i64 = hj->FuseUseI64Hash();
    bool build_is_right = hj->FuseBuildIsRight();
    idx_t probe_join_col = hj->FuseProbeJoinCol();
    idx_t left_cols = hj->FuseLeftColCount();
    idx_t probe_cols = probe->GetTypes().size();
    bool linear = hj->FuseUseLinearCache();
    const auto &key_cache = hj->FuseBuildKeyCache();
    const auto &key_cache_i64 = hj->FuseBuildKeyCacheI64();
    const auto &match_cache = hj->FuseBuildMatchCache();

    auto map_col = [&](idx_t combined) -> std::pair<bool, idx_t> {
        if (build_is_right) {
            if (combined < probe_cols) return {false, combined};
            return {true, combined - probe_cols};
        } else {
            if (combined < left_cols) return {true, combined};
            return {false, combined - left_cols};
        }
    };

    // Push projection to the probe scan - only parse cols we actually read.
    {
        std::vector<bool> probe_needed(probe_cols, false);
        probe_needed[probe_join_col] = true;
        for (idx_t gc : group_col_indices) {
            auto pr = map_col(gc);
            if (!pr.first && pr.second < probe_cols) probe_needed[pr.second] = true;
        }
        for (auto &info : agg_infos) {
            if (info.col_idx == INVALID_INDEX) continue;
            auto pr = map_col(info.col_idx);
            if (!pr.first && pr.second < probe_cols) probe_needed[pr.second] = true;
        }
        probe->SetNeededOutputs(probe_needed);
    }

    struct GKey { bool is_build; idx_t local; LogicalTypeId tid; };
    std::vector<GKey> gkeys;
    gkeys.reserve(group_col_indices.size());
    for (idx_t gc : group_col_indices) {
        auto pr = map_col(gc);
        LogicalTypeId tid = LogicalTypeId::SQLNULL;
        if (pr.first) {
            if (!build_rows.empty() && pr.second < build_rows[0].size())
                tid = build_rows[0][pr.second].type().id();
        } else {
            if (pr.second < probe->GetTypes().size())
                tid = probe->GetTypes()[pr.second].id();
        }
        gkeys.push_back({pr.first, pr.second, tid});
    }

    struct ACol { bool is_build; idx_t local; };
    std::vector<ACol> acols(num_aggs);
    for (idx_t a = 0; a < num_aggs; a++) {
        if (agg_infos[a].col_idx == INVALID_INDEX) { acols[a] = {false, INVALID_INDEX}; continue; }
        auto pr = map_col(agg_infos[a].col_idx);
        acols[a] = {pr.first, pr.second};
    }

    // Precompute per-build-row group key if all group cols are build-side.
    bool all_build_gkeys = true;
    for (auto &g : gkeys) if (!g.is_build) { all_build_gkeys = false; break; }

    // Fast path: when GROUP BY is all build-side, map each build_idx -> group_idx
    // directly (array index instead of hashing a string per probe row).
    std::vector<idx_t> build_to_gidx;
    std::vector<std::vector<Value>> group_vals_by_gidx;
    std::vector<std::vector<AggState>> states_by_gidx;
    if (all_build_gkeys) {
        std::unordered_map<std::string, idx_t> gkey_to_gidx;
        build_to_gidx.assign(build_rows.size(), INVALID_INDEX);
        // No-GROUP-BY queries (e.g. `SELECT COUNT(*) ... JOIN ...`) need the
        // aggregate to emit exactly one row even with zero matches. Pre-
        // seed a single group so COUNT-of-empty-join returns 0 rather than
        // no rows.
        if (gkeys.empty()) {
            group_vals_by_gidx.push_back({});
            states_by_gidx.emplace_back(num_aggs);
            gkey_to_gidx.emplace(std::string(), static_cast<idx_t>(0));
        }
        for (idx_t i = 0; i < build_rows.size(); i++) {
            std::string gkey;
            std::vector<Value> gvals(gkeys.size());
            for (idx_t j = 0; j < gkeys.size(); j++) {
                auto &v = build_rows[i][gkeys[j].local];
                gvals[j] = v;
                AppendKeyValue(gkey, v);
            }
            auto it = gkey_to_gidx.find(gkey);
            idx_t gidx;
            if (it == gkey_to_gidx.end()) {
                gidx = group_vals_by_gidx.size();
                group_vals_by_gidx.push_back(std::move(gvals));
                states_by_gidx.emplace_back(num_aggs);
                gkey_to_gidx.emplace(std::move(gkey), gidx);
            } else {
                gidx = it->second;
            }
            build_to_gidx[i] = gidx;
        }
    }

    std::unordered_map<std::string, std::vector<AggState>> group_states;
    std::unordered_map<std::string, std::vector<Value>> group_keys_map;
    std::vector<std::string> group_order;
    // The groups above are seeded from every build row, matched or not;
    // one no probe row joined is no group of the join's result. It was
    // emitted with COUNT(*) 0 (#38).
    std::vector<char> gidx_matched(states_by_gidx.size(), 0);

    DataChunk pchunk;
    while (probe->GetData(pchunk)) {
        idx_t cnt = pchunk.size();
        auto &key_vec = pchunk.GetVector(probe_join_col);
        auto key_tid = key_vec.GetType().id();
        for (idx_t i = 0; i < cnt; i++) {
            const std::vector<idx_t> *match_list = nullptr;
            if (use_i64) {
                if (!key_vec.GetValidity().RowIsValid(i)) {
                    // NULL probe key never matches.
                } else {
                    int64_t k = 0;
                    switch (key_tid) {
                    case LogicalTypeId::TINYINT:  k = reinterpret_cast<const int8_t *>(key_vec.GetData())[i]; break;
                    case LogicalTypeId::SMALLINT: k = reinterpret_cast<const int16_t *>(key_vec.GetData())[i]; break;
                    case LogicalTypeId::INTEGER:  k = reinterpret_cast<const int32_t *>(key_vec.GetData())[i]; break;
                    case LogicalTypeId::BIGINT:
                    case LogicalTypeId::HUGEINT:  k = reinterpret_cast<const int64_t *>(key_vec.GetData())[i]; break;
                    default: k = pchunk.GetValue(probe_join_col, i).GetValue<int64_t>();
                    }
                    if (linear) {
                        for (idx_t ki = 0; ki < key_cache_i64.size(); ki++) {
                            if (key_cache_i64[ki] == k) { match_list = match_cache[ki]; break; }
                        }
                    } else {
                        auto it = hash_tab_i64.find(k);
                        if (it != hash_tab_i64.end()) match_list = &it->second;
                    }
                }
            } else if (linear && key_tid == LogicalTypeId::VARCHAR) {
                auto &s = reinterpret_cast<const string_t *>(key_vec.GetData())[i];
                const char *d = s.GetData();
                uint32_t l = s.GetSize();
                for (idx_t ki = 0; ki < key_cache.size(); ki++) {
                    const auto &k = key_cache[ki];
                    if (k.size() == l && memcmp(k.data(), d, l) == 0) {
                        match_list = match_cache[ki];
                        break;
                    }
                }
            } else {
                std::string key_s;
                if (key_tid == LogicalTypeId::VARCHAR) {
                    auto &s = reinterpret_cast<const string_t *>(key_vec.GetData())[i];
                    key_s.assign(s.GetData(), s.GetSize());
                } else {
                    key_s = hj->JoinKey(pchunk.GetValue(probe_join_col, i));
                }
                auto it = hash_tab.find(key_s);
                if (it != hash_tab.end()) match_list = &it->second;
            }
            if (!match_list || match_list->empty()) continue;

            for (idx_t build_idx : *match_list) {
                auto &br = build_rows[build_idx];
                std::vector<AggState> *states_ptr;
                if (all_build_gkeys) {
                    // Direct array indexing - no string hashing in the hot loop.
                    idx_t gidx = build_to_gidx[build_idx];
                    gidx_matched[gidx] = 1;
                    states_ptr = &states_by_gidx[gidx];
                } else {
                    std::string gkey;
                    for (auto &g : gkeys) {
                        if (g.is_build) {
                            AppendKeyValue(gkey, br[g.local]);
                        } else {
                            AppendKeyValue(gkey, pchunk.GetValue(g.local, i));
                        }
                    }
                    auto git = group_states.find(gkey);
                    if (git == group_states.end()) {
                        std::vector<Value> gvals(gkeys.size());
                        for (idx_t j = 0; j < gkeys.size(); j++) {
                            if (gkeys[j].is_build) gvals[j] = br[gkeys[j].local];
                            else gvals[j] = pchunk.GetValue(gkeys[j].local, i);
                        }
                        group_keys_map[gkey] = std::move(gvals);
                        group_order.push_back(gkey);
                        git = group_states.emplace(gkey, std::vector<AggState>(num_aggs)).first;
                    }
                    states_ptr = &git->second;
                }
                auto &states = *states_ptr;

                for (idx_t a = 0; a < num_aggs; a++) {
                    auto &state = states[a];
                    auto &info = agg_infos[a];
                    if (info.name == "COUNT" && info.is_count_star) {
                        state.count++;
                        continue;
                    }
                    auto &ac = acols[a];
                    if (ac.local == INVALID_INDEX) continue;
                    if (ac.is_build) {
                        auto &v = br[ac.local];
                        if (v.IsNull()) continue;
                        if (info.name == "COUNT") { state.count++; continue; }
                        double d = 0;
                        auto tid = v.type().id();
                        if (tid == LogicalTypeId::INTEGER) d = v.GetValue<int32_t>();
                        else if (tid == LogicalTypeId::BIGINT) d = static_cast<double>(v.GetValue<int64_t>());
                        else if (tid == LogicalTypeId::DOUBLE) d = v.GetValue<double>();
                        if (info.name == "SUM" || info.name == "AVG") { state.count++; state.sum += d; }
                        else if (info.name == "MIN") { if (!state.has_min || v < state.min_val()) { state.min_val() = v; state.has_min = true; } }
                        else if (info.name == "MAX") { if (!state.has_max || v > state.max_val()) { state.max_val() = v; state.has_max = true; } }
                    } else {
                        auto &vec = pchunk.GetVector(ac.local);
                        if (!vec.GetValidity().RowIsValid(i)) continue;
                        auto tid = vec.GetType().id();
                        if (info.name == "COUNT") { state.count++; continue; }
                        double d = 0;
                        if (tid == LogicalTypeId::INTEGER)
                            d = reinterpret_cast<const int32_t *>(vec.GetData())[i];
                        else if (tid == LogicalTypeId::BIGINT)
                            d = static_cast<double>(reinterpret_cast<const int64_t *>(vec.GetData())[i]);
                        else if (tid == LogicalTypeId::DOUBLE)
                            d = reinterpret_cast<const double *>(vec.GetData())[i];
                        if (info.name == "SUM" || info.name == "AVG") { state.count++; state.sum += d; }
                        else if (info.name == "MIN") {
                            if (!state.has_min || d < state.sum_min) { state.sum_min = d; state.has_min = true; }
                        }
                        else if (info.name == "MAX") {
                            if (!state.has_max || d > state.sum_max) { state.sum_max = d; state.has_max = true; }
                        }
                    }
                }
            }
        }
    }

    // Normalize emit source: for the build-indexed fast path, feed
    // group_vals_by_gidx + states_by_gidx into the same emit loop.
    std::vector<std::vector<Value>> *emit_group_vals;
    std::vector<std::vector<AggState>> *emit_states;
    std::vector<std::vector<Value>> fallback_vals;
    std::vector<std::vector<AggState>> fallback_states;
    if (all_build_gkeys) {
        emit_group_vals = &group_vals_by_gidx;
        emit_states = &states_by_gidx;
    } else {
        fallback_vals.reserve(group_order.size());
        fallback_states.reserve(group_order.size());
        for (auto &gk : group_order) {
            fallback_vals.push_back(std::move(group_keys_map[gk]));
            fallback_states.push_back(std::move(group_states[gk]));
        }
        emit_group_vals = &fallback_vals;
        emit_states = &fallback_states;
    }
    for (idx_t gi = 0; gi < emit_group_vals->size(); gi++) {
        if (all_build_gkeys && !gkeys.empty() && !gidx_matched[gi]) continue;
        auto &states = (*emit_states)[gi];
        auto &gvals = (*emit_group_vals)[gi];
        std::vector<Value> row;
        row.reserve(gkeys.size() + num_aggs);
        for (auto &v : gvals) row.push_back(v);
        for (idx_t a = 0; a < num_aggs; a++) {
            auto &state = states[a];
            auto &info = agg_infos[a];
            Value r;
            if (info.name == "COUNT") r = Value::BIGINT(state.count);
            else if (info.name == "SUM") {
                auto &ac = acols[a];
                bool is_double = false;
                if (ac.local != INVALID_INDEX) {
                    if (ac.is_build) {
                        if (!build_rows.empty() && ac.local < build_rows[0].size())
                            is_double = build_rows[0][ac.local].type().id() == LogicalTypeId::DOUBLE;
                    } else {
                        is_double = probe->GetTypes()[ac.local].id() == LogicalTypeId::DOUBLE;
                    }
                }
                r = is_double ? Value::DOUBLE(state.sum) : Value::BIGINT(static_cast<int64_t>(state.sum));
            }
            else if (info.name == "AVG") r = state.count > 0 ? Value::DOUBLE(state.sum / state.count) : Value();
            else if (info.name == "MIN") {
                auto &ac = acols[a];
                if (!state.has_min) r = Value();
                else if (ac.is_build) r = state.min_val_or_null();
                else {
                    auto tid = probe->GetTypes()[ac.local].id();
                    if (tid == LogicalTypeId::INTEGER) r = Value::INTEGER(static_cast<int32_t>(state.sum_min));
                    else if (tid == LogicalTypeId::BIGINT) r = Value::BIGINT(static_cast<int64_t>(state.sum_min));
                    else r = Value::DOUBLE(state.sum_min);
                }
            }
            else if (info.name == "MAX") {
                auto &ac = acols[a];
                if (!state.has_max) r = Value();
                else if (ac.is_build) r = state.max_val_or_null();
                else {
                    auto tid = probe->GetTypes()[ac.local].id();
                    if (tid == LogicalTypeId::INTEGER) r = Value::INTEGER(static_cast<int32_t>(state.sum_max));
                    else if (tid == LogicalTypeId::BIGINT) r = Value::BIGINT(static_cast<int64_t>(state.sum_max));
                    else r = Value::DOUBLE(state.sum_max);
                }
            }
            row.push_back(r);
        }
        result_rows_.push_back(std::move(row));
    }
    return true;
}

// ============================================================================
// Update
// ============================================================================

class PhysicalUpdate : public PhysicalOperator {
public:
    PhysicalUpdate(TableCatalogEntry *table,
                   std::vector<BoundUpdateAssignment> assignments,
                   BoundExprPtr where_clause)
        : PhysicalOperator(PhysicalOperatorType::INSERT, {}),
          table_(table), assignments_(std::move(assignments)),
          where_clause_(std::move(where_clause)) {}

    void Init() override { done_ = false; affected_ = 0; }
    int64_t AffectedRows() const override { return affected_; }

    bool GetData(DataChunk &result) override {
        if (done_) return false;
        done_ = true;

        auto types = table_->GetTypes();
        auto &storage = table_->GetStorage();

        // Scan all data, modify matching rows, rebuild storage.
        std::vector<std::vector<Value>> all_rows;
        auto state = storage.InitScan();
        DataChunk chunk;
        while (true) {
            chunk.Initialize(types);
            if (!storage.Scan(state, chunk)) break;
            for (idx_t i = 0; i < chunk.size(); i++) {
                std::vector<Value> row;
                for (idx_t c = 0; c < chunk.ColumnCount(); c++)
                    row.push_back(chunk.GetValue(c, i));
                all_rows.push_back(std::move(row));
            }
        }

        // Create new storage and re-insert.
        auto new_storage = std::make_shared<DataTable>(types);

        for (auto &row : all_rows) {
            // Check WHERE.
            bool matches = true;
            if (where_clause_) {
                DataChunk single;
                single.Initialize(types);
                for (idx_t c = 0; c < row.size(); c++)
                    single.SetValue(c, 0, row[c]);
                Vector filter_result(LogicalType::BOOLEAN());
                ExpressionExecutor::Execute(*where_clause_, single, filter_result, 1);
                matches = filter_result.GetValidity().RowIsValid(0) &&
                          filter_result.GetData<bool>()[0];
            }

            if (matches) {
                affected_++;
                // Apply assignments - evaluate against current row.
                DataChunk row_chunk;
                row_chunk.Initialize(types);
                for (idx_t c = 0; c < row.size(); c++)
                    row_chunk.SetValue(c, 0, row[c]);

                for (auto &assign : assignments_) {
                    if (assign.value->GetExpressionType() == BoundExpressionType::CONSTANT) {
                        row[assign.column_index] = ExpressionExecutor::ExecuteScalar(*assign.value);
                    } else {
                        Vector res(assign.value->GetReturnType());
                        ExpressionExecutor::Execute(*assign.value, row_chunk, res, 1);
                        row[assign.column_index] = res.GetValue(0);
                    }
                }
            }

            DataChunk insert_chunk;
            insert_chunk.Initialize(types);
            for (idx_t c = 0; c < row.size(); c++)
                insert_chunk.SetValue(c, 0, row[c]);
            new_storage->Append(insert_chunk);
        }

        table_->SetStorage(new_storage);
        return false;
    }

private:
    TableCatalogEntry *table_;
    std::vector<BoundUpdateAssignment> assignments_;
    BoundExprPtr where_clause_;
    bool done_ = false;
    int64_t affected_ = 0;
};

// ============================================================================
// Delete
// ============================================================================

class PhysicalDelete : public PhysicalOperator {
public:
    PhysicalDelete(TableCatalogEntry *table, BoundExprPtr where_clause)
        : PhysicalOperator(PhysicalOperatorType::INSERT, {}),
          table_(table), where_clause_(std::move(where_clause)) {}

    void Init() override { done_ = false; affected_ = 0; }
    int64_t AffectedRows() const override { return affected_; }

    bool GetData(DataChunk &result) override {
        if (done_) return false;
        done_ = true;

        auto types = table_->GetTypes();
        auto &storage = table_->GetStorage();

        // Scan all data, keep non-matching rows.
        auto new_storage = std::make_shared<DataTable>(types);
        auto state = storage.InitScan();
        DataChunk chunk;

        while (true) {
            chunk.Initialize(types);
            if (!storage.Scan(state, chunk)) break;
            for (idx_t i = 0; i < chunk.size(); i++) {
                bool should_delete = false;
                if (where_clause_) {
                    DataChunk single;
                    single.Initialize(types);
                    for (idx_t c = 0; c < chunk.ColumnCount(); c++)
                        single.SetValue(c, 0, chunk.GetValue(c, i));
                    Vector filter_result(LogicalType::BOOLEAN());
                    ExpressionExecutor::Execute(*where_clause_, single, filter_result, 1);
                    should_delete = filter_result.GetValidity().RowIsValid(0) &&
                                    filter_result.GetData<bool>()[0];
                } else {
                    should_delete = true; // DELETE without WHERE = delete all.
                }

                if (!should_delete) {
                    DataChunk keep;
                    keep.Initialize(types);
                    for (idx_t c = 0; c < chunk.ColumnCount(); c++)
                        keep.SetValue(c, 0, chunk.GetValue(c, i));
                    new_storage->Append(keep);
                } else {
                    affected_++;
                }
            }
        }

        table_->SetStorage(new_storage);
        return false;
    }

private:
    TableCatalogEntry *table_;
    BoundExprPtr where_clause_;
    bool done_ = false;
    int64_t affected_ = 0;
};

class PhysicalDummyScan : public PhysicalOperator {
public:
    PhysicalDummyScan()
        : PhysicalOperator(PhysicalOperatorType::DUMMY_SCAN, {}) {}

    void Init() override { done_ = false; }

    bool GetData(DataChunk &result) override {
        if (done_) return false;
        done_ = true;
        // Return a single empty row - projections above will fill in constants.
        result.Initialize({});
        result.SetCardinality(1);
        return true;
    }

private:
    bool done_ = false;
};

// ============================================================================
// Physical Planner
// ============================================================================

PhysicalOpPtr PhysicalPlanner::Plan(const LogicalOperator &logical) {
    switch (logical.GetOperatorType()) {
    case LogicalOperatorType::GET:
        return PlanGet(static_cast<const LogicalGet &>(logical));
    case LogicalOperatorType::FILTER:
        return PlanFilter(static_cast<const LogicalFilter &>(logical));
    case LogicalOperatorType::PROJECTION:
        return PlanProjection(static_cast<const LogicalProjection &>(logical));
    case LogicalOperatorType::ORDER_BY:
        return PlanOrderBy(static_cast<const LogicalOrderBy &>(logical));
    case LogicalOperatorType::LIMIT:
        return PlanLimit(static_cast<const LogicalLimit &>(logical));
    case LogicalOperatorType::INSERT:
        return PlanInsert(static_cast<const LogicalInsert &>(logical));
    case LogicalOperatorType::CREATE_TABLE:
        return PlanCreateTable(static_cast<const LogicalCreateTable &>(logical));
    case LogicalOperatorType::DROP_TABLE:
        return PlanDropTable(static_cast<const LogicalDropTable &>(logical));
    case LogicalOperatorType::UPDATE:
        return PlanUpdateOp(static_cast<const LogicalUpdate &>(logical));
    case LogicalOperatorType::DELETE_STMT:
        return PlanDeleteOp(static_cast<const LogicalDeleteOp &>(logical));
    case LogicalOperatorType::WINDOW:
        return PlanWindow(static_cast<const LogicalWindow &>(logical));
    case LogicalOperatorType::DISTINCT:
        return PlanDistinct(static_cast<const LogicalDistinct &>(logical));
    case LogicalOperatorType::AGGREGATE:
        return PlanAggregate(static_cast<const LogicalAggregate &>(logical));
    case LogicalOperatorType::JOIN:
        return PlanJoin(static_cast<const LogicalJoin &>(logical));
    case LogicalOperatorType::DUMMY_SCAN:
        return PlanDummyScan(static_cast<const LogicalDummyScan &>(logical));
    default:
        throw InternalException("Unknown logical operator type");
    }
}

PhysicalOpPtr PhysicalPlanner::PlanGet(const LogicalGet &op) {
    return std::make_unique<PhysicalTableScan>(op.table);
}

PhysicalOpPtr PhysicalPlanner::PlanFilter(const LogicalFilter &op) {
    auto &mutable_op = const_cast<LogicalFilter &>(op);
    auto result = std::make_unique<PhysicalFilter>(
        std::move(mutable_op.condition), op.GetTypes());
    result->children.push_back(Plan(*op.children[0]));
    return result;
}

// Literal-equality peephole detection, marked noinline + cold so it stays out of the
// hot .text region. Returns true and fills `out_literal` if the shape
// matches (SELECT col WHERE col = literal); caller builds the synthetic
// count_op and wraps it in PhysicalLiteralEmitFilter.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline, cold))
#endif
static bool MatchColEqLiteralEmit(const LogicalProjection &op, Value &out_literal) {
    if (op.expressions.size() != 1 || op.children.empty()) return false;
    auto &proj_expr = op.expressions[0];
    if (!proj_expr || proj_expr->GetExpressionType() != BoundExpressionType::COLUMN_REF) return false;
    auto proj_col = static_cast<const BoundColumnRef &>(*proj_expr).column_index;
    auto *log_filter = dynamic_cast<const LogicalFilter *>(op.children[0].get());
    if (!log_filter || !log_filter->condition || log_filter->children.empty()) return false;
    auto &cond = *log_filter->condition;
    if (cond.GetExpressionType() != BoundExpressionType::COMPARISON) return false;
    auto &cmp = static_cast<const BoundComparison &>(cond);
    if (cmp.op != "=" && cmp.op != "==") return false;
    const BoundExpression *lhs = cmp.left.get();
    const BoundExpression *rhs = cmp.right.get();
    if (lhs && rhs && lhs->GetExpressionType() == BoundExpressionType::CONSTANT &&
        rhs->GetExpressionType() == BoundExpressionType::COLUMN_REF) std::swap(lhs, rhs);
    if (!lhs || !rhs ||
        lhs->GetExpressionType() != BoundExpressionType::COLUMN_REF ||
        rhs->GetExpressionType() != BoundExpressionType::CONSTANT) return false;
    auto col = static_cast<const BoundColumnRef &>(*lhs).column_index;
    if (col != proj_col || op.GetTypes().size() != 1) return false;
    auto out_tid = op.GetTypes()[0].id();
    if (out_tid != LogicalTypeId::BIGINT && out_tid != LogicalTypeId::INTEGER) return false;
    const auto &literal = static_cast<const BoundConstant &>(*rhs).value;
    auto literal_tid = literal.type().id();
    if (literal_tid != LogicalTypeId::BIGINT && literal_tid != LogicalTypeId::INTEGER) return false;
    if (out_tid == LogicalTypeId::BIGINT && literal_tid == LogicalTypeId::INTEGER)
        out_literal = Value::BIGINT((int64_t)literal.GetValue<int32_t>());
    else if (out_tid == LogicalTypeId::INTEGER && literal_tid == LogicalTypeId::BIGINT)
        out_literal = Value::INTEGER((int32_t)literal.GetValue<int64_t>());
    else
        out_literal = literal;
    return true;
}

PhysicalOpPtr PhysicalPlanner::PlanProjection(const LogicalProjection &op) {
    auto &mutable_op = const_cast<LogicalProjection &>(op);
    Value emit_literal;
    if (MatchColEqLiteralEmit(op, emit_literal)) {
        std::vector<BoundExprPtr> agg_funcs;
        agg_funcs.push_back(std::make_unique<BoundFunction>(
            "COUNT", std::vector<BoundExprPtr>{}, LogicalType::BIGINT(), true, false));
        auto count_op = std::make_unique<PhysicalHashAggregate>(
            std::vector<BoundExprPtr>{}, std::move(agg_funcs),
            std::vector<LogicalType>{LogicalType::BIGINT()});
        count_op->children.push_back(Plan(*op.children[0]));
        return std::make_unique<PhysicalLiteralEmitFilter>(
            op.GetTypes(), std::move(emit_literal), std::move(count_op));
    }
    auto result = std::make_unique<PhysicalProjection>(
        std::move(mutable_op.expressions), op.GetTypes());
    if (!op.children.empty()) {
        result->children.push_back(Plan(*op.children[0]));
    }
    // Push the projection's referenced columns down into a child OrderBy.
    // CollectTopN_Primitive's pass-2 decode then skips unprojected cols.
    // (SELECT SearchPhrase ... ORDER BY EventTime LIMIT 10) drops from
    // 1.7s -> ~225ms because pass-2 stops decoding 103 unused cols.
    {
        // Walk down through identity-projection/limit wrappers to find OrderBy.
        PhysicalOperator *cur = result->children.empty() ? nullptr
                                                         : result->children[0].get();
        for (int hops = 0; cur && hops < 4; hops++) {
            if (auto *ob = dynamic_cast<PhysicalOrderBy *>(cur)) {
                // Build mask of referenced col indices over the OrderBy's
                // INPUT schema. Each projection expression's column_index
                // refers to that same input space.
                std::vector<bool> mask(ob->GetTypes().size(), false);
                bool simple = true;
                std::function<void(const BoundExpression &)> walk =
                    [&](const BoundExpression &e) {
                        if (!simple) return;
                        switch (e.GetExpressionType()) {
                        case BoundExpressionType::COLUMN_REF: {
                            auto &cr = static_cast<const BoundColumnRef &>(e);
                            if (cr.column_index < mask.size())
                                mask[cr.column_index] = true;
                            else
                                simple = false;
                            break;
                        }
                        case BoundExpressionType::CONSTANT:
                            break;
                        case BoundExpressionType::COMPARISON: {
                            auto &c = static_cast<const BoundComparison &>(e);
                            walk(*c.left); walk(*c.right); break;
                        }
                        case BoundExpressionType::CONJUNCTION: {
                            auto &c = static_cast<const BoundConjunction &>(e);
                            walk(*c.left); walk(*c.right); break;
                        }
                        case BoundExpressionType::ARITHMETIC: {
                            auto &a = static_cast<const BoundArithmetic &>(e);
                            walk(*a.left); walk(*a.right); break;
                        }
                        case BoundExpressionType::NEGATION: {
                            auto &n = static_cast<const BoundNegation &>(e);
                            walk(*n.child); break;
                        }
                        case BoundExpressionType::UNARY_MINUS: {
                            auto &u = static_cast<const BoundUnaryMinus &>(e);
                            walk(*u.child); break;
                        }
                        case BoundExpressionType::CAST: {
                            auto &c = static_cast<const BoundCast &>(e);
                            walk(*c.child); break;
                        }
                        case BoundExpressionType::FUNCTION: {
                            auto &f = static_cast<const BoundFunction &>(e);
                            for (auto &arg : f.arguments) walk(*arg);
                            break;
                        }
                        default:
                            simple = false; break;
                        }
                    };
                for (auto &e : result->GetExpressions()) {
                    if (!e) { simple = false; break; }
                    walk(*e);
                }
                if (simple) ob->SetParentProjectionCols(std::move(mask));
                break;
            }
            // Allow PhysicalLimit / identity PhysicalProjection between us
            // and the OrderBy. Anything else stops the walk.
            if (dynamic_cast<PhysicalLimit *>(cur) ||
                (dynamic_cast<PhysicalProjection *>(cur) &&
                 static_cast<PhysicalProjection *>(cur)->IsIdentityProjection())) {
                cur = cur->children.empty() ? nullptr : cur->children[0].get();
                continue;
            }
            break;
        }
    }
    return result;
}

PhysicalOpPtr PhysicalPlanner::PlanOrderBy(const LogicalOrderBy &op) {
    auto &mutable_op = const_cast<LogicalOrderBy &>(op);
    auto result = std::make_unique<PhysicalOrderBy>(
        std::move(mutable_op.orders), op.GetTypes());
    result->children.push_back(Plan(*op.children[0]));
    return result;
}

PhysicalOpPtr PhysicalPlanner::PlanLimit(const LogicalLimit &op) {
    auto result = std::make_unique<PhysicalLimit>(
        op.limit_count, op.offset_count, op.GetTypes());
    result->children.push_back(Plan(*op.children[0]));
    return result;
}

PhysicalOpPtr PhysicalPlanner::PlanInsert(const LogicalInsert &op) {
    auto &mutable_op = const_cast<LogicalInsert &>(op);
    return std::make_unique<PhysicalInsert>(op.table, std::move(mutable_op.values));
}

PhysicalOpPtr PhysicalPlanner::PlanCreateTable(const LogicalCreateTable &op) {
    return std::make_unique<PhysicalCreateTable>(
        catalog_, op.table_name, op.columns, op.if_not_exists);
}

PhysicalOpPtr PhysicalPlanner::PlanDropTable(const LogicalDropTable &op) {
    return std::make_unique<PhysicalDropTable>(catalog_, op.table_name, op.if_exists);
}

PhysicalOpPtr PhysicalPlanner::PlanUpdateOp(const LogicalUpdate &op) {
    auto &mutable_op = const_cast<LogicalUpdate &>(op);
    return std::make_unique<PhysicalUpdate>(
        op.table, std::move(mutable_op.assignments), std::move(mutable_op.where_clause));
}

PhysicalOpPtr PhysicalPlanner::PlanDeleteOp(const LogicalDeleteOp &op) {
    auto &mutable_op = const_cast<LogicalDeleteOp &>(op);
    return std::make_unique<PhysicalDelete>(op.table, std::move(mutable_op.where_clause));
}

// Shapes of a window query PhysicalWindow computed wrongly, refused before
// it runs (Stage 8, #12; design L21). A window function nested in an
// expression is the executor's error already.
static void CheckWindowQuery(const std::vector<BoundExprPtr> &select_list, const BoundExprPtr &qualify) {
    std::vector<const BoundWindowExpression *> wins;
    for (auto &e : select_list)
        if (e->GetExpressionType() == BoundExpressionType::WINDOW)
            wins.push_back(static_cast<const BoundWindowExpression *>(e.get()));
    if (qualify) {
        // Only `window <op> constant` (either side): any other QUALIFY was
        // not applied at all.
        bool simple = false;
        if (qualify->GetExpressionType() == BoundExpressionType::COMPARISON) {
            auto &cmp = static_cast<const BoundComparison &>(*qualify);
            auto t = [](const BoundExprPtr &e) { return e ? e->GetExpressionType() : BoundExpressionType::CONSTANT; };
            if (t(cmp.left) == BoundExpressionType::WINDOW && t(cmp.right) == BoundExpressionType::CONSTANT) {
                simple = true;
                wins.push_back(static_cast<const BoundWindowExpression *>(cmp.left.get()));
            } else if (t(cmp.right) == BoundExpressionType::WINDOW && t(cmp.left) == BoundExpressionType::CONSTANT) {
                simple = true;
                wins.push_back(static_cast<const BoundWindowExpression *>(cmp.right.get()));
            }
        }
        if (!simple) {
            throw NotImplementedException("QUALIFY other than a window function compared with a constant "
                                          "is not supported");
        }
    }
    // Windows over different partitions or orders take a path that gives
    // an aggregate its whole partition, not its running frame.
    auto key = [](const BoundWindowExpression &w) {
        std::string k;
        for (auto &p : w.partition_by) {
            if (p->GetExpressionType() != BoundExpressionType::COLUMN_REF) return std::string("?");
            k += std::to_string(static_cast<const BoundColumnRef &>(*p).column_index) + ",";
        }
        k += "|";
        for (auto &o : w.order_by) {
            k += std::to_string(static_cast<const BoundColumnRef &>(*o.expression).column_index) +
                 (o.ascending ? "a," : "d,");
        }
        return k;
    };
    bool same = true;
    for (auto *w : wins)
        if (key(*w) != key(*wins.front()) || key(*w) == "?") same = false;
    if (same) return;
    for (auto *w : wins) {
        const auto &n = w->function_name;
        bool aggregate = n == "SUM" || n == "COUNT" || n == "AVG" || n == "MIN" || n == "MAX";
        if (aggregate && !w->order_by.empty()) {
            throw NotImplementedException("Window function " + n + " with ORDER BY beside windows over "
                                          "another partition or order is not supported");
        }
    }
}

PhysicalOpPtr PhysicalPlanner::PlanWindow(const LogicalWindow &op) {
    auto &mutable_op = const_cast<LogicalWindow &>(op);
    CheckWindowQuery(mutable_op.select_list, mutable_op.qualify);
    auto result = std::make_unique<PhysicalWindow>(
        std::move(mutable_op.select_list), std::move(mutable_op.qualify), op.GetTypes());
    result->children.push_back(Plan(*op.children[0]));
    return result;
}

PhysicalOpPtr PhysicalPlanner::PlanDistinct(const LogicalDistinct &op) {
    auto result = std::make_unique<PhysicalDistinct>(op.GetTypes());
    result->children.push_back(Plan(*op.children[0]));
    return result;
}

PhysicalOpPtr PhysicalPlanner::PlanAggregate(const LogicalAggregate &op) {
    auto &mutable_op = const_cast<LogicalAggregate &>(op);
    auto result = std::make_unique<PhysicalHashAggregate>(
        std::move(mutable_op.groups), std::move(mutable_op.aggregates), op.GetTypes());
    result->children.push_back(Plan(*op.children[0]));
    return result;
}

PhysicalOpPtr PhysicalPlanner::PlanJoin(const LogicalJoin &op) {
    auto &mutable_op = const_cast<LogicalJoin &>(op);
    idx_t left_cols = op.children[0]->GetTypes().size();
    idx_t right_cols = op.children[1]->GetTypes().size();
    auto result = std::make_unique<PhysicalHashJoin>(
        op.join_type, std::move(mutable_op.condition), op.GetTypes(),
        left_cols, right_cols);
    result->children.push_back(Plan(*op.children[0]));
    result->children.push_back(Plan(*op.children[1]));
    return result;
}

PhysicalOpPtr PhysicalPlanner::PlanDummyScan(const LogicalDummyScan &op) {
    return std::make_unique<PhysicalDummyScan>();
}

} // namespace zugodb
