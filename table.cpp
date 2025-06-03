/* Copyright (c) 2021 Xie Meiyi(xiemeiyi@hust.edu.cn) and OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by Meiyi & Wangyunlai on 2021/5/13.
//

#include <limits.h>
#include <string.h>
#include <algorithm>

#include "common/defs.h"
#include "common/lang/string.h"
#include "common/log/log.h"
#include "storage/buffer/disk_buffer_pool.h"
#include "storage/common/condition_filter.h"
#include "storage/common/meta_util.h"
#include "storage/index/bplus_tree_index.h"
#include "storage/index/index.h"
#include "storage/persist/persist.h"
#include "storage/record/record_manager.h"
#include "storage/table/table.h"
#include "storage/table/table_meta.h"
#include "table_view.h"
#include "sql/operator/project_physical_operator.h"
#include "storage/trx/trx.h"
#include "sql/expr/tuple.h"
#include "storage/table/table_meta.h"


Table::~Table() {
  if (record_handler_ != nullptr) {
    delete record_handler_;
    record_handler_ = nullptr;
  }

  if (data_buffer_pool_ != nullptr) {
    data_buffer_pool_->close_file();
    data_buffer_pool_ = nullptr;
  }

  for (std::vector<Index *>::iterator it = indexes_.begin(); it != indexes_.end(); ++it) {
    Index *index = *it;
    delete index;
  }
  indexes_.clear();

  LOG_INFO("Table has been closed: %s", name());
}

RC Table::create(int32_t table_id, const char *path, const char *name, const char *base_dir, int attribute_count,
                 const AttrInfoSqlNode attributes[]) {
  if (table_id < 0) {
    LOG_WARN("invalid table id. table_id=%d, table_name=%s", table_id, name);
    return RC::INVALID_ARGUMENT;
  }

  if (common::is_blank(name)) {
    LOG_WARN("Name cannot be empty");
    return RC::INVALID_ARGUMENT;
  }
  LOG_INFO("Begin to create table %s:%s", base_dir, name);

  if (attribute_count <= 0 || nullptr == attributes) {
    LOG_WARN("Invalid arguments. table_name=%s, attribute_count=%d, attributes=%p", name, attribute_count, attributes);
    return RC::INVALID_ARGUMENT;
  }

  RC rc = RC::SUCCESS;

  // 使用 table_name.table记录一个表的元数据
  // 判断表文件是否已经存在
  int fd = ::open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) {
    if (EEXIST == errno) {
      LOG_ERROR("Failed to create table file, it has been created. %s, EEXIST, %s", path, strerror(errno));
      return RC::SCHEMA_TABLE_EXIST;
    }
    LOG_ERROR("Create table file failed. filename=%s, errmsg=%d:%s", path, errno, strerror(errno));
    return RC::IOERR_OPEN;
  }

  close(fd);

  // 创建文件
  if ((rc = table_meta_.init(table_id, name, attribute_count, attributes)) != RC::SUCCESS) {
    LOG_ERROR("Failed to init table meta. name:%s, ret:%d", name, rc);
    return rc;  // delete table file
  }

  std::fstream fs;
  fs.open(path, std::ios_base::out | std::ios_base::binary);
  if (!fs.is_open()) {
    LOG_ERROR("Failed to open file for write. file name=%s, errmsg=%s", path, strerror(errno));
    return RC::IOERR_OPEN;
  }

  // 记录元数据到文件中
  table_meta_.serialize(fs);
  fs.close();

  std::string data_file = table_data_file(base_dir, name);
  BufferPoolManager &bpm = BufferPoolManager::instance();
  rc = bpm.create_file(data_file.c_str());
  if (rc != RC::SUCCESS) {
    LOG_ERROR("Failed to create disk buffer pool of data file. file name=%s", data_file.c_str());
    return rc;
  }

  rc = init_record_handler(base_dir);
  if (rc != RC::SUCCESS) {
    LOG_ERROR("Failed to create table %s due to init record handler failed.", data_file.c_str());
    // don't need to remove the data_file
    return rc;
  }

  base_dir_ = base_dir;
  LOG_INFO("Successfully create table %s:%s", base_dir, name);
  return rc;
}

RC Table::open(const char *meta_file, const char *base_dir) {
  // 加载元数据文件
  std::fstream fs;
  std::string meta_file_path = std::string(base_dir) + common::FILE_PATH_SPLIT_STR + meta_file;
  fs.open(meta_file_path, std::ios_base::in | std::ios_base::binary);
  if (!fs.is_open()) {
    LOG_ERROR("Failed to open meta file for read. file name=%s, errmsg=%s", meta_file_path.c_str(), strerror(errno));
    return RC::IOERR_OPEN;
  }
  if (table_meta_.deserialize(fs) < 0) {
    LOG_ERROR("Failed to deserialize table meta. file name=%s", meta_file_path.c_str());
    fs.close();
    return RC::INTERNAL;
  }
  fs.close();

  // 加载数据文件
  RC rc = init_record_handler(base_dir);
  if (rc != RC::SUCCESS) {
    LOG_ERROR("Failed to open table %s due to init record handler failed.", base_dir);
    // don't need to remove the data_file
    return rc;
  }

  base_dir_ = base_dir;

  const int index_num = table_meta_.index_num();
  for (int i = 0; i < index_num; i++) {
    const IndexMeta *index_meta = table_meta_.index(i);
    std::vector<const FieldMeta *> field_meta = table_meta_.fields(index_meta->fields());
    if (field_meta.size() == 0) {
      LOG_ERROR("Found invalid index meta info which has a non-exists field. table=%s, index=%s",
                name(), index_meta->name());
      // skip cleanup
      //  do all cleanup action in destructive Table function
      return RC::INTERNAL;
    }

    BplusTreeIndex *index = new BplusTreeIndex();
    std::string index_file = table_index_file(base_dir, name(), index_meta->name());
    rc = index->open(index_file.c_str(), *index_meta, field_meta);
    if (rc != RC::SUCCESS) {
      delete index;
      LOG_ERROR("Failed to open index. table=%s, index=%s, file=%s, rc=%s",
                name(), index_meta->name(), index_file.c_str(), strrc(rc));
      // skip cleanup
      //  do all cleanup action in destructive Table function.
      return rc;
    }
    indexes_.push_back(index);
  }

  return rc;
}
RC Table::delete_table(const char *path, const char *base_dir, const char *name) {
  std::string data_file = table_data_file(base_dir, name);
  RC rc1 = PersistHandler().remove_file(path);
  RC rc2 = PersistHandler().remove_file(data_file.c_str());
  if (OB_SUCC(rc1) && OB_SUCC(rc2)) {
    return RC::SUCCESS;
  } else {
    LOG_WARN("table not exist");
    return RC::SCHEMA_TABLE_NOT_EXIST;
  }
}

void Table::insert_map_into_tables(Record& record) {
  
  // std::unordered_map<Table*, RID> table_rid_map;
  // std::unordered_map<Table*, const FieldMeta*> table_field_map;
  RC rc = RC::SUCCESS;
  std::unordered_map<Table*, std::vector<pair<const FieldMeta*, Value>>> table_result_map;
  RowTuple tuple;
  tuple.set_record(&record);
  tuple.set_schema(this, table_meta_.field_metas());
  for(int i = 0; i < tuple.cell_num(); i++) {
    Value val;
    rc = tuple.cell_at(i, val);
    if(OB_FAIL(rc)) {
      LOG_ERROR("boring tuple cell at fail");
    }
    table_result_map[meta_.tables[i]].push_back({meta_.fields[i], val});
  }
  for(auto&[table, vec] : table_result_map) {
    Record record;
    rc = table->make_record_by_values(vec, record);
    if(OB_FAIL(rc)) {
      LOG_ERROR("boring make_record_by_values fail");
    }
    rc = table->insert_record(record);
    if(OB_FAIL(rc)) {
      LOG_ERROR("boring insert record tuple fail");
    }
  }
}

RC Table::insert_record(Record &record) {
  if(view_table_flag) {
    ProjectPhysicalOperator* project_oper = dynamic_cast<ProjectPhysicalOperator*>(view_rebuild_map[this->name()].get());
    assert(project_oper != nullptr);
    if(project_oper->select_expr_flag_) {
      //目前更新对于有表达式都是不允许的
      return RC::FILE_NOT_EXIST;
    }
    insert_map_into_tables(record);
  }
  RC rc = RC::SUCCESS;
  rc = record_handler_->insert_record(record.data(), table_meta_.record_size(), &record.rid());
  if (rc != RC::SUCCESS) {
    LOG_ERROR("Insert record failed. table name=%s, rc=%s", table_meta_.name(), strrc(rc));
    return rc;
  }

  rc = insert_entry_of_indexes(record.data(), record.rid());
  if (rc != RC::SUCCESS) {  // 可能出现了键值重复
    RC rc2 = delete_entry_of_indexes(record.data(), record.rid(), false /*error_on_not_exists*/);
    if (rc2 != RC::SUCCESS) {
      LOG_ERROR("Failed to rollback index data when insert index entries failed. table name=%s, rc=%d:%s",
                name(), rc2, strrc(rc2));
    }
    rc2 = record_handler_->delete_record(&record.rid());
    if (rc2 != RC::SUCCESS) {
      LOG_PANIC("Failed to rollback record data when insert index entries failed. table name=%s, rc=%d:%s",
                name(), rc2, strrc(rc2));
    }
    LOG_ERROR("duplicate key");
  }
  return rc;
}

