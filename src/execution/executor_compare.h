#pragma once

#include <algorithm>
#include <cstring>

#include "common/common.h"
#include "system/sm_meta.h"

inline int compare_raw_value(const char *lhs, const char *rhs, ColType type, int len) {
    if (type == TYPE_INT) {
        int l = *reinterpret_cast<const int *>(lhs);
        int r = *reinterpret_cast<const int *>(rhs);
        return (l > r) - (l < r);
    }
    if (type == TYPE_FLOAT) {
        float l = *reinterpret_cast<const float *>(lhs);
        float r = *reinterpret_cast<const float *>(rhs);
        return (l > r) - (l < r);
    }
    return strncmp(lhs, rhs, len);
}

inline bool eval_compare(int cmp, CompOp op) {
    switch (op) {
        case OP_EQ: return cmp == 0;
        case OP_NE: return cmp != 0;
        case OP_LT: return cmp < 0;
        case OP_GT: return cmp > 0;
        case OP_LE: return cmp <= 0;
        case OP_GE: return cmp >= 0;
    }
    return false;
}

inline std::vector<ColMeta>::const_iterator find_col_meta(const std::vector<ColMeta> &cols, const TabCol &target) {
    auto pos = std::find_if(cols.begin(), cols.end(), [&](const ColMeta &col) {
        return col.tab_name == target.tab_name && col.name == target.col_name;
    });
    if (pos == cols.end()) {
        throw ColumnNotFoundError(target.tab_name + "." + target.col_name);
    }
    return pos;
}

inline bool eval_condition(const Condition &cond, const std::vector<ColMeta> &cols, const char *record_data) {
    auto lhs_col = find_col_meta(cols, cond.lhs_col);
    const char *lhs = record_data + lhs_col->offset;
    const char *rhs = nullptr;
    if (cond.is_rhs_val) {
        rhs = cond.rhs_val.raw->data;
    } else {
        auto rhs_col = find_col_meta(cols, cond.rhs_col);
        rhs = record_data + rhs_col->offset;
    }
    return eval_compare(compare_raw_value(lhs, rhs, lhs_col->type, lhs_col->len), cond.op);
}

inline bool eval_conditions(const std::vector<Condition> &conds, const std::vector<ColMeta> &cols,
                            const char *record_data) {
    for (auto &cond : conds) {
        if (!eval_condition(cond, cols, record_data)) {
            return false;
        }
    }
    return true;
}
