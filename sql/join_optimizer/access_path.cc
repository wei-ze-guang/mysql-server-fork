/* Copyright (c) 2020, 2026, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is designed to work with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have either included with
   the program or referenced in the documentation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "sql/join_optimizer/access_path.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "mem_root_deque.h"
#include "my_base.h"
#include "my_dbug.h"
#include "mysql/components/services/bits/psi_bits.h"
#include "prealloced_array.h"
#include "sql/command_mapping.h"  // get_sql_command_string
#include "sql/field.h"
#include "sql/filesort.h"
#include "sql/handler.h"
#include "sql/item_cmpfunc.h"
#include "sql/item_func.h"
#include "sql/item_sum.h"
#include "sql/item_subselect.h"
#include "sql/iterators/basic_row_iterators.h"
#include "sql/iterators/bka_iterator.h"
#include "sql/iterators/composite_iterators.h"
#include "sql/iterators/delete_rows_iterator.h"
#include "sql/iterators/hash_join_iterator.h"
#include "sql/iterators/ref_row_iterators.h"
#include "sql/iterators/row_iterator.h"
#include "sql/iterators/sorting_iterator.h"
#include "sql/iterators/timing_iterator.h"
#include "sql/iterators/window_iterators.h"
#include "sql/join_optimizer/bit_utils.h"
#include "sql/join_optimizer/cost_model.h"
#include "sql/join_optimizer/estimate_selectivity.h"
#include "sql/join_optimizer/overflow_bitset.h"
#include "sql/join_optimizer/relational_expression.h"
#include "sql/join_optimizer/walk_access_paths.h"
#include "sql/mem_root_array.h"
#include "sql/pack_rows.h"
#include "sql/range_optimizer/geometry_index_range_scan.h"
#include "sql/range_optimizer/group_index_skip_scan.h"
#include "sql/range_optimizer/group_index_skip_scan_plan.h"
#include "sql/range_optimizer/index_merge.h"
#include "sql/range_optimizer/index_range_scan.h"
#include "sql/range_optimizer/index_skip_scan.h"
#include "sql/range_optimizer/index_skip_scan_plan.h"
#include "sql/range_optimizer/internal.h"
#include "sql/range_optimizer/range_optimizer.h"
#include "sql/range_optimizer/reverse_index_range_scan.h"
#include "sql/range_optimizer/rowid_ordered_retrieval.h"
#include "sql/sql_array.h"
#include "sql/sql_const.h"
#include "sql/sql_executor.h"
#include "sql/sql_lex.h"
#include "sql/sql_list.h"
#include "sql/sql_opt_exec_shared.h"
#include "sql/sql_optimizer.h"
#include "sql/sql_update.h"
#include "sql/system_variables.h"
#include "sql/table.h"
#include "sql/visible_fields.h"
#include "sql/window.h"
#include "sql/wzg_probe/wzg_probe.h"
#include "template_utils.h"

using pack_rows::TableCollection;
using std::all_of;
using std::vector;

AccessPath *NewSortAccessPath(THD *thd, AccessPath *child, Filesort *filesort,
                              ORDER *order, bool count_examined_rows) {
  assert(child != nullptr);
  assert(filesort != nullptr);
  assert(order != nullptr);

  AccessPath *path = new (thd->mem_root) AccessPath;
  path->type = AccessPath::SORT;
  path->count_examined_rows = count_examined_rows;
  path->sort().child = child;
  path->sort().filesort = filesort;
  path->sort().order = order;
  path->sort().remove_duplicates = filesort->m_remove_duplicates;
  path->sort().unwrap_rollup = false;
  path->sort().limit = filesort->limit;
  path->sort().force_sort_rowids = !filesort->using_addon_fields();

  if (filesort->using_addon_fields()) {
    path->sort().tables_to_get_rowid_for = 0;
  } else {
    if (filesort->tables.size() == 1 &&
        filesort->tables[0]->pos_in_table_list == nullptr) {
      // This can happen if we sort a single temporary table
      // which is not in the table list (e.g., one that was
      // specifically created for us). Filesort has special-casing
      // to always get the row ID in this case.
      path->sort().tables_to_get_rowid_for = 0;
    } else {
      FindTablesToGetRowidFor(path);
    }
  }
  path->has_group_skip_scan = child->has_group_skip_scan;
  return path;
}

AccessPath *NewDeleteRowsAccessPath(THD *thd, AccessPath *child,
                                    table_map delete_tables,
                                    table_map immediate_tables) {
  assert(IsSubset(immediate_tables, delete_tables));
  AccessPath *path = new (thd->mem_root) AccessPath;
  path->type = AccessPath::DELETE_ROWS;
  path->delete_rows().child = child;
  path->delete_rows().tables_to_delete_from = delete_tables;
  path->delete_rows().immediate_tables = immediate_tables;
  return path;
}

AccessPath *NewUpdateRowsAccessPath(THD *thd, AccessPath *child,
                                    table_map update_tables,
                                    table_map immediate_tables) {
  assert(IsSubset(immediate_tables, update_tables));
  AccessPath *path = new (thd->mem_root) AccessPath;
  path->type = AccessPath::UPDATE_ROWS;
  path->update_rows().child = child;
  path->update_rows().tables_to_update = update_tables;
  path->update_rows().immediate_tables = immediate_tables;
  return path;
}

static Mem_root_array<Item_values_column *> *GetTableValueConstructorOutputRefs(
    MEM_ROOT *mem_root, const JOIN *join) {
  // If the table value constructor has a single row, the values are contained
  // directly in join->fields, and there are no Item_values_column output refs.
  if (join->query_block->row_value_list->size() == 1) {
    return nullptr;
  }

  auto columns = new (mem_root) Mem_root_array<Item_values_column *>(mem_root);
  if (columns == nullptr) return nullptr;

  for (Item *column : VisibleFields(*join->fields)) {
    if (columns->push_back(down_cast<Item_values_column *>(column))) {
      return nullptr;
    }
  }

  return columns;
}

AccessPath *NewTableValueConstructorAccessPath(const THD *thd,
                                               const JOIN *join) {
  AccessPath *path = new (thd->mem_root) AccessPath;
  path->type = AccessPath::TABLE_VALUE_CONSTRUCTOR;
  // The iterator keeps track of which row it is at in examined_rows,
  // so we always need to give it the pointer.
  path->count_examined_rows = true;
  path->table_value_constructor().output_refs =
      GetTableValueConstructorOutputRefs(thd->mem_root, join);
  return path;
}

static AccessPath *FindSingleAccessPathOfType(AccessPath *path,
                                              AccessPath::Type type) {
  AccessPath *found_path = nullptr;

  auto func = [type, &found_path](AccessPath *subpath, const JOIN *) {
#ifdef NDEBUG
    constexpr bool fast_exit = true;
#else
    constexpr bool fast_exit = false;
#endif
    if (subpath->type == type) {
      assert(found_path == nullptr);
      found_path = subpath;
      // If not in debug mode, stop as soon as we find the first one.
      if (fast_exit) {
        return true;
      }
    }
    return false;
  };
  // Our users generally want to stop at STREAM or MATERIALIZE nodes,
  // since they are table-oriented and those nodes have their own tables.
  WalkAccessPaths(path, /*join=*/nullptr,
                  WalkAccessPathPolicy::STOP_AT_MATERIALIZATION, func);
  return found_path;
}

static RowIterator *FindSingleIteratorOfType(AccessPath *path,
                                             AccessPath::Type type) {
  AccessPath *found_path = FindSingleAccessPathOfType(path, type);
  if (found_path == nullptr) {
    return nullptr;
  } else {
    return found_path->iterator->real_iterator();
  }
}

TABLE *GetBasicTable(const AccessPath *path) {
  switch (path->type) {
    // Basic access paths (those with no children, at least nominally).
    case AccessPath::TABLE_SCAN:
      return path->table_scan().table;
    case AccessPath::SAMPLE_SCAN:
      return path->sample_scan().table;
    case AccessPath::INDEX_SCAN:
      return path->index_scan().table;
    case AccessPath::INDEX_DISTANCE_SCAN:
      return path->index_distance_scan().table;
    case AccessPath::REF:
      return path->ref().table;
    case AccessPath::REF_OR_NULL:
      return path->ref_or_null().table;
    case AccessPath::EQ_REF:
      return path->eq_ref().table;
    case AccessPath::PUSHED_JOIN_REF:
      return path->pushed_join_ref().table;
    case AccessPath::FULL_TEXT_SEARCH:
      return path->full_text_search().table;
    case AccessPath::CONST_TABLE:
      return path->const_table().table;
    case AccessPath::MRR:
      return path->mrr().table;
    case AccessPath::FOLLOW_TAIL:
      return path->follow_tail().table;
    case AccessPath::INDEX_RANGE_SCAN:
      return path->index_range_scan().used_key_part[0].field->table;
    case AccessPath::INDEX_MERGE:
      return path->index_merge().table;
    case AccessPath::ROWID_INTERSECTION:
      return path->rowid_intersection().table;
    case AccessPath::ROWID_UNION:
      return path->rowid_union().table;
    case AccessPath::INDEX_SKIP_SCAN:
      return path->index_skip_scan().table;
    case AccessPath::GROUP_INDEX_SKIP_SCAN:
      return path->group_index_skip_scan().table;
    case AccessPath::DYNAMIC_INDEX_RANGE_SCAN:
      return path->dynamic_index_range_scan().table;

    // Basic access paths that don't correspond to a specific table.
    case AccessPath::TABLE_VALUE_CONSTRUCTOR:
    case AccessPath::FAKE_SINGLE_ROW:
    case AccessPath::ZERO_ROWS:
    case AccessPath::ZERO_ROWS_AGGREGATED:
    case AccessPath::MATERIALIZED_TABLE_FUNCTION:
    case AccessPath::UNQUALIFIED_COUNT:

    // Note, some other AccessPaths may use its own temporary (derived) table.
    // We intentionally do not return such TABLEs.
    default:
      return nullptr;
  }
}

table_map GetUsedTableMap(const AccessPath *path, bool include_pruned_tables) {
  table_map tmap = 0;
  WalkTablesUnderAccessPath(
      const_cast<AccessPath *>(path),
      [&tmap](TABLE *table) {
        if (table->pos_in_table_list == nullptr) {
          // Materialization within a JOIN (e.g., for sorting). The table won't
          // have a map, so the caller will need to find the table manually.
          tmap |= RAND_TABLE_BIT;
        } else {
          tmap |= table->pos_in_table_list->map();
        }
        return false;
      },
      include_pruned_tables);
  return tmap;
}

static Prealloced_array<TABLE *, 4> GetUsedTables(AccessPath *child,
                                                  bool include_pruned_tables) {
  Prealloced_array<TABLE *, 4> tables{PSI_NOT_INSTRUMENTED};
  WalkTablesUnderAccessPath(
      child,
      [&tables](TABLE *table) {
        tables.push_back(table);
        return false;
      },
      include_pruned_tables);
  return tables;
}

Mem_root_array<TABLE *> CollectTables(THD *thd, AccessPath *root_path) {
  Mem_root_array<TABLE *> tables(thd->mem_root);
  WalkTablesUnderAccessPath(
      root_path, [&tables](TABLE *table) { return tables.push_back(table); },
      /*include_pruned_tables=*/true);
  return tables;
}

/**
  Get the tables that are accessed by EQ_REF and can be on the inner side of an
  outer join. These need some extra care in AggregateIterator when handling
  NULL-complemented rows, so that the cache in EQRefIterator is not disturbed by
  AggregateIterator's switching between groups.
 */
static table_map GetNullableEqRefTables(const AccessPath *root_path) {
  table_map tables = 0;
  WalkAccessPaths(
      root_path, /*join=*/nullptr,
      WalkAccessPathPolicy::STOP_AT_MATERIALIZATION,
      [&tables](const AccessPath *path, const JOIN *) {
        if (path->type == AccessPath::EQ_REF) {
          const auto &param = path->eq_ref();
          if (param.table->is_nullable() && !param.ref->disable_cache) {
            tables |= param.table->pos_in_table_list->map();
          }
        }
        return false;
      });
  return tables;
}

namespace {

// Mirrors QEP_TAB::pfs_batch_update(), with one addition:
// If there is more than one table, batch mode will be handled by the join
// iterators on the probe side, so joins will return false.
bool ShouldEnableBatchMode(AccessPath *path) {
  switch (path->type) {
    case AccessPath::TABLE_SCAN:
    case AccessPath::INDEX_SCAN:
    case AccessPath::INDEX_DISTANCE_SCAN:
    case AccessPath::REF:
    case AccessPath::REF_OR_NULL:
    case AccessPath::PUSHED_JOIN_REF:
    case AccessPath::FULL_TEXT_SEARCH:
    case AccessPath::DYNAMIC_INDEX_RANGE_SCAN:
      return true;
    case AccessPath::FILTER:
      if (path->filter().condition->has_subquery()) {
        return false;
      } else {
        return ShouldEnableBatchMode(path->filter().child);
      }
    case AccessPath::SORT:
      return ShouldEnableBatchMode(path->sort().child);
    case AccessPath::EQ_REF:
    case AccessPath::CONST_TABLE:
      // These can read only one row per scan, so batch mode will never be a
      // win (fall through).
    default:
      // All others, in particular joins.
      return false;
  }
}

/**
  If the path is a FILTER path marked that subqueries are to be materialized,
  do so. If not, do nothing.

  It is important that this is not called until the entire plan is ready;
  not just when planning a single query block. The reason is that a query
  block A with materializable subqueries may itself be part of a materializable
  subquery B, so if one calls this when planning A, the subqueries in A will
  irrevocably be materialized, even if that is not the optimal plan given B.
  Thus, this is done when creating iterators.
 */
bool FinalizeMaterializedSubqueries(THD *thd, JOIN *join, AccessPath *path) {
  if (path->type != AccessPath::FILTER ||
      !path->filter().materialize_subqueries) {
    return false;
  }
  return WalkItem(
      path->filter().condition, enum_walk::POSTFIX, [thd, join](Item *item) {
        if (!is_quantified_comp_predicate(item)) {
          return false;
        }
        Item_in_subselect *item_subs = down_cast<Item_in_subselect *>(item);
        if (item_subs->strategy == Subquery_strategy::SUBQ_MATERIALIZATION) {
          // This subquery is already set up for materialization.
          return false;
        }
        Query_block *qb = item_subs->query_expr()->first_query_block();
        if (!item_subs->subquery_allows_materialization(thd, qb,
                                                        join->query_block)) {
          return false;
        }
        if (item_subs->finalize_materialization_transform(thd, qb->join)) {
          return true;
        }
        item_subs->create_iterators(thd);
        return false;
      });
}

struct IteratorToBeCreated {
  AccessPath *path;
  JOIN *join;
  bool eligible_for_batch_mode;
  unique_ptr_destroy_only<RowIterator> *destination;
  Bounds_checked_array<unique_ptr_destroy_only<RowIterator>> children;

