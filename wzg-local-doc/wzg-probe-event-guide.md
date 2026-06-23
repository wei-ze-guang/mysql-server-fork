# WZG Probe 事件链路阅读指南

记录时间：2026-06-23

这份文档给后续接手的 AI 或开发者看。目标是让人看到 `wzg-probe.jsonl` 后，知道应该按什么顺序读、每类事件在解释 MySQL 的哪个内部行为、哪些字段可以把一条 SQL、一个事务、一条 InnoDB 记录串起来。

## 核心目标

WZG Probe 不是简单记录“函数被调用了”。它要把 MySQL 内部行为翻译成可读链路：

```text
客户端 SQL
  -> Server 层解析、解析名字、优化、执行
  -> handler 接口把访问请求交给 InnoDB
  -> InnoDB 在 B+Tree、锁、MVCC、undo/redo/binlog 里做真实工作
  -> Server 把结果返回客户端
```

每条重要日志尽量回答四个问题：

- `what_happened`：这一步实际发生了什么。
- `internal_change`：MySQL/InnoDB 内部状态改了什么。
- `internal_meaning`：这个内部变化代表什么。
- `next_step`：后面谁会继续使用这个结果。

日志 key 使用英文，面向人的说明使用中文。不要把示例值写死到代码里，表名、索引、事务 id、LSN、roll_ptr 等必须来自运行时。

## 串联字段

读日志时优先用这些字段串起来：

| 字段 | 用途 |
| --- | --- |
| `connection_uuid` | 串联一次客户端连接内的所有事件。 |
| `query_id` | 串联一条 SQL 的 Server 层事件。 |
| `raw_sql` | 客户端原始 SQL，用来确认当前事件属于哪条语句。 |
| `transaction_id` | 串联 InnoDB 事务、锁、undo、prepare、commit、rollback。 |
| `table` / `index` | 确认操作的是哪张表、哪个索引。 |
| `record_space_id` / `record_page_no` / `record_heap_no` | 定位 InnoDB B+Tree 页里的具体记录。 |
| `roll_ptr` / `db_roll_ptr_after` | 串联聚簇索引记录和 undo 记录。 |
| `prepare_lsn` / `current_lsn` / `flushed_to_disk_lsn` | 理解 redo 日志推进和刷盘。 |
| `binlog_file` / `binlog_position_after_write` | 定位 binlog 写入位置。 |

## 总体阶段

一条普通 SQL 大致按这个顺序出现：

```text
connection.start
command.dispatch
sql.query_received
parser.start
parser.finish
resolver.start
resolver.tables_resolved
resolver.fields_resolved
resolver.conditions_resolved
resolver.finish
optimizer.start
executor.run_start
executor.iterator_create
handler.index_read_start / handler.range_read_start
innodb.search_tuple
innodb.index_read_start
innodb.lock_request
innodb.index_read_finish
handler.index_read_finish / handler.range_read_finish
executor.result_send_start
executor.finish
sql.parse_execute
```

不是所有 SQL 都会出现每个事件。例如 `SELECT 1` 没有表访问；DDL 会绕过很多查询优化事件；后台 purge 没有用户 SQL。

## Parser / Resolver / Optimizer

Parser 负责读懂 SQL 结构，不决定怎么执行：

```text
parser.start
parser.finish
parser.error
```

Resolver 负责把 SQL 文本里的名字变成 MySQL 内部对象：

```text
resolver.start
resolver.tables_resolved
resolver.fields_resolved
resolver.conditions_resolved
resolver.finish
```

Optimizer 负责决定怎么更便宜地执行：

```text
optimizer.start
optimizer.condition_analysis
optimizer.access_path
optimizer.join_order
optimizer.range_analysis
optimizer.condition_attach
optimizer.subquery_strategy
```

阅读重点：

- `optimizer_input` 应该写清楚已经确认要读什么表、返回什么字段、按什么条件过滤。
- `access_path` 说明最终是主键查找、普通索引、range、index scan，还是全表扫描。
- `range_analysis` 说明条件怎么变成索引边界。
- `join_order` 说明多表查询先读哪张表、后读哪张表。

