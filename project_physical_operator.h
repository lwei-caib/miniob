/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by WangYunlai on 2022/07/01.
//

#pragma once

#include "sql/expr/tuple.h"
#include "sql/operator/physical_operator.h"

/**
 * @brief 选择/投影物理算子
 * @ingroup PhysicalOperator
 */
class ProjectPhysicalOperator : public PhysicalOperator {
 public:
  ProjectPhysicalOperator() {}

  virtual ~ProjectPhysicalOperator() = default;

  void add_expressions(std::vector<std::unique_ptr<Expression>> &&expressions) {}
  void add_projection(const Table *table, const FieldMeta *field);

  PhysicalOperatorType type() const override { return PhysicalOperatorType::PROJECT; }

  RC open(Trx *trx) override;
  RC next() override;
  RC close() override;

  int cell_num() const { return tuple_.cell_num(); }
  std::string name() const override { return create_table_name != "" ? create_table_name : create_view_name; }
  std::string view_name() const {return create_view_name;}
  Tuple *current_tuple() override;
  void set_view_name(std::string name) {create_view_name = name;}
  void set_name(std::string name) { create_table_name = name; }
  const ProjectTuple &get_project_tuple() const { return tuple_; }
  // 为了方便
  std::vector<AttrInfoSqlNode> attrs_;
  std::vector<Table *> tables_;

  std::vector<Expression *> select_expr_;
  bool select_expr_flag_{false};

  bool func_fast_path_{false};

 private:
  ProjectTuple tuple_;
  ValueListTuple expr_tuple_;
  std::string create_table_name{""};
  std::string create_view_name{""};
  bool agg_flag_{false};
  bool fun_fast_path_flag_{false};
};
