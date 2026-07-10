/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#include "analyze.h"

#include <algorithm>

namespace {

AggFunc convert_agg_type(ast::AggType type) {
    switch (type) {
        case ast::AGG_COUNT: return AGG_FUNC_COUNT;
        case ast::AGG_MAX: return AGG_FUNC_MAX;
        case ast::AGG_MIN: return AGG_FUNC_MIN;
        case ast::AGG_SUM: return AGG_FUNC_SUM;
        case ast::AGG_AVG: return AGG_FUNC_AVG;
        default: return AGG_FUNC_NONE;
    }
}

std::string agg_name(AggFunc func) {
    switch (func) {
        case AGG_FUNC_COUNT: return "COUNT";
        case AGG_FUNC_MAX: return "MAX";
        case AGG_FUNC_MIN: return "MIN";
        case AGG_FUNC_SUM: return "SUM";
        case AGG_FUNC_AVG: return "AVG";
        default: return "";
    }
}

bool same_col(const TabCol &lhs, const TabCol &rhs) {
    return lhs.tab_name == rhs.tab_name && lhs.col_name == rhs.col_name;
}

bool is_numeric(ColType type) {
    return type == TYPE_INT || type == TYPE_FLOAT;
}

}  // namespace

std::shared_ptr<Query> Analyze::do_analyze(std::shared_ptr<ast::TreeNode> parse) {
    auto query = std::make_shared<Query>();
    if (auto x = std::dynamic_pointer_cast<ast::SelectStmt>(parse)) {
        query->tables = x->tabs;
        for (const auto &tab_name : query->tables) {
            if (!sm_manager_->db_.is_table(tab_name)) {
                throw TableNotFoundError(tab_name);
            }
        }

        std::vector<ColMeta> all_cols;
        get_all_cols(query->tables, all_cols);

        for (const auto &sv_group_col : x->group_bys) {
            query->group_cols.push_back(check_column(
                all_cols, {sv_group_col->tab_name, sv_group_col->col_name}));
        }

        auto select_items = x->select_items;
        if (select_items.empty()) {
            for (const auto &col : all_cols) {
                select_items.push_back(std::make_shared<ast::SelectItem>(
                    ast::AGG_NONE, std::make_shared<ast::Col>(col.tab_name, col.name), false));
            }
        }

        for (const auto &item : select_items) {
            SelectExpr expr;
            expr.is_aggregate = item->agg_type != ast::AGG_NONE;
            if (!expr.is_aggregate) {
                expr.col = check_column(all_cols, {item->col->tab_name, item->col->col_name});
                auto meta = sm_manager_->db_.get_table(expr.col.tab_name).get_col(expr.col.col_name);
                expr.output_type = meta->type;
                expr.output_len = meta->len;
                expr.output_name = item->alias.empty() ? expr.col.col_name : item->alias;
                query->cols.push_back(expr.col);
            } else {
                query->has_aggregation = true;
                expr.aggregate.func = convert_agg_type(item->agg_type);
                expr.aggregate.count_star = item->count_star;
                if (item->count_star) {
                    if (expr.aggregate.func != AGG_FUNC_COUNT) {
                        throw RMDBError("Only COUNT supports '*'");
                    }
                } else {
                    expr.aggregate.col = check_column(all_cols, {item->col->tab_name, item->col->col_name});
                    auto meta = sm_manager_->db_.get_table(expr.aggregate.col.tab_name)
                                    .get_col(expr.aggregate.col.col_name);
                    expr.aggregate.input_type = meta->type;
                    expr.aggregate.input_len = meta->len;
                    if (expr.aggregate.func != AGG_FUNC_COUNT && meta->type == TYPE_STRING) {
                        throw IncompatibleTypeError(agg_name(expr.aggregate.func), coltype2str(meta->type));
                    }
                }
                expr.output_type = expr.aggregate.func == AGG_FUNC_COUNT ? TYPE_INT
                    : (expr.aggregate.func == AGG_FUNC_AVG ? TYPE_FLOAT : expr.aggregate.input_type);
                expr.output_len = expr.output_type == TYPE_STRING ? expr.aggregate.input_len : sizeof(int);
                const std::string argument = item->count_star ? "*" : expr.aggregate.col.col_name;
                expr.output_name = item->alias.empty()
                    ? agg_name(expr.aggregate.func) + "(" + argument + ")" : item->alias;
            }
            query->select_exprs.push_back(expr);
            query->output_cols.push_back({"", expr.output_name});
        }

        query->has_aggregation = query->has_aggregation || !query->group_cols.empty() || !x->havings.empty();
        if (query->has_aggregation) {
            for (const auto &expr : query->select_exprs) {
                if (!expr.is_aggregate && std::none_of(query->group_cols.begin(), query->group_cols.end(),
                        [&](const TabCol &group_col) { return same_col(group_col, expr.col); })) {
                    throw RMDBError("Selected column must appear in GROUP BY: " + expr.col.col_name);
                }
            }
        }

        for (const auto &sv_having : x->havings) {
            HavingCondition having;
            ColType result_type;
            if (sv_having->lhs != nullptr) {
                having.aggregate.func = convert_agg_type(sv_having->lhs->agg_type);
                having.aggregate.count_star = sv_having->lhs->count_star;
                if (!having.aggregate.count_star) {
                    having.aggregate.col = check_column(
                        all_cols, {sv_having->lhs->col->tab_name, sv_having->lhs->col->col_name});
                    auto meta = sm_manager_->db_.get_table(having.aggregate.col.tab_name)
                                    .get_col(having.aggregate.col.col_name);
                    having.aggregate.input_type = meta->type;
                    having.aggregate.input_len = meta->len;
                    if (having.aggregate.func != AGG_FUNC_COUNT && meta->type == TYPE_STRING) {
                        throw IncompatibleTypeError(agg_name(having.aggregate.func), coltype2str(meta->type));
                    }
                }
                result_type = having.aggregate.func == AGG_FUNC_COUNT ? TYPE_INT
                    : (having.aggregate.func == AGG_FUNC_AVG ? TYPE_FLOAT : having.aggregate.input_type);
            } else {
                having.is_aggregate = false;
                having.col = check_column(all_cols, {sv_having->lhs_col->tab_name, sv_having->lhs_col->col_name});
                if (std::none_of(query->group_cols.begin(), query->group_cols.end(),
                        [&](const TabCol &group_col) { return same_col(group_col, having.col); })) {
                    throw RMDBError("HAVING column must appear in GROUP BY: " + having.col.col_name);
                }
                auto meta = sm_manager_->db_.get_table(having.col.tab_name).get_col(having.col.col_name);
                having.col_type = meta->type;
                having.col_len = meta->len;
                result_type = meta->type;
            }
            having.op = convert_sv_comp_op(sv_having->op);
            having.rhs = convert_sv_value(sv_having->rhs);
            if (result_type != having.rhs.type && !(is_numeric(result_type) && is_numeric(having.rhs.type))) {
                throw IncompatibleTypeError(coltype2str(result_type), coltype2str(having.rhs.type));
            }
            if (!having.is_aggregate) {
                having.rhs.init_raw(having.rhs.type == TYPE_STRING ? having.col_len : sizeof(int));
            }
            query->having_conds.push_back(std::move(having));
        }

        get_clause(x->conds, query->conds);
        check_clause(query->tables, query->conds);

        for (const auto &sv_order : x->orders) {
            OrderBySpec order;
            order.is_desc = sv_order->orderby_dir == ast::OrderBy_DESC;
            TabCol requested{sv_order->col->tab_name, sv_order->col->col_name};
            if (query->has_aggregation) {
                auto match = std::find_if(query->select_exprs.begin(), query->select_exprs.end(),
                    [&](const SelectExpr &expr) {
                        if (requested.tab_name.empty() && expr.output_name == requested.col_name) return true;
                        return !expr.is_aggregate && same_col(expr.col, requested);
                    });
                if (match == query->select_exprs.end()) {
                    throw ColumnNotFoundError(requested.col_name);
                }
                order.col = {"", match->output_name};
            } else {
                auto alias = std::find_if(query->select_exprs.begin(), query->select_exprs.end(),
                    [&](const SelectExpr &expr) {
                        return requested.tab_name.empty() && expr.output_name == requested.col_name;
                    });
                order.col = alias == query->select_exprs.end() ? check_column(all_cols, requested) : alias->col;
            }
            query->order_bys.push_back(order);
        }

        if (x->limit < -1) {
            throw RMDBError("LIMIT must not be negative");
        }
        query->limit = x->limit;
    } else if (auto x = std::dynamic_pointer_cast<ast::UpdateStmt>(parse)) {
        if (!sm_manager_->db_.is_table(x->tab_name)) throw TableNotFoundError(x->tab_name);
        TabMeta &tab = sm_manager_->db_.get_table(x->tab_name);
        for (const auto &sv_set_clause : x->set_clauses) {
            SetClause set_clause;
            set_clause.lhs = {x->tab_name, sv_set_clause->col_name};
            auto lhs_col = tab.get_col(set_clause.lhs.col_name);
            set_clause.rhs = convert_sv_value(sv_set_clause->val);
            if (lhs_col->type != set_clause.rhs.type) {
                throw IncompatibleTypeError(coltype2str(lhs_col->type), coltype2str(set_clause.rhs.type));
            }
            set_clause.rhs.init_raw(lhs_col->len);
            query->set_clauses.push_back(std::move(set_clause));
        }
        get_clause(x->conds, query->conds);
        check_clause({x->tab_name}, query->conds);
    } else if (auto x = std::dynamic_pointer_cast<ast::DeleteStmt>(parse)) {
        if (!sm_manager_->db_.is_table(x->tab_name)) throw TableNotFoundError(x->tab_name);
        get_clause(x->conds, query->conds);
        check_clause({x->tab_name}, query->conds);
    } else if (auto x = std::dynamic_pointer_cast<ast::InsertStmt>(parse)) {
        if (!sm_manager_->db_.is_table(x->tab_name)) throw TableNotFoundError(x->tab_name);
        const auto &cols = sm_manager_->db_.get_table(x->tab_name).cols;
        if (x->vals.size() != cols.size()) throw InvalidValueCountError();
        for (const auto &sv_val : x->vals) {
            query->values.push_back(convert_sv_value(sv_val));
        }
        for (size_t i = 0; i < query->values.size(); ++i) {
            auto &value = query->values[i];
            const ColType target_type = cols[i].type;
            if (target_type == TYPE_FLOAT && value.type == TYPE_INT) {
                value.set_float(static_cast<float>(value.int_val));
            } else if (target_type == TYPE_INT && value.type == TYPE_FLOAT) {
                value.set_int(static_cast<int>(value.float_val));
            } else if (target_type != value.type) {
                throw IncompatibleTypeError(coltype2str(target_type), coltype2str(value.type));
            }
        }
    }
    query->parse = std::move(parse);
    return query;
}

