/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#pragma once

#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "executor_abstract.h"
#include "executor_compare.h"

class AggregationExecutor : public AbstractExecutor {
   private:
    struct AggregateBinding {
        AggregateExpr expr;
        ColMeta input_col{};
        bool has_input = false;
    };

    struct AggregateState {
        int64_t count = 0;
        double sum = 0;
        double min = 0;
        double max = 0;
        bool initialized = false;
    };

    struct GroupState {
        std::string group_bytes;
        std::vector<AggregateState> aggregates;
    };

    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<SelectExpr> select_exprs_;
    std::vector<TabCol> group_cols_;
    std::vector<HavingCondition> having_conds_;
    std::vector<ColMeta> group_input_cols_;
    std::vector<size_t> group_offsets_;
    std::vector<AggregateBinding> bindings_;
    std::vector<int> select_agg_idx_;
    std::vector<int> select_group_idx_;
    std::vector<int> having_agg_idx_;
    std::vector<int> having_group_idx_;
    std::vector<ColMeta> cols_;
    size_t len_ = 0;

    std::vector<GroupState> groups_;
    std::unordered_map<std::string, size_t> group_index_;
    std::vector<size_t> visible_groups_;
    size_t pos_ = 0;

    static bool same_col(const TabCol &lhs, const TabCol &rhs) {
        return lhs.tab_name == rhs.tab_name && lhs.col_name == rhs.col_name;
    }

    static bool same_aggregate(const AggregateExpr &lhs, const AggregateExpr &rhs) {
        return lhs.func == rhs.func && lhs.count_star == rhs.count_star &&
               (lhs.count_star || same_col(lhs.col, rhs.col));
    }

    int add_binding(const AggregateExpr &expr) {
        for (size_t i = 0; i < bindings_.size(); ++i) {
            if (same_aggregate(bindings_[i].expr, expr)) return static_cast<int>(i);
        }
        AggregateBinding binding;
        binding.expr = expr;
        if (!expr.count_star) {
            binding.input_col = *get_col(prev_->cols(), expr.col);
            binding.has_input = true;
        }
        bindings_.push_back(binding);
        return static_cast<int>(bindings_.size() - 1);
    }

    static double read_numeric(const char *data, ColType type) {
        return type == TYPE_INT ? *reinterpret_cast<const int *>(data)
                                : *reinterpret_cast<const float *>(data);
    }

    std::pair<std::string, std::string> make_group_key(const RmRecord &record) const {
        std::string key;
        std::string bytes;
        size_t total_len = 0;
        for (const auto &col : group_input_cols_) total_len += col.len;
        key.reserve(total_len);
        bytes.reserve(total_len);
        for (const auto &col : group_input_cols_) {
            const char *value = record.data + col.offset;
            bytes.append(value, col.len);
            if (col.type == TYPE_FLOAT) {
                float normalized = *reinterpret_cast<const float *>(value);
                if (normalized == 0.0F) normalized = 0.0F;
                key.append(reinterpret_cast<const char *>(&normalized), sizeof(normalized));
            } else {
                key.append(value, col.len);
            }
        }
        return {std::move(key), std::move(bytes)};
    }

    size_t ensure_group(const std::string &key, std::string bytes) {
        auto found = group_index_.find(key);
        if (found != group_index_.end()) return found->second;
        const size_t idx = groups_.size();
        GroupState group;
        group.group_bytes = std::move(bytes);
        group.aggregates.resize(bindings_.size());
        groups_.push_back(std::move(group));
        group_index_.emplace(key, idx);
        return idx;
    }

    void update_group(GroupState &group, const RmRecord &record) {
        for (size_t i = 0; i < bindings_.size(); ++i) {
            const auto &binding = bindings_[i];
            auto &state = group.aggregates[i];
            if (binding.expr.func == AGG_FUNC_COUNT) {
                ++state.count;
                continue;
            }
            const double value = read_numeric(record.data + binding.input_col.offset, binding.input_col.type);
            state.sum += value;
            ++state.count;
            if (!state.initialized) {
                state.min = state.max = value;
                state.initialized = true;
            } else {
                if (value < state.min) state.min = value;
                if (value > state.max) state.max = value;
            }
        }
    }

    double aggregate_value(const AggregateBinding &binding, const AggregateState &state) const {
        switch (binding.expr.func) {
            case AGG_FUNC_COUNT: return static_cast<double>(state.count);
            case AGG_FUNC_MAX: return state.initialized ? state.max : 0;
            case AGG_FUNC_MIN: return state.initialized ? state.min : 0;
            case AGG_FUNC_SUM: return state.sum;
            case AGG_FUNC_AVG: return state.count == 0 ? 0 : state.sum / static_cast<double>(state.count);
            default: return 0;
        }
    }

