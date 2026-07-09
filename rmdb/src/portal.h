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

#include <cerrno>
#include <cstring>
#include <algorithm>
#include <set>
#include <sstream>
#include <string>
#include "optimizer/plan.h"
#include "execution/executor_abstract.h"
#include "execution/executor_nestedloop_join.h"
#include "execution/executor_projection.h"
#include "execution/executor_seq_scan.h"
#include "execution/executor_filter.h"
#include "execution/executor_index_scan.h"
#include "execution/executor_update.h"
#include "execution/executor_insert.h"
#include "execution/executor_delete.h"
#include "execution/execution_sort.h"
#include "common/common.h"

typedef enum portalTag{
    PORTAL_Invalid_Query = 0,
    PORTAL_ONE_SELECT,
    PORTAL_DML_WITHOUT_SELECT,
    PORTAL_MULTI_QUERY,
    PORTAL_CMD_UTILITY
} portalTag;


struct PortalStmt {
    portalTag tag;
    
    std::vector<TabCol> sel_cols;
    std::unique_ptr<AbstractExecutor> root;
    std::shared_ptr<Plan> plan;
    
    PortalStmt(portalTag tag_, std::vector<TabCol> sel_cols_, std::unique_ptr<AbstractExecutor> root_, std::shared_ptr<Plan> plan_) :
            tag(tag_), sel_cols(std::move(sel_cols_)), root(std::move(root_)), plan(std::move(plan_)) {}
};

class Portal
{
   private:
    SmManager *sm_manager_;

    std::string col_to_string(const TabCol &col) {
        return col.tab_name + "." + col.col_name;
    }

    std::string op_to_string(CompOp op) {
        switch (op) {
            case OP_EQ: return "=";
            case OP_NE: return "<>";
            case OP_LT: return "<";
            case OP_GT: return ">";
            case OP_LE: return "<=";
            case OP_GE: return ">=";
        }
        return "";
    }

    std::string value_to_string(const Value &value) {
        std::ostringstream os;
        if (value.type == TYPE_INT) {
            os << value.int_val;
        } else if (value.type == TYPE_FLOAT) {
            os << value.float_val;
        } else {
            os << "'" << value.str_val << "'";
        }
        return os.str();
    }

    std::string cond_to_string(const Condition &cond) {
        std::string rhs = cond.is_rhs_val ? value_to_string(cond.rhs_val) : col_to_string(cond.rhs_col);
        return col_to_string(cond.lhs_col) + op_to_string(cond.op) + rhs;
    }

    std::string join_strings(std::vector<std::string> values) {
        std::sort(values.begin(), values.end());
        std::ostringstream os;
        for (size_t i = 0; i < values.size(); i++) {
            if (i != 0) {
                os << ", ";
            }
            os << values[i];
        }
        return os.str();
    }

    std::vector<std::string> condition_strings(const std::vector<Condition> &conds) {
        std::vector<std::string> values;
        for (auto &cond : conds) {
            values.push_back(cond_to_string(cond));
        }
        std::sort(values.begin(), values.end());
        return values;
    }

    void collect_tables(std::shared_ptr<Plan> plan, std::vector<std::string> &tables) {
        if (auto x = std::dynamic_pointer_cast<ScanPlan>(plan)) {
            tables.push_back(x->tab_name_);
        } else if (auto x = std::dynamic_pointer_cast<FilterPlan>(plan)) {
            collect_tables(x->subplan_, tables);
        } else if (auto x = std::dynamic_pointer_cast<ProjectionPlan>(plan)) {
            collect_tables(x->subplan_, tables);
        } else if (auto x = std::dynamic_pointer_cast<JoinPlan>(plan)) {
            collect_tables(x->left_, tables);
            collect_tables(x->right_, tables);
        } else if (auto x = std::dynamic_pointer_cast<SortPlan>(plan)) {
            collect_tables(x->subplan_, tables);
        }
    }

    void append_indent(std::ostringstream &os, int depth) {
        for (int i = 0; i < depth; i++) {
            os << "\t";
        }
    }

