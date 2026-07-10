#undef NDEBUG
#include <cassert>
#include <iostream>
#include <memory>
#include <string>

#include "analyze/analyze.h"
#include "parser/parser.h"

std::shared_ptr<ast::TreeNode> parse_sql(const std::string &sql) {
    YY_BUFFER_STATE buffer = yy_scan_string(sql.c_str());
    const int result = yyparse();
    yy_delete_buffer(buffer);
    if (result != 0) return nullptr;
    return ast::parse_tree;
}

template <typename Fn>
void expect_failure(Fn fn) {
    bool failed = false;
    try {
        fn();
    } catch (const RMDBError &) {
        failed = true;
    }
    assert(failed);
}

int main() {
    SmManager sm(nullptr, nullptr, nullptr, nullptr);
    TabMeta grade;
    grade.name = "grade";
    grade.cols = {{"grade", "course", TYPE_STRING, 20, 0, false},
                  {"grade", "id", TYPE_INT, 4, 20, false},
                  {"grade", "score", TYPE_FLOAT, 4, 24, false}};
    sm.db_.SetTabMeta("grade", grade);
    Analyze analyze(&sm);

    auto valid = parse_sql(
        "select id,max(score) as top,avg(score) as mean from grade where score < 60 "
        "group by id having count(*) > 1 and min(score) > 10 order by top desc,id limit 5;");
    assert(valid != nullptr);
    auto query = analyze.do_analyze(valid);
    assert(query->has_aggregation);
    assert(query->select_exprs.size() == 3);
    assert(query->group_cols.size() == 1);
    assert(query->having_conds.size() == 2);
    assert(query->order_bys.size() == 2);
    assert(query->limit == 5);
    assert(query->conds.size() == 1);
    assert(query->conds[0].rhs_val.type == TYPE_INT);

    expect_failure([&] {
        analyze.do_analyze(parse_sql("select id,score from grade group by course;"));
    });
    expect_failure([&] {
        analyze.do_analyze(parse_sql("select max(course) from grade;"));
    });
    expect_failure([&] {
        analyze.do_analyze(parse_sql("select id,count(*) from grade;"));
    });
    assert(parse_sql("select id,max(score) from grade where max(score) > 90 group by id;") == nullptr);

    std::cout << "aggregation_analyze_test: PASS\n";
    return 0;
}