TabCol Analyze::check_column(const std::vector<ColMeta> &all_cols, TabCol target) {
    if (target.tab_name.empty()) {
        std::string tab_name;
        for (const auto &col : all_cols) {
            if (col.name == target.col_name) {
                if (!tab_name.empty()) throw AmbiguousColumnError(target.col_name);
                tab_name = col.tab_name;
            }
        }
        if (tab_name.empty()) throw ColumnNotFoundError(target.col_name);
        target.tab_name = tab_name;
    } else {
        auto found = std::find_if(all_cols.begin(), all_cols.end(), [&](const ColMeta &col) {
            return col.tab_name == target.tab_name && col.name == target.col_name;
        });
        if (found == all_cols.end()) throw ColumnNotFoundError(target.tab_name + "." + target.col_name);
    }
    return target;
}

void Analyze::get_all_cols(const std::vector<std::string> &tab_names, std::vector<ColMeta> &all_cols) {
    for (const auto &tab_name : tab_names) {
        const auto &cols = sm_manager_->db_.get_table(tab_name).cols;
        all_cols.insert(all_cols.end(), cols.begin(), cols.end());
    }
}

void Analyze::get_clause(const std::vector<std::shared_ptr<ast::BinaryExpr>> &sv_conds,
                         std::vector<Condition> &conds) {
    conds.clear();
    for (const auto &expr : sv_conds) {
        Condition cond;
        cond.lhs_col = {expr->lhs->tab_name, expr->lhs->col_name};
        cond.op = convert_sv_comp_op(expr->op);
        if (auto rhs_val = std::dynamic_pointer_cast<ast::Value>(expr->rhs)) {
            cond.is_rhs_val = true;
            cond.rhs_val = convert_sv_value(rhs_val);
        } else if (auto rhs_col = std::dynamic_pointer_cast<ast::Col>(expr->rhs)) {
            cond.is_rhs_val = false;
            cond.rhs_col = {rhs_col->tab_name, rhs_col->col_name};
        } else {
            throw RMDBError("Aggregate functions are not allowed in WHERE");
        }
        conds.push_back(std::move(cond));
    }
}