    bool passes_having(const GroupState &group) const {
        for (size_t i = 0; i < having_conds_.size(); ++i) {
            const auto &condition = having_conds_[i];
            int cmp;
            if (condition.is_aggregate) {
                const int binding_idx = having_agg_idx_[i];
                const double lhs = aggregate_value(bindings_[binding_idx], group.aggregates[binding_idx]);
                const double rhs = condition.rhs.type == TYPE_INT ? condition.rhs.int_val : condition.rhs.float_val;
                cmp = (lhs > rhs) - (lhs < rhs);
            } else {
                const int group_idx = having_group_idx_[i];
                const auto &meta = group_input_cols_[group_idx];
                cmp = compare_typed_value(group.group_bytes.data() + group_offsets_[group_idx],
                                          meta.type, meta.len, condition.rhs.raw->data,
                                          condition.rhs.type, condition.rhs.raw->size);
            }
            if (!eval_compare(cmp, condition.op)) return false;
        }
        return true;
    }

   public:
    AggregationExecutor(std::unique_ptr<AbstractExecutor> prev, std::vector<SelectExpr> select_exprs,
                        std::vector<TabCol> group_cols, std::vector<HavingCondition> having_conds)
        : prev_(std::move(prev)), select_exprs_(std::move(select_exprs)),
          group_cols_(std::move(group_cols)), having_conds_(std::move(having_conds)) {
        size_t group_offset = 0;
        for (const auto &group_col : group_cols_) {
            auto meta = *get_col(prev_->cols(), group_col);
            group_input_cols_.push_back(meta);
            group_offsets_.push_back(group_offset);
            group_offset += meta.len;
        }

        select_agg_idx_.assign(select_exprs_.size(), -1);
        select_group_idx_.assign(select_exprs_.size(), -1);
        for (size_t i = 0; i < select_exprs_.size(); ++i) {
            const auto &expr = select_exprs_[i];
            if (expr.is_aggregate) {
                select_agg_idx_[i] = add_binding(expr.aggregate);
            } else {
                for (size_t j = 0; j < group_cols_.size(); ++j) {
                    if (same_col(expr.col, group_cols_[j])) {
                        select_group_idx_[i] = static_cast<int>(j);
                        break;
                    }
                }
            }
            ColMeta output_col;
            output_col.tab_name = "";
            output_col.name = expr.output_name;
            output_col.type = expr.output_type;
            output_col.len = expr.output_len;
            output_col.offset = static_cast<int>(len_);
            output_col.index = false;
            len_ += output_col.len;
            cols_.push_back(output_col);
        }
        for (const auto &having : having_conds_) {
            if (having.is_aggregate) {
                having_agg_idx_.push_back(add_binding(having.aggregate));
                having_group_idx_.push_back(-1);
            } else {
                having_agg_idx_.push_back(-1);
                int group_idx = -1;
                for (size_t i = 0; i < group_cols_.size(); ++i) {
                    if (same_col(having.col, group_cols_[i])) {
                        group_idx = static_cast<int>(i);
                        break;
                    }
                }
                having_group_idx_.push_back(group_idx);
            }
        }
    }

    void beginTuple() override {
        groups_.clear();
        group_index_.clear();
        visible_groups_.clear();
        pos_ = 0;
        if (group_cols_.empty()) ensure_group("", "");

        for (prev_->beginTuple(); !prev_->is_end(); prev_->nextTuple()) {
            auto record = prev_->Next();
            if (record == nullptr) continue;
            auto key = make_group_key(*record);
            const size_t idx = ensure_group(key.first, std::move(key.second));
            update_group(groups_[idx], *record);
        }
        for (size_t i = 0; i < groups_.size(); ++i) {
            if (passes_having(groups_[i])) visible_groups_.push_back(i);
        }
    }

    void nextTuple() override {
        if (pos_ < visible_groups_.size()) ++pos_;
    }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end()) return nullptr;
        const auto &group = groups_[visible_groups_[pos_]];
        auto result = std::make_unique<RmRecord>(static_cast<int>(len_));
        for (size_t i = 0; i < select_exprs_.size(); ++i) {
            const auto &expr = select_exprs_[i];
            char *out = result->data + cols_[i].offset;
            if (!expr.is_aggregate) {
                const int group_idx = select_group_idx_[i];
                memcpy(out, group.group_bytes.data() + group_offsets_[group_idx], cols_[i].len);
                continue;
            }
            const int binding_idx = select_agg_idx_[i];
            const double value = aggregate_value(bindings_[binding_idx], group.aggregates[binding_idx]);
            if (cols_[i].type == TYPE_INT) {
                *reinterpret_cast<int *>(out) = static_cast<int>(value);
            } else {
                *reinterpret_cast<float *>(out) = static_cast<float>(value);
            }
        }
        return result;
    }

    bool is_end() const override { return pos_ >= visible_groups_.size(); }
    const std::vector<ColMeta> &cols() const override { return cols_; }
    size_t tupleLen() const override { return len_; }
    Rid &rid() override { return _abstract_rid; }
};