RC Table::visit_record(const RID &rid, bool readonly, std::function<void(Record &)> visitor) {
  return record_handler_->visit_record(rid, readonly, visitor);
}

RC Table::get_record(const RID &rid, Record &record) {
  const int record_size = table_meta_.record_size();
  char *record_data = (char *)malloc(record_size);
  ASSERT(nullptr != record_data, "failed to malloc memory. record data size=%d", record_size);

  auto copier = [&record, record_data, record_size](Record &record_src) {
    memcpy(record_data, record_src.data(), record_size);
    record.set_rid(record_src.rid());
  };
  RC rc = record_handler_->visit_record(rid, true /*readonly*/, copier);
  if (rc != RC::SUCCESS) {
    free(record_data);
    LOG_WARN("failed to visit record. rid=%s, table=%s, rc=%s", rid.to_string().c_str(), name(), strrc(rc));
    return rc;
  }

  record.set_data_owner(record_data, record_size);
  return rc;
}

RC Table::recover_insert_record(Record &record) {
  RC rc = RC::SUCCESS;
  rc = record_handler_->recover_insert_record(record.data(), table_meta_.record_size(), record.rid());
  if (rc != RC::SUCCESS) {
    LOG_ERROR("Insert record failed. table name=%s, rc=%s", table_meta_.name(), strrc(rc));
    return rc;
  }

  rc = insert_entry_of_indexes(record.data(), record.rid());
  if (rc != RC::SUCCESS) {  // 可能出现了键值重复
    RC rc2 = delete_entry_of_indexes(record.data(), record.rid(), false /*error_on_not_exists*/);
    if (rc2 != RC::SUCCESS) {
      LOG_ERROR("Failed to rollback index data when insert index entries failed. table name=%s, rc=%d:%s",
                name(), rc2, strrc(rc2));
    }
    rc2 = record_handler_->delete_record(&record.rid());
    if (rc2 != RC::SUCCESS) {
      LOG_PANIC("Failed to rollback record data when insert index entries failed. table name=%s, rc=%d:%s",
                name(), rc2, strrc(rc2));
    }
  }
  return rc;
}