  void AllocChildren(MEM_ROOT *mem_root, int num_children) {
    children =
        Bounds_checked_array<unique_ptr_destroy_only<RowIterator>>::Alloc(
            mem_root, num_children);
  }
};

std::string WzgIteratorDoubleToString(double value) {
  std::ostringstream out;
  out << value;
  return out.str();
}

std::string WzgIteratorTableName(const TABLE *table) {
  if (table == nullptr || table->s == nullptr) return "无";
  std::string value;
  if (table->s->db.str != nullptr && table->s->db.length > 0) {
    value.append(table->s->db.str, table->s->db.length);
    value.push_back('.');
  }
  if (table->s->table_name.str != nullptr && table->s->table_name.length > 0)
    value.append(table->s->table_name.str, table->s->table_name.length);
  return value.empty() ? "<unknown>" : value;
}

std::string WzgIteratorKeyParts(const KEY &key) {
  std::string value;
  for (uint part_no = 0; part_no < key.user_defined_key_parts; ++part_no) {
    if (part_no > 0) value.append(", ");
    const KEY_PART_INFO &part = key.key_part[part_no];
    if (part.field != nullptr && part.field->field_name != nullptr)
      value.append(part.field->field_name);
    else
      value.append("<expression>");
  }
  return value.empty() ? "<unknown>" : value;
}

std::string WzgIteratorIndexName(const TABLE *table, uint key_no) {
  if (table == nullptr || table->s == nullptr || table->key_info == nullptr ||
      key_no == MAX_KEY || key_no >= table->s->keys)
    return "无";
  const KEY &key = table->key_info[key_no];
  std::string value(key.name == nullptr ? "<unnamed>" : key.name);
  value.push_back('(');
  value.append(WzgIteratorKeyParts(key));
  value.push_back(')');
  return value;
}

const char *WzgIteratorAccessPathName(AccessPath::Type type) {
  switch (type) {
    case AccessPath::TABLE_SCAN:
      return "TABLE_SCAN";
    case AccessPath::SAMPLE_SCAN:
      return "SAMPLE_SCAN";
    case AccessPath::INDEX_SCAN:
      return "INDEX_SCAN";
    case AccessPath::INDEX_DISTANCE_SCAN:
      return "INDEX_DISTANCE_SCAN";
    case AccessPath::REF:
      return "REF";
    case AccessPath::REF_OR_NULL:
      return "REF_OR_NULL";
    case AccessPath::EQ_REF:
      return "EQ_REF";
    case AccessPath::PUSHED_JOIN_REF:
      return "PUSHED_JOIN_REF";
    case AccessPath::FULL_TEXT_SEARCH:
      return "FULL_TEXT_SEARCH";
    case AccessPath::CONST_TABLE:
      return "CONST_TABLE";
    case AccessPath::MRR:
      return "MRR";
    case AccessPath::FOLLOW_TAIL:
      return "FOLLOW_TAIL";
    case AccessPath::INDEX_RANGE_SCAN:
      return "INDEX_RANGE_SCAN";
    case AccessPath::INDEX_MERGE:
      return "INDEX_MERGE";
    case AccessPath::ROWID_INTERSECTION:
      return "ROWID_INTERSECTION";
    case AccessPath::ROWID_UNION:
      return "ROWID_UNION";
    case AccessPath::INDEX_SKIP_SCAN:
      return "INDEX_SKIP_SCAN";
    case AccessPath::GROUP_INDEX_SKIP_SCAN:
      return "GROUP_INDEX_SKIP_SCAN";
    case AccessPath::DYNAMIC_INDEX_RANGE_SCAN:
      return "DYNAMIC_INDEX_RANGE_SCAN";
    case AccessPath::TABLE_VALUE_CONSTRUCTOR:
      return "TABLE_VALUE_CONSTRUCTOR";
    case AccessPath::FAKE_SINGLE_ROW:
      return "FAKE_SINGLE_ROW";
    case AccessPath::ZERO_ROWS:
      return "ZERO_ROWS";
    case AccessPath::ZERO_ROWS_AGGREGATED:
      return "ZERO_ROWS_AGGREGATED";
    case AccessPath::MATERIALIZED_TABLE_FUNCTION:
      return "MATERIALIZED_TABLE_FUNCTION";
    case AccessPath::UNQUALIFIED_COUNT:
      return "UNQUALIFIED_COUNT";
    case AccessPath::NESTED_LOOP_JOIN:
      return "NESTED_LOOP_JOIN";
    case AccessPath::NESTED_LOOP_SEMIJOIN_WITH_DUPLICATE_REMOVAL:
      return "NESTED_LOOP_SEMIJOIN_WITH_DUPLICATE_REMOVAL";
    case AccessPath::BKA_JOIN:
      return "BKA_JOIN";
    case AccessPath::HASH_JOIN:
      return "HASH_JOIN";
    case AccessPath::FILTER:
      return "FILTER";
    case AccessPath::SORT:
      return "SORT";
    case AccessPath::AGGREGATE:
      return "AGGREGATE";
    case AccessPath::TEMPTABLE_AGGREGATE:
      return "TEMPTABLE_AGGREGATE";
    case AccessPath::LIMIT_OFFSET:
      return "LIMIT_OFFSET";
    case AccessPath::STREAM:
      return "STREAM";
    case AccessPath::MATERIALIZE:
      return "MATERIALIZE";
    case AccessPath::MATERIALIZE_INFORMATION_SCHEMA_TABLE:
      return "MATERIALIZE_INFORMATION_SCHEMA_TABLE";
    case AccessPath::APPEND:
      return "APPEND";
    case AccessPath::WINDOW:
      return "WINDOW";
    case AccessPath::WEEDOUT:
      return "WEEDOUT";
    case AccessPath::REMOVE_DUPLICATES:
      return "REMOVE_DUPLICATES";
    case AccessPath::REMOVE_DUPLICATES_ON_INDEX:
      return "REMOVE_DUPLICATES_ON_INDEX";
    case AccessPath::ALTERNATIVE:
      return "ALTERNATIVE";
    case AccessPath::CACHE_INVALIDATOR:
      return "CACHE_INVALIDATOR";
    case AccessPath::DELETE_ROWS:
      return "DELETE_ROWS";
    case AccessPath::UPDATE_ROWS:
      return "UPDATE_ROWS";
  }
  return "UNKNOWN";
}

const char *WzgIteratorReadableName(AccessPath::Type type) {
  switch (type) {
    case AccessPath::TABLE_SCAN:
      return "全表扫描读取器";
    case AccessPath::INDEX_SCAN:
      return "索引扫描读取器";
    case AccessPath::INDEX_DISTANCE_SCAN:
      return "向量距离索引读取器";
    case AccessPath::REF:
      return "普通索引匹配读取器";
    case AccessPath::REF_OR_NULL:
      return "索引匹配或 NULL 读取器";
    case AccessPath::EQ_REF:
      return "唯一索引匹配读取器";
    case AccessPath::PUSHED_JOIN_REF:
      return "下推 JOIN 索引读取器";
    case AccessPath::FULL_TEXT_SEARCH:
      return "全文索引读取器";
    case AccessPath::CONST_TABLE:
      return "常量表读取器";
    case AccessPath::MRR:
      return "Multi-Range Read 批量读取器";
    case AccessPath::FOLLOW_TAIL:
      return "递归查询增量读取器";
    case AccessPath::INDEX_RANGE_SCAN:
      return "索引范围读取器";
    case AccessPath::INDEX_MERGE:
      return "多个索引合并读取器";
    case AccessPath::ROWID_INTERSECTION:
      return "多个索引取交集读取器";
    case AccessPath::ROWID_UNION:
      return "多个索引取并集读取器";
    case AccessPath::INDEX_SKIP_SCAN:
      return "索引 skip scan 读取器";
    case AccessPath::GROUP_INDEX_SKIP_SCAN:
      return "GROUP BY 索引 skip scan 读取器";
    case AccessPath::DYNAMIC_INDEX_RANGE_SCAN:
      return "动态索引范围读取器";
    case AccessPath::NESTED_LOOP_JOIN:
      return "嵌套循环 JOIN 读取器";
    case AccessPath::HASH_JOIN:
      return "Hash Join 读取器";
    case AccessPath::FILTER:
      return "过滤条件读取器";
    case AccessPath::SORT:
      return "排序读取器";
    case AccessPath::MATERIALIZE:
      return "物化中间结果读取器";
    case AccessPath::STREAM:
      return "流式写入临时结果读取器";
    case AccessPath::LIMIT_OFFSET:
      return "LIMIT/OFFSET 读取器";
    case AccessPath::AGGREGATE:
    case AccessPath::TEMPTABLE_AGGREGATE:
      return "聚合读取器";
    case AccessPath::WINDOW:
      return "窗口函数读取器";
    case AccessPath::TABLE_VALUE_CONSTRUCTOR:
      return "VALUES 行构造读取器";
    case AccessPath::FAKE_SINGLE_ROW:
      return "单行虚拟读取器";
    case AccessPath::ZERO_ROWS:
      return "空结果读取器";
    case AccessPath::ZERO_ROWS_AGGREGATED:
      return "空结果聚合读取器";
    case AccessPath::MATERIALIZED_TABLE_FUNCTION:
      return "物化表函数读取器";
    case AccessPath::UNQUALIFIED_COUNT:
      return "无需条件的 COUNT 读取器";
    case AccessPath::NESTED_LOOP_SEMIJOIN_WITH_DUPLICATE_REMOVAL:
      return "去重半连接嵌套循环读取器";
    case AccessPath::BKA_JOIN:
      return "Batched Key Access JOIN 读取器";
    case AccessPath::MATERIALIZE_INFORMATION_SCHEMA_TABLE:
      return "物化 information_schema 表读取器";
    case AccessPath::APPEND:
      return "追加多个输入的读取器";
    case AccessPath::WEEDOUT:
      return "半连接去重读取器";
    case AccessPath::REMOVE_DUPLICATES:
      return "去重读取器";
    case AccessPath::REMOVE_DUPLICATES_ON_INDEX:
      return "按索引去重读取器";
    case AccessPath::ALTERNATIVE:
      return "运行时二选一读取器";
    case AccessPath::CACHE_INVALIDATOR:
      return "缓存失效包装读取器";
    case AccessPath::DELETE_ROWS:
      return "删除行读取器";
    case AccessPath::UPDATE_ROWS:
      return "更新行读取器";
    case AccessPath::SAMPLE_SCAN:
      return "采样扫描读取器";
  }
  return "未知读取器";
}

TABLE *WzgIteratorTargetTable(const AccessPath *path) {
  switch (path->type) {
    case AccessPath::TABLE_SCAN:
      return path->table_scan().table;
    case AccessPath::INDEX_SCAN:
      return path->index_scan().table;
    case AccessPath::INDEX_DISTANCE_SCAN:
      return path->index_distance_scan().table;
    case AccessPath::REF:
      return path->ref().table;
    case AccessPath::REF_OR_NULL:
      return path->ref_or_null().table;
    case AccessPath::EQ_REF:
      return path->eq_ref().table;
    case AccessPath::PUSHED_JOIN_REF:
      return path->pushed_join_ref().table;
    case AccessPath::FULL_TEXT_SEARCH:
      return path->full_text_search().table;
    case AccessPath::CONST_TABLE:
      return path->const_table().table;
    case AccessPath::MRR:
      return path->mrr().table;
    case AccessPath::FOLLOW_TAIL:
      return path->follow_tail().table;
    case AccessPath::INDEX_RANGE_SCAN:
      return path->index_range_scan().used_key_part[0].field->table;
    case AccessPath::INDEX_MERGE:
      return path->index_merge().table;
    case AccessPath::ROWID_INTERSECTION:
      return path->rowid_intersection().table;
    case AccessPath::ROWID_UNION:
      return path->rowid_union().table;
    case AccessPath::INDEX_SKIP_SCAN:
      return path->index_skip_scan().table;
    case AccessPath::GROUP_INDEX_SKIP_SCAN:
      return path->group_index_skip_scan().table;
    case AccessPath::DYNAMIC_INDEX_RANGE_SCAN:
      return path->dynamic_index_range_scan().table;
    case AccessPath::MATERIALIZED_TABLE_FUNCTION:
      return path->materialized_table_function().table;
    case AccessPath::STREAM:
      return path->stream().table;
    case AccessPath::MATERIALIZE_INFORMATION_SCHEMA_TABLE:
      return path->materialize_information_schema_table().table_list == nullptr
                 ? nullptr
                 : path->materialize_information_schema_table()
                       .table_list->table;
    default:
      return nullptr;
  }
}

std::string WzgIteratorChosenIndex(const AccessPath *path) {
  TABLE *table = WzgIteratorTargetTable(path);
  uint key_no = MAX_KEY;
  switch (path->type) {
    case AccessPath::INDEX_SCAN:
      key_no = path->index_scan().idx;
      break;
    case AccessPath::INDEX_DISTANCE_SCAN:
      key_no = path->index_distance_scan().idx;
      break;
    case AccessPath::REF:
      key_no = path->ref().ref->key;
      break;
    case AccessPath::REF_OR_NULL:
      key_no = path->ref_or_null().ref->key;
      break;
    case AccessPath::EQ_REF:
      key_no = path->eq_ref().ref->key;
      break;
    case AccessPath::PUSHED_JOIN_REF:
      key_no = path->pushed_join_ref().ref->key;
      break;
    case AccessPath::FULL_TEXT_SEARCH:
      key_no = path->full_text_search().ref->key;
      break;
    case AccessPath::CONST_TABLE:
      key_no = path->const_table().ref->key;
      break;
    case AccessPath::MRR:
      key_no = path->mrr().ref->key;
      break;
    case AccessPath::INDEX_RANGE_SCAN:
      key_no = path->index_range_scan().index;
      break;
    case AccessPath::INDEX_SKIP_SCAN:
      key_no = path->index_skip_scan().index;
      break;
    case AccessPath::GROUP_INDEX_SKIP_SCAN:
      key_no = path->group_index_skip_scan().index;
      break;
    default:
      break;
  }
  return WzgIteratorIndexName(table, key_no);
}

std::string WzgIteratorPurpose(const AccessPath *path) {
  switch (path->type) {
    case AccessPath::TABLE_SCAN:
      return "逐行读取整张表，把行交给上层过滤或 JOIN";
    case AccessPath::INDEX_SCAN:
      return "按索引顺序扫描记录，把行交给上层处理";
    case AccessPath::REF:
    case AccessPath::REF_OR_NULL:
    case AccessPath::EQ_REF:
    case AccessPath::PUSHED_JOIN_REF:
      return "根据常量或前面表的值，通过索引查找匹配行";
    case AccessPath::INDEX_RANGE_SCAN:
      return "按索引范围读取一段记录";
    case AccessPath::INDEX_MERGE:
      return "合并多个索引扫描结果后再读取匹配行";
    case AccessPath::ROWID_INTERSECTION:
      return "对多个索引结果取交集，得到更精确的候选行";
    case AccessPath::ROWID_UNION:
      return "对多个索引结果取并集，覆盖 OR 等多分支条件";
    case AccessPath::NESTED_LOOP_JOIN:
      return "外层每产生一行，再驱动内层读取匹配行";
    case AccessPath::HASH_JOIN:
      return "先构建哈希表，再用另一侧输入进行匹配";
    case AccessPath::FILTER:
      return "读取子节点结果后判断过滤条件";
    case AccessPath::SORT:
      return "读取子节点结果后执行 filesort 排序";
    case AccessPath::MATERIALIZE:
      return "把子查询或中间结果写入内部临时结果，后续再读取";
    case AccessPath::STREAM:
      return "把上游结果流式写入临时表或上层结果";
    case AccessPath::LIMIT_OFFSET:
      return "跳过 OFFSET 并限制最多返回 LIMIT 行";
    case AccessPath::AGGREGATE:
    case AccessPath::TEMPTABLE_AGGREGATE:
      return "对输入行做聚合计算";
    case AccessPath::WINDOW:
      return "对输入行计算窗口函数";
    default:
      return "把这个执行计划节点转换成可读取的 RowIterator";
  }
}

wzg_probe::Event &WzgAddExecutorFactFields(wzg_probe::Event &event,
                                           const char *subsystem,
                                           const char *role,
                                           const char *action_name,
                                           const char *phase,
                                           const char *summary,
                                           const char *object_type,
                                           const std::string &object_id,
                                           const char *source_function) {
  return event.field("actor.component", "executor")
      .field("actor.subsystem", subsystem)
      .field("actor.role", role)
      .field("action.name", action_name)
      .field("action.phase", phase)
      .field("action.summary", summary)
      .field("object.type", object_type)
      .field("object.id", object_id)
      .field("debug.source_file", "sql/join_optimizer/access_path.cc")
      .field("debug.source_function", source_function)
      .field("debug.source_note",
             "这里记录执行器访问路径对应的 iterator 事实，不重新执行 SQL");
}

const char *WzgRangeFlagName(enum ha_rkey_function flag) {
  switch (flag) {
    case HA_READ_KEY_EXACT:
      return "HA_READ_KEY_EXACT";
    case HA_READ_KEY_OR_NEXT:
      return "HA_READ_KEY_OR_NEXT";
    case HA_READ_KEY_OR_PREV:
      return "HA_READ_KEY_OR_PREV";
    case HA_READ_AFTER_KEY:
      return "HA_READ_AFTER_KEY";
    case HA_READ_BEFORE_KEY:
      return "HA_READ_BEFORE_KEY";
    default:
      return "OTHER_HA_READ_FLAG";
  }
}

const char *WzgRangeFlagMeaning(enum ha_rkey_function flag, bool start_key) {
  switch (flag) {
    case HA_READ_KEY_EXACT:
      return "等值定位，要求 key 正好匹配这个边界";
    case HA_READ_KEY_OR_NEXT:
      return start_key ? "起点包含边界值，找不到正好等于边界时从下一条开始"
                       : "终点方向使用 key 或后一条";
    case HA_READ_KEY_OR_PREV:
      return start_key ? "起点方向使用 key 或前一条"
                       : "终点包含边界值，找不到正好等于边界时到前一条为止";
    case HA_READ_AFTER_KEY:
      return start_key ? "起点不包含边界值，从大于该 key 的下一条开始"
                       : "终点包含该 key 前缀范围，用于结束边界比较";
    case HA_READ_BEFORE_KEY:
      return start_key ? "起点方向读取边界之前的记录"
                       : "终点不包含边界值，到小于该 key 的记录为止";
    default:
      return "其他 handler 读取标记";
  }
}

std::string WzgKeypartMapToString(key_part_map map) {
  std::ostringstream out;
  out << map;
  return out.str();
}

std::string WzgRangeEndpointText(const key_range &range, bool start_key,
                                 uint range_flags) {
  std::string value(start_key ? "start_key: " : "end_key: ");
  const bool no_boundary =
      start_key ? (range_flags & NO_MIN_RANGE) : (range_flags & NO_MAX_RANGE);
  value.append("flag=");
  value.append(WzgRangeFlagName(range.flag));
  value.append("，length=");
  value.append(std::to_string(range.length));
  value.append("，keypart_map=");
  value.append(WzgKeypartMapToString(range.keypart_map));
  value.append("，meaning=");
  if (no_boundary) {
    value.append(start_key ? "没有下界，从索引可用范围的起点开始扫描"
                           : "没有上界，一直扫描到索引可用范围结束");
  } else {
    value.append(WzgRangeFlagMeaning(range.flag, start_key));
  }
  return value;
}

std::string WzgRangeFlagsText(uint flags) {
  std::string value;
  auto append = [&value](const char *name) {
    if (!value.empty()) value.append(", ");
    value.append(name);
  };
  if (flags & NO_MIN_RANGE) append("NO_MIN_RANGE");
  if (flags & NO_MAX_RANGE) append("NO_MAX_RANGE");
  if (flags & NEAR_MIN) append("NEAR_MIN");
  if (flags & NEAR_MAX) append("NEAR_MAX");
  if (flags & UNIQUE_RANGE) append("UNIQUE_RANGE");
  if (flags & EQ_RANGE) append("EQ_RANGE");
  if (flags & NULL_RANGE) append("NULL_RANGE");
  if (flags & GEOM_FLAG) append("GEOM_FLAG");
  if (flags & SKIP_RANGE) append("SKIP_RANGE");
  if (flags & SKIP_RECORDS_IN_RANGE) append("SKIP_RECORDS_IN_RANGE");
  if (flags & DESC_FLAG) append("DESC_FLAG");
  return value.empty() ? "无特殊标记" : value;
}

std::string WzgRangeReadableText(const QUICK_RANGE *range,
                                 const KEY_PART_INFO *key_part) {
  if (range == nullptr || key_part == nullptr) return "未知范围";
  String text;
  append_range_to_string(range, key_part, &text);
  return text.length() == 0 ? "未知范围"
                            : std::string(text.ptr(), text.length());
}

std::string WzgRangeParameterText(const QUICK_RANGE *range,
                                  const KEY_PART_INFO *key_part,
                                  unsigned range_idx) {
  if (range == nullptr) return "未知范围";
  key_range start_key;
  key_range end_key;
  start_key.key = range->min_key;
  start_key.length = range->min_length;
  start_key.keypart_map = range->min_keypart_map;
  start_key.flag = ((range->flag & NEAR_MIN)   ? HA_READ_AFTER_KEY
                    : (range->flag & EQ_RANGE) ? HA_READ_KEY_EXACT
                                                : HA_READ_KEY_OR_NEXT);
  end_key.key = range->max_key;
  end_key.length = range->max_length;
  end_key.keypart_map = range->max_keypart_map;
  end_key.flag =
      (range->flag & NEAR_MAX ? HA_READ_BEFORE_KEY : HA_READ_AFTER_KEY);
  std::string value("range ");
  value.append(std::to_string(range_idx + 1));
  value.append(": ");
  value.append(WzgRangeReadableText(range, key_part));
  value.append("；");
  value.append(WzgRangeEndpointText(start_key, true, range->flag));
  value.append("；");
  value.append(WzgRangeEndpointText(end_key, false, range->flag));
  value.append("；range_flags=");
  value.append(WzgRangeFlagsText(range->flag));
  return value;
}

std::string WzgRangeParametersList(const QUICK_RANGE *const *ranges,
                                   unsigned num_ranges,
                                   const KEY_PART_INFO *key_part) {
  if (ranges == nullptr || num_ranges == 0) return "无范围参数";
  constexpr unsigned kMaxPrintedRanges = 6;
  std::string value;
  const unsigned printed = std::min(num_ranges, kMaxPrintedRanges);
  for (unsigned i = 0; i < printed; ++i) {
    if (i > 0) value.append(" | ");
    value.append(WzgRangeParameterText(ranges[i], key_part, i));
  }
  if (num_ranges > printed) {
    value.append(" | 还有 ");
    value.append(std::to_string(num_ranges - printed));
    value.append(" 个范围未展开");
  }
  return value;
}

std::string WzgUsedKeyPartsText(const KEY &key, key_part_map used_map) {
  std::string value;
  key_part_map bit = 1;
  for (uint part_no = 0; part_no < key.user_defined_key_parts; ++part_no) {
    if (used_map & bit) {
      if (!value.empty()) value.append(", ");
      const KEY_PART_INFO &part = key.key_part[part_no];
      if (part.field != nullptr && part.field->field_name != nullptr)
        value.append(part.field->field_name);
      else
        value.append("<expression>");
    }
    bit <<= 1;
  }
  return value.empty() ? "无" : value;
}

Index_lookup *WzgRefLookup(const AccessPath *path) {
  switch (path->type) {
    case AccessPath::REF:
      return path->ref().ref;
    case AccessPath::REF_OR_NULL:
      return path->ref_or_null().ref;
    case AccessPath::EQ_REF:
      return path->eq_ref().ref;
    case AccessPath::PUSHED_JOIN_REF:
      return path->pushed_join_ref().ref;
    case AccessPath::CONST_TABLE:
      return path->const_table().ref;
    default:
      return nullptr;
  }
}

const char *WzgRefLookupTypeText(AccessPath::Type type) {
  switch (type) {
    case AccessPath::REF:
      return "REF，按普通索引值查找，可能返回多行";
    case AccessPath::REF_OR_NULL:
      return "REF_OR_NULL，先按索引值查找，再额外处理 NULL 匹配";
    case AccessPath::EQ_REF:
      return "EQ_REF，按唯一索引或主键查找，每次最多返回一行";
    case AccessPath::PUSHED_JOIN_REF:
      return "PUSHED_JOIN_REF，下推到存储引擎的 JOIN 索引查找";
    case AccessPath::CONST_TABLE:
      return "CONST_TABLE，按唯一索引提前读取成常量表";
    default:
      return "不是 ref 类索引查找";
  }
}

std::string WzgRefLookupKeyParts(const TABLE *table, const Index_lookup *ref) {
  if (table == nullptr || table->key_info == nullptr || table->s == nullptr ||
      ref == nullptr || ref->key < 0 || static_cast<uint>(ref->key) >= table->s->keys)
    return "无";

  const KEY &key = table->key_info[ref->key];
  std::string value;
  const uint parts = std::min(ref->key_parts, key.user_defined_key_parts);
  for (uint part_no = 0; part_no < parts; ++part_no) {
    if (!value.empty()) value.append(", ");
    const KEY_PART_INFO &part = key.key_part[part_no];
    if (part.field != nullptr && part.field->field_name != nullptr)
      value.append(part.field->field_name);
    else
      value.append("<expression>");
  }
  return value.empty() ? "无" : value;
}

std::string WzgRefLookupValues(THD *thd, const Index_lookup *ref) {
  if (ref == nullptr || ref->items == nullptr || ref->key_parts == 0)
    return "无";

  constexpr uint kMaxPrintedParts = 8;
  std::string value;
  const uint printed = std::min(ref->key_parts, kMaxPrintedParts);
  for (uint part_no = 0; part_no < printed; ++part_no) {
    if (!value.empty()) value.append("；");
    value.append("keypart ");
    value.append(std::to_string(part_no + 1));
    value.append(" = ");
    Item *item = ref->items[part_no];
    if (item == nullptr) {
      value.append("未知表达式");
      continue;
    }
    char buffer[512];
    String text(buffer, sizeof(buffer), system_charset_info);
    text.length(0);
    item->print(thd, &text, QT_ORDINARY);
    value.append(text.length() == 0 ? "未知表达式"
                                    : std::string(text.ptr(), text.length()));
  }
  if (ref->key_parts > printed) {
    value.append("；还有 ");
    value.append(std::to_string(ref->key_parts - printed));
    value.append(" 个 keypart 未展开");
  }
  return value;
}

std::string WzgRefLookupNullRejecting(const Index_lookup *ref) {
  if (ref == nullptr || ref->key_parts == 0) return "无";
  std::string value;
  for (uint part_no = 0; part_no < ref->key_parts; ++part_no) {
    if (ref->null_rejecting & (key_part_map{1} << part_no)) {
      if (!value.empty()) value.append(", ");
      value.append("keypart ");
      value.append(std::to_string(part_no + 1));
    }
  }
  return value.empty() ? "无；查找值为 NULL 时仍需按访问方式继续处理"
                       : value + "；这些列的查找值为 NULL 时不会产生匹配行";
}

std::string WzgRefLookupSource(const Index_lookup *ref) {
  if (ref == nullptr) return "未知";
  if (ref->depend_map == 0)
    return "常量、系统已知值，或优化阶段已经确定的值";
  return "来自前面已经读到的表、外层查询，或运行时表达式";
}

std::string WzgRefLookupCacheText(const Index_lookup *ref) {
  if (ref == nullptr) return "未知";
  if (ref->disable_cache)
    return "不复用上一次相同 key 的查找结果，通常因为索引条件下推等执行细节可能影响结果";
  return "允许复用相同 key 的查找结果，避免重复定位";
}

std::string WzgTableStatsRowsText(const TABLE *table) {
  if (table == nullptr || table->file == nullptr) return "未知";
  if (table->file->stats.records == HA_POS_ERROR) return "未知";
  return std::to_string(table->file->stats.records);
}

std::string WzgIndexScanDirectionText(bool reverse) {
  return reverse ? "反向扫描，从索引末尾向前读取"
                 : "正向扫描，从索引开头向后读取";
}

std::string WzgIndexScanHandlerCallsText(bool reverse) {
  return reverse ? "ha_index_init -> ha_index_last -> ha_index_prev"
                 : "ha_index_init -> ha_index_first -> ha_index_next";
}

std::string WzgIndexCoveringText(const TABLE *table, uint key_no) {
  if (table == nullptr || table->s == nullptr || key_no >= table->s->keys)
    return "未知";
  if (!table->covering_keys.is_set(key_no))
    return "否；这个索引不能单独提供本次查询需要的全部列";
  if (table->no_keyread)
    return "索引覆盖本次查询需要的列，但当前表关闭了 keyread 优化";
  return "是；这个索引覆盖本次查询需要的列，执行时可能只读索引记录";
}

const Table_ref *WzgTableRefForTable(const TABLE *table) {
  return table == nullptr ? nullptr : table->pos_in_table_list;
}

const char *WzgLockedRowActionText(thr_locked_row_action action) {
  switch (action) {
    case THR_NOWAIT:
      return "NOWAIT，遇到已被锁住的行不等待，直接返回锁等待错误";
    case THR_SKIP:
      return "SKIP LOCKED，遇到已被锁住的行会跳过";
    case THR_WAIT:
      return "WAIT，按锁等待规则等待";
    case THR_DEFAULT:
    default:
      return "默认等待策略，通常按 innodb_lock_wait_timeout 等规则等待";
  }
}

std::string WzgLockingClauseText(const Table_ref *table_ref) {
  if (table_ref == nullptr) return "无";
  const Lock_descriptor &descriptor = table_ref->lock_descriptor();
  if (descriptor.type == TL_WRITE) return "FOR UPDATE";
  if (descriptor.type == TL_READ_WITH_SHARED_LOCKS)
    return "FOR SHARE / LOCK IN SHARE MODE";
  return "无";
}

std::string WzgLockingIntentText(const Table_ref *table_ref) {
  if (table_ref == nullptr) return "未知";
  const Lock_descriptor &descriptor = table_ref->lock_descriptor();
  if (descriptor.type == TL_WRITE)
    return "对读取到的记录准备加排他锁，通常用于随后更新或防止别人修改这些行";
  if (descriptor.type == TL_READ_WITH_SHARED_LOCKS)
    return "对读取到的记录准备加共享锁，允许别人读，但阻止别人修改这些行";
  return "不是加锁读";
}

std::string WzgLockingReadNextStepText(const Table_ref *table_ref) {
  if (table_ref == nullptr) return "未知";
  const Lock_descriptor &descriptor = table_ref->lock_descriptor();
  if (descriptor.type == TL_WRITE)
    return "handler/InnoDB 读取记录时会进入当前读和排他行锁判断，后续 InnoDB 决定 record/gap/next-key 锁";
  if (descriptor.type == TL_READ_WITH_SHARED_LOCKS)
    return "handler/InnoDB 读取记录时会进入当前读和共享行锁判断，后续 InnoDB 决定具体锁范围";
  return "继续普通读取";
}

TABLE *WzgLockingReadTargetTable(const AccessPath *path) {
  return WzgIteratorTargetTable(path);
}

void WzgEmitLockingRead(THD *thd, const AccessPath *path, const JOIN *join) {
  if (path == nullptr || thd == nullptr || thd->query().str == nullptr ||
      thd->query().length == 0)
    return;

  TABLE *table = WzgLockingReadTargetTable(path);
  const Table_ref *table_ref = WzgTableRefForTable(table);
  if (table_ref == nullptr) return;
  const Lock_descriptor &descriptor = table_ref->lock_descriptor();
  if (descriptor.type != TL_WRITE &&
      descriptor.type != TL_READ_WITH_SHARED_LOCKS)
    return;

  Query_block *query_block = join == nullptr ? nullptr : join->query_block;
  WZG_PROBE_EVENT(thd, "executor.locking_read")
      .message("执行器识别到这是加锁读，读取记录时会要求存储引擎返回当前版本并处理行锁")
      .sql_command(get_sql_command_string(thd->lex->sql_command))
      .field("query_block_number",
             query_block == nullptr
                 ? std::uint64_t{0}
                 : static_cast<std::uint64_t>(query_block->select_number))
      .field("table", WzgIteratorTableName(table))
      .field("access_path_type", WzgIteratorAccessPathName(path->type))
      .field("chosen_index", WzgIteratorChosenIndex(path))
      .field("locking_clause", WzgLockingClauseText(table_ref))
      .field("read_type", "当前读；不是普通 MVCC 快照读")
      .field("lock_intent", WzgLockingIntentText(table_ref))
      .field("locked_row_action",
             WzgLockedRowActionText(descriptor.action))
      .field("server_decision",
             "Server/执行器已经把这张表标记为加锁读，但还没有在这里真正持有 InnoDB 行锁")
      .field("server_mdl_relation",
             "前面的 server.mdl_lock 保护表结构；这里说明读取记录时需要行级加锁语义")
      .field("intention_lock_note",
             "面试常说的 InnoDB IS/IX 表意向锁属于存储引擎层；Server 这里不会直接决定具体 IS/IX/record/gap/next-key 锁")
      .field("estimated_rows", WzgIteratorDoubleToString(path->num_output_rows()))
      .field("next_step", WzgLockingReadNextStepText(table_ref))
      .emit();
}

void WzgEmitTableScanPlan(THD *thd, const AccessPath *path,
                          const JOIN *join) {
  if (path == nullptr || path->type != AccessPath::TABLE_SCAN ||
      thd == nullptr || thd->query().str == nullptr || thd->query().length == 0)
    return;

  const auto &param = path->table_scan();
  Query_block *query_block = join == nullptr ? nullptr : join->query_block;
  wzg_probe::Event event(thd, "executor.table_scan", "instant");
  WzgAddExecutorFactFields(event, "iterator", "全表扫描读取器",
                           "scan_table", "ready",
                           "准备按 handler 顺序接口读取整张表",
                           "table", WzgIteratorTableName(param.table),
                           "WzgEmitTableScanPlan")
      .message("执行器准备做全表扫描，从表数据中逐行读取记录")
      .sql_command(get_sql_command_string(thd->lex->sql_command))
      .field("runtime.query_block_number",
             query_block == nullptr
                 ? std::uint64_t{0}
                 : static_cast<std::uint64_t>(query_block->select_number))
      .field("runtime.table", WzgIteratorTableName(param.table))
      .field("runtime.scan_type", "TABLE_SCAN")
      .field("runtime.handler_init_call", "ha_rnd_init(true)")
      .field("runtime.handler_read_call", "ha_rnd_next")
      .field("runtime.table_rows_estimate", WzgTableStatsRowsText(param.table))
      .field("runtime.estimated_output_rows",
             WzgIteratorDoubleToString(path->num_output_rows()))
      .field("runtime.estimated_cost", WzgIteratorDoubleToString(path->cost()))
      .field("runtime.counts_examined_rows", path->count_examined_rows)
      .field("decision.summary", "执行器将逐行扫描整张表")
      .field("decision.reason",
             "当前访问路径没有可用索引 key 或范围边界来直接定位候选记录")
      .field("decision.result",
             "TableScanIterator 会通过 handler 随机/顺序表扫描接口获取下一条记录")
      .field("decision.impact",
             "读出的行继续进入过滤、JOIN、排序、聚合或结果发送节点")
      .field("explain_zh.handler_read_call",
             "ha_rnd_next 是 handler 层获取下一条表记录的接口")
      .field("query_block_number",
             query_block == nullptr
                 ? std::uint64_t{0}
                 : static_cast<std::uint64_t>(query_block->select_number))
      .field("table", WzgIteratorTableName(param.table))
      .field("scan_type", "TABLE_SCAN，全表扫描")
      .field("handler_init_call", "ha_rnd_init(true)")
      .field("handler_read_call", "ha_rnd_next")
      .field("table_rows_estimate", WzgTableStatsRowsText(param.table))
      .field("estimated_output_rows",
             WzgIteratorDoubleToString(path->num_output_rows()))
      .field("estimated_cost", WzgIteratorDoubleToString(path->cost()))
      .field("counts_examined_rows", path->count_examined_rows)
      .field("why_this_read",
             "当前访问路径没有使用索引定位边界或 key，执行器会请求存储引擎按表记录顺序逐行返回")
      .field("parameter_meaning",
             "ha_rnd_init(true) 表示开始随机/顺序表扫描；ha_rnd_next 每次从表中取下一条记录；成功时记录写入 table->record[0]")
      .field("next_step",
             "TableScanIterator::Read 调用 handler，存储引擎逐行返回记录给 SQL 层过滤、JOIN 或返回客户端")
      .emit();
}

void WzgEmitIndexScanPlan(THD *thd, const AccessPath *path,
                          const JOIN *join) {
  if (path == nullptr || path->type != AccessPath::INDEX_SCAN ||
      thd == nullptr || thd->query().str == nullptr || thd->query().length == 0)
    return;

  const auto &param = path->index_scan();
  Query_block *query_block = join == nullptr ? nullptr : join->query_block;
  wzg_probe::Event event(thd, "executor.index_scan", "instant");
  WzgAddExecutorFactFields(event, "iterator", "索引扫描读取器",
                           "scan_index", "ready",
                           "准备按索引叶子顺序扫描记录",
                           "index", WzgIteratorIndexName(param.table, param.idx),
                           "WzgEmitIndexScanPlan")
      .message("执行器准备做全索引扫描，按索引顺序逐条读取记录")
      .sql_command(get_sql_command_string(thd->lex->sql_command))
      .field("runtime.query_block_number",
             query_block == nullptr
                 ? std::uint64_t{0}
                 : static_cast<std::uint64_t>(query_block->select_number))
      .field("runtime.table", WzgIteratorTableName(param.table))
      .field("runtime.index", WzgIteratorIndexName(param.table, param.idx))
      .field("runtime.scan_type", "INDEX_SCAN")
      .field("runtime.scan_direction", param.reverse ? "reverse" : "forward")
      .field("runtime.handler_init_call", "ha_index_init")
      .field("runtime.handler_read_calls",
             WzgIndexScanHandlerCallsText(param.reverse))
      .field("runtime.use_index_order", param.use_order)
      .field("runtime.covering_index", WzgIndexCoveringText(param.table, param.idx))
      .field("runtime.table_rows_estimate", WzgTableStatsRowsText(param.table))
      .field("runtime.estimated_output_rows",
             WzgIteratorDoubleToString(path->num_output_rows()))
      .field("runtime.estimated_cost", WzgIteratorDoubleToString(path->cost()))
      .field("runtime.counts_examined_rows", path->count_examined_rows)
      .field("decision.summary", "执行器将扫描整个索引")
      .field("decision.reason",
             "当前计划需要索引顺序或覆盖索引收益，但没有具体 lookup key 或范围边界")
      .field("decision.result",
             "IndexScanIterator 会按方向调用 handler 索引首条/下一条接口")
      .field("decision.impact",
             "索引顺序可能满足 ORDER BY，覆盖索引时可减少回表")
      .field("explain_zh.covering_index",
             "覆盖索引表示所需列可从索引记录取得，可能不需要读取完整聚簇记录")
      .field("query_block_number",
             query_block == nullptr
                 ? std::uint64_t{0}
                 : static_cast<std::uint64_t>(query_block->select_number))
      .field("table", WzgIteratorTableName(param.table))
      .field("index", WzgIteratorIndexName(param.table, param.idx))
      .field("scan_type", "INDEX_SCAN，全索引扫描")
      .field("scan_direction", WzgIndexScanDirectionText(param.reverse))
      .field("handler_init_call", "ha_index_init")
      .field("handler_read_calls", WzgIndexScanHandlerCallsText(param.reverse))
      .field("use_index_order",
             param.use_order ? "是；上层需要保留索引顺序"
                             : "否；当前节点不要求按索引顺序向上返回")
      .field("covering_index", WzgIndexCoveringText(param.table, param.idx))
      .field("table_rows_estimate", WzgTableStatsRowsText(param.table))
      .field("estimated_output_rows",
             WzgIteratorDoubleToString(path->num_output_rows()))
      .field("estimated_cost", WzgIteratorDoubleToString(path->cost()))
      .field("counts_examined_rows", path->count_examined_rows)
      .field("why_this_read",
             "当前计划选择扫描整个索引叶子；它不是按某个具体 key 定位，也不是读取一段范围边界")
      .field("parameter_meaning",
             "idx 表示要扫描的索引编号；use_order 表示是否需要保持索引顺序；reverse 决定从索引开头还是末尾开始读")
      .field("next_step",
             "IndexScanIterator 调用 handler 索引扫描接口，存储引擎按索引顺序返回记录")
      .emit();
}

std::string WzgSortItemText(THD *thd, const st_sort_field &sort_field) {
  if (sort_field.item == nullptr) return "未知表达式";
  char buffer[512];
  String text(buffer, sizeof(buffer), system_charset_info);
  text.length(0);
  sort_field.item->print(thd, &text, QT_ORDINARY);
  std::string value =
      text.length() == 0 ? "未知表达式" : std::string(text.ptr(), text.length());
  value.append(sort_field.reverse ? " DESC" : " ASC");
  return value;
}

std::string WzgSortOrderText(THD *thd, const Filesort *filesort) {
  if (filesort == nullptr || filesort->sortorder == nullptr ||
      filesort->sort_order_length() == 0)
    return "无排序表达式";

  constexpr uint kMaxPrintedSortFields = 8;
  std::string value;
  const uint sort_fields =
      std::min(filesort->sort_order_length(), kMaxPrintedSortFields);
  for (uint i = 0; i < sort_fields; ++i) {
    if (!value.empty()) value.append("；");
    value.append(std::to_string(i + 1));
    value.append(". ");
    value.append(WzgSortItemText(thd, filesort->sortorder[i]));
  }
  if (filesort->sort_order_length() > sort_fields) {
    value.append("；还有 ");
    value.append(std::to_string(filesort->sort_order_length() - sort_fields));
    value.append(" 个排序字段未展开");
  }
  return value;
}

std::string WzgSortTablesText(const Filesort *filesort) {
  if (filesort == nullptr || filesort->tables.empty()) return "无";
  constexpr size_t kMaxPrintedTables = 8;
  std::string value;
  const size_t tables = std::min(filesort->tables.size(), kMaxPrintedTables);
  for (size_t i = 0; i < tables; ++i) {
    if (!value.empty()) value.append(", ");
    value.append(WzgIteratorTableName(filesort->tables[i]));
  }
  if (filesort->tables.size() > tables) {
    value.append(", 还有 ");
    value.append(std::to_string(filesort->tables.size() - tables));
    value.append(" 张表未展开");
  }
  return value;
}

std::string WzgJoinInputText(const AccessPath *input);
std::string WzgSortRowsEstimateText(double rows);

std::string WzgOrderExpressionText(THD *thd, const ORDER *order) {
  if (order == nullptr) return "无";
  constexpr uint kMaxPrintedItems = 8;
  std::string value;
  uint printed = 0;
  for (const ORDER *item = order; item != nullptr && printed < kMaxPrintedItems;
       item = item->next, ++printed) {
    if (!value.empty()) value.append("；");
    value.append(std::to_string(printed + 1));
    value.append(". ");
    if (item->item == nullptr || *item->item == nullptr) {
      value.append("未知表达式");
      continue;
    }
    char buffer[512];
    String text(buffer, sizeof(buffer), system_charset_info);
    text.length(0);
    (*item->item)->print(thd, &text, QT_ORDINARY);
    value.append(text.length() == 0 ? "未知表达式"
                                    : std::string(text.ptr(), text.length()));
    value.append(item->direction == ORDER_DESC ? " DESC" : " ASC");
  }
  if (order != nullptr && printed == kMaxPrintedItems)
    value.append("；还有更多分组字段未展开");
  return value.empty() ? "无" : value;
}

const char *WzgSumFuncTypeText(Item_sum::Sumfunctype type) {
  switch (type) {
    case Item_sum::COUNT_FUNC:
      return "COUNT";
    case Item_sum::COUNT_DISTINCT_FUNC:
      return "COUNT DISTINCT";
    case Item_sum::SUM_FUNC:
      return "SUM";
    case Item_sum::SUM_DISTINCT_FUNC:
      return "SUM DISTINCT";
    case Item_sum::AVG_FUNC:
      return "AVG";
    case Item_sum::AVG_DISTINCT_FUNC:
      return "AVG DISTINCT";
    case Item_sum::MIN_FUNC:
      return "MIN";
    case Item_sum::MAX_FUNC:
      return "MAX";
    case Item_sum::STD_FUNC:
      return "STD/STDDEV";
    case Item_sum::VARIANCE_FUNC:
      return "VARIANCE";
    case Item_sum::SUM_BIT_FUNC:
      return "BIT 聚合";
    case Item_sum::GROUP_CONCAT_FUNC:
      return "GROUP_CONCAT";
    case Item_sum::JSON_AGG_FUNC:
      return "JSON 聚合";
    case Item_sum::ROW_NUMBER_FUNC:
      return "ROW_NUMBER";
    case Item_sum::RANK_FUNC:
      return "RANK";
    case Item_sum::DENSE_RANK_FUNC:
      return "DENSE_RANK";
    case Item_sum::CUME_DIST_FUNC:
      return "CUME_DIST";
    case Item_sum::PERCENT_RANK_FUNC:
      return "PERCENT_RANK";
    case Item_sum::NTILE_FUNC:
      return "NTILE";
    case Item_sum::LEAD_LAG_FUNC:
      return "LEAD/LAG";
    case Item_sum::FIRST_LAST_VALUE_FUNC:
      return "FIRST_VALUE/LAST_VALUE";
    case Item_sum::NTH_VALUE_FUNC:
      return "NTH_VALUE";
    case Item_sum::GEOMETRY_AGGREGATE_FUNC:
      return "空间聚合";
    case Item_sum::UDF_SUM_FUNC:
      return "用户自定义聚合";
    case Item_sum::ROLLUP_SUM_SWITCHER_FUNC:
      return "ROLLUP 聚合切换器";
    default:
      return "窗口函数或其他聚合";
  }
}

std::string WzgAggregateFunctionsText(THD *thd, const JOIN *join) {
  if (join == nullptr || join->sum_funcs == nullptr ||
      join->sum_funcs[0] == nullptr)
    return "无聚合函数";

  constexpr uint kMaxPrintedItems = 8;
  std::string value;
  uint printed = 0;
  for (Item_sum **item = join->sum_funcs; *item != nullptr; ++item) {
    if (printed >= kMaxPrintedItems) {
      value.append("；还有更多聚合函数未展开");
      break;
    }
    if (!value.empty()) value.append("；");
    value.append(std::to_string(printed + 1));
    value.append(". ");
    value.append(WzgSumFuncTypeText((*item)->sum_func()));
    value.append(": ");
    char buffer[512];
    String text(buffer, sizeof(buffer), system_charset_info);
    text.length(0);
    (*item)->print(thd, &text, QT_ORDINARY);
    value.append(text.length() == 0 ? "未知聚合表达式"
                                    : std::string(text.ptr(), text.length()));
    ++printed;
  }
  return value.empty() ? "无聚合函数" : value;
}

std::string WzgAggregateTypeText(const JOIN *join, AccessPath::Type type,
                                 bool rollup) {
  if (type == AccessPath::TEMPTABLE_AGGREGATE)
    return "临时表聚合，先把分组结果写入内部临时表，再读取聚合后的行";
  if (rollup) return "ROLLUP 分组聚合，会额外产生小计/合计行";
  if (join != nullptr && join->implicit_grouping)
    return "无 GROUP BY 的整体聚合，所有输入行合成一组";
  if (join != nullptr && join->group_optimized_away)
    return "GROUP BY 被优化为单组或常量分组";
  if (join != nullptr && join->grouped)
    return "流式分组聚合，输入按分组顺序读取，同组行连续合并";
  return "聚合节点";
}

std::string WzgAggregateHowText(const JOIN *join, AccessPath::Type type) {
  if (type == AccessPath::TEMPTABLE_AGGREGATE)
    return "执行器读取输入行，按 GROUP BY key 在临时表中查找同组记录；找到则更新 COUNT/SUM 等值，找不到则插入新组";
  if (join != nullptr && join->implicit_grouping)
    return "执行器读取所有输入行，不按字段分组，把 COUNT/SUM 等聚合值累加成一行结果";
  if (join != nullptr && join->grouped)
    return "执行器按分组字段比较相邻输入行；同组时累加聚合值，分组变化时输出上一组结果";
  return "执行器从子节点读取行并计算聚合结果";
}

void WzgEmitAggregatePlan(THD *thd, const AccessPath *path, const JOIN *join) {
  if (path == nullptr || thd == nullptr || thd->query().str == nullptr ||
      thd->query().length == 0)
    return;
  if (path->type != AccessPath::AGGREGATE &&
      path->type != AccessPath::TEMPTABLE_AGGREGATE)
    return;
  if (join == nullptr) return;

  AccessPath *input = nullptr;
  const bool rollup = path->type == AccessPath::AGGREGATE &&
                      path->aggregate().olap == ROLLUP_TYPE;
  bool uses_temporary_table = false;
  std::string temp_table("无");
  if (path->type == AccessPath::AGGREGATE) {
    input = path->aggregate().child;
  } else {
    input = path->temptable_aggregate().subquery_path;
    uses_temporary_table = true;
    temp_table = WzgIteratorTableName(path->temptable_aggregate().table);
  }

  Query_block *query_block = join->query_block;
  WZG_PROBE_EVENT(thd, "executor.aggregate")
      .message(uses_temporary_table
                   ? "执行器准备使用临时表完成分组聚合"
                   : "执行器准备对读取结果做聚合计算")
      .sql_command(get_sql_command_string(thd->lex->sql_command))
      .field("query_block_number",
             query_block == nullptr
                 ? std::uint64_t{0}
                 : static_cast<std::uint64_t>(query_block->select_number))
      .field("aggregate_type", WzgAggregateTypeText(join, path->type, rollup))
      .field("group_by", WzgOrderExpressionText(thd, join->group_list.order))
      .field("aggregate_functions", WzgAggregateFunctionsText(thd, join))
      .field("input_source", WzgJoinInputText(input))
      .field("estimated_input_rows",
             input == nullptr ? "未知"
                              : WzgSortRowsEstimateText(input->num_output_rows()))
      .field("estimated_output_groups",
             WzgSortRowsEstimateText(path->num_output_rows()))
      .field("uses_temporary_table", uses_temporary_table)
      .field("temporary_table", temp_table)
      .field("rollup", rollup)
      .field("implicit_grouping", join->implicit_grouping)
      .field("group_optimized_away", join->group_optimized_away)
      .field("how_it_works", WzgAggregateHowText(join, path->type))
      .field("next_step",
             uses_temporary_table
                 ? "TemptableAggregateIterator 读取输入行，写入或更新聚合临时表，再扫描临时表输出结果"
                 : "AggregateIterator 从子节点读取行，按分组状态累加聚合值并输出结果")
      .emit();
}

std::string WzgSortPayloadText(const AccessPath *path, Filesort *filesort) {
  if (path == nullptr || path->type != AccessPath::SORT || filesort == nullptr)
    return "未知";
  if (path->sort().force_sort_rowids)
    return "排序记录 rowid，排序后再回表定位原始行";
  return filesort->using_addon_fields()
             ? "排序记录会带上需要返回的附加字段，排序后可直接继续处理"
             : "排序记录只保存 rowid，排序后需要按 rowid 取回原始行";
}

std::string WzgSortLimitText(ha_rows limit) {
  if (limit == HA_POS_ERROR) return "无限制";
  return std::to_string(limit);
}

std::string WzgSortRowsEstimateText(double rows) {
  if (rows < 0.0) return "未知";
  return WzgIteratorDoubleToString(rows);
}

std::string WzgJoinInputText(const AccessPath *input);

std::string WzgItemExpressionText(THD *thd, const Item *item) {
  if (item == nullptr) return "无";
  char buffer[1024];
  String text(buffer, sizeof(buffer), system_charset_info);
  text.length(0);
  item->print(thd, &text, QT_ORDINARY);
  return text.length() == 0 ? "未知表达式"
                            : std::string(text.ptr(), text.length());
}

void WzgEmitFilterEval(THD *thd, const AccessPath *path, const JOIN *join) {
  if (path == nullptr || path->type != AccessPath::FILTER || thd == nullptr ||
      thd->query().str == nullptr || thd->query().length == 0)
    return;

  const auto &param = path->filter();
  Query_block *query_block = join == nullptr ? nullptr : join->query_block;
  WZG_PROBE_EVENT(thd, "executor.filter_plan")
      .message("执行器准备在读取到行后判断过滤条件")
      .sql_command(get_sql_command_string(thd->lex->sql_command))
      .field("query_block_number",
             query_block == nullptr
                 ? std::uint64_t{0}
                 : static_cast<std::uint64_t>(query_block->select_number))
      .field("filter_condition", WzgItemExpressionText(thd, param.condition))
      .field("input_source", WzgJoinInputText(param.child))
      .field("estimated_input_rows",
             param.child == nullptr
                 ? "未知"
                 : WzgSortRowsEstimateText(param.child->num_output_rows()))
      .field("estimated_output_rows",
             WzgSortRowsEstimateText(path->num_output_rows()))
      .field("materialize_subqueries_before_filter",
             param.materialize_subqueries
                 ? "是；判断条件前会先完成条件中的可物化子查询"
                 : "否")
      .field("when_checked", "子节点每返回一行后立即判断这个条件")
      .field("pass_behavior", "条件结果为 TRUE 的行继续交给上层执行节点")
      .field("reject_behavior",
             "条件结果为 FALSE 或 NULL 的行不会继续向上返回")
      .field("note", "这里记录过滤节点准备判断的条件，不是逐行判断结果")
      .field("next_step", "FilterIterator 调用子节点 Read，拿到一行后计算条件表达式")
      .emit();
}

std::string WzgLimitRowsText(ha_rows rows) {
  if (rows == HA_POS_ERROR) return "无限制";
  return std::to_string(rows);
}

std::string WzgLimitReturnRowsText(ha_rows limit, ha_rows offset) {
  if (limit == HA_POS_ERROR) return "无限制";
  if (limit <= offset) return "0";
  return std::to_string(limit - offset);
}

void WzgEmitLimitOffset(THD *thd, const AccessPath *path, const JOIN *join) {
  if (path == nullptr || path->type != AccessPath::LIMIT_OFFSET ||
      thd == nullptr || thd->query().str == nullptr || thd->query().length == 0)
    return;

  const auto &param = path->limit_offset();
  Query_block *query_block = join == nullptr ? nullptr : join->query_block;
  WZG_PROBE_EVENT(thd, "executor.limit_offset")
      .message("执行器准备应用 LIMIT/OFFSET，控制最多向上返回多少行")
      .sql_command(get_sql_command_string(thd->lex->sql_command))
      .field("query_block_number",
             query_block == nullptr
                 ? std::uint64_t{0}
                 : static_cast<std::uint64_t>(query_block->select_number))
      .field("offset", WzgLimitRowsText(param.offset))
      .field("return_limit",
             WzgLimitReturnRowsText(param.limit, param.offset))
      .field("read_until_row", WzgLimitRowsText(param.limit))
      .field("internal_limit_meaning",
             "MySQL 这里的 read_until_row 包含被 OFFSET 跳过的行；SQL 层最多返回 return_limit 行")
      .field("count_all_rows",
             param.count_all_rows
                 ? "是；即使达到 LIMIT，也会继续读取子节点以统计完整行数"
                 : "否；达到 LIMIT 后可以停止继续读取")
      .field("reject_multiple_rows",
             param.reject_multiple_rows
                 ? "是；如果子节点返回超过允许行数，会按标量子查询等语义报错"
                 : "否")
      .field("input_source", WzgJoinInputText(param.child))
      .field("estimated_input_rows",
             param.child == nullptr
                 ? "未知"
                 : WzgSortRowsEstimateText(param.child->num_output_rows()))
      .field("estimated_output_rows",
             WzgSortRowsEstimateText(path->num_output_rows()))
      .field("when_applied",
             "子节点每返回一行后，先跳过 OFFSET 指定的行数，再最多放行 LIMIT 指定的行数")
      .field("pass_behavior", "位于 LIMIT/OFFSET 范围内的行继续交给上层节点或客户端")
      .field("stop_behavior",
             param.count_all_rows
                 ? "达到 LIMIT 后仍继续读完子节点，只是不再向上返回额外行"
                 : "看到 read_until_row 指定的行数后停止继续向子节点读取")
      .field("note",
             "这里解释 LIMIT/OFFSET 节点；如果 filesort.limit 显示无限制，LIMIT 可能就在这个节点处理")
      .field("next_step",
             "LimitOffsetIterator 调用子节点 Read，并按 offset/limit 计数决定是否返回这一行")
      .emit();
}

void WzgEmitSortPlan(THD *thd, const AccessPath *path, const JOIN *join) {
  if (path == nullptr || path->type != AccessPath::SORT || thd == nullptr ||
      thd->query().str == nullptr || thd->query().length == 0)
    return;

  const auto &param = path->sort();
  Filesort *filesort = param.filesort;
  if (filesort == nullptr) return;

  Query_block *query_block = join == nullptr ? nullptr : join->query_block;
  WZG_PROBE_EVENT(thd, "executor.sort")
      .message(filesort->m_remove_duplicates
                   ? "执行器准备对结果做 filesort，并在排序过程中去重"
                   : "执行器准备对读取结果做 filesort 排序")
      .sql_command(get_sql_command_string(thd->lex->sql_command))
      .field("query_block_number",
             query_block == nullptr
                 ? std::uint64_t{0}
                 : static_cast<std::uint64_t>(query_block->select_number))
      .field("sort_type", "filesort")
      .field("sort_by", WzgSortOrderText(thd, filesort))
      .field("sort_tables", WzgSortTablesText(filesort))
      .field("remove_duplicates", filesort->m_remove_duplicates)
      .field("limit", WzgSortLimitText(filesort->limit))
      .field("estimated_input_rows",
             param.child == nullptr ? "未知"
                                    : WzgSortRowsEstimateText(
                                          param.child->num_output_rows()))
      .field("estimated_output_rows", WzgSortRowsEstimateText(path->num_output_rows()))
      .field("sort_payload", WzgSortPayloadText(path, filesort))
      .field("needs_rowid_fetch",
             param.tables_to_get_rowid_for == 0
                 ? "否"
                 : "是；排序后需要按 rowid 回到相关表取完整行")
      .field("reason",
             filesort->m_remove_duplicates
                 ? "当前计划需要排序后的唯一结果，执行器使用 filesort 完成排序和去重"
                 : "当前执行计划不能直接按目标顺序返回结果，需要额外排序")
      .field("result_destination", "排序后的行继续交给上层执行节点或返回客户端")
      .field("next_step", "SortingIterator 从子节点读取行，生成排序 key，再调用 filesort 排序")
      .emit();
}

std::string WzgMaterializeLimitText(ha_rows limit) {
  if (limit == HA_POS_ERROR) return "无限制";
  return std::to_string(limit);
}

std::string WzgRawSqlLowerText(THD *thd) {
  if (thd == nullptr || thd->query().str == nullptr ||
      thd->query().length == 0)
    return "";

  std::string sql(thd->query().str, thd->query().length);
  std::transform(sql.begin(), sql.end(), sql.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return sql;
}

bool WzgRawSqlContainsWord(THD *thd, const char *word) {
  if (word == nullptr) return false;
  const std::string sql = WzgRawSqlLowerText(thd);
  return !sql.empty() && sql.find(word) != std::string::npos;
}

bool WzgRawSqlContainsSetOperation(THD *thd) {
  return WzgRawSqlContainsWord(thd, "union") ||
         WzgRawSqlContainsWord(thd, "intersect") ||
         WzgRawSqlContainsWord(thd, "except");
}

std::string WzgSetOperationNameText(THD *thd, AccessPath::Type type) {
  const std::string sql = WzgRawSqlLowerText(thd);
  const bool has_union = sql.find("union") != std::string::npos;
  const bool has_union_all = sql.find("union all") != std::string::npos;
  const bool has_union_distinct =
      sql.find("union distinct") != std::string::npos;
  const bool has_intersect = sql.find("intersect") != std::string::npos;
  const bool has_except = sql.find("except") != std::string::npos;
  if (has_intersect && has_except) return "INTERSECT / EXCEPT 组合集合操作";
  if (has_intersect) return "INTERSECT，保留多个 SELECT 都出现的行";
  if (has_except) return "EXCEPT，保留左侧 SELECT 有、右侧 SELECT 没有的行";
  if (has_union_all && has_union_distinct)
    return "UNION 混合 DISTINCT 和 ALL";
  if (has_union_all && type == AccessPath::APPEND)
    return "UNION ALL，直接拼接多个 SELECT 的结果";
  if (has_union_all) return "UNION ALL，保留重复行";
  if (has_union) return "UNION DISTINCT，合并多个 SELECT 后去重";
  return "集合操作";
}

std::string WzgMaterializeAccessTypeText(AccessPath::Type type) {
  switch (type) {
    case AccessPath::MATERIALIZE:
      return "MATERIALIZE，物化子查询、派生表、CTE 或 UNION 等中间结果";
    case AccessPath::STREAM:
      return "STREAM，把上游结果流式写入内部临时结果";
    case AccessPath::TEMPTABLE_AGGREGATE:
      return "TEMPTABLE_AGGREGATE，先写临时表再读取聚合结果";
    case AccessPath::MATERIALIZED_TABLE_FUNCTION:
      return "MATERIALIZED_TABLE_FUNCTION，先执行表函数并写成内部表";
    case AccessPath::MATERIALIZE_INFORMATION_SCHEMA_TABLE:
      return "MATERIALIZE_INFORMATION_SCHEMA_TABLE，物化 information_schema 查询结果";
    default:
      return "不是物化节点";
  }
}

std::string WzgMaterializeOperandText(const MaterializePathParameters *param) {
  if (param == nullptr || param->m_operands.empty()) return "无输入查询块";
  constexpr size_t kMaxPrintedOperands = 8;
  std::string value;
  const size_t count = std::min(param->m_operands.size(), kMaxPrintedOperands);
  for (size_t i = 0; i < count; ++i) {
    const MaterializePathParameters::Operand &operand = param->m_operands[i];
    if (!value.empty()) value.append("；");
    value.append(std::to_string(i + 1));
    value.append(". 查询块 ");
    value.append(std::to_string(operand.select_number));
    value.append("，预计输出 ");
    value.append(operand.subquery_path == nullptr
                     ? "未知"
                     : WzgSortRowsEstimateText(
                           operand.subquery_path->num_output_rows()));
    value.append(" 行");
    if (operand.copy_items) value.append("，写入时复制表达式结果");
    if (operand.is_recursive_reference) value.append("，递归引用输入");
    if (operand.disable_deduplication_by_hash_field)
      value.append("，这一支不使用 hash 字段去重");
  }
  if (param->m_operands.size() > count) {
    value.append("；还有 ");
    value.append(std::to_string(param->m_operands.size() - count));
    value.append(" 个输入未展开");
  }
  return value;
}

std::string WzgMaterializeDedupText(const MaterializePathParameters *param) {
  if (param == nullptr || param->m_operands.empty()) return "未知";
  if (param->m_operands.size() > 1)
    return "可能需要按 UNION/INTERSECT/EXCEPT 等集合语义去重或计数，具体由各输入和临时表参数决定";
  const MaterializePathParameters::Operand &operand = param->m_operands[0];
  if (operand.disable_deduplication_by_hash_field)
    return "不使用 hash 字段去重";
  return "单个输入，通常只是写入中间结果；是否唯一由临时表键和上层计划决定";
}

std::string WzgMaterializeTargetText(const TABLE *table) {
  if (table == nullptr) return "内部临时结果";
  std::string name = WzgIteratorTableName(table);
  return name == "<unknown>" || name == "无" ? "内部临时结果" : name;
}

std::string WzgMaterializeReasonText(const AccessPath *path) {
  if (path == nullptr) return "未知";
  switch (path->type) {
    case AccessPath::MATERIALIZE:
      return "当前计划需要先生成一份中间结果，再由外层查询、集合操作或后续读取器继续使用";
    case AccessPath::STREAM:
      return "当前计划需要把上游行流式写入临时结果，供后续节点读取";
    case AccessPath::TEMPTABLE_AGGREGATE:
      return "当前计划使用临时表保存分组或聚合中间结果，再读取聚合后的行";
    case AccessPath::MATERIALIZED_TABLE_FUNCTION:
      return "表函数需要先产出行并写入内部表，再像普通表一样读取";
    case AccessPath::MATERIALIZE_INFORMATION_SCHEMA_TABLE:
      return "information_schema 的结果需要先生成内部表，再应用读取或过滤";
    default:
      return "不是物化节点";
  }
}

TABLE *WzgMaterializeTargetTable(const AccessPath *path) {
  switch (path->type) {
    case AccessPath::MATERIALIZE:
      return path->materialize().param == nullptr ? nullptr
                                                  : path->materialize().param->table;
    case AccessPath::STREAM:
      return path->stream().table;
    case AccessPath::TEMPTABLE_AGGREGATE:
      return path->temptable_aggregate().table;
    case AccessPath::MATERIALIZED_TABLE_FUNCTION:
      return path->materialized_table_function().table;
    case AccessPath::MATERIALIZE_INFORMATION_SCHEMA_TABLE:
      return path->materialize_information_schema_table().table_list == nullptr
                 ? nullptr
                 : path->materialize_information_schema_table().table_list->table;
    default:
      return nullptr;
  }
}

std::string WzgSetOperationAppendInputsText(
    const Mem_root_array<AppendPathParameters> *children) {
  if (children == nullptr || children->empty()) return "无输入分支";
  constexpr size_t kMaxPrintedChildren = 8;
  std::string value;
  const size_t count = std::min(children->size(), kMaxPrintedChildren);
  for (size_t i = 0; i < count; ++i) {
    const AppendPathParameters &child = (*children)[i];
    if (!value.empty()) value.append("；");
    value.append(std::to_string(i + 1));
    value.append(". ");
    if (child.join != nullptr && child.join->query_block != nullptr) {
      value.append("查询块 ");
      value.append(std::to_string(child.join->query_block->select_number));
    } else {
      value.append("已物化的集合结果或子集合");
    }
    value.append("，访问路径 ");
    value.append(child.path == nullptr ? "未知"
                                       : WzgIteratorAccessPathName(
                                             child.path->type));
    value.append("，预计输出 ");
    value.append(child.path == nullptr
                     ? "未知"
                     : WzgSortRowsEstimateText(child.path->num_output_rows()));
    value.append(" 行");
  }
  if (children->size() > count) {
    value.append("；还有 ");
    value.append(std::to_string(children->size() - count));
    value.append(" 个输入分支未展开");
  }
  return value;
}

std::string WzgSetOperationMaterializeInputsText(
    const MaterializePathParameters *param) {
  if (param == nullptr || param->m_operands.empty()) return "无输入分支";
  constexpr size_t kMaxPrintedOperands = 8;
  std::string value;
  const size_t count = std::min(param->m_operands.size(), kMaxPrintedOperands);
  for (size_t i = 0; i < count; ++i) {
    const MaterializePathParameters::Operand &operand = param->m_operands[i];
    if (!value.empty()) value.append("；");
    value.append(std::to_string(i + 1));
    value.append(". 查询块 ");
    value.append(std::to_string(operand.select_number));
    value.append("，集合分支序号 ");
    value.append(std::to_string(operand.m_operand_idx + 1));
    value.append("/");
    value.append(std::to_string(operand.m_total_operands));
    value.append("，预计输出 ");
    value.append(operand.subquery_path == nullptr
                     ? "未知"
                     : WzgSortRowsEstimateText(
                           operand.subquery_path->num_output_rows()));
    value.append(" 行");
    value.append(operand.disable_deduplication_by_hash_field
                     ? "，这一支保留重复行"
                     : "，这一支参与集合去重/匹配");
  }
  if (param->m_operands.size() > count) {
    value.append("；还有 ");
    value.append(std::to_string(param->m_operands.size() - count));
    value.append(" 个输入分支未展开");
  }
  return value;
}

std::string WzgSetOperationDedupText(THD *thd, const AccessPath *path) {
  if (path == nullptr) return "未知";
  if (path->type == AccessPath::APPEND)
    return "不去重；APPEND/UNION ALL 会按分支顺序保留重复行";
  if (path->type != AccessPath::MATERIALIZE)
    return "不是集合操作物化节点";

  const MaterializePathParameters *param = path->materialize().param;
  if (param == nullptr || param->m_operands.empty()) return "未知";
  if (WzgRawSqlContainsWord(thd, "intersect"))
    return "需要按集合语义保留多个输入都出现的行，临时结果会辅助记录匹配情况";
  if (WzgRawSqlContainsWord(thd, "except"))
    return "需要从左侧结果中排除右侧出现的行，临时结果会辅助记录匹配情况";

  bool has_dedup_branch = false;
  bool has_all_branch = false;
  for (const MaterializePathParameters::Operand &operand : param->m_operands) {
    has_dedup_branch |= !operand.disable_deduplication_by_hash_field;
    has_all_branch |= operand.disable_deduplication_by_hash_field;
  }
  if (has_dedup_branch && has_all_branch)
    return "混合去重；UNION DISTINCT 部分去重，UNION ALL 部分保留重复行";
  if (has_dedup_branch)
    return "去重；写入集合临时结果时只保留不重复的结果行";
  return "不去重；所有输入分支写入时保留重复行";
}

std::string WzgSetOperationMethodText(const AccessPath *path) {
  if (path == nullptr) return "未知";
  if (path->type == AccessPath::APPEND)
    return "APPEND，依次读取每个 SELECT 分支并把结果向上返回";
  if (path->type == AccessPath::MATERIALIZE)
    return "MATERIALIZE，先把多个 SELECT 分支写入内部临时结果，再从临时结果读取";
  return "未知";
}

std::string WzgSetOperationNextStepText(const AccessPath *path) {
  if (path == nullptr) return "未知";
  if (path->type == AccessPath::APPEND)
    return "AppendIterator 先读第一个分支，读完后切到下一个分支";
  if (path->type == AccessPath::MATERIALIZE)
    return "MaterializeIterator 执行各个输入分支，把行写入集合临时结果并按集合语义处理重复";
  return "继续执行当前访问路径";
}

void WzgEmitSetOperationPlan(THD *thd, const AccessPath *path,
                             const JOIN *join) {
  if (path == nullptr || thd == nullptr || thd->query().str == nullptr ||
      thd->query().length == 0 || !WzgRawSqlContainsSetOperation(thd))
    return;
  if (path->type != AccessPath::APPEND && path->type != AccessPath::MATERIALIZE)
    return;

  const MaterializePathParameters *materialize_param =
      path->type == AccessPath::MATERIALIZE ? path->materialize().param
                                            : nullptr;
  const bool materialize_set_op =
      materialize_param != nullptr && materialize_param->m_operands.size() > 1;
  if (path->type == AccessPath::MATERIALIZE && !materialize_set_op) return;

  Query_block *query_block = join == nullptr ? nullptr : join->query_block;
  wzg_probe::Event event(thd, "executor.set_operation", "instant");
  event.message(path->type == AccessPath::APPEND
                    ? "执行器准备按 UNION ALL 方式拼接多个 SELECT 分支结果"
                    : "执行器准备把集合操作分支写入内部临时结果，并按 UNION/INTERSECT/EXCEPT 语义处理")
      .sql_command(get_sql_command_string(thd->lex->sql_command))
      .field("query_block_number",
             query_block == nullptr
                 ? std::uint64_t{0}
                 : static_cast<std::uint64_t>(query_block->select_number))
      .field("set_operation", WzgSetOperationNameText(thd, path->type))
      .field("merge_method", WzgSetOperationMethodText(path))
      .field("estimated_output_rows",
             WzgSortRowsEstimateText(path->num_output_rows()))
      .field("deduplication", WzgSetOperationDedupText(thd, path))
      .field("result_destination",
             path->type == AccessPath::APPEND
                 ? "直接交给上层节点或客户端"
                 : "内部集合临时结果，后续再读取并返回")
      .field("next_step", WzgSetOperationNextStepText(path));

  if (path->type == AccessPath::APPEND) {
    const auto &param = path->append();
    event.field("input_branch_count",
                param.children == nullptr
                    ? std::uint64_t{0}
                    : static_cast<std::uint64_t>(param.children->size()))
        .field("input_branches",
               WzgSetOperationAppendInputsText(param.children))
        .field("duplicate_behavior",
               "UNION ALL 不比较行是否重复，每个分支读到的行都会继续向上输出");
  } else {
    event.field("input_branch_count",
                materialize_param == nullptr
                    ? std::uint64_t{0}
                    : static_cast<std::uint64_t>(
                          materialize_param->m_operands.size()))
        .field("input_branches",
               WzgSetOperationMaterializeInputsText(materialize_param))
        .field("temporary_result",
               WzgMaterializeTargetText(WzgMaterializeTargetTable(path)))
        .field("limit_rows",
               materialize_param == nullptr
                   ? "未知"
                   : WzgMaterializeLimitText(materialize_param->limit_rows))
        .field("duplicate_behavior",
               "是否保留重复行由集合操作类型决定：UNION DISTINCT 去重，UNION ALL 保留，INTERSECT/EXCEPT 按集合匹配规则处理");
  }
  event.emit();
}

void WzgEmitMaterializePlan(THD *thd, const AccessPath *path,
                            const JOIN *join) {
  if (path == nullptr || thd == nullptr || thd->query().str == nullptr ||
      thd->query().length == 0)
    return;

  Query_block *query_block = join == nullptr ? nullptr : join->query_block;
  wzg_probe::Event event(thd, "executor.materialize", "instant");
  event.message("执行器准备把中间结果写入内部临时结果，后续再读取或匹配")
      .sql_command(get_sql_command_string(thd->lex->sql_command))
      .field("query_block_number",
             query_block == nullptr
                 ? std::uint64_t{0}
                 : static_cast<std::uint64_t>(query_block->select_number))
      .field("materialize_type", WzgMaterializeAccessTypeText(path->type))
      .field("result_table",
             WzgMaterializeTargetText(WzgMaterializeTargetTable(path)))
      .field("estimated_output_rows",
             WzgSortRowsEstimateText(path->num_output_rows()))
      .field("reason", WzgMaterializeReasonText(path))
      .field("result_destination",
             "内部临时结果，后续由执行计划中的其他读取器继续读取");

  switch (path->type) {
    case AccessPath::MATERIALIZE: {
      const MaterializePathParameters *param = path->materialize().param;
      if (param == nullptr) return;
      event.field("operand_count",
                  static_cast<std::uint64_t>(param->m_operands.size()))
          .field("input_sources", WzgMaterializeOperandText(param))
          .field("deduplication", WzgMaterializeDedupText(param))
          .field("rematerialize_each_scan", param->rematerialize)
          .field("limit_rows", WzgMaterializeLimitText(param->limit_rows))
          .field("reject_multiple_rows", param->reject_multiple_rows)
          .field("cte_materialization", param->cte == nullptr ? "否" : "是")
          .field("next_step",
                 "MaterializeIterator 执行输入计划，把产生的行写入内部临时结果");
      break;
    }
    case AccessPath::STREAM:
      event.field("operand_count", std::uint64_t{1})
          .field("input_sources", "上游子节点的输出行")
          .field("deduplication", "不在 STREAM 节点去重")
          .field("provide_rowid",
                 path->stream().provide_rowid
                     ? "需要提供 rowid，供上层之后回到原始行"
                     : "不需要提供 rowid")
          .field("next_step", "StreamingIterator 读取上游行并写入内部临时结果");
      break;
    case AccessPath::TEMPTABLE_AGGREGATE:
      event.field("operand_count", std::uint64_t{1})
          .field("input_sources", "聚合输入子计划")
          .field("deduplication", "按聚合/分组临时表规则合并同组行")
          .field("next_step",
                 "TemptableAggregateIterator 先写聚合临时表，再读取聚合结果");
      break;
    case AccessPath::MATERIALIZED_TABLE_FUNCTION:
      event.field("operand_count", std::uint64_t{1})
          .field("input_sources", "表函数输出")
          .field("deduplication", "不在表函数物化节点去重")
          .field("next_step",
                 "MaterializedTableFunctionIterator 执行表函数并写入内部表");
      break;
    case AccessPath::MATERIALIZE_INFORMATION_SCHEMA_TABLE:
      event.field("operand_count", std::uint64_t{1})
          .field("input_sources", "information_schema 内部查询结果")
          .field("deduplication", "不在该节点单独说明去重")
          .field("next_step",
                 "MaterializeInformationSchemaTableIterator 生成内部结果表并应用条件");
      break;
    default:
      return;
  }
  event.emit();
}

const char *WzgJoinTypeName(JoinType join_type) {
  switch (join_type) {
    case JoinType::INNER:
      return "INNER JOIN";
    case JoinType::OUTER:
      return "LEFT JOIN";
    case JoinType::ANTI:
      return "ANTI JOIN";
    case JoinType::SEMI:
      return "SEMI JOIN";
    case JoinType::FULL_OUTER:
      return "FULL OUTER JOIN";
  }
  return "UNKNOWN JOIN";
}

const char *WzgJoinTypeMeaning(JoinType join_type) {
  switch (join_type) {
    case JoinType::INNER:
      return "只返回两边都匹配的行";
    case JoinType::OUTER:
      return "保留外层输入的行，内层没有匹配时补 NULL";
    case JoinType::ANTI:
      return "返回外层中找不到内层匹配的行，常见于 NOT EXISTS";
    case JoinType::SEMI:
      return "只判断外层行是否存在匹配，常见于 EXISTS/IN，不需要返回内层重复行";
    case JoinType::FULL_OUTER:
      return "保留左右两边未匹配的行";
  }
  return "未知 JOIN 类型";
}

std::string WzgJoinInputText(const AccessPath *input) {
  if (input == nullptr) return "未知输入";
  TABLE *table = WzgIteratorTargetTable(input);
  std::string value = WzgIteratorReadableName(input->type);
  if (table != nullptr) {
    value.append("，目标表 ");
    value.append(WzgIteratorTableName(table));
  } else {
    value.append("，输入来自子计划");
  }
  value.append("，预计输出 ");
  value.append(WzgSortRowsEstimateText(input->num_output_rows()));
  value.append(" 行");
  return value;
}

std::string WzgJoinConditionText(THD *thd, const JoinPredicate *predicate) {
  if (predicate == nullptr || predicate->expr == nullptr)
    return "无可直接展示的 JOIN 条件";

  std::string value;
  constexpr size_t kMaxPrintedConditions = 6;
  size_t printed = 0;
  for (Item_eq_base *cond : predicate->expr->equijoin_conditions) {
    if (cond == nullptr) continue;
    if (!value.empty()) value.append("；");
    if (printed >= kMaxPrintedConditions) {
      value.append("还有更多等值条件未展开");
      break;
    }
    char buffer[512];
    String text(buffer, sizeof(buffer), system_charset_info);
    text.length(0);
    cond->print(thd, &text, QT_ORDINARY);
    value.append(text.length() == 0 ? "未知等值条件"
                                    : std::string(text.ptr(), text.length()));
    ++printed;
  }
  return value.empty() ? "无等值 JOIN 条件或条件已在其他节点处理" : value;
}

std::string WzgJoinMethodName(AccessPath::Type type) {
  switch (type) {
    case AccessPath::NESTED_LOOP_JOIN:
      return "Nested Loop Join，嵌套循环 JOIN";
    case AccessPath::NESTED_LOOP_SEMIJOIN_WITH_DUPLICATE_REMOVAL:
      return "Nested Loop Semijoin With Duplicate Removal，带去重的半连接嵌套循环";
    case AccessPath::BKA_JOIN:
      return "Batched Key Access Join，批量索引 JOIN";
    case AccessPath::HASH_JOIN:
      return "Hash Join，哈希 JOIN";
    default:
      return "不是 JOIN 执行节点";
  }
}

std::string WzgJoinMethodMeaning(const AccessPath *path) {
  if (path == nullptr) return "未知";
  switch (path->type) {
    case AccessPath::NESTED_LOOP_JOIN:
      return "先读外层输入，每得到一行就驱动内层输入读取匹配行";
    case AccessPath::NESTED_LOOP_SEMIJOIN_WITH_DUPLICATE_REMOVAL:
      return "先读外层输入，再读内层匹配行；对指定 key 去重，避免同一个外层 key 产生重复半连接结果";
    case AccessPath::BKA_JOIN:
      return "先攒一批外层行的 key，再批量请求内层索引读取，减少逐行随机访问";
    case AccessPath::HASH_JOIN:
      return "先读取 build 侧建立哈希表，再读取 probe 侧用 JOIN key 到哈希表中匹配";
    default:
      return "不是 JOIN 执行节点";
  }
}

void WzgEmitJoinMethod(THD *thd, const AccessPath *path, const JOIN *join) {
  if (path == nullptr || thd == nullptr || thd->query().str == nullptr ||
      thd->query().length == 0)
    return;
  if (path->type != AccessPath::NESTED_LOOP_JOIN &&
      path->type != AccessPath::NESTED_LOOP_SEMIJOIN_WITH_DUPLICATE_REMOVAL &&
      path->type != AccessPath::BKA_JOIN && path->type != AccessPath::HASH_JOIN)
    return;

  Query_block *query_block = join == nullptr ? nullptr : join->query_block;

  switch (path->type) {
    case AccessPath::NESTED_LOOP_JOIN: {
      const auto &param = path->nested_loop_join();
      wzg_probe::Event event(thd, "executor.join_method", "instant");
      WzgAddExecutorFactFields(event, "join", "JOIN 读取器",
                               "choose_join_method", "ready",
                               "准备按嵌套循环方式执行 JOIN",
                               "join", WzgJoinMethodName(path->type),
                               "WzgEmitJoinMethod")
          .message("执行器准备按嵌套循环方式执行 JOIN")
          .sql_command(get_sql_command_string(thd->lex->sql_command))
          .field("runtime.query_block_number",
                 query_block == nullptr
                     ? std::uint64_t{0}
                     : static_cast<std::uint64_t>(
                           query_block->select_number))
          .field("runtime.join_method", WzgJoinMethodName(path->type))
          .field("runtime.join_type", WzgJoinTypeName(param.join_type))
          .field("runtime.outer_input", WzgJoinInputText(param.outer))
          .field("runtime.inner_input", WzgJoinInputText(param.inner))
          .field("runtime.join_condition",
                 WzgJoinConditionText(thd, param.join_predicate))
          .field("runtime.batch_mode", param.pfs_batch_mode)
          .field("decision.summary", WzgJoinMethodMeaning(path))
          .field("decision.reason",
                 "该 JOIN 节点使用外层行驱动内层读取的执行模型")
          .field("decision.result",
                 "NestedLoopIterator 后续反复读取 outer 并初始化 inner")
          .field("decision.impact",
                 "inner_input 的读取次数通常受 outer_input 产生的行数影响")
          .field("explain_zh.outer_input",
                 "outer_input 是先被读取的一侧，每产生一行会驱动 inner_input")
          .field("query_block_number",
                 query_block == nullptr
                     ? std::uint64_t{0}
                     : static_cast<std::uint64_t>(
                           query_block->select_number))
          .field("join_method", WzgJoinMethodName(path->type))
          .field("join_type", WzgJoinTypeName(param.join_type))
          .field("join_type_meaning", WzgJoinTypeMeaning(param.join_type))
          .field("outer_input", WzgJoinInputText(param.outer))
          .field("inner_input", WzgJoinInputText(param.inner))
          .field("execution_order",
                 "先读取 outer_input；outer 每产生一行，再进入 inner_input 查找或扫描匹配行")
          .field("join_condition",
                 WzgJoinConditionText(thd, param.join_predicate))
          .field("batch_mode", param.pfs_batch_mode)
          .field("method_meaning", WzgJoinMethodMeaning(path))
          .field("next_step",
                 "NestedLoopIterator 调用外层 Read，随后反复初始化并读取内层迭代器")
          .emit();
      break;
    }
    case AccessPath::NESTED_LOOP_SEMIJOIN_WITH_DUPLICATE_REMOVAL: {
      const auto &param = path->nested_loop_semijoin_with_duplicate_removal();
      std::string key_name =
          param.key == nullptr || param.key->name == nullptr ? "未知 key"
                                                             : param.key->name;
      wzg_probe::Event event(thd, "executor.join_method", "instant");
      WzgAddExecutorFactFields(event, "join", "半连接去重读取器",
                               "choose_join_method", "ready",
                               "准备按带去重的半连接嵌套循环方式执行 JOIN",
                               "join", WzgJoinMethodName(path->type),
                               "WzgEmitJoinMethod")
          .message("执行器准备按带去重的半连接嵌套循环方式执行 JOIN")
          .sql_command(get_sql_command_string(thd->lex->sql_command))
          .field("runtime.query_block_number",
                 query_block == nullptr
                     ? std::uint64_t{0}
                     : static_cast<std::uint64_t>(
                           query_block->select_number))
          .field("runtime.join_method", WzgJoinMethodName(path->type))
          .field("runtime.join_type", "SEMI JOIN")
          .field("runtime.outer_input", WzgJoinInputText(param.outer))
          .field("runtime.inner_input", WzgJoinInputText(param.inner))
          .field("runtime.duplicate_removal_table",
                 WzgIteratorTableName(param.table))
          .field("runtime.duplicate_removal_key", key_name)
          .field("runtime.duplicate_removal_key_length",
                 static_cast<std::uint64_t>(param.key_len))
          .field("decision.summary", WzgJoinMethodMeaning(path))
          .field("decision.reason",
                 "半连接只需要判断是否存在匹配，重复 key 不需要重复输出")
          .field("decision.result",
                 "iterator 会按 duplicate_removal_key 跳过已输出过的半连接结果")
          .field("decision.impact",
                 "减少同一外层 key 产生的重复半连接输出")
          .field("explain_zh.duplicate_removal_key",
                 "用于判断半连接结果是否已经输出过的内部去重 key")
          .field("query_block_number",
                 query_block == nullptr
                     ? std::uint64_t{0}
                     : static_cast<std::uint64_t>(
                           query_block->select_number))
          .field("join_method", WzgJoinMethodName(path->type))
          .field("join_type", "SEMI JOIN")
          .field("join_type_meaning", WzgJoinTypeMeaning(JoinType::SEMI))
          .field("outer_input", WzgJoinInputText(param.outer))
          .field("inner_input", WzgJoinInputText(param.inner))
          .field("duplicate_removal_table", WzgIteratorTableName(param.table))
          .field("duplicate_removal_key", key_name)
          .field("duplicate_removal_key_length",
                 static_cast<std::uint64_t>(param.key_len))
          .field("execution_order",
                 "先读外层输入，再找内层匹配；同一个去重 key 已产生过结果时会跳过重复输出")
          .field("method_meaning", WzgJoinMethodMeaning(path))
          .field("next_step",
                 "NestedLoopSemiJoinWithDuplicateRemovalIterator 按 key 去重并返回半连接结果")
          .emit();
      break;
    }
    case AccessPath::BKA_JOIN: {
      const auto &param = path->bka_join();
      wzg_probe::Event event(thd, "executor.join_method", "instant");
      WzgAddExecutorFactFields(event, "join", "BKA JOIN 读取器",
                               "choose_join_method", "ready",
                               "准备按 BKA 批量索引方式执行 JOIN",
                               "join", WzgJoinMethodName(path->type),
                               "WzgEmitJoinMethod")
          .message("执行器准备按 BKA 批量索引方式执行 JOIN")
          .sql_command(get_sql_command_string(thd->lex->sql_command))
          .field("runtime.query_block_number",
                 query_block == nullptr
                     ? std::uint64_t{0}
                     : static_cast<std::uint64_t>(
                           query_block->select_number))
          .field("runtime.join_method", WzgJoinMethodName(path->type))
          .field("runtime.join_type", WzgJoinTypeName(param.join_type))
          .field("runtime.outer_input", WzgJoinInputText(param.outer))
          .field("runtime.inner_input", WzgJoinInputText(param.inner))
          .field("runtime.join_buffer_size",
                 static_cast<std::uint64_t>(thd->variables.join_buff_size))
          .field("runtime.mrr_length_per_record",
                 static_cast<std::uint64_t>(param.mrr_length_per_rec))
          .field("runtime.records_per_key_estimate",
                 WzgIteratorDoubleToString(param.rec_per_key))
          .field("runtime.store_rowids", param.store_rowids)
          .field("decision.summary", WzgJoinMethodMeaning(path))
          .field("decision.reason",
                 "批量收集外层 key 后可用 MRR 减少内层随机索引访问")
          .field("decision.result",
                 "BKAIterator 使用 join buffer 驱动内层 MultiRangeRowIterator")
          .field("decision.impact",
                 "内层索引读取按批次发生，而不是每个 outer 行立即查一次")
          .field("explain_zh.join_buffer_size",
                 "本线程 join_buffer_size 变量，决定 BKA 可用于缓存外层 key 的空间上限")
          .field("query_block_number",
                 query_block == nullptr
                     ? std::uint64_t{0}
                     : static_cast<std::uint64_t>(
                           query_block->select_number))
          .field("join_method", WzgJoinMethodName(path->type))
          .field("join_type", WzgJoinTypeName(param.join_type))
          .field("join_type_meaning", WzgJoinTypeMeaning(param.join_type))
          .field("outer_input", WzgJoinInputText(param.outer))
          .field("inner_input", WzgJoinInputText(param.inner))
          .field("join_buffer_size",
                 static_cast<std::uint64_t>(thd->variables.join_buff_size))
          .field("mrr_length_per_record",
                 static_cast<std::uint64_t>(param.mrr_length_per_rec))
          .field("records_per_key_estimate",
                 WzgIteratorDoubleToString(param.rec_per_key))
          .field("store_rowids", param.store_rowids)
          .field("execution_order",
                 "先收集一批 outer_input 的查找 key，再让 inner_input 通过 MRR 批量读取匹配行")
          .field("method_meaning", WzgJoinMethodMeaning(path))
          .field("next_step",
                 "BKAIterator 使用 join buffer 收集 key，并驱动 MultiRangeRowIterator 批量查内层表")
          .emit();
      break;
    }
    case AccessPath::HASH_JOIN: {
      const auto &param = path->hash_join();
      const JoinPredicate *predicate = param.join_predicate;
      JoinType join_type{JoinType::INNER};
      if (predicate != nullptr && predicate->expr != nullptr) {
        switch (predicate->expr->type) {
          case RelationalExpression::INNER_JOIN:
          case RelationalExpression::STRAIGHT_INNER_JOIN:
            join_type = JoinType::INNER;
            break;
          case RelationalExpression::LEFT_JOIN:
            join_type = JoinType::OUTER;
            break;
          case RelationalExpression::ANTIJOIN:
            join_type = JoinType::ANTI;
            break;
          case RelationalExpression::SEMIJOIN:
            join_type =
                param.rewrite_semi_to_inner ? JoinType::INNER : JoinType::SEMI;
            break;
          case RelationalExpression::FULL_OUTER_JOIN:
            join_type = JoinType::FULL_OUTER;
            break;
          case RelationalExpression::TABLE:
          default:
            join_type = JoinType::INNER;
            break;
        }
      }
      const std::uint64_t condition_count =
          predicate == nullptr || predicate->expr == nullptr
              ? std::uint64_t{0}
              : static_cast<std::uint64_t>(
                    predicate->expr->equijoin_conditions.size());
      wzg_probe::Event event(thd, "executor.join_method", "instant");
      WzgAddExecutorFactFields(event, "join", "Hash Join 读取器",
                               "choose_join_method", "ready",
                               "准备按 Hash Join 方式执行 JOIN",
                               "join", WzgJoinMethodName(path->type),
                               "WzgEmitJoinMethod")
          .message("执行器准备按 Hash Join 方式执行 JOIN")
          .sql_command(get_sql_command_string(thd->lex->sql_command))
          .field("runtime.query_block_number",
                 query_block == nullptr
                     ? std::uint64_t{0}
                     : static_cast<std::uint64_t>(
                           query_block->select_number))
          .field("runtime.join_method", WzgJoinMethodName(path->type))
          .field("runtime.join_type", WzgJoinTypeName(join_type))
          .field("runtime.build_input", WzgJoinInputText(param.inner))
          .field("runtime.probe_input", WzgJoinInputText(param.outer))
          .field("runtime.hash_key_conditions",
                 WzgJoinConditionText(thd, predicate))
          .field("runtime.hash_key_condition_count", condition_count)
          .field("runtime.allow_spill_to_disk", param.allow_spill_to_disk)
          .field("runtime.store_rowids", param.store_rowids)
          .field("runtime.rewrite_semi_to_inner", param.rewrite_semi_to_inner)
          .field("decision.summary", WzgJoinMethodMeaning(path))
          .field("decision.reason",
                 "等值 JOIN 条件可作为 hash key，执行器可以先构建哈希表再探测")
          .field("decision.result",
                 "HashJoinIterator 先读取 build_input 建表，再用 probe_input 匹配")
          .field("decision.impact",
                 "build/probe 两侧顺序影响内存占用、是否溢写磁盘和匹配方式")
          .field("explain_zh.hash_key_condition_count",
                 "参与 hash key 的等值 JOIN 条件数量")
          .field("query_block_number",
                 query_block == nullptr
                     ? std::uint64_t{0}
                     : static_cast<std::uint64_t>(
                           query_block->select_number))
          .field("join_method", WzgJoinMethodName(path->type))
          .field("join_type", WzgJoinTypeName(join_type))
          .field("join_type_meaning", WzgJoinTypeMeaning(join_type))
          .field("build_input", WzgJoinInputText(param.inner))
          .field("probe_input", WzgJoinInputText(param.outer))
          .field("hash_key_conditions", WzgJoinConditionText(thd, predicate))
          .field("hash_key_condition_count", condition_count)
          .field("allow_spill_to_disk", param.allow_spill_to_disk)
          .field("store_rowids", param.store_rowids)
          .field("rewrite_semi_to_inner", param.rewrite_semi_to_inner)
          .field("execution_order",
                 "先读取 build_input 建哈希表；再读取 probe_input，用 JOIN key 到哈希表中查匹配")
          .field("method_meaning", WzgJoinMethodMeaning(path))
          .field("next_step",
                 "HashJoinIterator 构建哈希表，并按 JOIN 类型返回匹配行、保留行或反匹配行")
          .emit();
      break;
    }
    default:
      break;
  }
}

void WzgEmitRefLookupKey(THD *thd, const AccessPath *path, const JOIN *join) {
  if (path == nullptr || thd == nullptr || thd->query().str == nullptr ||
      thd->query().length == 0)
    return;

  Index_lookup *ref = WzgRefLookup(path);
  TABLE *table = WzgIteratorTargetTable(path);
  if (ref == nullptr || table == nullptr || table->key_info == nullptr ||
      table->s == nullptr || ref->key < 0 ||
      static_cast<uint>(ref->key) >= table->s->keys)
    return;

  Query_block *query_block = join == nullptr ? nullptr : join->query_block;
  wzg_probe::Event event(thd, "executor.ref_lookup_key", "instant");
  WzgAddExecutorFactFields(event, "iterator", "索引精确查找读取器",
                           "prepare_ref_key", "ready",
                           "准备把表达式值编码成索引查找 key",
                           "index", WzgIteratorIndexName(table, ref->key),
                           "WzgEmitRefLookupKey")
      .message("索引精确查找读取器已准备好 key 参数，后续会用这些值到索引中查找匹配行")
      .sql_command(get_sql_command_string(thd->lex->sql_command))
      .field("runtime.query_block_number",
             query_block == nullptr
                 ? std::uint64_t{0}
                 : static_cast<std::uint64_t>(query_block->select_number))
      .field("runtime.table", WzgIteratorTableName(table))
      .field("runtime.index", WzgIteratorIndexName(table, ref->key))
      .field("runtime.lookup_type", WzgRefLookupTypeText(path->type))
      .field("runtime.lookup_key_parts", WzgRefLookupKeyParts(table, ref))
      .field("runtime.lookup_values", WzgRefLookupValues(thd, ref))
      .field("runtime.lookup_value_source", WzgRefLookupSource(ref))
      .field("runtime.key_parts_count",
             static_cast<std::uint64_t>(ref->key_parts))
      .field("runtime.key_length", static_cast<std::uint64_t>(ref->key_length))
      .field("runtime.null_rejecting", WzgRefLookupNullRejecting(ref))
      .field("runtime.has_guarded_conditions", ref->has_guarded_conds())
      .field("runtime.cache_behavior", WzgRefLookupCacheText(ref))
      .field("decision.summary", "执行器将用 ref key 做索引匹配读取")
      .field("decision.reason",
             "lookup_values 已由常量或前面表的列值确定，可写入 key_buff 做索引定位")
      .field("decision.result",
             "RefIterator/EQRefIterator 后续调用 handler 索引读取接口")
      .field("decision.impact",
             "内层表可按外层行的 key 精确查找，常见于 JOIN 的 inner lookup")
      .field("explain_zh.lookup_values",
             "本次索引查找要编码进 key buffer 的表达式，日志不读取真实业务行值")
      .field("query_block_number",
             query_block == nullptr
                 ? std::uint64_t{0}
                 : static_cast<std::uint64_t>(query_block->select_number))
      .field("table", WzgIteratorTableName(table))
      .field("index", WzgIteratorIndexName(table, ref->key))
      .field("lookup_type", WzgRefLookupTypeText(path->type))
      .field("lookup_key_parts", WzgRefLookupKeyParts(table, ref))
      .field("lookup_values", WzgRefLookupValues(thd, ref))
      .field("lookup_value_source", WzgRefLookupSource(ref))
      .field("key_parts_count", static_cast<std::uint64_t>(ref->key_parts))
      .field("key_length", static_cast<std::uint64_t>(ref->key_length))
      .field("null_rejecting", WzgRefLookupNullRejecting(ref))
      .field("has_guarded_conditions", ref->has_guarded_conds())
      .field("cache_behavior", WzgRefLookupCacheText(ref))
      .field("parameter_meaning",
             "lookup_values 是本次索引查找要写入 key_buff 的表达式；key_length 是编码后的 key 字节长度；lookup_key_parts 是这些值对应的索引列")
      .field("next_step",
             "RefIterator/EQRefIterator 调用 handler 索引读取接口，存储引擎按这个 key 定位匹配记录")
      .emit();
}

void WzgEmitIndexRangeBounds(THD *thd, const AccessPath *path,
                             const JOIN *join) {
  if (path == nullptr || path->type != AccessPath::INDEX_RANGE_SCAN ||
      thd == nullptr || thd->query().str == nullptr || thd->query().length == 0)
    return;

  const auto &param = path->index_range_scan();
  if (param.ranges == nullptr || param.num_ranges == 0 ||
      param.used_key_part == nullptr || param.used_key_part[0].field == nullptr)
    return;

  TABLE *table = param.used_key_part[0].field->table;
  if (table == nullptr || table->key_info == nullptr || table->s == nullptr ||
      param.index >= table->s->keys)
    return;

  const KEY &key = table->key_info[param.index];
  key_part_map used_map = 0;
  for (unsigned i = 0; i < param.num_ranges; ++i) {
    used_map |= param.ranges[i]->min_keypart_map;
    used_map |= param.ranges[i]->max_keypart_map;
  }
  Query_block *query_block = join == nullptr ? nullptr : join->query_block;
  wzg_probe::Event event(thd, "executor.index_range_bounds", "instant");
  WzgAddExecutorFactFields(event, "iterator", "索引范围读取器",
                           "prepare_range_bounds", "ready",
                           "准备索引范围扫描的起止边界",
                           "index", WzgIteratorIndexName(table, param.index),
                           "WzgEmitIndexRangeBounds")
      .message("索引范围读取器已准备好边界参数，后续会按这些边界请求存储引擎读取")
      .sql_command(get_sql_command_string(thd->lex->sql_command))
      .field("runtime.query_block_number",
             query_block == nullptr
                 ? std::uint64_t{0}
                 : static_cast<std::uint64_t>(query_block->select_number))
      .field("runtime.table", WzgIteratorTableName(table))
      .field("runtime.index", WzgIteratorIndexName(table, param.index))
      .field("runtime.range_count", static_cast<std::uint64_t>(param.num_ranges))
      .field("runtime.used_key_parts", WzgUsedKeyPartsText(key, used_map))
      .field("runtime.range_parameters",
             WzgRangeParametersList(param.ranges, param.num_ranges,
                                    key.key_part))
      .field("decision.summary", "执行器将按索引边界扫描一段或多段范围")
      .field("decision.reason",
             "range optimizer 已生成 start_key/end_key，handler 可用这些边界定位 B+Tree")
      .field("decision.result",
             "IndexRangeScanIterator 后续调用 handler 范围扫描接口")
      .field("decision.impact",
             "存储引擎只需扫描满足边界的索引区间，再把候选记录返回 SQL 层")
      .field("explain_zh.range_parameters",
             "start_key/end_key 是传给 handler 的索引边界；flag 表示边界包含和定位方式")
      .field("query_block_number",
             query_block == nullptr
                 ? std::uint64_t{0}
                 : static_cast<std::uint64_t>(query_block->select_number))
      .field("table", WzgIteratorTableName(table))
      .field("index", WzgIteratorIndexName(table, param.index))
      .field("range_count", static_cast<std::uint64_t>(param.num_ranges))
      .field("used_key_parts", WzgUsedKeyPartsText(key, used_map))
      .field("range_parameters",
             WzgRangeParametersList(param.ranges, param.num_ranges,
                                    key.key_part))
      .field("parameter_meaning",
             "start_key/end_key 是传给 handler 的索引边界；length 是编码后的 key 字节长度；keypart_map 表示使用了哪些索引列；flag 表示是否包含边界以及如何定位第一条记录")
      .field("next_step",
             "IndexRangeScanIterator 调用 handler 接口，存储引擎根据这些边界在索引 B+Tree 中定位和扫描")
      .emit();
}

std::string WzgWindowFunctionsText(THD *thd, Window *window) {
  if (window == nullptr) return "无窗口函数";
  constexpr uint kMaxPrintedItems = 8;
  std::string value;
  uint printed = 0;
  List_iterator<Item_sum> it(window->functions());
  Item_sum *item = nullptr;
  while ((item = it++)) {
    if (printed >= kMaxPrintedItems) {
      value.append("；还有更多窗口函数未展开");
      break;
    }
    if (!value.empty()) value.append("；");
    value.append(std::to_string(printed + 1));
    value.append(". ");
    value.append(WzgSumFuncTypeText(item->sum_func()));
    value.append(": ");
    value.append(WzgItemExpressionText(thd, item));
    ++printed;
  }
  return value.empty() ? "无窗口函数" : value;
}

std::string WzgWindowDefinitionText(THD *thd, const Window *window) {
  if (window == nullptr) return "无";
  char buffer[1024];
  String text(buffer, sizeof(buffer), system_charset_info);
  text.length(0);
  window->print(thd, &text, QT_ORDINARY, true);
  return text.length() == 0 ? "未知窗口定义"
                            : std::string(text.ptr(), text.length());
}

std::string WzgWindowPartitionText(THD *thd, const Window *window) {
  if (window == nullptr || window->first_partition_by() == nullptr)
    return "无；整批输入行作为一个分区";
  return WzgOrderExpressionText(thd, window->first_partition_by());
}

std::string WzgWindowOrderText(THD *thd, const Window *window) {
  if (window == nullptr || window->first_order_by() == nullptr)
    return "无；窗口函数不要求分区内排序";
  return WzgOrderExpressionText(thd, window->first_order_by());
}

std::string WzgWindowFrameText(THD *thd, const Window *window) {
  if (window == nullptr) return "无";
  const std::string definition = WzgWindowDefinitionText(thd, window);
  return definition.empty() ? "未知窗口范围" : definition;
}

std::string WzgWindowBufferReason(Window *window, bool needs_buffering) {
  if (window == nullptr) return "未知";
  if (!needs_buffering)
    return "否；当前窗口函数可以边读输入边计算，不需要先缓存后续行";

  std::string value("是；");
  bool has_reason = false;
  auto append = [&value, &has_reason](const char *reason) {
    if (has_reason) value.append("；");
    value.append(reason);
    has_reason = true;
  };
  if (window->needs_partition_cardinality())
    append("需要知道整个分区有多少行，例如 CUME_DIST、NTILE 或类似函数");
  if (window->needs_peerset())
    append("需要读取当前 ORDER BY 同值组 peer set 后才能计算");
  if (window->needs_last_peer_in_frame())
    append("需要知道窗口 frame 内最后一个 peer row");
  if (window->static_aggregates())
    append("聚合值对整个分区固定，需要按分区缓存后复用");
  if (!has_reason)
    append("窗口 frame 或 LEAD/LAG/NTH_VALUE 等计算需要访问当前行之后的行");
  return value;
}

std::string WzgWindowExecutionModeText(bool needs_buffering) {
  return needs_buffering
             ? "BufferingWindowIterator，会先缓存分区/窗口 frame 所需行再输出结果"
             : "WindowIterator，输入行到达后即可计算并继续向上输出";
}

std::string WzgWindowHowText(bool needs_buffering) {
  return needs_buffering
             ? "执行器按 PARTITION BY 和 ORDER BY 的顺序读取输入；遇到一个分区时，把计算 frame 所需的行放入窗口缓存，再为每一行计算窗口函数值"
             : "执行器按输入顺序逐行读取；每行进入当前分区后立即更新窗口状态并计算这一行的窗口函数值";
}

void WzgEmitWindowPlan(THD *thd, const AccessPath *path, const JOIN *join) {
  if (path == nullptr || path->type != AccessPath::WINDOW || thd == nullptr ||
      thd->query().str == nullptr || thd->query().length == 0)
    return;

  const auto &param = path->window();
  Query_block *query_block = join == nullptr ? nullptr : join->query_block;
  Window *window = param.window;
  WZG_PROBE_EVENT(thd, "executor.window")
      .message("执行器准备计算窗口函数，输入行不会被 GROUP BY 合并，每行会得到自己的窗口计算结果")
      .sql_command(get_sql_command_string(thd->lex->sql_command))
      .field("query_block_number",
             query_block == nullptr
                 ? std::uint64_t{0}
                 : static_cast<std::uint64_t>(query_block->select_number))
      .field("window_name",
             window == nullptr ? "无" : std::string(window->printable_name()))
      .field("window_functions", WzgWindowFunctionsText(thd, window))
      .field("partition_by", WzgWindowPartitionText(thd, window))
      .field("order_by", WzgWindowOrderText(thd, window))
      .field("frame", WzgWindowFrameText(thd, window))
      .field("input_source", WzgJoinInputText(param.child))
      .field("estimated_input_rows",
             param.child == nullptr
                 ? "未知"
                 : WzgSortRowsEstimateText(param.child->num_output_rows()))
      .field("estimated_output_rows",
             WzgSortRowsEstimateText(path->num_output_rows()))
      .field("needs_buffering", param.needs_buffering)
      .field("buffering_reason",
             WzgWindowBufferReason(window, param.needs_buffering))
      .field("iterator_mode",
             WzgWindowExecutionModeText(param.needs_buffering))
      .field("uses_temporary_table",
             param.temp_table_param != nullptr
                 ? "是；窗口函数结果通过内部临时行结构传递给上层节点"
                 : "未知")
      .field("frame_buffer_table",
             window == nullptr || window->frame_buffer() == nullptr
                 ? "无"
                 : WzgIteratorTableName(window->frame_buffer()))
      .field("short_circuit",
             window != nullptr && window->short_circuit()
                 ? "是；这是最后一个窗口步骤，结果可以直接继续向上输出"
                 : "否")
      .field("how_it_works", WzgWindowHowText(param.needs_buffering))
      .field("next_step",
             param.needs_buffering
                 ? "BufferingWindowIterator 读取输入行，按分区缓存并计算窗口函数结果"
                 : "WindowIterator 读取输入行，逐行计算窗口函数结果")
      .emit();
}

std::string WzgDistinctGroupItemsText(THD *thd, Item **group_items,
                                      int group_items_size) {
  if (group_items == nullptr || group_items_size <= 0) return "无";
  constexpr int kMaxPrintedItems = 8;
  std::string value;
  const int printed = std::min(group_items_size, kMaxPrintedItems);
  for (int i = 0; i < printed; ++i) {
    if (!value.empty()) value.append("；");
    value.append(std::to_string(i + 1));
    value.append(". ");
    value.append(WzgItemExpressionText(thd, group_items[i]));
  }
  if (group_items_size > printed) {
    value.append("；还有 ");
    value.append(std::to_string(group_items_size - printed));
    value.append(" 个去重字段未展开");
  }
  return value.empty() ? "无" : value;
}

std::string WzgDistinctIndexKeyText(const TABLE *table, const KEY *key,
                                    unsigned key_len) {
  if (table == nullptr || key == nullptr) return "未知索引 key";
  std::string value;
  uint used_length = 0;
  for (uint i = 0; i < key->user_defined_key_parts; ++i) {
    const KEY_PART_INFO &part = key->key_part[i];
    if (key_len > 0 && used_length >= key_len) break;
    if (!value.empty()) value.append(", ");
    value.append(part.field != nullptr && part.field->field_name != nullptr
                     ? part.field->field_name
                     : "<expression>");
    used_length += part.store_length;
  }
  return value.empty() ? WzgIteratorIndexName(table, key - table->key_info)
                       : value;
}

bool WzgRawSqlContainsDistinct(THD *thd) {
  if (thd == nullptr || thd->query().str == nullptr ||
      thd->query().length == 0)
    return false;

  std::string sql(thd->query().str, thd->query().length);
  std::transform(sql.begin(), sql.end(), sql.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return sql.find("distinct") != std::string::npos;
}

void WzgEmitDistinctPlan(THD *thd, const AccessPath *path, const JOIN *join) {
  if (path == nullptr || thd == nullptr || thd->query().str == nullptr ||
      thd->query().length == 0)
    return;

  Query_block *query_block = join == nullptr ? nullptr : join->query_block;
  if ((path->type == AccessPath::AGGREGATE ||
       path->type == AccessPath::TEMPTABLE_AGGREGATE) &&
      WzgRawSqlContainsDistinct(thd)) {
    AccessPath *input = nullptr;
    bool uses_temporary_table = false;
    std::string temp_table("无");
    if (path->type == AccessPath::AGGREGATE) {
      input = path->aggregate().child;
    } else {
      input = path->temptable_aggregate().subquery_path;
      uses_temporary_table = true;
      temp_table = WzgIteratorTableName(path->temptable_aggregate().table);
    }

    WZG_PROBE_EVENT(thd, "executor.distinct")
        .message("执行器准备把 DISTINCT 当作分组去重处理，每组只输出一行结果")
        .sql_command(get_sql_command_string(thd->lex->sql_command))
        .field("query_block_number",
               query_block == nullptr
                   ? std::uint64_t{0}
                   : static_cast<std::uint64_t>(query_block->select_number))
        .field("dedup_method",
               uses_temporary_table ? "临时表分组去重"
                                    : "流式分组去重")
        .field("distinct_columns",
               join == nullptr ? "未知"
                               : WzgOrderExpressionText(thd,
                                                        join->group_list.order))
        .field("input_source", WzgJoinInputText(input))
        .field("estimated_input_rows",
               input == nullptr
                   ? "未知"
                   : WzgSortRowsEstimateText(input->num_output_rows()))
        .field("estimated_output_groups",
               WzgSortRowsEstimateText(path->num_output_rows()))
        .field("uses_temporary_table", uses_temporary_table)
        .field("temporary_table", temp_table)
        .field("input_meaning",
               "优化器已经把 DISTINCT 的唯一值要求转成分组结果；相同 DISTINCT 字段值属于同一组")
        .field("duplicate_behavior",
               "同一组里的多行只产生一行输出，所以最终返回的是不重复的结果列组合")
        .field("next_step",
               uses_temporary_table
                   ? "TemptableAggregateIterator 把输入写入聚合临时表，再输出唯一分组"
                   : "AggregateIterator 按分组字段比较相邻输入行，每组输出一次")
        .emit();
    return;
  }

  if (path->type == AccessPath::SORT &&
      path->sort().filesort != nullptr &&
      path->sort().filesort->m_remove_duplicates) {
    WZG_PROBE_EVENT(thd, "executor.distinct")
        .message("执行器准备用 filesort 排序去重，只保留结果列组合不重复的行")
        .sql_command(get_sql_command_string(thd->lex->sql_command))
        .field("query_block_number",
               query_block == nullptr
                   ? std::uint64_t{0}
                   : static_cast<std::uint64_t>(query_block->select_number))
        .field("dedup_method", "filesort 排序去重")
        .field("distinct_columns", WzgSortOrderText(thd, path->sort().filesort))
        .field("input_source", WzgJoinInputText(path->sort().child))
        .field("estimated_input_rows",
               path->sort().child == nullptr
                   ? "未知"
                   : WzgSortRowsEstimateText(
                         path->sort().child->num_output_rows()))
        .field("input_meaning",
               "先按 DISTINCT 字段排序，相邻结果行相同则只保留第一行")
        .field("next_step", "SortingIterator 排序时去掉重复结果行，再把唯一行交给上层节点")
        .emit();
    return;
  }

  if (path->type == AccessPath::REMOVE_DUPLICATES) {
    const auto &param = path->remove_duplicates();
    WZG_PROBE_EVENT(thd, "executor.distinct")
        .message("执行器准备在读取有序结果时去重，相邻重复行只返回一行")
        .sql_command(get_sql_command_string(thd->lex->sql_command))
        .field("query_block_number",
               query_block == nullptr
                   ? std::uint64_t{0}
                   : static_cast<std::uint64_t>(query_block->select_number))
        .field("dedup_method", "RemoveDuplicatesIterator 相邻行比较去重")
        .field("distinct_columns",
               WzgDistinctGroupItemsText(thd, param.group_items,
                                         param.group_items_size))
        .field("input_source", WzgJoinInputText(param.child))
        .field("input_meaning",
               "输入需要已经按去重字段排好序；当前行和上一行去重字段相同就跳过")
        .field("next_step", "RemoveDuplicatesIterator 读取子节点行，比较缓存字段后决定是否返回")
        .emit();
    return;
  }

  if (path->type == AccessPath::REMOVE_DUPLICATES_ON_INDEX) {
    const auto &param = path->remove_duplicates_on_index();
    WZG_PROBE_EVENT(thd, "executor.distinct")
        .message("执行器准备利用索引顺序去重，相同索引前缀只返回第一行")
        .sql_command(get_sql_command_string(thd->lex->sql_command))
        .field("query_block_number",
               query_block == nullptr
                   ? std::uint64_t{0}
                   : static_cast<std::uint64_t>(query_block->select_number))
        .field("dedup_method", "RemoveDuplicatesOnIndexIterator 索引前缀去重")
        .field("table", WzgIteratorTableName(param.table))
        .field("index", WzgIteratorIndexName(
                            param.table,
                            param.table == nullptr || param.table->key_info == nullptr
                                ? MAX_KEY
                                : static_cast<uint>(param.key - param.table->key_info)))
        .field("distinct_columns",
               WzgDistinctIndexKeyText(param.table, param.key,
                                       param.loosescan_key_len))
        .field("key_length",
               static_cast<std::uint64_t>(param.loosescan_key_len))
        .field("input_meaning",
               "输入按索引顺序读取；相邻行的索引去重前缀相同就跳过")
        .field("next_step", "RemoveDuplicatesOnIndexIterator 比较当前索引 key 和上一行 key")
        .emit();
    return;
  }

  if (path->type == AccessPath::GROUP_INDEX_SKIP_SCAN) {
    const auto &param = path->group_index_skip_scan();
    const GroupIndexSkipScanParameters *gparam = param.param;
    WZG_PROBE_EVENT(thd, "executor.distinct")
        .message("执行器准备利用 GROUP_INDEX_SKIP_SCAN 读取每组第一条索引记录，天然跳过重复值")
        .sql_command(get_sql_command_string(thd->lex->sql_command))
        .field("query_block_number",
               query_block == nullptr
                   ? std::uint64_t{0}
                   : static_cast<std::uint64_t>(query_block->select_number))
        .field("dedup_method", "GROUP_INDEX_SKIP_SCAN 索引跳组去重")
        .field("table", WzgIteratorTableName(param.table))
        .field("index", WzgIteratorIndexName(param.table, param.index))
        .field("distinct_columns",
               gparam == nullptr
                   ? "未知"
                   : WzgUsedKeyPartsText(*gparam->index_info,
                                         (key_part_map{1}
                                          << param.num_used_key_parts) -
                                             1))
        .field("used_key_parts",
               static_cast<std::uint64_t>(param.num_used_key_parts))
        .field("input_meaning",
               "这类计划常用于 DISTINCT 或 GROUP BY；执行器按索引前缀跳到下一组，所以不会逐行返回同组重复值")
        .field("duplicate_behavior",
               "同一个去重 key 下的后续索引项会被跳过，只输出该组代表行")
        .field("next_step", "GroupIndexSkipScanIterator 调用存储引擎索引定位接口跳到下一组")
        .emit();
  }
}

void WzgEmitIteratorCreate(THD *thd, const AccessPath *path, const JOIN *join) {
  if (path == nullptr || thd == nullptr) return;
  if (thd->query().str == nullptr || thd->query().length == 0) return;
  Query_block *query_block = join == nullptr ? nullptr : join->query_block;
  TABLE *target_table = WzgIteratorTargetTable(path);
  const std::string target_table_name = WzgIteratorTableName(target_table);
  wzg_probe::Event event(thd, "executor.iterator_create", "instant");
  WzgAddExecutorFactFields(event, "iterator", "RowIterator 工厂",
                           "create_iterator", "finish",
                           "把访问路径节点转换成可执行 RowIterator",
                           target_table == nullptr ? "iterator" : "table",
                           target_table == nullptr
                               ? WzgIteratorAccessPathName(path->type)
                               : target_table_name,
                           "WzgEmitIteratorCreate")
      .message("执行器已根据访问路径创建读取器")
      .sql_command(get_sql_command_string(thd->lex->sql_command))
      .field("runtime.query_block_number",
             query_block == nullptr
                 ? std::uint64_t{0}
                 : static_cast<std::uint64_t>(query_block->select_number))
      .field("runtime.access_path_type", WzgIteratorAccessPathName(path->type))
      .field("runtime.iterator_kind", WzgIteratorReadableName(path->type))
      .field("runtime.target_table", target_table_name)
      .field("runtime.chosen_index", WzgIteratorChosenIndex(path))
      .field("runtime.estimated_output_rows",
             WzgIteratorDoubleToString(path->num_output_rows()))
      .field("runtime.estimated_cost", WzgIteratorDoubleToString(path->cost()))
      .field("runtime.counts_examined_rows", path->count_examined_rows)
      .field("decision.summary", WzgIteratorPurpose(path))
      .field("decision.reason",
             "优化器已经选择该 AccessPath，执行器在这里创建对应 RowIterator")
      .field("decision.result",
             "后续执行阶段将调用这个 iterator 的 Init 和 Read")
      .field("decision.impact",
             "该 iterator 决定这一计划节点如何产出行、是否读取表或驱动上层节点")
      .field("explain_zh.estimated_output_rows",
             "优化器估算的输出行数，不等于实际读取或返回行数")
      .field("explain_zh.counts_examined_rows",
             "该 iterator 的读取是否计入 examined rows 统计")
      .field("query_block_number",
             query_block == nullptr
                 ? std::uint64_t{0}
                 : static_cast<std::uint64_t>(query_block->select_number))
      .field("access_path_type", WzgIteratorAccessPathName(path->type))
      .field("iterator_kind", WzgIteratorReadableName(path->type))
      .field("target_table", target_table_name)
      .field("chosen_index", WzgIteratorChosenIndex(path))
      .field("estimated_output_rows",
             WzgIteratorDoubleToString(path->num_output_rows()))
      .field("estimated_cost", WzgIteratorDoubleToString(path->cost()))
      .field("counts_examined_rows", path->count_examined_rows)
      .field("purpose", WzgIteratorPurpose(path))
      .field("next_step", "执行阶段调用这个读取器的 Init 和 Read 方法获取数据")
      .emit();
}

void SetupJobsForChildren(MEM_ROOT *mem_root, AccessPath *child, JOIN *join,
                          bool eligible_for_batch_mode,
                          IteratorToBeCreated *job,
                          Mem_root_array<IteratorToBeCreated> *todo) {
  // Make jobs for the child, and we'll return to this job later.
  job->AllocChildren(mem_root, 1);
  todo->push_back(*job);
  todo->push_back(
      {child, join, eligible_for_batch_mode, &job->children[0], {}});
}

void SetupJobsForChildren(MEM_ROOT *mem_root, AccessPath *outer,
                          AccessPath *inner, JOIN *join,
                          bool inner_eligible_for_batch_mode,
                          IteratorToBeCreated *job,
                          Mem_root_array<IteratorToBeCreated> *todo) {
  // Make jobs for the children, and we'll return to this job later.
  // Note that we push the inner before the outer job, so that we get
  // left created before right (invalidators in materialization access paths,
  // used in the old join optimizer, depend on this).
  job->AllocChildren(mem_root, 2);
  todo->push_back(*job);
  todo->push_back(
      {inner, join, inner_eligible_for_batch_mode, &job->children[1], {}});
  todo->push_back({outer, join, false, &job->children[0], {}});
}

}  // namespace

const Mem_root_array<Item *> *GetExtraHashJoinConditions(
    MEM_ROOT *mem_root, bool using_hypergraph_optimizer,
    const vector<HashJoinCondition> &equijoin_conditions,
    const Mem_root_array<Item *> &other_conditions) {
  if (!using_hypergraph_optimizer) {
    // The old optimizer has already collected the necessary conditions in
    // other_conditions or in a filter on top of the hash join.
    return &other_conditions;
  }

  if (all_of(equijoin_conditions.begin(), equijoin_conditions.end(),
             [](const HashJoinCondition &condition) {
               return condition.store_full_sort_key();
             })) {
    // When we have no partially stored hash keys, there are no more conditions
    // to add.
    return &other_conditions;
  }

  // If we have at least one part of the hash key that cannot be stored fully in
  // the hash join buffer, we need to add the corresponding equijoin condition
  // as an extra condition to evaluate after the hash join. Append it to the
  // non-equijoin predicates that we already have.
  Mem_root_array<Item *> *extra_conditions =
      new (mem_root) Mem_root_array<Item *>(mem_root, other_conditions);
  if (extra_conditions == nullptr) return nullptr;

  for (const HashJoinCondition &condition : equijoin_conditions) {
    if (!condition.store_full_sort_key()) {
      if (extra_conditions->push_back(condition.join_condition())) {
        return nullptr;
      }
    }
  }

  return extra_conditions;
}

unique_ptr_destroy_only<RowIterator> CreateIteratorFromAccessPath(
    THD *thd, MEM_ROOT *mem_root, AccessPath *top_path, JOIN *top_join,
    bool top_eligible_for_batch_mode) {
  assert(IteratorsAreNeeded(thd, top_path));

  unique_ptr_destroy_only<RowIterator> ret;
  Mem_root_array<IteratorToBeCreated> todo(mem_root);
  todo.push_back({top_path, top_join, top_eligible_for_batch_mode, &ret, {}});

  // The access path trees can be pretty deep, and the stack frames can be big
  // on certain compilers/setups, so instead of explicit recursion, we push jobs
  // onto a MEM_ROOT-backed stack. This uses a little more RAM (the MEM_ROOT
  // typically lives to the end of the query), but reduces the stack usage
  // greatly.
  //
  // The general rule is that if an iterator requires any children, it will push
  // jobs for their access paths at the end of the stack and then re-push
  // itself. When the children are instantiated and we get back to the original
  // iterator, we'll actually instantiate it. (We distinguish between the two
  // cases on basis of whether job.children has been allocated or not; the child
  // iterator's destination will point into this array. The child list needs
  // to be allocated in a way that doesn't move around if the TODO job list
  // is reallocated, which we do by means of allocating it directly on the
  // MEM_ROOT.)
  while (!todo.empty()) {
    IteratorToBeCreated job = todo.back();
    todo.pop_back();

    AccessPath *path = job.path;
    JOIN *join = job.join;
    bool eligible_for_batch_mode = job.eligible_for_batch_mode;

    if (job.join != nullptr) {
      assert(!job.join->needs_finalize);
    }

    unique_ptr_destroy_only<RowIterator> iterator;

    ha_rows *examined_rows = nullptr;
    if (path->count_examined_rows && join != nullptr) {
      examined_rows = &join->examined_rows;
    }

    switch (path->type) {
      case AccessPath::TABLE_SCAN: {
        const auto &param = path->table_scan();
        iterator = NewIterator<TableScanIterator>(
            thd, mem_root, param.table, path->num_output_rows(), examined_rows);
        break;
      }
      case AccessPath::INDEX_SCAN: {
        const auto &param = path->index_scan();
        if (param.reverse) {
          iterator = NewIterator<IndexScanIterator<true>>(
              thd, mem_root, param.table, param.idx, param.use_order,
              path->num_output_rows(), examined_rows);
        } else {
          iterator = NewIterator<IndexScanIterator<false>>(
              thd, mem_root, param.table, param.idx, param.use_order,
              path->num_output_rows(), examined_rows);
        }
        break;
      }
      case AccessPath::INDEX_DISTANCE_SCAN: {
        const auto &param = path->index_distance_scan();
        iterator = NewIterator<IndexDistanceScanIterator>(
            thd, mem_root, param.table, param.idx, param.range,
            path->num_output_rows(), examined_rows);
        break;
      }
      case AccessPath::REF: {
        const auto &param = path->ref();
        if (param.reverse) {
          iterator = NewIterator<RefIterator<true>>(
              thd, mem_root, param.table, param.ref, param.use_order,
              path->num_output_rows(), examined_rows);
        } else {
          iterator = NewIterator<RefIterator<false>>(
              thd, mem_root, param.table, param.ref, param.use_order,
              path->num_output_rows(), examined_rows);
        }
        break;
      }
      case AccessPath::REF_OR_NULL: {
        const auto &param = path->ref_or_null();
        iterator = NewIterator<RefOrNullIterator>(
            thd, mem_root, param.table, param.ref, param.use_order,
            path->num_output_rows(), examined_rows);
        break;
      }
      case AccessPath::EQ_REF: {
        const auto &param = path->eq_ref();
        iterator = NewIterator<EQRefIterator>(thd, mem_root, param.table,
                                              param.ref, examined_rows);
        break;
      }
      case AccessPath::PUSHED_JOIN_REF: {
        const auto &param = path->pushed_join_ref();
        iterator = NewIterator<PushedJoinRefIterator>(
            thd, mem_root, param.table, param.ref, param.use_order,
            param.is_unique, examined_rows);
        break;
      }
      case AccessPath::FULL_TEXT_SEARCH: {
        const auto &param = path->full_text_search();
        iterator = NewIterator<FullTextSearchIterator>(
            thd, mem_root, param.table, param.ref, param.ft_func,
            param.use_order, param.use_limit, examined_rows);
        break;
      }
      case AccessPath::CONST_TABLE: {
        const auto &param = path->const_table();
        iterator = NewIterator<ConstIterator>(thd, mem_root, param.table,
                                              param.ref, examined_rows);
        break;
      }
      case AccessPath::MRR: {
        const auto &param = path->mrr();
        const auto &bka_param = param.bka_path->bka_join();
        iterator = NewIterator<MultiRangeRowIterator>(
            thd, mem_root, param.table, param.ref, param.mrr_flags,
            bka_param.join_type,
            GetUsedTables(bka_param.outer, /*include_pruned_tables=*/true),
            bka_param.store_rowids, bka_param.tables_to_get_rowid_for);
        break;
      }
      case AccessPath::FOLLOW_TAIL: {
        const auto &param = path->follow_tail();
        iterator = NewIterator<FollowTailIterator>(
            thd, mem_root, param.table, path->num_output_rows(), examined_rows);
        break;
      }
      case AccessPath::INDEX_RANGE_SCAN: {
        const auto &param = path->index_range_scan();
        TABLE *table = param.used_key_part[0].field->table;
        if (param.geometry) {
          iterator = NewIterator<GeometryIndexRangeScanIterator>(
              thd, mem_root, table, examined_rows, path->num_output_rows(),
              param.index, param.need_rows_in_rowid_order, param.reuse_handler,
              mem_root, param.mrr_flags, param.mrr_buf_size,
              Bounds_checked_array{param.ranges, param.num_ranges});
        } else if (param.reverse) {
          iterator = NewIterator<ReverseIndexRangeScanIterator>(
              thd, mem_root, table, examined_rows, path->num_output_rows(),
              param.index, mem_root, param.mrr_flags,
              Bounds_checked_array{param.ranges, param.num_ranges},
              param.using_extended_key_parts);
        } else {
          iterator = NewIterator<IndexRangeScanIterator>(
              thd, mem_root, table, examined_rows, path->num_output_rows(),
              param.index, param.need_rows_in_rowid_order, param.reuse_handler,
              mem_root, param.mrr_flags, param.mrr_buf_size,
              Bounds_checked_array{param.ranges, param.num_ranges});
        }
        break;
      }
      case AccessPath::INDEX_MERGE: {
        const auto &param = path->index_merge();
        unique_ptr_destroy_only<RowIterator> pk_quick_select;
        if (job.children.is_null()) {
          job.AllocChildren(mem_root, param.children->size());
          todo.push_back(job);
          for (size_t child_idx = 0; child_idx < param.children->size();
               ++child_idx) {
            todo.push_back({(*param.children)[child_idx],
                            join,
                            /*eligible_for_batch_mode=*/false,
                            &job.children[child_idx],
                            {}});
          }
          continue;
        }
        Mem_root_array<unique_ptr_destroy_only<RowIterator>> children(mem_root);
        children.reserve(param.children->size());
        for (size_t child_idx = 0; child_idx < param.children->size();
             ++child_idx) {
          AccessPath *range_scan = (*param.children)[child_idx];
          if (param.allow_clustered_primary_key_scan &&
              param.table->file->primary_key_is_clustered() &&
              range_scan->index_range_scan().index ==
                  param.table->s->primary_key) {
            assert(pk_quick_select == nullptr);
            pk_quick_select = std::move(job.children[child_idx]);
          } else {
            children.push_back(std::move(job.children[child_idx]));
          }
        }

        iterator = NewIterator<IndexMergeIterator>(
            thd, mem_root, mem_root, param.table, std::move(pk_quick_select),
            std::move(children));
        break;
      }
      case AccessPath::ROWID_INTERSECTION: {
        const auto &param = path->rowid_intersection();
        if (job.children.is_null()) {
          job.AllocChildren(mem_root, param.children->size() +
                                          (param.cpk_child != nullptr ? 1 : 0));
          todo.push_back(job);
          for (size_t child_idx = 0; child_idx < param.children->size();
               ++child_idx) {
            todo.push_back({(*param.children)[child_idx],
                            join,
                            /*eligible_for_batch_mode=*/false,
                            &job.children[child_idx],
                            {}});
          }
          if (param.cpk_child != nullptr) {
            todo.push_back({param.cpk_child,
                            join,
                            /*eligible_for_batch_mode=*/false,
                            &job.children[param.children->size()],
                            {}});
          }
          continue;
        }

        // TODO(sgunders): Consider just sending in the array here,
        // changing types in the constructor.
        Mem_root_array<unique_ptr_destroy_only<RowIterator>> children(mem_root);
        children.reserve(param.children->size());
        for (size_t child_idx = 0; child_idx < param.children->size();
             ++child_idx) {
          children.push_back(std::move(job.children[child_idx]));
        }

        unique_ptr_destroy_only<RowIterator> cpk_child;
        if (param.cpk_child != nullptr) {
          cpk_child = std::move(job.children[param.children->size()]);
        }
        iterator = NewIterator<RowIDIntersectionIterator>(
            thd, mem_root, mem_root, param.table, param.retrieve_full_rows,
            param.need_rows_in_rowid_order, std::move(children),
            std::move(cpk_child));
        break;
      }
      case AccessPath::ROWID_UNION: {
        const auto &param = path->rowid_union();
        if (job.children.is_null()) {
          job.AllocChildren(mem_root, param.children->size());
          todo.push_back(job);
          for (size_t child_idx = 0; child_idx < param.children->size();
               ++child_idx) {
            todo.push_back({(*param.children)[child_idx],
                            join,
                            /*eligible_for_batch_mode=*/false,
                            &job.children[child_idx],
                            {}});
          }
          continue;
        }
        // TODO(sgunders): Consider just sending in the array here,
        // changing types in the constructor.
        Mem_root_array<unique_ptr_destroy_only<RowIterator>> children(mem_root);
        children.reserve(param.children->size());
        for (unique_ptr_destroy_only<RowIterator> &child : job.children) {
          children.push_back(std::move(child));
        }
        iterator = NewIterator<RowIDUnionIterator>(
            thd, mem_root, mem_root, param.table, std::move(children));
        break;
      }
      case AccessPath::INDEX_SKIP_SCAN: {
        const IndexSkipScanParameters *param = path->index_skip_scan().param;
        iterator = NewIterator<IndexSkipScanIterator>(
            thd, mem_root, path->index_skip_scan().table, param->index_info,
            path->index_skip_scan().index, param->eq_prefix_len,
            param->eq_prefix_key_parts, param->eq_prefixes,
            path->index_skip_scan().num_used_key_parts, mem_root,
            param->has_aggregate_function, param->min_range_key,
            param->max_range_key, param->min_search_key, param->max_search_key,
            param->range_cond_flag, param->range_key_len);
        break;
      }
      case AccessPath::GROUP_INDEX_SKIP_SCAN: {
        const GroupIndexSkipScanParameters *param =
            path->group_index_skip_scan().param;
        iterator = NewIterator<GroupIndexSkipScanIterator>(
            thd, mem_root, path->group_index_skip_scan().table,
            &param->min_functions, &param->max_functions,
            param->have_agg_distinct, param->min_max_arg_part,
            param->group_prefix_len, param->group_key_parts,
            param->real_key_parts, param->max_used_key_length,
            param->index_info, path->group_index_skip_scan().index,
            param->key_infix_len, mem_root, param->is_index_scan,
            &param->prefix_ranges, &param->key_infix_ranges,
            &param->min_max_ranges);
        break;
      }
      case AccessPath::DYNAMIC_INDEX_RANGE_SCAN: {
        const auto &param = path->dynamic_index_range_scan();
        iterator = NewIterator<DynamicRangeIterator>(
            thd, mem_root, param.table, param.qep_tab, examined_rows);
        break;
      }
      case AccessPath::TABLE_VALUE_CONSTRUCTOR: {
        assert(join != nullptr);
        Query_block *query_block = join->query_block;
        iterator = NewIterator<TableValueConstructorIterator>(
            thd, mem_root, examined_rows, *query_block->row_value_list,
            path->table_value_constructor().output_refs);
        break;
      }
      case AccessPath::FAKE_SINGLE_ROW:
        iterator =
            NewIterator<FakeSingleRowIterator>(thd, mem_root, examined_rows);
        break;
      case AccessPath::ZERO_ROWS: {
        iterator = NewIterator<ZeroRowsIterator>(thd, mem_root,
                                                 CollectTables(thd, path));
        break;
      }
      case AccessPath::ZERO_ROWS_AGGREGATED:
        iterator = NewIterator<ZeroRowsAggregatedIterator>(thd, mem_root, join,
                                                           examined_rows);
        break;
      case AccessPath::MATERIALIZED_TABLE_FUNCTION: {
        const auto &param = path->materialized_table_function();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.table_path, join,
                               eligible_for_batch_mode, &job, &todo);
          continue;
        }
        iterator = NewIterator<MaterializedTableFunctionIterator>(
            thd, mem_root, param.table_function, param.table,
            std::move(job.children[0]));
        break;
      }
      case AccessPath::UNQUALIFIED_COUNT:
        iterator = NewIterator<UnqualifiedCountIterator>(thd, mem_root, join);
        break;
      case AccessPath::NESTED_LOOP_JOIN: {
        const auto &param = path->nested_loop_join();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.outer, param.inner, join,
                               eligible_for_batch_mode, &job, &todo);
          continue;
        }

        iterator = NewIterator<NestedLoopIterator>(
            thd, mem_root, std::move(job.children[0]),
            std::move(job.children[1]), param.join_type, param.pfs_batch_mode);
        break;
      }
      case AccessPath::NESTED_LOOP_SEMIJOIN_WITH_DUPLICATE_REMOVAL: {
        const auto &param = path->nested_loop_semijoin_with_duplicate_removal();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.outer, param.inner, join,
                               eligible_for_batch_mode, &job, &todo);
          continue;
        }
        iterator = NewIterator<NestedLoopSemiJoinWithDuplicateRemovalIterator>(
            thd, mem_root, std::move(job.children[0]),
            std::move(job.children[1]), param.table, param.key, param.key_len);
        break;
      }
      case AccessPath::BKA_JOIN: {
        const auto &param = path->bka_join();
        AccessPath *mrr_path =
            FindSingleAccessPathOfType(param.inner, AccessPath::MRR);
        if (job.children.is_null()) {
          mrr_path->mrr().bka_path = path;
          SetupJobsForChildren(mem_root, param.outer, param.inner, join,
                               /*inner_eligible_for_batch_mode=*/false, &job,
                               &todo);
          continue;
        }

        MultiRangeRowIterator *mrr_iterator =
            down_cast<MultiRangeRowIterator *>(
                mrr_path->iterator->real_iterator());
        iterator = NewIterator<BKAIterator>(
            thd, mem_root, std::move(job.children[0]),
            GetUsedTables(param.outer, /*include_pruned_tables=*/true),
            std::move(job.children[1]), thd->variables.join_buff_size,
            param.mrr_length_per_rec, param.rec_per_key, param.store_rowids,
            param.tables_to_get_rowid_for, mrr_iterator, param.join_type);
        break;
      }
      case AccessPath::HASH_JOIN: {
        const auto &param = path->hash_join();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.outer, param.inner, join,
                               /*inner_eligible_for_batch_mode=*/true, &job,
                               &todo);
          continue;
        }
        const JoinPredicate *join_predicate = param.join_predicate;
        vector<HashJoinCondition> conditions;
        conditions.reserve(join_predicate->expr->equijoin_conditions.size());
        for (Item_eq_base *cond : join_predicate->expr->equijoin_conditions) {
          conditions.emplace_back(cond, thd->mem_root);
        }
        const Mem_root_array<Item *> *extra_conditions =
            GetExtraHashJoinConditions(
                mem_root, thd->lex->using_hypergraph_optimizer(), conditions,
                join_predicate->expr->join_conditions);
        if (extra_conditions == nullptr) return nullptr;
        const bool probe_input_batch_mode =
            eligible_for_batch_mode && ShouldEnableBatchMode(param.outer);
        double estimated_build_rows = param.inner->num_output_rows();
        if (param.inner->num_output_rows() < 0.0) {
          // Not all access paths may propagate their costs properly.
          // Choose a fairly safe estimate (it's better to be too large
          // than too small).
          estimated_build_rows = 1048576.0;
        }
        JoinType join_type{JoinType::INNER};
        switch (join_predicate->expr->type) {
          case RelationalExpression::INNER_JOIN:
          case RelationalExpression::STRAIGHT_INNER_JOIN:
            join_type = JoinType::INNER;
            break;
          case RelationalExpression::LEFT_JOIN:
            join_type = JoinType::OUTER;
            break;
          case RelationalExpression::ANTIJOIN:
            join_type = JoinType::ANTI;
            break;
          case RelationalExpression::SEMIJOIN:
            join_type =
                param.rewrite_semi_to_inner ? JoinType::INNER : JoinType::SEMI;
            break;
          case RelationalExpression::TABLE:
          default:
            assert(false);
        }
        // See if we can allow the hash table to keep its contents across Init()
        // calls.
        //
        // The old optimizer will sometimes push join conditions referring
        // to outer tables (in the same query block) down in under the hash
        // operation, so without analysis of each filter and join condition, we
        // cannot say for sure, and thus have to turn it off. But the hypergraph
        // optimizer sets parameter_tables properly, so we're safe if we just
        // check that.
        //
        // Regardless of optimizer, we can push outer references down in under
        // the hash, but join->hash_table_generation will increase whenever we
        // need to recompute the query block (in JOIN::clear_hash_tables()).
        //
        // TODO(sgunders): The old optimizer had a concept of _when_ to clear
        // derived tables (invalidators), and this is somehow similar. If it
        // becomes a performance issue, consider reintroducing them.
        //
        // TODO(sgunders): Should this perhaps be set as a flag on the access
        // path instead of being computed here? We do make the same checks in
        // the cost model, so perhaps it should set the flag as well.
        uint64_t *hash_table_generation =
            (thd->lex->using_hypergraph_optimizer() &&
             path->parameter_tables == 0)
                ? &join->hash_table_generation
                : nullptr;

        const auto first_row_cost = [](const AccessPath &p) {
          return p.init_cost() + p.cost() / std::max(p.num_output_rows(), 1.0);
        };

        // If the probe (outer) input is empty, the join result will be empty,
        // and we do not need to read the build input. For inner join and
        // semijoin, the converse is also true. To benefit from this, we want to
        // start with the input where the cost of reading the first row is
        // lowest. (We only do this for Hypergraph, as the cost data for the
        // traditional optimizer are incomplete, and since we are reluctant to
        // change existing behavior.) Note that we always try the probe input
        // first for left join and antijoin.
        const HashJoinInput first_input =
            (thd->lex->using_hypergraph_optimizer() &&
             first_row_cost(*param.inner) > first_row_cost(*param.outer))
                ? HashJoinInput::kProbe
                : HashJoinInput::kBuild;

        iterator = NewIterator<HashJoinIterator>(
            thd, mem_root, std::move(job.children[1]),
            GetUsedTables(param.inner, /*include_pruned_tables=*/true),
            estimated_build_rows, std::move(job.children[0]),
            GetUsedTables(param.outer, /*include_pruned_tables=*/true),
            param.store_rowids, param.tables_to_get_rowid_for,
            thd->variables.join_buff_size, std::move(conditions),
            param.allow_spill_to_disk, join_type, *extra_conditions,
            first_input, probe_input_batch_mode, hash_table_generation);
        break;
      }
      case AccessPath::FILTER: {
        const auto &param = path->filter();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.child, join,
                               eligible_for_batch_mode, &job, &todo);
          continue;
        }
        if (FinalizeMaterializedSubqueries(thd, join, path)) {
          return nullptr;
        }
        iterator = NewIterator<FilterIterator>(
            thd, mem_root, std::move(job.children[0]), param.condition);
        break;
      }
      case AccessPath::SORT: {
        const auto &param = path->sort();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.child, join,
                               eligible_for_batch_mode, &job, &todo);
          continue;
        }
        ha_rows num_rows_estimate = param.child->num_output_rows() < 0.0
                                        ? HA_POS_ERROR
                                        : lrint(param.child->num_output_rows());
        Filesort *filesort = param.filesort;
        iterator = NewIterator<SortingIterator>(
            thd, mem_root, filesort, std::move(job.children[0]),
            num_rows_estimate, param.tables_to_get_rowid_for, examined_rows);
        if (filesort->m_remove_duplicates) {
          filesort->tables[0]->duplicate_removal_iterator =
              down_cast<SortingIterator *>(iterator->real_iterator());
        } else {
          filesort->tables[0]->sorting_iterator =
              down_cast<SortingIterator *>(iterator->real_iterator());
        }
        break;
      }
      case AccessPath::AGGREGATE: {
        const auto &param = path->aggregate();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.child, join,
                               eligible_for_batch_mode, &job, &todo);
          continue;
        }
        Prealloced_array<TABLE *, 4> tables =
            GetUsedTables(param.child, /*include_pruned_tables=*/true);
        iterator = NewIterator<AggregateIterator>(
            thd, mem_root, std::move(job.children[0]), join,
            TableCollection(tables, /*store_rowids=*/false,
                            /*tables_to_get_rowid_for=*/0,
                            GetNullableEqRefTables(param.child)),
            param.olap == ROLLUP_TYPE);
        break;
      }
      case AccessPath::TEMPTABLE_AGGREGATE: {
        const auto &param = path->temptable_aggregate();
        if (job.children.is_null()) {
          job.AllocChildren(mem_root, 2);
          todo.push_back(job);
          todo.push_back({param.subquery_path,
                          join,
                          /*eligible_for_batch_mode=*/true,
                          &job.children[0],
                          {}});
          todo.push_back({param.table_path,
                          join,
                          eligible_for_batch_mode,
                          &job.children[1],
                          {}});
          continue;
        }

        iterator = unique_ptr_destroy_only<RowIterator>(
            temptable_aggregate_iterator::CreateIterator(
                thd, std::move(job.children[0]), param.temp_table_param,
                param.table, std::move(job.children[1]), join,
                param.ref_slice));

        break;
      }
      case AccessPath::LIMIT_OFFSET: {
        const auto &param = path->limit_offset();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.child, join,
                               eligible_for_batch_mode, &job, &todo);
          continue;
        }
        ha_rows *send_records = nullptr;
        if (param.send_records_override != nullptr) {
          send_records = param.send_records_override;
        } else if (join != nullptr) {
          send_records = &join->send_records;
        }
        iterator = NewIterator<LimitOffsetIterator>(
            thd, mem_root, std::move(job.children[0]), param.limit,
            param.offset, param.count_all_rows, param.reject_multiple_rows,
            send_records);
        break;
      }
      case AccessPath::STREAM: {
        const auto &param = path->stream();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.child, param.join,
                               eligible_for_batch_mode, &job, &todo);
          continue;
        }
        iterator = NewIterator<StreamingIterator>(
            thd, mem_root, std::move(job.children[0]), param.temp_table_param,
            param.table, param.provide_rowid, param.join, param.ref_slice);
        break;
      }
      case AccessPath::MATERIALIZE: {
        // The table access path should be a single iterator, not a tree.
        // (ALTERNATIVE counts as a single iterator in this regard.)
        assert(
            path->materialize().table_path->type == AccessPath::TABLE_SCAN ||
            path->materialize().table_path->type == AccessPath::LIMIT_OFFSET ||
            path->materialize().table_path->type == AccessPath::REF ||
            path->materialize().table_path->type == AccessPath::REF_OR_NULL ||
            path->materialize().table_path->type == AccessPath::EQ_REF ||
            path->materialize().table_path->type == AccessPath::ALTERNATIVE ||
            path->materialize().table_path->type == AccessPath::CONST_TABLE ||
            path->materialize().table_path->type == AccessPath::INDEX_SCAN ||
            path->materialize().table_path->type ==
                AccessPath::INDEX_RANGE_SCAN ||
            path->materialize().table_path->type ==
                AccessPath::DYNAMIC_INDEX_RANGE_SCAN);

        MaterializePathParameters *param = path->materialize().param;
        if (job.children.is_null()) {
          job.AllocChildren(mem_root, param->m_operands.size() + 1);
          todo.push_back(job);
          todo.push_back({path->materialize().table_path,
                          join,
                          eligible_for_batch_mode,
                          &job.children[0],
                          {}});
          for (size_t i = 0; i < param->m_operands.size(); ++i) {
            const MaterializePathParameters::Operand &from =
                param->m_operands[i];
            todo.push_back({from.subquery_path,
                            from.join,
                            /*eligible_for_batch_mode=*/true,
                            &job.children[i + 1],
                            {}});
          }
          continue;
        }
        unique_ptr_destroy_only<RowIterator> table_iterator =
            std::move(job.children[0]);
        Mem_root_array<materialize_iterator::Operand> operands(
            thd->mem_root, param->m_operands.size());
        for (size_t i = 0; i < param->m_operands.size(); ++i) {
          const MaterializePathParameters::Operand &from = param->m_operands[i];
          materialize_iterator::Operand &to = operands[i];
          to.subquery_iterator = std::move(job.children[i + 1]);
          to.select_number = from.select_number;
          to.join = from.join;
          to.disable_deduplication_by_hash_field =
              from.disable_deduplication_by_hash_field;
          to.copy_items = from.copy_items;
          to.temp_table_param = from.temp_table_param;
          to.is_recursive_reference = from.is_recursive_reference;
          to.m_first_distinct = from.m_first_distinct;
          to.m_total_operands = from.m_total_operands;
          to.m_operand_idx = from.m_operand_idx;
          to.m_estimated_output_rows = from.subquery_path->num_output_rows();

          if (to.is_recursive_reference) {
            // Find the recursive reference to ourselves; there should be
            // exactly one, as per the standard.
            RowIterator *recursive_reader = FindSingleIteratorOfType(
                from.subquery_path, AccessPath::FOLLOW_TAIL);
            if (recursive_reader == nullptr) {
              // The recursive reference was optimized away, e.g. due to an
              // impossible WHERE condition, so we're not a recursive
              // reference after all.
              to.is_recursive_reference = false;
            } else {
              to.recursive_reader =
                  down_cast<FollowTailIterator *>(recursive_reader);
            }
          }
        }
        JOIN *subjoin = param->ref_slice == -1 ? nullptr : operands[0].join;

        iterator = unique_ptr_destroy_only<RowIterator>(
            materialize_iterator::CreateIterator(
                thd, std::move(operands), param, std::move(table_iterator),
                subjoin));

        break;
      }
      case AccessPath::MATERIALIZE_INFORMATION_SCHEMA_TABLE: {
        const auto &param = path->materialize_information_schema_table();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.table_path, join,
                               eligible_for_batch_mode, &job, &todo);
          continue;
        }
        iterator = NewIterator<MaterializeInformationSchemaTableIterator>(
            thd, mem_root, std::move(job.children[0]), param.table_list,
            param.condition);
        break;
      }
      case AccessPath::APPEND: {
        const auto &param = path->append();
        if (job.children.is_null()) {
          job.AllocChildren(mem_root, param.children->size());
          todo.push_back(job);
          for (size_t child_idx = 0; child_idx < param.children->size();
               ++child_idx) {
            const AppendPathParameters &child_param =
                (*param.children)[child_idx];
            todo.push_back({child_param.path,
                            child_param.join,
                            /*eligible_for_batch_mode=*/true,
                            &job.children[child_idx],
                            {}});
          }
          continue;
        }
        // TODO(sgunders): Consider just sending in the array here,
        // changing types in the constructor.
        vector<unique_ptr_destroy_only<RowIterator>> children;
        children.reserve(param.children->size());
        for (unique_ptr_destroy_only<RowIterator> &child : job.children) {
          children.push_back(std::move(child));
        }
        iterator =
            NewIterator<AppendIterator>(thd, mem_root, std::move(children));
        break;
      }
      case AccessPath::WINDOW: {
        const auto &param = path->window();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.child, join,
                               eligible_for_batch_mode, &job, &todo);
          continue;
        }
        WzgEmitWindowPlan(thd, path, join);
        if (param.needs_buffering) {
          iterator = NewIterator<BufferingWindowIterator>(
              thd, mem_root, std::move(job.children[0]), param.temp_table_param,
              join, param.ref_slice);
        } else {
          iterator = NewIterator<WindowIterator>(
              thd, mem_root, std::move(job.children[0]), param.temp_table_param,
              join, param.ref_slice);
        }
        break;
      }
      case AccessPath::WEEDOUT: {
        const auto &param = path->weedout();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.child, join,
                               eligible_for_batch_mode, &job, &todo);
          continue;
        }
        iterator = NewIterator<WeedoutIterator>(
            thd, mem_root, std::move(job.children[0]), param.weedout_table,
            param.tables_to_get_rowid_for);
        break;
      }
      case AccessPath::REMOVE_DUPLICATES: {
        const auto &param = path->remove_duplicates();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.child, join,
                               eligible_for_batch_mode, &job, &todo);
          continue;
        }
        iterator = NewIterator<RemoveDuplicatesIterator>(
            thd, mem_root, std::move(job.children[0]), join, param.group_items,
            param.group_items_size);
        break;
      }
      case AccessPath::REMOVE_DUPLICATES_ON_INDEX: {
        const auto &param = path->remove_duplicates_on_index();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.child, join,
                               eligible_for_batch_mode, &job, &todo);
          continue;
        }
        iterator = NewIterator<RemoveDuplicatesOnIndexIterator>(
            thd, mem_root, std::move(job.children[0]), param.table, param.key,
            param.loosescan_key_len);
        break;
      }
      case AccessPath::ALTERNATIVE: {
        const auto &param = path->alternative();
        if (job.children.is_null()) {
          job.AllocChildren(mem_root, 2);
          todo.push_back(job);
          todo.push_back({param.child,
                          join,
                          eligible_for_batch_mode,
                          &job.children[0],
                          {}});
          todo.push_back({param.table_scan_path,
                          join,
                          eligible_for_batch_mode,
                          &job.children[1],
                          {}});
          continue;
        }
        iterator = NewIterator<AlternativeIterator>(
            thd, mem_root, param.table_scan_path->table_scan().table,
            std::move(job.children[0]), std::move(job.children[1]),
            param.used_ref);
        break;
      }
      case AccessPath::CACHE_INVALIDATOR: {
        const auto &param = path->cache_invalidator();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.child, join,
                               eligible_for_batch_mode, &job, &todo);
          continue;
        }
        iterator = NewIterator<CacheInvalidatorIterator>(
            thd, mem_root, std::move(job.children[0]), param.name);
        break;
      }
      case AccessPath::DELETE_ROWS: {
        const auto &param = path->delete_rows();
        if (job.children.is_null()) {
          // Setting up tables for delete must be done before the child
          // iterators are created, as some of the child iterators need to see
          // the final read set when they are constructed, so doing it in
          // DeleteRowsIterator's constructor or Init() is too late.
          SetUpTablesForDelete(thd, join);
          SetupJobsForChildren(mem_root, param.child, join,
                               eligible_for_batch_mode, &job, &todo);
          continue;
        }
        iterator = NewIterator<DeleteRowsIterator>(
            thd, mem_root, std::move(job.children[0]), join,
            param.tables_to_delete_from, param.immediate_tables);
        break;
      }
      case AccessPath::UPDATE_ROWS: {
        const auto &param = path->update_rows();
        if (job.children.is_null()) {
          // Do the final setup for UPDATE before the child iterators are
          // created.
          if (FinalizeOptimizationForUpdate(join)) {
            return nullptr;
          }
          SetupJobsForChildren(mem_root, param.child, join,
                               eligible_for_batch_mode, &job, &todo);
          continue;
        }
        iterator = CreateUpdateRowsIterator(thd, mem_root, join,
                                            std::move(job.children[0]));
        break;
      }
      case AccessPath::SAMPLE_SCAN: { /* LCOV_EXCL_LINE */
        // SampleScan can be executed only in the secondary engine.
        assert(false); /* LCOV_EXCL_LINE */
      }
    }

    if (iterator == nullptr) {
      return nullptr;
    }

    WzgEmitIteratorCreate(thd, path, join);
    WzgEmitFilterEval(thd, path, join);
    WzgEmitLimitOffset(thd, path, join);
    WzgEmitSortPlan(thd, path, join);
    WzgEmitSetOperationPlan(thd, path, join);
    WzgEmitMaterializePlan(thd, path, join);
    WzgEmitAggregatePlan(thd, path, join);
    WzgEmitJoinMethod(thd, path, join);
    WzgEmitDistinctPlan(thd, path, join);
    WzgEmitLockingRead(thd, path, join);
    WzgEmitTableScanPlan(thd, path, join);
    WzgEmitIndexScanPlan(thd, path, join);
    WzgEmitRefLookupKey(thd, path, join);
    WzgEmitIndexRangeBounds(thd, path, join);
    path->iterator = iterator.get();
    *job.destination = std::move(iterator);
  }
  return ret;
}