void Analyze::check_clause(const std::vector<std::string> &tab_names, std::vector<Condition> &conds) {
    std::vector<ColMeta> all_cols;
    get_all_cols(tab_names, all_cols);
    for (auto &cond : conds) {
        cond.lhs_col = check_column(all_cols, cond.lhs_col);
        if (!cond.is_rhs_val) cond.rhs_col = check_column(all_cols, cond.rhs_col);
        auto lhs_col = sm_manager_->db_.get_table(cond.lhs_col.tab_name).get_col(cond.lhs_col.col_name);
        ColType rhs_type;
        if (cond.is_rhs_val) {
            rhs_type = cond.rhs_val.type;
            const int raw_len = rhs_type == TYPE_STRING ? lhs_col->len : sizeof(int);
            cond.rhs_val.init_raw(raw_len);
        } else {
            rhs_type = sm_manager_->db_.get_table(cond.rhs_col.tab_name).get_col(cond.rhs_col.col_name)->type;
        }
        if (lhs_col->type != rhs_type && !(is_numeric(lhs_col->type) && is_numeric(rhs_type))) {
            throw IncompatibleTypeError(coltype2str(lhs_col->type), coltype2str(rhs_type));
        }
    }
}

Value Analyze::convert_sv_value(const std::shared_ptr<ast::Value> &sv_val) {
    Value val;
    if (auto int_lit = std::dynamic_pointer_cast<ast::IntLit>(sv_val)) {
        val.set_int(int_lit->val);
    } else if (auto float_lit = std::dynamic_pointer_cast<ast::FloatLit>(sv_val)) {
        val.set_float(float_lit->val);
    } else if (auto str_lit = std::dynamic_pointer_cast<ast::StringLit>(sv_val)) {
        val.set_str(str_lit->val);
    } else {
        throw InternalError("Unexpected value type");
    }
    return val;
}

CompOp Analyze::convert_sv_comp_op(ast::SvCompOp op) {
    static const std::map<ast::SvCompOp, CompOp> ops = {
        {ast::SV_OP_EQ, OP_EQ}, {ast::SV_OP_NE, OP_NE}, {ast::SV_OP_LT, OP_LT},
        {ast::SV_OP_GT, OP_GT}, {ast::SV_OP_LE, OP_LE}, {ast::SV_OP_GE, OP_GE},
    };
    return ops.at(op);
}
