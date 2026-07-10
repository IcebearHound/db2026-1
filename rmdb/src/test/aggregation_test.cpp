#undef NDEBUG
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "execution/execution_sort.h"
#include "execution/executor_aggregation.h"
#include "execution/executor_limit.h"

class VectorExecutor : public AbstractExecutor {
   public:
    VectorExecutor(std::vector<ColMeta> cols, std::vector<RmRecord> rows)
        : cols_(std::move(cols)), rows_(std::move(rows)) {
        len_ = cols_.empty() ? 0 : cols_.back().offset + cols_.back().len;
    }

    void beginTuple() override { pos_ = 0; }
    void nextTuple() override { if (pos_ < rows_.size()) ++pos_; }
    bool is_end() const override { return pos_ >= rows_.size(); }
    std::unique_ptr<RmRecord> Next() override {
        return is_end() ? nullptr : std::make_unique<RmRecord>(rows_[pos_]);
    }
    const std::vector<ColMeta> &cols() const override { return cols_; }
    size_t tupleLen() const override { return len_; }
    Rid &rid() override { return _abstract_rid; }

   private:
    std::vector<ColMeta> cols_;
    std::vector<RmRecord> rows_;
    size_t len_ = 0;
    size_t pos_ = 0;
};

std::vector<ColMeta> grade_cols() {
    return {{"grade", "course", TYPE_STRING, 20, 0, false},
            {"grade", "id", TYPE_INT, 4, 20, false},
            {"grade", "score", TYPE_FLOAT, 4, 24, false}};
}

RmRecord grade_row(const std::string &course, int id, float score) {
    RmRecord row(28);
    memset(row.data, 0, row.size);
    memcpy(row.data, course.data(), std::min<size_t>(course.size(), 20));
    *reinterpret_cast<int *>(row.data + 20) = id;
    *reinterpret_cast<float *>(row.data + 24) = score;
    return row;
}

AggregateExpr aggregate(AggFunc func, const std::string &column = "", ColType type = TYPE_INT) {
    AggregateExpr expr;
    expr.func = func;
    expr.count_star = column.empty();
    expr.col = {"grade", column};
    expr.input_type = type;
    expr.input_len = type == TYPE_STRING ? 20 : 4;
    return expr;
}

SelectExpr group_column(const std::string &name, ColType type, int len) {
    SelectExpr expr;
    expr.col = {"grade", name};
    expr.output_name = name;
    expr.output_type = type;
    expr.output_len = len;
    return expr;
}

SelectExpr aggregate_column(AggFunc func, const std::string &column, ColType input_type,
                            const std::string &name, ColType output_type) {
    SelectExpr expr;
    expr.is_aggregate = true;
    expr.aggregate = aggregate(func, column, input_type);
    expr.output_name = name;
    expr.output_type = output_type;
    expr.output_len = 4;
    return expr;
}

std::vector<RmRecord> sample_rows() {
    std::vector<RmRecord> rows;
    rows.push_back(grade_row("DataStructure", 1, 95));
    rows.push_back(grade_row("DataStructure", 2, 93.5));
    rows.push_back(grade_row("DataStructure", 3, 94.5));
    rows.push_back(grade_row("ComputerNetworks", 1, 99));
    rows.push_back(grade_row("ComputerNetworks", 2, 88.5));
    rows.push_back(grade_row("ComputerNetworks", 3, 92.5));
    rows.push_back(grade_row("C++", 1, 92));
    rows.push_back(grade_row("C++", 2, 89));
    rows.push_back(grade_row("C++", 3, 89.5));
    rows.push_back(grade_row("ParallelCompute", 1, 100));
    return rows;
}

void test_group_and_having() {
    std::vector<SelectExpr> select = {
        group_column("id", TYPE_INT, 4),
        aggregate_column(AGG_FUNC_MAX, "score", TYPE_FLOAT, "max_score", TYPE_FLOAT),
        aggregate_column(AGG_FUNC_MIN, "score", TYPE_FLOAT, "min_score", TYPE_FLOAT),
        aggregate_column(AGG_FUNC_SUM, "score", TYPE_FLOAT, "sum_score", TYPE_FLOAT)};
    HavingCondition having;
    having.aggregate = aggregate(AGG_FUNC_COUNT);
    having.op = OP_GT;
    having.rhs.set_int(3);
    AggregationExecutor executor(std::make_unique<VectorExecutor>(grade_cols(), sample_rows()),
                                 select, std::vector<TabCol>{{"grade", "id"}}, {having});
    executor.beginTuple();
    assert(!executor.is_end());
    auto row = executor.Next();
    assert(*reinterpret_cast<int *>(row->data) == 1);
    assert(*reinterpret_cast<float *>(row->data + 4) == 100.0F);
    assert(*reinterpret_cast<float *>(row->data + 8) == 92.0F);
    assert(*reinterpret_cast<float *>(row->data + 12) == 386.0F);
    executor.nextTuple();
    assert(executor.is_end());
}

void test_empty_count() {
    SelectExpr count = aggregate_column(AGG_FUNC_COUNT, "", TYPE_INT, "row_num", TYPE_INT);
    AggregationExecutor executor(std::make_unique<VectorExecutor>(grade_cols(), std::vector<RmRecord>{}),
                                 {count}, {}, {});
    executor.beginTuple();
    assert(!executor.is_end());
    auto row = executor.Next();
    assert(*reinterpret_cast<int *>(row->data) == 0);
    executor.nextTuple();
    assert(executor.is_end());
}

void test_sort_and_limit() {
    std::unique_ptr<AbstractExecutor> root = std::make_unique<VectorExecutor>(grade_cols(), sample_rows());
    root = std::make_unique<SortExecutor>(std::move(root),
        std::vector<OrderBySpec>{{{"grade", "score"}, true}, {{"grade", "id"}, false}});
    root = std::make_unique<LimitExecutor>(std::move(root), 3);
    std::vector<float> expected{100, 99, 95};
    size_t i = 0;
    for (root->beginTuple(); !root->is_end(); root->nextTuple()) {
        auto row = root->Next();
        assert(*reinterpret_cast<float *>(row->data + 24) == expected[i++]);
    }
    assert(i == expected.size());
}

void test_aggregation_performance() {
    std::vector<RmRecord> rows;
    rows.reserve(250000);
    for (int i = 0; i < 250000; ++i) rows.push_back(grade_row("load", i % 1000, i % 101));
    std::vector<SelectExpr> select = {
        group_column("id", TYPE_INT, 4),
        aggregate_column(AGG_FUNC_AVG, "score", TYPE_FLOAT, "avg_score", TYPE_FLOAT)};
    auto start = std::chrono::steady_clock::now();
    AggregationExecutor executor(std::make_unique<VectorExecutor>(grade_cols(), std::move(rows)),
                                 select, std::vector<TabCol>{{"grade", "id"}}, {});
    size_t groups = 0;
    for (executor.beginTuple(); !executor.is_end(); executor.nextTuple()) {
        assert(executor.Next() != nullptr);
        ++groups;
    }
    assert(groups == 1000);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    std::cout << "aggregation_test: 250000 rows, 1000 groups, " << elapsed << " ms\n";
}

int main() {
    test_group_and_having();
    test_empty_count();
    test_sort_and_limit();
    test_aggregation_performance();
    std::cout << "aggregation_test: PASS\n";
    return 0;
}