const char *Table::name() const { return table_meta_.name(); }

const TableMeta &Table::table_meta() const { return table_meta_; }
RC Table::make_record_update_new(std::vector<std::pair<const FieldMeta*, Value>> vec, Record& record, const RID& rid) {
  std::vector<Value> vals;
  Record old_record;
  this->get_record(rid, old_record);
  RowTuple old_tuple;
  RC rc = RC::SUCCESS;
  old_tuple.set_record(&old_record);
  old_tuple.set_schema(this, table_meta_.field_metas());
  for(int i = table_meta_.sys_field_num(); i < table_meta_.field_num(); i++) {
    const FieldMeta* field = table_meta_.field(i);
    bool is_null = true;
    for(int j = 0; j < vec.size(); j++) {
      if(field == vec[j].first) {
        vals.push_back(vec[j].second);
        is_null = false;
        break;
      }
    }
    if(is_null) {
      Value val;
      rc = old_tuple.cell_at(i - table_meta_.sys_field_num(), val);
      vals.push_back(val);
      if(OB_FAIL(rc)) {
        return rc;
      }
    }
  }
  return make_record(vals.size(), vals.data(), record);  
}
// 缺少的用null值填充
RC Table::make_record_by_values(std::vector<pair<const FieldMeta*, Value>> vec, Record& record) {
  std::vector<Value> vals;
  for(int i = table_meta_.sys_field_num(); i < table_meta_.field_num(); i++) {
    const FieldMeta* field = table_meta_.field(i);
    bool is_null = true;
    for(int j = 0; j < vec.size(); j++) {
      if(field == vec[j].first) {
        vals.push_back(vec[j].second);
        is_null = false;
        break;
      }
    }
    if(is_null) {
      Value val;
      Value::set_null(val, field->type());
      vals.push_back(val);
    }
  }
  return make_record(vals.size(), vals.data(), record);
}
RC Table::make_record(int value_num, const Value *values, Record &record) {
  // 检查字段类型是否一致
  if (value_num + table_meta_.sys_field_num() != table_meta_.field_num()) {
    LOG_WARN("Input values don't match the table's schema, table name:%s", table_meta_.name());
    return RC::SCHEMA_FIELD_MISSING;
  }

  const int normal_field_start_index = table_meta_.sys_field_num();
  for (int i = 0; i < value_num; i++) {
    const FieldMeta *field = table_meta_.field(i + normal_field_start_index);
    const Value &value = values[i];
    // std::cout << "[Table::make_record] current value: " << value.to_string() << std::endl;
    if (field->type() != value.attr_type()) {
      LOG_ERROR("Invalid value type. table name =%s, field name=%s, type=%d, but given=%d",
                table_meta_.name(), field->name(), field->type(), value.attr_type());
      return RC::SCHEMA_FIELD_TYPE_MISMATCH;
    }
  }

  // 复制所有字段的值
  int record_size = table_meta_.record_size();
  // std::cout << "[Table::make_record] current record_size: " << record_size << std::endl;
  char *record_data = (char *)malloc(sizeof(char) * record_size);

  for (int i = 0; i < value_num; i++) {
    const FieldMeta *field = table_meta_.field(i + normal_field_start_index);
    const Value &value = values[i];
    size_t copy_len = field->len();
    if (field->type() == TEXT && strlen(value.get_string().c_str()) > 65535) {
      return RC::VARIABLE_NOT_VALID;
    }
    if (field->type() == CHARS || field->type() == TEXT) {
      const size_t data_len = value.length();
      if (copy_len > data_len) {
        copy_len = data_len + 1;
      }
    }
    // Invalid date value
    if (field->type() == DATE && value.get_date() == -1) {
      return RC::VARIABLE_NOT_VALID;
    }
    if (field->type() != TEXT) {
      memcpy(record_data + field->offset(), value.data(), copy_len);
    } else {
      for (int i = 0; i < copy_len; i++) {
        if (islower(value.data()[i])) {
          record_data[field->offset() + i] = value.data()[i] - 32;
        } else {
          record_data[field->offset() + i] = value.data()[i];
        }
      }
    }
  }

  record.set_data_owner(record_data, record_size);
  return RC::SUCCESS;
}

