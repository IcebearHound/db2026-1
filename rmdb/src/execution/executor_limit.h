/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#pragma once

#include <memory>

#include "executor_abstract.h"

class LimitExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    size_t limit_;
    size_t pos_ = 0;

   public:
    LimitExecutor(std::unique_ptr<AbstractExecutor> prev, size_t limit)
        : prev_(std::move(prev)), limit_(limit) {}

    void beginTuple() override {
        pos_ = 0;
        prev_->beginTuple();
    }

    void nextTuple() override {
        if (!is_end()) {
            ++pos_;
            prev_->nextTuple();
        }
    }

    std::unique_ptr<RmRecord> Next() override { return is_end() ? nullptr : prev_->Next(); }
    bool is_end() const override { return pos_ >= limit_ || prev_->is_end(); }
    const std::vector<ColMeta> &cols() const override { return prev_->cols(); }
    size_t tupleLen() const override { return prev_->tupleLen(); }
    Rid &rid() override { return prev_->rid(); }
};
