/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#pragma once

#include "execution_defs.h"
#include "executor_abstract.h"

class FilterExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<Condition> conds_;
    std::unique_ptr<RmRecord> current_;

    void advance() {
        current_.reset();
        while (!prev_->is_end()) {
            auto rec = prev_->Next();
            if (eval_conds(conds_, rec.get(), prev_->cols())) {
                current_ = std::move(rec);
                return;
            }
            prev_->nextTuple();
        }
    }

   public:
    FilterExecutor(std::unique_ptr<AbstractExecutor> prev, std::vector<Condition> conds)
        : prev_(std::move(prev)), conds_(std::move(conds)) {}

    void beginTuple() override {
        prev_->beginTuple();
        advance();
    }

    void nextTuple() override {
        if (!prev_->is_end()) {
            prev_->nextTuple();
        }
        advance();
    }

    bool is_end() const override { return current_ == nullptr; }

    std::unique_ptr<RmRecord> Next() override {
        rows_++;
        return std::make_unique<RmRecord>(*current_);
    }

    size_t tupleLen() const override { return prev_->tupleLen(); }

    const std::vector<ColMeta> &cols() const override { return prev_->cols(); }

    std::string getType() override { return "FilterExecutor"; }

    ColMeta get_col_offset(const TabCol &target) override { return prev_->get_col_offset(target); }

    void reset_stats() override {
        rows_ = 0;
        prev_->reset_stats();
    }

    std::vector<AbstractExecutor *> children() const override { return {prev_.get()}; }

    Rid &rid() override { return prev_->rid(); }
};