RC Table::init_record_handler(const char *base_dir) {
  std::string data_file = table_data_file(base_dir, table_meta_.name());

  RC rc = BufferPoolManager::instance().open_file(data_file.c_str(), data_buffer_pool_);
  if (rc != RC::SUCCESS) {
    LOG_ERROR("Failed to open disk buffer pool for file:%s. rc=%d:%s", data_file.c_str(), rc, strrc(rc));
    return rc;
  }

  record_handler_ = new RecordFileHandler();
  rc = record_handler_->init(data_buffer_pool_);
  if (rc != RC::SUCCESS) {
    LOG_ERROR("Failed to init record handler. rc=%s", strrc(rc));
    data_buffer_pool_->close_file();
    data_buffer_pool_ = nullptr;
    delete record_handler_;
    record_handler_ = nullptr;
    return rc;
  }

  return rc;
}

RC Table::get_record_scanner(RecordFileScanner &scanner, Trx *trx, bool readonly) {
  RC rc = scanner.open_scan(this, *data_buffer_pool_, trx, readonly, nullptr);
  if (rc != RC::SUCCESS) {
    LOG_ERROR("failed to open scanner. rc=%s", strrc(rc));
  }
  return rc;
}

RC Table::create_index(Trx *trx, std::vector<const FieldMeta *> field_meta, const char *index_name, bool unique) {
  if (common::is_blank(index_name) || field_meta.size() == 0) {
    LOG_INFO("Invalid input arguments, table name is %s, index_name is blank or attribute_name is blank", name());
    return RC::INVALID_ARGUMENT;
  }

  IndexMeta new_index_meta;
  RC rc = new_index_meta.init(index_name, field_meta, unique);
  if (rc != RC::SUCCESS) {
    LOG_INFO("Failed to init IndexMeta in table:%s, index_name:%s", 
             name(), index_name);
    return rc;
  }

  // 创建索引相关数据
  BplusTreeIndex *index = new BplusTreeIndex();
  std::string index_file = table_index_file(base_dir_.c_str(), name(), index_name);
  rc = index->create(index_file.c_str(), new_index_meta, field_meta);
  if (rc != RC::SUCCESS) {
    delete index;
    LOG_ERROR("Failed to create bplus tree index. file name=%s, rc=%d:%s", index_file.c_str(), rc, strrc(rc));
    return rc;
  }

  // 遍历当前的所有数据，插入这个索引
  RecordFileScanner scanner;
  rc = get_record_scanner(scanner, trx, true /*readonly*/);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to create scanner while creating index. table=%s, index=%s, rc=%s", 
             name(), index_name, strrc(rc));
    return rc;
  }

  Record record;
  while (scanner.has_next()) {
    rc = scanner.next(record);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to scan records while creating index. table=%s, index=%s, rc=%s",
               name(), index_name, strrc(rc));
      return rc;
    }
    rc = index->insert_entry(record.data(), &record.rid());
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to insert record into index while creating index. table=%s, index=%s, rc=%s",
               name(), index_name, strrc(rc));
      return rc;
    }
  }
  scanner.close_scan();
  LOG_INFO("inserted all records into new index. table=%s, index=%s", name(), index_name);

  indexes_.push_back(index);

  /// 接下来将这个索引放到表的元数据中
  TableMeta new_table_meta(table_meta_);
  rc = new_table_meta.add_index(new_index_meta);
  if (rc != RC::SUCCESS) {
    LOG_ERROR("Failed to add index (%s) on table (%s). error=%d:%s", index_name, name(), rc, strrc(rc));
    return rc;
  }

  /// 内存中有一份元数据，磁盘文件也有一份元数据。修改磁盘文件时，先创建一个临时文件，写入完成后再rename为正式文件
  /// 这样可以防止文件内容不完整
  // 创建元数据临时文件
  std::string tmp_file = table_meta_file(base_dir_.c_str(), name()) + ".tmp";
  std::fstream fs;
  fs.open(tmp_file, std::ios_base::out | std::ios_base::binary | std::ios_base::trunc);
  if (!fs.is_open()) {
    LOG_ERROR("Failed to open file for write. file name=%s, errmsg=%s", tmp_file.c_str(), strerror(errno));
    return RC::IOERR_OPEN;  // 创建索引中途出错，要做还原操作
  }
  if (new_table_meta.serialize(fs) < 0) {
    LOG_ERROR("Failed to dump new table meta to file: %s. sys err=%d:%s", tmp_file.c_str(), errno, strerror(errno));
    return RC::IOERR_WRITE;
  }
  fs.close();

  // 覆盖原始元数据文件
  std::string meta_file = table_meta_file(base_dir_.c_str(), name());
  int ret = rename(tmp_file.c_str(), meta_file.c_str());
  if (ret != 0) {
    LOG_ERROR("Failed to rename tmp meta file (%s) to normal meta file (%s) while creating index (%s) on table (%s). "
              "system error=%d:%s",
              tmp_file.c_str(), meta_file.c_str(), index_name, name(), errno, strerror(errno));
    return RC::IOERR_WRITE;
  }

  table_meta_.swap(new_table_meta);

  LOG_INFO("Successfully added a new index (%s) on the table (%s)", index_name, name());
  return rc;
}