## Executor / Handler / InnoDB 读取

Executor 负责按计划拉取行，handler 是 Server 和存储引擎之间的接口：

```text
executor.run_start
executor.iterator_create
executor.index_range_bounds
handler.index_read_start
handler.range_read_start
innodb.search_tuple
innodb.index_read_start
innodb.index_read_finish
handler.index_read_finish
handler.range_read_finish
executor.projection
executor.result_send_start
executor.finish
```

阅读重点：

- `handler.index_read_start` 说明 Server 给存储引擎传了什么 key。
- `innodb.search_tuple` 说明 InnoDB 把 handler 的二进制 key 转成内部 tuple，用于 B+Tree 比较。
- `innodb.index_read_finish` 说明 InnoDB 是否找到记录，并把结果写回 MySQL record buffer。
- `executor.result_send_start` 说明 Server 层开始按 MySQL 协议逐行返回结果。

## 锁

Server 层锁和 InnoDB 锁要分开看：

```text
server.mdl_lock
innodb.lock_request
innodb.lock_wait
innodb.deadlock_detect
innodb.lock_release_transaction
```

阅读重点：

- `server.mdl_lock` 是元数据锁，保护表结构，不是行锁。
- `innodb.lock_request` 应说明锁在表上还是索引记录上。
- 行锁要写清楚是记录锁、间隙锁、临键锁、插入意向锁，还是表级意向锁。
- `lock_target` 的 `space/page/heap` 是 InnoDB 锁系统定位记录的坐标。
- `record_values` 能看到这把锁对应的索引记录部分字段。

## MVCC / ReadView

普通快照读和当前读要分开看：

```text
innodb.mvcc_read_view_create
innodb.mvcc_read_view_reuse
innodb.mvcc_version_check
innodb.mvcc_undo_chain_step
innodb.mvcc_undo_version_lookup
innodb.mvcc_current_read
```

阅读重点：

- 普通 `SELECT` 通常使用 ReadView。
- `UPDATE`、`DELETE`、`SELECT ... FOR UPDATE` 是当前读，读最新版本并申请锁。
- `DB_TRX_ID` 表示这条记录版本由哪个事务产生。
- `DB_ROLL_PTR` 不是 C++ 指针，而是定位 undo 记录的位置引用。
- 如果当前版本对 ReadView 不可见，InnoDB 会沿 `DB_ROLL_PTR` 找旧版本。

## DELETE + ROLLBACK 链路

这是目前最适合讲 undo、MVCC、delete mark 的样例。

测试 SQL：

```sql
USE wzg_probe_test;
DROP TABLE IF EXISTS delete_users;
CREATE TABLE delete_users (
  id INT PRIMARY KEY,
  name VARCHAR(32),
  age INT
) ENGINE=InnoDB;
INSERT INTO delete_users VALUES (1, 'alice', 18), (2, 'bob', 20);

START TRANSACTION;
DELETE FROM delete_users WHERE id = 1;
ROLLBACK;
```

应该重点看这条链：

```text
innodb.delete_mark_record
innodb.undo_lookup_step
innodb.undo_lookup_match
innodb.rollback_delete_mark
innodb.undo_lookup_stop
```

### innodb.delete_mark_record

解释 DELETE 在 InnoDB 内部怎么表示。

核心含义：

```text
DELETE 不是马上把行从 B+Tree 页里抹掉。
InnoDB 先把聚簇索引记录 deleted flag 设置为 true，
然后把 DB_TRX_ID 写成当前删除事务 id，
把 DB_ROLL_PTR 指向刚生成的 delete undo。
```

关键字段：

| 字段 | 说明 |
| --- | --- |
| `transaction_id` | 正在执行 DELETE 的 InnoDB 事务。 |
| `table` / `index` | 被 delete mark 的表和聚簇索引。 |
| `record_space_id` / `record_page_no` / `record_heap_no` | 这条记录在 InnoDB 页中的坐标。 |
| `delete_mark_after` | DELETE 后记录头 deleted flag 的结果。 |
| `db_trx_id_after` | 记录最新版本属于哪个事务。 |
| `db_roll_ptr_after` | 这条记录最新版本指向的 undo 记录。 |

