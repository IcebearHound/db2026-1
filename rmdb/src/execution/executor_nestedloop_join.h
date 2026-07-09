/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#pragma once

#include <cstring>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class NestedLoopJoinExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> left_;
    std::unique_ptr<AbstractExecutor> right_;
    size_t len_;
    std::vector<ColMeta> cols_;
    std::vector<Condition> fed_conds_;
    bool isend = false;
    std::unique_ptr<RmRecord> left_rec_;
    std::unique_ptr<RmRecord> current_;

    std::unique_ptr<RmRecord> join_records(const RmRecord *left_rec, const RmRecord *right_rec) {
        auto rec = std::make_unique<RmRecord>(len_);
        memcpy(rec->data, left_rec->data, left_->tupleLen());
        memcpy(rec->data + left_->tupleLen(), right_rec->data, right_->tupleLen());
        return rec;
    }

    void advance() {
        current_.reset();
        while (!left_->is_end()) {
            while (!right_->is_end()) {
                auto right_rec = right_->Next();
                auto joined = join_records(left_rec_.get(), right_rec.get());
                if (eval_conds(fed_conds_, joined.get(), cols_)) {
                    current_ = std::move(joined);
                    isend = false;
                    return;
                }
                right_->nextTuple();
            }
            left_->nextTuple();
            if (left_->is_end()) {
                break;
            }
            left_rec_ = left_->Next();
            right_->beginTuple();
        }
        isend = true;
    }

   public:
    NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right,
                           std::vector<Condition> conds) {
        left_ = std::move(left);
        right_ = std::move(right);
        len_ = left_->tupleLen() + right_->tupleLen();
        cols_ = left_->cols();
        auto right_cols = right_->cols();
        for (auto &col : right_cols) {
            col.offset += left_->tupleLen();
        }
        cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());
        fed_conds_ = std::move(conds);
    }

    void beginTuple() override {
        left_->beginTuple();
        if (left_->is_end()) {
            isend = true;
            current_.reset();
            return;
        }
        left_rec_ = left_->Next();
        right_->beginTuple();
        advance();
    }

    void nextTuple() override {
        if (!right_->is_end()) {
            right_->nextTuple();
        }
        advance();
    }

    bool is_end() const override { return isend; }

    std::unique_ptr<RmRecord> Next() override {
        rows_++;
        return std::make_unique<RmRecord>(*current_);
    }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    std::string getType() override { return "NestedLoopJoinExecutor"; }

    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }

    void reset_stats() override {
        rows_ = 0;
        left_->reset_stats();
        right_->reset_stats();
    }

    std::vector<AbstractExecutor *> children() const override { return {left_.get(), right_.get()}; }

    Rid &rid() override { return _abstract_rid; }
};