RC Table::delete_record(const Record &record) {
  RC rc = RC::SUCCESS;
  if(view_table_flag) {
    if(!meta_.updatable) {
      return RC::UNIMPLENMENT;
    }
    ProjectPhysicalOperator* project_oper = dynamic_cast<ProjectPhysicalOperator*>(view_rebuild_map[this->name()].get());
    assert(project_oper != nullptr);
    std::unordered_set<Table*> table_deleted;
    const auto& rid_vec = meta_.rid_map[record.rid()];
    for(int i = 0; i < rid_vec.size(); i++) {
      if(table_deleted.count(meta_.tables[i]) == 0) {
        table_deleted.insert(meta_.tables[i]);
        Record record;
        rc =  meta_.tables[i]->get_record(rid_vec[i], record);
        if(OB_FAIL(rc)) {
          LOG_ERROR("boring get view record error");
          return rc;
        }
        rc =  meta_.tables[i]->delete_record(record);
        if(OB_FAIL(rc)) {
          LOG_ERROR("boring delete view record error");
          return rc;
        }
      }
    }
  }
  for (Index *index : indexes_) {
    rc = index->delete_entry(record.data(), &record.rid());
    ASSERT(RC::SUCCESS == rc,
           "failed to delete entry from index. table name=%s, index name=%s, rid=%s, rc=%s",
           name(),
           index->index_meta().name(),
           record.rid().to_string().c_str(),
           strrc(rc));
  }
  rc = record_handler_->delete_record(&record.rid());
  return rc;
}