### innodb.undo_lookup_step

解释回滚时怎么开始找 undo。

核心含义：

```text
ROLLBACK 从当前事务最后生成的 undo 开始往前处理。
找到一条 undo 不代表已经能撤销，
还要解析它的类型，并定位到对应的聚簇索引记录。
```

关键字段：

| 字段 | 说明 |
| --- | --- |
| `roll_ptr` | 当前弹出的 undo 记录位置。 |
| `undo_rseg_id` | undo 所在 rollback segment id。 |
| `undo_page_no` | undo 页号。 |
| `undo_offset` | undo 记录在页内偏移。 |
| `undo_record_type` | 例如 `TRX_UNDO_DEL_MARK_REC`。 |
| `lookup_order` | 回滚按后进先出处理 undo。 |

### innodb.undo_lookup_match

解释怎么算“找到了要撤销的那条记录”。

核心含义：

```text
undo 里保存 row reference。
InnoDB 用它去聚簇索引 B+Tree 找记录。
找到记录后，还要检查记录当前 DB_ROLL_PTR 是否等于 undo 的 roll_ptr。
相等才说明这条记录就是这条 undo 要撤销的版本。
```

关键字段：

| 字段 | 说明 |
| --- | --- |
| `roll_ptr_from_undo` | undo 自己的位置。 |
| `roll_ptr_on_record` | 当前聚簇索引记录里的 DB_ROLL_PTR。 |
| `match_result` | `matched` 才表示匹配成功。 |
| `match_standard` | 用 row reference + roll_ptr 相等作为判断标准。 |

### innodb.rollback_delete_mark

解释找到以后怎么撤销 DELETE。

核心含义：

```text
回滚 DELETE 不是重新 INSERT 一条新行，
而是在原聚簇索引记录上撤销 deleted flag，
并恢复记录需要的事务系统字段。
```

关键字段：

| 字段 | 说明 |
| --- | --- |
| `roll_ptr` | 使用哪条 undo 撤销。 |
| `new_trx_id_to_restore` | undo 解析出的要恢复的事务字段。 |
| `rollback_result` | 是否撤销成功。 |

### innodb.undo_lookup_stop

解释什么时候停止找 undo。

停止标准：

```text
trx_roll_pop_top_rec_of_trx 返回空，说明事务 undo 链已经走完。
如果是部分回滚，到 roll_limit/savepoint 边界也会停止。
```

## DELETE + COMMIT + Purge 链路

测试 SQL：

```sql
USE wzg_probe_test;
DROP TABLE IF EXISTS purge_users;
CREATE TABLE purge_users (
  id INT PRIMARY KEY,
  name VARCHAR(32)
) ENGINE=InnoDB;
INSERT INTO purge_users VALUES (1, 'alice'), (2, 'bob');

DELETE FROM purge_users WHERE id = 1;
COMMIT;
```

重点看：

```text
innodb.delete_mark_record
transaction.two_phase_commit_prepare_start
transaction.two_phase_commit_prepare_finish
server.binlog_ordered_commit_start
innodb.redo_log_flush_commit
server.binlog_cache_flush
server.binlog_sync
transaction.two_phase_commit_commit_stage
innodb.purge_delete_record
```

`innodb.purge_delete_record` 通常由后台 purge 线程产生，可能不是立刻出现。

核心含义：

```text
COMMIT 后 delete-marked 记录仍可能留在 B+Tree 页里。
只有当 purge 判断没有活跃 ReadView 需要旧版本时，才物理删除记录。
```

## 三大日志和二阶段提交

事务提交重点看这条链：

```text
innodb.undo_log_create
transaction.two_phase_commit_prepare_start
transaction.two_phase_commit_prepare_finish
server.binlog_ordered_commit_start
innodb.redo_log_flush_commit
server.binlog_cache_flush
server.binlog_sync
transaction.two_phase_commit_commit_stage
innodb.lock_release_transaction
```

