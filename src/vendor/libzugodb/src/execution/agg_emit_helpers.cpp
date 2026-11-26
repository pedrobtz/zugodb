#include "zugodb/execution/agg_emit_helpers.hpp"
#include "zugodb/common/exception.hpp"
#include "zugodb/common/types/float_order.hpp"

#include <algorithm>
#include <cmath>

namespace zugodb {

EmitAggKind ResolveEmitAggKind(const std::string& n) {
    if (n == "COUNT") return EmitAggKind::Count;
    if (n == "SUM") return EmitAggKind::Sum;
    if (n == "AVG") return EmitAggKind::Avg;
    if (n == "MIN") return EmitAggKind::Min;
    if (n == "MAX") return EmitAggKind::Max;
    if (n == "STRING_AGG" || n == "LISTAGG" || n == "GROUP_CONCAT")
        return EmitAggKind::StringAgg;
    if (n == "STDDEV" || n == "STDDEV_SAMP") return EmitAggKind::StddevSamp;
    if (n == "STDDEV_POP") return EmitAggKind::StddevPop;
    if (n == "VARIANCE" || n == "VAR_SAMP") return EmitAggKind::VarSamp;
    if (n == "VAR_POP") return EmitAggKind::VarPop;
    if (n == "MEDIAN") return EmitAggKind::Median;
    if (n == "BOOL_AND") return EmitAggKind::BoolAnd;
    if (n == "BOOL_OR") return EmitAggKind::BoolOr;
    return EmitAggKind::Other;
}

bool EmitAggValue(const EmitAggDesc& desc, const EmitAggView& view,
                  std::vector<Value>& out_row) {
    switch (desc.kind) {
    case EmitAggKind::Count:
        out_row.push_back(Value::BIGINT(view.count));
        return true;
    case EmitAggKind::Sum: {
        // SQL standard: SUM of an empty set (or all-NULL inputs) is NULL,
        // not 0. The previous unconditional emit hard-coded the wrong
        // result. view.count is incremented only when a non-NULL value
        // is added to view.sum, so count==0 IS the "no inputs contributed"
        // case for both empty inputs and all-NULL inputs.
        if (view.count == 0) {
            out_row.push_back(Value());
            return true;
        }
        // Algebraic collapse: SUM(col +/- N) = SUM(col) + N*COUNT(col).
        // 90 SUMs over the same column reduce to ONE scan; each
        // agg's offset adjusts the emitted answer at this point.
        double s = view.sum;
        if (desc.sum_with_offset) s += desc.sum_offset * (double)view.count;
        if (desc.return_type_id == LogicalTypeId::BIGINT) {
            // The sum is a double: exact only below 2^53. Past it, DuckDB's
            // exact (HUGEINT) sum is not what the engine has (Stage 8, #12).
            if (std::fabs(s) >= 9007199254740992.0) {
                throw OutOfRangeException("SUM past 2^53 is not computed exactly");
            }
            out_row.push_back(Value::BIGINT(static_cast<int64_t>(s)));
        } else {
            out_row.push_back(Value::DOUBLE(s));
        }
        return true;
    }
    case EmitAggKind::Avg:
        if (view.count > 0) {
            out_row.push_back(Value::DOUBLE(view.sum / view.count));
        } else {
            out_row.push_back(Value());
        }
        return true;
    case EmitAggKind::Min:
        if (!view.has_min) {
            out_row.push_back(Value());
        } else if (view.min_val_ptr) {
            out_row.push_back(*view.min_val_ptr);
        } else {
            // Fused numeric path writes only sum_min (double); synthesize
            // a typed Value here using the agg's return type.
            switch (desc.return_type_id) {
            case LogicalTypeId::INTEGER:
                out_row.push_back(Value::INTEGER((int32_t)view.sum_min)); break;
            case LogicalTypeId::BIGINT:
                out_row.push_back(Value::BIGINT((int64_t)view.sum_min)); break;
            case LogicalTypeId::FLOAT:
                out_row.push_back(Value::FLOAT((float)view.sum_min)); break;
            default:
                out_row.push_back(Value::DOUBLE(view.sum_min)); break;
            }
        }
        return true;
    case EmitAggKind::Max:
        if (!view.has_max) {
            out_row.push_back(Value());
        } else if (view.max_val_ptr) {
            out_row.push_back(*view.max_val_ptr);
        } else {
            switch (desc.return_type_id) {
            case LogicalTypeId::INTEGER:
                out_row.push_back(Value::INTEGER((int32_t)view.sum_max)); break;
            case LogicalTypeId::BIGINT:
                out_row.push_back(Value::BIGINT((int64_t)view.sum_max)); break;
            case LogicalTypeId::FLOAT:
                out_row.push_back(Value::FLOAT((float)view.sum_max)); break;
            default:
                out_row.push_back(Value::DOUBLE(view.sum_max)); break;
            }
        }
        return true;
    case EmitAggKind::StringAgg:
        out_row.push_back(view.str_started && view.str_agg
                              ? Value::VARCHAR(*view.str_agg)
                              : Value());
        return true;
    case EmitAggKind::StddevSamp:
        if (view.count > 1) {
            double mean = view.sum / view.count;
            double var = (view.sum_sq - view.count * mean * mean) /
                         (view.count - 1);
            out_row.push_back(Value::DOUBLE(std::sqrt(var)));
        } else {
            out_row.push_back(Value());
        }
        return true;
    case EmitAggKind::StddevPop:
        if (view.count > 0) {
            double mean = view.sum / view.count;
            double var = (view.sum_sq - view.count * mean * mean) / view.count;
            // One value's is 0, as DuckDB's, a NaN or an infinity too.
            if (view.count == 1) var = 0.0;
            out_row.push_back(Value::DOUBLE(std::sqrt(var)));
        } else {
            out_row.push_back(Value());
        }
        return true;
    case EmitAggKind::VarSamp:
        if (view.count > 1) {
            double mean = view.sum / view.count;
            double var = (view.sum_sq - view.count * mean * mean) /
                         (view.count - 1);
            out_row.push_back(Value::DOUBLE(var));
        } else {
            out_row.push_back(Value());
        }
        return true;
    case EmitAggKind::VarPop:
        if (view.count > 0) {
            double mean = view.sum / view.count;
            double var = (view.sum_sq - view.count * mean * mean) / view.count;
            if (view.count == 1) var = 0.0;
            out_row.push_back(Value::DOUBLE(var));
        } else {
            out_row.push_back(Value());
        }
        return true;
    case EmitAggKind::Median:
        if (view.values && !view.values->empty()) {
            auto& vals = *view.values;
            // DuckDB's order, NaN the largest; IEEE's < made the sort
            // undefined over a NaN (#42).
            std::sort(vals.begin(), vals.end(),
                      [](double x, double y) { return SqlLess(x, y); });
            size_t mid = vals.size() / 2;
            // DuckDB's interpolation of the middle two: lo * 0.5 + hi * 0.5
            // for a DOUBLE (the median of -inf and -2 is -inf), and
            // lo + (hi - lo) * 0.5 in FLOAT for a FLOAT (a NaN there).
            double median = vals[mid];
            if (vals.size() % 2 == 0) {
                if (desc.arg_float) {
                    float lo = static_cast<float>(vals[mid - 1]), hi = static_cast<float>(vals[mid]);
                    median = static_cast<double>(lo + (hi - lo) * 0.5f);
                } else {
                    median = vals[mid - 1] * (1.0 - 0.5) + vals[mid] * 0.5;
                }
            }
            out_row.push_back(Value::DOUBLE(median));
        } else {
            out_row.push_back(Value());
        }
        return true;
    case EmitAggKind::BoolAnd:
        out_row.push_back(view.count > 0 ? Value::BOOLEAN(view.bool_and_v) : Value());
        return true;
    case EmitAggKind::BoolOr:
        out_row.push_back(view.count > 0 ? Value::BOOLEAN(view.bool_or_v) : Value());
        return true;
    case EmitAggKind::Other:
        return false;
    }
    return false;
}

} // namespace zugodb
