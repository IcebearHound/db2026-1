/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#pragma once

#include <algorithm>
#include <memory>
#include <vector>

#include "executor_abstract.h"
#include "executor_compare.h"

class SortExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<OrderBySpec> order_bys_;
    std::vector<ColMeta> order_cols_;
    std::vector<std::unique_ptr<RmRecord>> tuples_;
    size_t pos_ = 0;

   public:
    SortExecutor(std::unique_ptr<AbstractExecutor> prev, std::vector<OrderBySpec> order_bys)
        : prev_(std::move(prev)), order_bys_(std::move(order_bys)) {
        for (const auto &order : order_bys_) {
            order_cols_.push_back(*get_col(prev_->cols(), order.col));
        }
    }

    void beginTuple() override {
        tuples_.clear();
        for (prev_->beginTuple(); !prev_->is_end(); prev_->nextTuple()) {
            auto tuple = prev_->Next();
            if (tuple != nullptr) tuples_.push_back(std::move(tuple));
        }
        std::stable_sort(tuples_.begin(), tuples_.end(), [&](const auto &lhs, const auto &rhs) {
            for (size_t i = 0; i < order_cols_.size(); ++i) {
                const auto &col = order_cols_[i];
                const int cmp = compare_raw_value(lhs->data + col.offset, rhs->data + col.offset,
                                                  col.type, col.len);
                if (cmp != 0) return order_bys_[i].is_desc ? cmp > 0 : cmp < 0;
            }
            return false;
        });
        pos_ = 0;
    }

    void nextTuple() override {
        if (pos_ < tuples_.size()) ++pos_;
    }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end()) return nullptr;
        return std::make_unique<RmRecord>(*tuples_[pos_]);
    }

    bool is_end() const override { return pos_ >= tuples_.size(); }
    const std::vector<ColMeta> &cols() const override { return prev_->cols(); }
    size_t tupleLen() const override { return prev_->tupleLen(); }
    Rid &rid() override { return _abstract_rid; }
};
