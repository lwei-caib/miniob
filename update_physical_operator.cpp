#include "sql/operator/update_physical_operator.h"
#include "common/log/log.h"
#include "event/sql_debug.h"
#include "sql/stmt/delete_stmt.h"
#include "storage/record/record.h"
#include "storage/table/table.h"
#include "storage/trx/trx.h"

RC UpdatePhysicalOperator::open(Trx *trx) {
  if (children_.empty()) {
    return RC::SUCCESS;
  }
  std::unique_ptr<PhysicalOperator> &child = children_[0];
  RC rc = child->open(trx);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to open child operator: %s", strrc(rc));
    return rc;
  }
  trx_ = trx;

  return RC::SUCCESS;
}

RC UpdatePhysicalOperator::next() {
  RC rc;
  if (children_.empty()) {
    sql_debug("update: no child operator");
    return RC::RECORD_EOF;
  }

  PhysicalOperator *child = children_[0].get();
  const TableMeta &table_meta = table_->table_meta();
  while (RC::SUCCESS == (rc = child->next())) {
    sql_debug("update: get next record");
    Tuple *tuple = child->current_tuple();
    if (nullptr == tuple) {
      sql_debug("update: no tuple");
      LOG_WARN("failed to get current record: %s", strrc(rc));
      return rc;
    }

    auto *row_tuple = dynamic_cast<RowTuple *>(tuple);
    Record old_record = row_tuple->record();
    int record_size = table_meta.record_size();
    char *data = new char[record_size];
    memcpy(data, old_record.data(), record_size);

    for (size_t i = 0; i < values_.size(); ++i) {
      const Value &value = values_[i];
      const FieldMeta &field_meta = field_metas_[i];
      int offset = field_meta.offset();
      if (value.length() > field_meta.len()) {
        sql_debug("update: value length is too long: %d, %d", value.length(), field_meta.len());
        LOG_WARN("value length is too long: %d, %d", value.length(), field_meta.len());
        return RC::INVALID_ARGUMENT;
      }
      for (int j = 0; j < value.length(); ++j) {
        if (value.attr_type() != TEXT) {
          data[offset + j] = value.data()[j];
        } else {
          if (islower(value.data()[j])) {
            data[offset + j] = value.data()[j] - 32;
          } else {
            data[offset + j] = value.data()[j];
          }
        }
      }
      if ((value.attr_type() == AttrType::CHARS || value.attr_type() == AttrType::TEXT) &&
          value.length() < field_meta.len()) {
        data[offset + value.length()] = '\0';
      }
    }
    Record new_record;
    new_record.set_rid(old_record.rid());
    new_record.set_data(data, record_size);
    
    rc = table_->update_record(old_record, new_record);
    if (rc != RC::SUCCESS) {
      sql_debug("update: failed to insert record: %s", strrc(rc));
      LOG_WARN("failed to update:delete record: %s", strrc(rc));
      return rc;
    }
  }
  sql_debug("update: next: %s", strrc(rc));
  return RC::RECORD_EOF;
}

RC UpdatePhysicalOperator::close() {
  if (!children_.empty()) {
    children_[0]->close();
  }
  return RC::SUCCESS;
}