RC Table::insert_entry_of_indexes(const char *record, const RID &rid) {
  RC rc = RC::SUCCESS;
  for (Index *index : indexes_) {
    rc = index->insert_entry(record, &rid);
    if (rc != RC::SUCCESS) {
      break;
    }
  }
  return rc;
}

RC Table::delete_entry_of_indexes(const char *record, const RID &rid, bool error_on_not_exists) {
  RC rc = RC::SUCCESS;
  for (Index *index : indexes_) {
    rc = index->delete_entry(record, &rid);
    if (rc != RC::SUCCESS) {
      if (rc != RC::RECORD_INVALID_KEY || !error_on_not_exists) {
        break;
      }
    }
  }
  return rc;
}

Index *Table::find_index(const char *index_name) const {
  for (Index *index : indexes_) {
    if (0 == strcmp(index->index_meta().name(), index_name)) {
      return index;
    }
  }
  return nullptr;
}
Index *Table::find_index_by_field(const char *field_name) const {
  const TableMeta &table_meta = this->table_meta();
  std::vector<string> field_names;
  field_names.push_back(field_name);
  const IndexMeta *index_meta = table_meta.find_index_by_field(field_names);
  if (index_meta != nullptr) {
    return this->find_index(index_meta->name());
  }
  return nullptr;
}