    void explain_node(std::shared_ptr<Plan> plan, AbstractExecutor *exec, int depth, std::ostringstream &os) {
        if (auto x = std::dynamic_pointer_cast<ProjectionPlan>(plan)) {
            append_indent(os, depth);
            if (x->output_star_) {
                os << "Project(columns=[*], rows=" << exec->rows() << ")\n";
            } else {
                std::vector<std::string> cols;
                for (auto &col : x->sel_cols_) {
                    cols.push_back(col_to_string(col));
                }
                os << "Project(columns=[" << join_strings(cols) << "], rows=" << exec->rows() << ")\n";
            }
            auto kids = exec->children();
            explain_node(x->subplan_, kids[0], depth + 1, os);
        } else if (auto x = std::dynamic_pointer_cast<JoinPlan>(plan)) {
            append_indent(os, depth);
            std::vector<std::string> tables;
            collect_tables(plan, tables);
            os << "Join(tables=[" << join_strings(tables) << "], condition=["
               << join_strings(condition_strings(x->conds_)) << "], rows=" << exec->rows() << ")\n";
            auto kids = exec->children();
            explain_node(x->left_, kids[0], depth + 1, os);
            explain_node(x->right_, kids[1], depth + 1, os);
        } else if (auto x = std::dynamic_pointer_cast<FilterPlan>(plan)) {
            append_indent(os, depth);
            os << "Filter(condition=[" << join_strings(condition_strings(x->conds_)) << "], rows=" << exec->rows() << ")\n";
            auto kids = exec->children();
            explain_node(x->subplan_, kids[0], depth + 1, os);
        } else if (auto x = std::dynamic_pointer_cast<ScanPlan>(plan)) {
            if (!x->conds_.empty()) {
                append_indent(os, depth);
                os << "Filter(condition=[" << join_strings(condition_strings(x->conds_)) << "], rows=" << exec->rows() << ")\n";
                depth++;
            }
            append_indent(os, depth);
            os << "Scan(table=" << x->tab_name_ << ", type=SeqScan, rows=" << exec->scanned_rows() << ")\n";
        } else if (auto x = std::dynamic_pointer_cast<SortPlan>(plan)) {
            auto kids = exec->children();
            explain_node(x->subplan_, kids[0], depth, os);
        }
    }

    void run_explain_analyze(std::shared_ptr<PortalStmt> portal, Context *context) {
        portal->root->reset_stats();
        for (portal->root->beginTuple(); !portal->root->is_end(); portal->root->nextTuple()) {
            (void)portal->root->Next();
        }
        auto dml = std::dynamic_pointer_cast<DMLPlan>(portal->plan);
        std::ostringstream os;
        explain_node(dml->subplan_, portal->root.get(), 0, os);
        std::string text = os.str();
        memcpy(context->data_send_ + *(context->offset_), text.c_str(), text.size());
        *(context->offset_) += static_cast<int>(text.size());
        std::fstream outfile;
        outfile.open("output.txt", std::ios::out | std::ios::app);
        outfile << text;
        outfile.close();
    }
    

   public:
    Portal(SmManager *sm_manager) : sm_manager_(sm_manager){}
    ~Portal(){}

    // 将查询执行计划转换成对应的算子树
    std::shared_ptr<PortalStmt> start(std::shared_ptr<Plan> plan, Context *context)
    {
        // 这里可以将select进行拆分，例如：一个select，带有return的select等
        if (auto x = std::dynamic_pointer_cast<OtherPlan>(plan)) {
            return std::make_shared<PortalStmt>(PORTAL_CMD_UTILITY, std::vector<TabCol>(), std::unique_ptr<AbstractExecutor>(),plan);
        } else if(auto x = std::dynamic_pointer_cast<SetKnobPlan>(plan)) {
            return std::make_shared<PortalStmt>(PORTAL_CMD_UTILITY, std::vector<TabCol>(), std::unique_ptr<AbstractExecutor>(), plan); 
        } else if (auto x = std::dynamic_pointer_cast<DDLPlan>(plan)) {
            return std::make_shared<PortalStmt>(PORTAL_MULTI_QUERY, std::vector<TabCol>(), std::unique_ptr<AbstractExecutor>(),plan);
        } else if (auto x = std::dynamic_pointer_cast<DMLPlan>(plan)) {
            switch(x->tag) {
                case T_select:
                {
                    std::shared_ptr<ProjectionPlan> p = std::dynamic_pointer_cast<ProjectionPlan>(x->subplan_);
                    std::unique_ptr<AbstractExecutor> root= convert_plan_executor(p, context);
                    return std::make_shared<PortalStmt>(PORTAL_ONE_SELECT, std::move(p->sel_cols_), std::move(root), plan);
                }
                    
                case T_Update:
                {
                    std::unique_ptr<AbstractExecutor> scan= convert_plan_executor(x->subplan_, context);
                    std::vector<Rid> rids;
                    for (scan->beginTuple(); !scan->is_end(); scan->nextTuple()) {
                        rids.push_back(scan->rid());
                    }
                    std::unique_ptr<AbstractExecutor> root =std::make_unique<UpdateExecutor>(sm_manager_, 
                                                            x->tab_name_, x->set_clauses_, x->conds_, rids, context);
                    return std::make_shared<PortalStmt>(PORTAL_DML_WITHOUT_SELECT, std::vector<TabCol>(), std::move(root), plan);
                }
                case T_Delete:
                {
                    std::unique_ptr<AbstractExecutor> scan= convert_plan_executor(x->subplan_, context);
                    std::vector<Rid> rids;
                    for (scan->beginTuple(); !scan->is_end(); scan->nextTuple()) {
                        rids.push_back(scan->rid());
                    }

                    std::unique_ptr<AbstractExecutor> root =
                        std::make_unique<DeleteExecutor>(sm_manager_, x->tab_name_, x->conds_, rids, context);

                    return std::make_shared<PortalStmt>(PORTAL_DML_WITHOUT_SELECT, std::vector<TabCol>(), std::move(root), plan);
                }

                case T_Insert:
                {
                    std::unique_ptr<AbstractExecutor> root =
                            std::make_unique<InsertExecutor>(sm_manager_, x->tab_name_, x->values_, context);
            
                    return std::make_shared<PortalStmt>(PORTAL_DML_WITHOUT_SELECT, std::vector<TabCol>(), std::move(root), plan);
                }


                default:
                    throw InternalError("Unexpected field type");
                    break;
            }
        } else {
            throw InternalError("Unexpected field type");
        }
        return nullptr;
    }