各自解释：

| 事件 | 解释重点 |
| --- | --- |
| `innodb.undo_log_create` | 修改记录前保存旧版本，用于 rollback 和 MVCC。 |
| `transaction.two_phase_commit_prepare_start` | InnoDB 不能直接 commit，先进入 prepared 状态等待 binlog 结果。 |
| `transaction.two_phase_commit_prepare_finish` | undo 段状态和事务状态已经变成可恢复的 prepared。 |
| `innodb.redo_log_flush_commit` | redo 保证脏页没刷盘时也能崩溃恢复。 |
| `server.binlog_cache_flush` | binlog 记录逻辑变更，用于复制和时间点恢复。 |
| `server.binlog_sync` | 按 `sync_binlog` 决定 binlog 是否同步落盘。 |
| `transaction.two_phase_commit_commit_stage` | binlog 成功后，通知 InnoDB 把 prepared 事务真正 commit。 |

为什么需要二阶段提交：

```text
如果只写 redo、不写 binlog，主从复制和时间点恢复会丢事务。
如果只写 binlog、InnoDB 没提交，本机数据又没有这个事务。
所以 MySQL 先让 InnoDB prepare，再写 binlog，最后 InnoDB commit。
```

## 推荐过滤命令

按事件名看 DELETE 回滚链：

```bash
rg 'delete_mark_record|undo_lookup|rollback_delete_mark' \
  _runtime/mysql-local/logs/wzg-probe.jsonl
```

按事务 id 看完整事务：

```bash
rg '"transaction_id":"39711"' _runtime/mysql-local/logs/wzg-probe.jsonl
```

按一条 SQL 看：

```bash
rg '"raw_sql":"DELETE FROM delete_users WHERE id=1"' \
  _runtime/mysql-local/logs/wzg-probe.jsonl
```

按三大日志看：

```bash
rg 'undo_log_create|redo_log_flush_commit|binlog_cache_flush|binlog_sync|two_phase_commit' \
  _runtime/mysql-local/logs/wzg-probe.jsonl
```

## 默认忽略的客户端探测 SQL

很多 GUI、IDE、JDBC/ODBC 驱动连接 MySQL 后，会自动发送探测语句，例如：

```text
SHOW DATABASES
SHOW TABLES
SHOW VARIABLES
SELECT DATABASE()
SELECT @@version_comment
SET NAMES utf8mb4
USE some_db
```

这些语句会把学习日志冲得很乱。WZG Probe 在收到 `COM_QUERY` 后，会对当前 `raw_sql` 做一次轻量归一化；如果判断是常见客户端初始化或元数据探测 SQL，就设置当前 SQL 的 suppress 标记。之后同一条 SQL 触发的 parser、resolver、optimizer、executor、handler、InnoDB 事件都会在 WZG Probe 统一出口被丢弃。

注意：

- 连接生命周期事件仍会保留，例如 `connection.start`、`command.dispatch`。
- 只过滤当前 SQL，不影响下一个用户 SQL。
- 如果后续需要研究 `SHOW` 或 `SELECT @@...` 自身执行流程，需要临时放开 `sql/wzg_probe/wzg_trace_context.cc` 里的过滤规则。

## 后续继续加日志的原则

继续插桩前先问这几个问题：

1. 这条日志能不能说明 MySQL 内部实际改变了什么？
2. 这个值是不是来自当前运行时结构，而不是硬编码例子？
3. 用户看完是否知道为什么要做这一步？
4. 后续事件能不能通过 `transaction_id`、`roll_ptr`、`LSN`、`space/page/heap` 串起来？
5. 会不会在逐行热路径里刷爆日志？

优先补充方向：

- `UPDATE` 的字段变化和隐藏列更新。
- `INSERT` 的聚簇索引插入、二级索引插入、回滚删除。
- `redo` 更细粒度的 redo record 类型和 LSN 区间。
- `binlog` row event 类型，例如 WRITE_ROWS、UPDATE_ROWS、DELETE_ROWS。
- 可视化 adapter：把 JSONL 转成 timeline、graph 或 markdown。