void FindTablesToGetRowidFor(AccessPath *path) {
  table_map handled_by_others = 0;

  auto add_tables_handled_by_others = [path, &handled_by_others](
                                          AccessPath *subpath, const JOIN *) {
    if (path == subpath) return false;  // Skip ourselves.
    switch (subpath->type) {
      case AccessPath::HASH_JOIN:
        handled_by_others |=
            GetUsedTableMap(subpath, /*include_pruned_tables=*/true);
        FindTablesToGetRowidFor(subpath);
        return true;  // Don't double-traverse.
      case AccessPath::BKA_JOIN:
        handled_by_others |= GetUsedTableMap(subpath->bka_join().outer,
                                             /*include_pruned_tables=*/true);
        FindTablesToGetRowidFor(subpath);
        return true;  // Don't double-traverse.
      case AccessPath::STREAM: {
        subpath->stream().provide_rowid = true;
        TABLE *table = subpath->stream().table;
        if (table->pos_in_table_list == nullptr) {
          // Don't need to set anything; see comment on the similar
          // test in NewSortAccessPath().
        } else {
          handled_by_others |= table->pos_in_table_list->map();
        }
        // Doesn't really matter, we don't cross query blocks anyway.
        return true;
      }
      default:
        return false;
    }
  };

  // We stop at MATERIALIZE and STREAM (they supply row IDs for us without
  // having to ask the tables below).
  switch (path->type) {
    case AccessPath::HASH_JOIN:
      WalkAccessPaths(path, /*join=*/nullptr,
                      WalkAccessPathPolicy::STOP_AT_MATERIALIZATION,
                      add_tables_handled_by_others);
      path->hash_join().store_rowids = true;
      path->hash_join().tables_to_get_rowid_for =
          GetUsedTableMap(path, /*include_pruned_tables=*/true) &
          ~handled_by_others;
      break;
    case AccessPath::BKA_JOIN:
      WalkAccessPaths(path->bka_join().outer, /*join=*/nullptr,
                      WalkAccessPathPolicy::STOP_AT_MATERIALIZATION,
                      add_tables_handled_by_others);
      path->bka_join().store_rowids = true;
      path->bka_join().tables_to_get_rowid_for =
          GetUsedTableMap(path->bka_join().outer,
                          /*include_pruned_tables=*/true) &
          ~handled_by_others;
      break;
    case AccessPath::WEEDOUT:
      WalkAccessPaths(path, /*join=*/nullptr,
                      WalkAccessPathPolicy::STOP_AT_MATERIALIZATION,
                      add_tables_handled_by_others);
      path->weedout().tables_to_get_rowid_for =
          GetUsedTableMap(path, /*include_pruned_tables=*/true) &
          ~handled_by_others;
      break;
    case AccessPath::SORT:
      WalkAccessPaths(path, /*join=*/nullptr,
                      WalkAccessPathPolicy::STOP_AT_MATERIALIZATION,
                      add_tables_handled_by_others);
      path->sort().tables_to_get_rowid_for =
          GetUsedTableMap(path, /*include_pruned_tables=*/true) &
          ~handled_by_others;
      break;
    default:
      my_abort();
  }
}