RC Table::sync() {
  RC rc = RC::SUCCESS;
  for (Index *index : indexes_) {
    rc = index->sync();
    if (rc != RC::SUCCESS) {
      LOG_ERROR("Failed to flush index's pages. table=%s, index=%s, rc=%d:%s",
          name(),
          index->index_meta().name(),
          rc,
          strrc(rc));
      return rc;
    }
  }
  LOG_INFO("Sync table over. table=%s", name());
  return rc;
}
RC Table::update_record_real_records(const Record &old_record, Record &new_record) {
  RC rc = RC::SUCCESS;
  std::unordered_map<Table*, std::vector<pair<const FieldMeta*, Value>>> table_result_map;
  std::unordered_map<Table*, RID> table_rid_map;
  RowTuple new_tuple;
  new_tuple.set_record(&new_record);
  new_tuple.set_schema(this, table_meta_.field_metas());
  LOG_TRACE("boring update enter");
  for(int i = 0; i < new_tuple.cell_num(); i++) {
    Value val;
    rc = new_tuple.cell_at(i, val);
    if(OB_FAIL(rc)) {
      LOG_ERROR("boring tuple cell at fail");
      return rc;
    }
    table_result_map[meta_.tables[i]].push_back({meta_.fields[i], val});
    if(table_rid_map.count(meta_.tables[i]) == 0) {
      table_rid_map[meta_.tables[i]]  = meta_.rid_map[old_record.rid()][i];
    }
  }
  for(auto&[table, vec] : table_result_map) {
    Record the_new_record;
    rc = table->make_record_update_new(vec, the_new_record, table_rid_map[table]);
    if(OB_FAIL(rc)) {
      LOG_ERROR("boring make_record_by_values fail");
      return rc;
    }
    Record the_old_record;
    rc = table->get_record(table_rid_map[table], the_old_record);
    if(OB_FAIL(rc)) {
      LOG_ERROR("boring insert record tuple fail");
      return rc;
    }    
    the_new_record.set_rid(the_old_record.rid());
    LOG_ERROR("boring %d  %d %d", the_old_record.rid().page_num, the_new_record.rid().page_num, table_rid_map[table].page_num);
    LOG_ERROR("boring %d  %d %d", the_old_record.rid().slot_num, the_new_record.rid().slot_num, table_rid_map[table].slot_num);
    rc = table->update_record(the_old_record, the_new_record);
    if(OB_FAIL(rc)) {
      LOG_ERROR("boring insert record tuple fail");
      return rc;
    }
  }
  return RC::SUCCESS;
}
RC Table::update_record(const Record &old_record, Record &new_record) {

  RC rc;
  if(view_table_flag) {
    ProjectPhysicalOperator* project_oper = dynamic_cast<ProjectPhysicalOperator*>(view_rebuild_map[this->name()].get());
    assert(project_oper != nullptr);
    if(!meta_.updatable) {
      return RC::UNIMPLENMENT;
    }
    rc = update_record_real_records(old_record, new_record);
  }
  assert(old_record.rid() == new_record.rid());
  rc = delete_entry_of_indexes(old_record.data(), old_record.rid(), false /*error_on_not_exists*/);
  if (rc != RC::SUCCESS) {
    LOG_ERROR("Failed to delete entry of indexes. table=%s, rc=%d:%s", name(), rc, strrc(rc));
    return rc;
  }

  // TODO(2023): check unique here
  rc = insert_entry_of_indexes(new_record.data(), new_record.rid());
  if (rc != RC::SUCCESS) {
    LOG_ERROR("Failed to insert entry of indexes. table=%s, rc=%d:%s", name(), rc, strrc(rc));
    return rc;
  }

  rc = record_handler_->update_record(old_record, new_record);
  if (rc != RC::SUCCESS) {
    LOG_ERROR("Failed to update record. table=%s, rc=%d:%s", name(), rc, strrc(rc));
    return rc;
  }
  return rc;
}