    // 遍历算子树并执行算子生成执行结果
    void run(std::shared_ptr<PortalStmt> portal, QlManager* ql, txn_id_t *txn_id, Context *context){
        switch(portal->tag) {
            case PORTAL_ONE_SELECT:
            {
                auto dml = std::dynamic_pointer_cast<DMLPlan>(portal->plan);
                if (dml != nullptr && dml->explain_analyze_) {
                    run_explain_analyze(portal, context);
                } else {
                    ql->select_from(std::move(portal->root), std::move(portal->sel_cols), context);
                }
                break;
            }

            case PORTAL_DML_WITHOUT_SELECT:
            {
                ql->run_dml(std::move(portal->root));
                break;
            }
            case PORTAL_MULTI_QUERY:
            {
                ql->run_mutli_query(portal->plan, context);
                break;
            }
            case PORTAL_CMD_UTILITY:
            {
                ql->run_cmd_utility(portal->plan, txn_id, context);
                break;
            }
            default:
            {
                throw InternalError("Unexpected field type");
            }
        }
    }

    // 清空资源
    void drop(){}


    std::unique_ptr<AbstractExecutor> convert_plan_executor(std::shared_ptr<Plan> plan, Context *context)
    {
        if(auto x = std::dynamic_pointer_cast<ProjectionPlan>(plan)){
            return std::make_unique<ProjectionExecutor>(convert_plan_executor(x->subplan_, context),
                                                        x->sel_cols_);
        } else if(auto x = std::dynamic_pointer_cast<FilterPlan>(plan)) {
            return std::make_unique<FilterExecutor>(convert_plan_executor(x->subplan_, context), x->conds_);
        } else if(auto x = std::dynamic_pointer_cast<ScanPlan>(plan)) {
            if(x->tag == T_SeqScan) {
                return std::make_unique<SeqScanExecutor>(sm_manager_, x->tab_name_, x->conds_, context);
            }
            else {
                return std::make_unique<IndexScanExecutor>(sm_manager_, x->tab_name_, x->conds_, x->index_col_names_, context);
            } 
        } else if(auto x = std::dynamic_pointer_cast<JoinPlan>(plan)) {
            std::unique_ptr<AbstractExecutor> left = convert_plan_executor(x->left_, context);
            std::unique_ptr<AbstractExecutor> right = convert_plan_executor(x->right_, context);
            std::unique_ptr<AbstractExecutor> join = std::make_unique<NestedLoopJoinExecutor>(
                                std::move(left), 
                                std::move(right), std::move(x->conds_));
            return join;
        } else if(auto x = std::dynamic_pointer_cast<SortPlan>(plan)) {
            return std::make_unique<SortExecutor>(convert_plan_executor(x->subplan_, context), 
                                            x->sel_col_, x->is_desc_);
        }
        return nullptr;
    }

};