// Move the join conditions that are left in path->filter_predicates into the
// hash join predicate of the given HASH_JOIN access path. Note that join
// conditions with subqueries are not moved. If the subqueries need to be
// materialized, then a filter access path is expected from the caller.
// So they will continue to stay as filters on top of the hash join.
//
// TODO(khatlen): It's a bit of a hack to widen the hash join condition like
// this after the plan has been found. It would be better if we found a way to
// encode the necessary information in the hypergraph itself. For example, when
// creating cycles in the hypergraph, we could add redundant complex hyperedges
// in addition to the simple cycle edges that we currently add.
static void MoveFilterPredicatesIntoHashJoinCondition(
    THD *thd, AccessPath *path, const Mem_root_array<Predicate> &predicates,
    int num_where_predicates) {
  Mem_root_array<Item_eq_base *> equijoin_conditions(thd->mem_root);
  Mem_root_array<Item *> join_conditions(thd->mem_root);
  MutableOverflowBitset moved_predicates(thd->mem_root, predicates.size());

  for (int filter_idx : BitsSetIn(path->filter_predicates)) {
    if (filter_idx >= num_where_predicates) break;
    const Predicate &predicate = predicates[filter_idx];
    if (!predicate.was_join_condition) continue;

    Item *condition = predicate.condition;
    // Conditions with subqueries are not moved.
    if (condition->has_subquery()) continue;
    moved_predicates.SetBit(filter_idx);
    if (condition->type() == Item::FUNC_ITEM &&
        down_cast<Item_func *>(condition)
            ->contains_only_equi_join_condition()) {
      equijoin_conditions.push_back(down_cast<Item_eq_base *>(condition));
    } else {
      join_conditions.push_back(condition);
    }
  }

  if (equijoin_conditions.empty() && join_conditions.empty()) {
    // No join conditions were found in the filter predicates.
    return;
  }

  // Create a new JoinPredicate with all the conditions. We don't fully
  // initialize it, since we're done planning and don't need most of the
  // information any more. Just add enough to make EXPLAIN and
  // CreateIteratorFromAccessPath() happy.
  // TODO(khatlen): Maybe it's better to put directly into the access path those
  // few parts of the join predicate that are needed, and leave the actual
  // predicate and relational expression out.
  auto &param = path->hash_join();
  for (Item_eq_base *item : param.join_predicate->expr->equijoin_conditions) {
    equijoin_conditions.push_back(item);
  }
  for (Item *item : param.join_predicate->expr->join_conditions) {
    join_conditions.push_back(item);
  }
  RelationalExpression *expr = new (thd->mem_root) RelationalExpression(thd);
  expr->type = param.join_predicate->expr->type;
  expr->equijoin_conditions = std::move(equijoin_conditions);
  expr->join_conditions = std::move(join_conditions);
  JoinPredicate *join_predicate = new (thd->mem_root) JoinPredicate;
  join_predicate->expr = expr;
  param.join_predicate = join_predicate;

  path->filter_predicates = OverflowBitset::Xor(
      thd->mem_root, path->filter_predicates, std::move(moved_predicates));
}

