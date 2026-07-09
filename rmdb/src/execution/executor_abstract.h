/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include <cstring>

#include "execution_defs.h"
#include "common/common.h"
#include "index/ix.h"
#include "system/sm.h"

class AbstractExecutor {
   public:
    Rid _abstract_rid;

    Context *context_;
    size_t rows_ = 0;

    virtual ~AbstractExecutor() = default;

    virtual size_t tupleLen() const { return 0; };

    virtual const std::vector<ColMeta> &cols() const {
        std::vector<ColMeta> *_cols = nullptr;
        return *_cols;
    };

    virtual std::string getType() { return "AbstractExecutor"; };

    virtual void beginTuple(){};

    virtual void nextTuple(){};

    virtual bool is_end() const { return true; };

    virtual Rid &rid() = 0;

    virtual std::unique_ptr<RmRecord> Next() = 0;

    virtual ColMeta get_col_offset(const TabCol &target) { return ColMeta();};

    virtual void reset_stats() { rows_ = 0; }

    virtual size_t rows() const { return rows_; }

    virtual size_t scanned_rows() const { return rows_; }

    virtual std::vector<AbstractExecutor *> children() const { return {}; }

    std::vector<ColMeta>::const_iterator get_col(const std::vector<ColMeta> &rec_cols, const TabCol &target) {
        auto pos = std::find_if(rec_cols.begin(), rec_cols.end(), [&](const ColMeta &col) {
            return col.tab_name == target.tab_name && col.name == target.col_name;
        });
        if (pos == rec_cols.end()) {
            throw ColumnNotFoundError(target.tab_name + '.' + target.col_name);
        }
        return pos;
    }

    static int compare_data(const char *lhs, const char *rhs, ColType type, int len) {
        switch (type) {
            case TYPE_INT: {
                int a = *reinterpret_cast<const int *>(lhs);
                int b = *reinterpret_cast<const int *>(rhs);
                return (a < b) ? -1 : ((a > b) ? 1 : 0);
            }
            case TYPE_FLOAT: {
                float a = *reinterpret_cast<const float *>(lhs);
                float b = *reinterpret_cast<const float *>(rhs);
                return (a < b) ? -1 : ((a > b) ? 1 : 0);
            }
            case TYPE_STRING:
                return memcmp(lhs, rhs, len);
            default:
                throw InternalError("Unexpected data type");
        }
    }

    static bool compare_result(int cmp, CompOp op) {
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

    bool eval_cond(const Condition &cond, const RmRecord *rec, const std::vector<ColMeta> &rec_cols) {
        auto lhs_col = get_col(rec_cols, cond.lhs_col);
        const char *lhs = rec->data + lhs_col->offset;
        const char *rhs = nullptr;
        ColType rhs_type;
        int rhs_len;
        if (cond.is_rhs_val) {
            rhs = cond.rhs_val.raw->data;
            rhs_type = cond.rhs_val.type;
            rhs_len = lhs_col->len;
        } else {
            auto rhs_col = get_col(rec_cols, cond.rhs_col);
            rhs = rec->data + rhs_col->offset;
            rhs_type = rhs_col->type;
            rhs_len = rhs_col->len;
        }
        if (lhs_col->type != rhs_type) {
            throw IncompatibleTypeError(coltype2str(lhs_col->type), coltype2str(rhs_type));
        }
        return compare_result(compare_data(lhs, rhs, lhs_col->type, rhs_len), cond.op);
    }

    bool eval_conds(const std::vector<Condition> &conds, const RmRecord *rec, const std::vector<ColMeta> &rec_cols) {
        for (const auto &cond : conds) {
            if (!eval_cond(cond, rec, rec_cols)) {
                return false;
            }
        }
        return true;
    }
};