Item *ConditionFromFilterPredicates(const Mem_root_array<Predicate> &predicates,
                                    OverflowBitset mask,
                                    int num_where_predicates) {
  List<Item> items;
  for (int pred_idx : BitsSetIn(mask)) {
    if (pred_idx >= num_where_predicates) break;
    items.push_back(predicates[pred_idx].condition);
  }
  return CreateConjunction(&items);
}

void ExpandSingleFilterAccessPath(THD *thd, AccessPath *path, const JOIN *join,
                                  const Mem_root_array<Predicate> &predicates,
                                  unsigned num_where_predicates) {
  // Expand join filters for nested loop joins.
  if (path->type == AccessPath::NESTED_LOOP_JOIN &&
      !path->nested_loop_join().already_expanded_predicates &&
      !(path->nested_loop_join().equijoin_predicates.empty() &&
        path->nested_loop_join()
            .join_predicate->expr->join_conditions.empty()) &&
      path->nested_loop_join().inner->type != AccessPath::ZERO_ROWS) {
    AccessPath *right_path = path->nested_loop_join().inner;
    const RelationalExpression *expr =
        path->nested_loop_join().join_predicate->expr;

    // While we're collecting the join conditions, calculate cost and output
    // rows (purely for display purposes). Note that this mirrors the
    // calculation we are doing in CostingReceiver::ProposeNestedLoopJoin();
    // we don't have space in the AccessPath to store it there.
    double filter_cost = right_path->cost();
    double filter_rows = right_path->num_output_rows();

    List<Item> items;
    for (size_t filter_idx :
         BitsSetIn(path->nested_loop_join().equijoin_predicates)) {
      Item *condition = expr->equijoin_conditions[filter_idx];
      items.push_back(condition);
      filter_cost +=
          EstimateFilterCost(thd, filter_rows, condition, join->query_block)
              .cost_if_not_materialized;
      filter_rows *= EstimateSelectivity(thd, condition, *expr->companion_set);
    }
    for (Item *condition : expr->join_conditions) {
      items.push_back(condition);
      filter_cost +=
          EstimateFilterCost(thd, filter_rows, condition, join->query_block)
              .cost_if_not_materialized;
      filter_rows *= EstimateSelectivity(thd, condition, *expr->companion_set);
    }
    assert(!items.is_empty());

    AccessPath *filter_path = new (thd->mem_root) AccessPath;
    filter_path->type = AccessPath::FILTER;
    filter_path->filter().child = right_path;
    filter_path->has_group_skip_scan = right_path->has_group_skip_scan;

    // We don't bother trying to materialize subqueries in join conditions,
    // since they should be very rare.
    filter_path->filter().materialize_subqueries = false;

    CopyBasicProperties(*right_path, filter_path);
    filter_path->filter().condition = CreateConjunction(&items);
    filter_path->set_cost(filter_cost);
    filter_path->set_num_output_rows(filter_rows);

    path->nested_loop_join().inner = filter_path;

    // Since multiple root paths may have their filters expanded,
    // and the same nested loop may be a subpath in several
    // of them, we need to make sure we don't add the join predicates
    // more than once, so mark them as done here.
    path->nested_loop_join().already_expanded_predicates = true;
  }

  // If a hash join follows an edge that is part of a cycle in the hypergraph,
  // there may be other applicable join predicates left in filter_predicates.
  // Say we have {t1,t2} HJ {t3} along the t1.a=t3.a edge. If there is also a
  // t2.b=t3.b edge, that predicate will be in filtered_predicates. In this
  // case, it is desirable to have t1.a=t3.a AND t2.b=t3.b as the hash join
  // predicate, and remove t2.b=t3.b from the filter predicates.
  if (path->type == AccessPath::HASH_JOIN &&
      path->hash_join().join_predicate->expr->join_predicate_first !=
          path->hash_join().join_predicate->expr->join_predicate_last) {
    MoveFilterPredicatesIntoHashJoinCondition(thd, path, predicates,
                                              num_where_predicates);
  }

  // Expand filters _after_ the access path (these are much more common).
  Item *condition = ConditionFromFilterPredicates(
      predicates, path->filter_predicates, num_where_predicates);
  if (condition == nullptr) {
    return;
  }
  AccessPath *new_path = new (thd->mem_root) AccessPath(*path);
  new_path->filter_predicates.Clear();
  new_path->set_num_output_rows(path->num_output_rows_before_filter);
  new_path->set_cost(path->cost_before_filter());

  // We don't really know how much of init_cost comes from the filter,
  // but we need to heed the invariant that cost >= init_cost
  // also for the new (non-filter) path we're creating, even if it's
  // just for display. Heuristically allocate as much as possible to
  // the filter.
  double filter_only_cost = path->cost() - path->cost_before_filter();
  new_path->set_init_cost(
      std::max(new_path->init_cost() - filter_only_cost, 0.0));
  new_path->set_init_once_cost(
      std::max(new_path->init_once_cost() - filter_only_cost, 0.0));
  assert(new_path->cost() >= new_path->init_cost());
  assert(new_path->init_cost() >= new_path->init_once_cost());

  path->type = AccessPath::FILTER;
  path->filter().condition = condition;
  path->filter().child = new_path;
  path->has_group_skip_scan = new_path->has_group_skip_scan;
  path->filter().materialize_subqueries = false;

  // Clear filter_predicates, but keep applied_sargable_join_predicates.
  MutableOverflowBitset applied_sargable_join_predicates =
      path->applied_sargable_join_predicates().Clone(thd->mem_root);
  applied_sargable_join_predicates.ClearBits(0, num_where_predicates);
  path->filter_predicates = std::move(applied_sargable_join_predicates);
}

void ExpandFilterAccessPaths(THD *thd, AccessPath *path_arg, const JOIN *join,
                             const Mem_root_array<Predicate> &predicates,
                             unsigned num_where_predicates) {
  WalkAccessPaths(path_arg, join, WalkAccessPathPolicy::ENTIRE_QUERY_BLOCK,
                  [thd, &predicates, num_where_predicates](
                      AccessPath *path, const JOIN *sub_join) {
                    ExpandSingleFilterAccessPath(
                        thd, path, sub_join, predicates, num_where_predicates);
                    return false;
                  });
}

table_map GetHashJoinTables(AccessPath *path) {
  table_map tables = 0;
  WalkAccessPaths(
      path, /*join=*/nullptr, WalkAccessPathPolicy::STOP_AT_MATERIALIZATION,
      [&tables](AccessPath *subpath, const JOIN *) {
        if (subpath->type == AccessPath::HASH_JOIN) {
          tables |= GetUsedTableMap(subpath, /*include_pruned_tables=*/true);
          return true;
        }
        return false;
      });
  return tables;
}
