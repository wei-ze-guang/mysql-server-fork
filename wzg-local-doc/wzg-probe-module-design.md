# WZG Probe 模块设计和插桩目标

记录时间：2026-06-22

这份文档给后续接手的 AI 或开发者看。读完后应该马上知道：我们为什么改 MySQL 源码、日志要写成什么风格、已经插了哪些阶段、后面继续写时要注意什么。

## 一句话目标

WZG Probe 是编译进 `mysqld` 的源码插桩模块，用来把一条 SQL 在 MySQL Server 和 InnoDB 内部经历的关键阶段记录成 JSON Lines 日志。

这个项目不是做高性能监控，也不是替代 MySQL 自带的 optimizer trace、performance_schema 或 explain。它的目标是：

- 让人看懂一条 SQL 从客户端进来，到解析、绑定、优化、执行、访问存储引擎、返回客户端的大致过程。
- 日志必须尽量通俗易懂，适合学习源码和面试复盘。
- 日志必须尽量贴近真实源码行为，不编故事，不硬编码示例值。
- 插桩不能改变 MySQL 原来的执行结果。
- 可以接受一定性能损耗，但不能为了日志破坏真实执行流程。

## 当前项目状态

当前已经不是“只搭模块，不插执行路径”的初始阶段。现在 WZG Probe 已经接入了 MySQL 的多个关键阶段：

```text
客户端命令
  -> parser 解析器
  -> resolver 名字解析和语义准备
  -> optimizer 优化器
  -> executor 执行器
  -> handler 接口
  -> InnoDB 部分访问路径
  -> Server 返回结果给客户端
```

当前日志文件默认写到：

```text
_runtime/mysql-local/logs/wzg-probe.jsonl
```

本地编译和运行说明见：

```text
wzg-local-doc/mysql-local-build-report.md
```

事件链路怎么阅读、典型 SQL 应该看哪些事件，见：

```text
wzg-local-doc/wzg-probe-event-guide.md
```

## 日志风格要求

### Key 使用英文

JSON key 必须使用英文，方便后续程序解析、可视化和 ECS adapter 转换。

例如：

```jsonc
{
  "event_name": "optimizer.access_path",
  "fields": {
    "table": "wzg_probe_test.orders",
    "access_method": "range",
    "chosen_index": "PRIMARY"
  }
}
```

### Value 尽量使用中文解释

面向人的字段值、`message`、`note`、`reason`、`next_step` 尽量使用中文，要求通俗、具体。

好的写法：

```jsonc
{
  "event_name": "optimizer.start",
  "message": "开始优化查询，准备决定这条 SQL 怎样执行更快",
  "fields": {
    "optimizer_input": "已经确认要读取 orders 表，返回 user_id 字段，并按 id <= 2 过滤",
    "next_step": "查看表的数据量、索引和过滤条件，选择更省成本的执行方式"
  }
}
```

不要写太内部化、用户看不懂的话：

```jsonc
{
  "optimizer_input": "resolver 已经完成表名、字段名和条件表达式绑定"
}
```

如果必须出现内部概念，要马上解释它对 SQL 执行有什么意义。

### 不硬编码业务值

日志里的表名、字段名、索引名、条件、估算行数、访问方式、锁类型、返回行数等，必须从当前 MySQL 运行时结构里取。

可以写固定解释文本，例如：

```text
这里不重新执行 ORDER BY、GROUP BY、DISTINCT、UNION 或窗口函数
```

不能把示例查询里的 `users`、`id = 1`、`PRIMARY` 写死到通用逻辑里。

### 不逐行刷业务数据

执行器、handler、InnoDB 很容易进入逐行循环。默认原则：

- 可以记录“开始读取”“读取完成”“第一次开始发送结果”等关键节点。
- 不记录每一行的真实业务数据。
- 不按每行输出日志，除非以后专门打开调试开关。

这样可以避免日志太大，也减少泄露业务数据的风险。

## 原始事件结构

WZG Probe 输出 JSON Lines，每行是一条事件。

```jsonc
{
  "schema": "wzg.raw.v1",                  // 日志格式版本
  "event_id": 1001,                        // WZG Probe 自增事件 ID

  "ts_ns": 123456789,                      // 事件发生时间，单位纳秒
  "duration_ns": 50000,                    // 事件耗时，单位纳秒；瞬时事件可以为 0

  "event_name": "optimizer.access_path",   // 事件名称，点分层级
  "event_phase": "instant",                // instant、begin、end
  "event_level": "info",                   // debug、info、warn、error

  "thread_id": 8,                          // MySQL THD 线程 ID
  "connection_id": 8,                      // MySQL 连接 ID，第一版等同于 thread_id
  "connection_uuid": "conn-8-1",           // WZG 生成的连接生命周期 ID
  "query_id": 42,                          // MySQL THD query_id

  "user": "root",                          // 当前连接用户
  "host": "localhost",                     // 当前连接来源主机
  "db": "wzg_probe_test",                  // 当前默认数据库

  "command": "COM_QUERY",                  // MySQL 协议命令
  "sql_command": "select",                 // SQL 语义命令
  "raw_sql": "select user_id from orders", // 客户端发来的原始 SQL 快照
  "query": "select user_id from orders",   // 当前 SQL 文本快照

  "error": false,                          // 当前事件记录时 THD 是否有错误
  "error_code": 0,                         // MySQL 错误码
  "warning_count": 0,                      // 当前语句 warning 数量

  "message": "最终选择了这张表的读取方式",
  "fields": {                              // 插桩点自定义字段，不同事件可以不同
    "table": "wzg_probe_test.orders",
    "access_method": "range",
    "chosen_index": "PRIMARY"
  }
}
```

`raw_sql` 很重要。它表示客户端发来的原始 SQL，应该贯穿 parser、resolver、optimizer、executor、handler 等阶段。后续如果加入 `rewritten_sql`、`normalized_sql` 或执行计划摘要，可以和 `raw_sql` 对比。

## 模块位置和依赖原则

WZG Probe 模块放在：

```text
sql/wzg_probe/
```

当前核心文件：

```text
sql/wzg_probe/CMakeLists.txt
sql/wzg_probe/wzg_probe.h
sql/wzg_probe/wzg_probe.cc
sql/wzg_probe/wzg_trace_context.h
sql/wzg_probe/wzg_trace_context.cc
sql/wzg_probe/wzg_trace_event.h
sql/wzg_probe/wzg_trace_event.cc
sql/wzg_probe/wzg_json_writer.h
sql/wzg_probe/wzg_json_writer.cc
sql/wzg_probe/wzg_file_sink.h
sql/wzg_probe/wzg_file_sink.cc
```

`sql/CMakeLists.txt` 已接入 `wzg_probe` 并链接到 `mysqld`。

依赖原则：

- `wzg_probe.h` 是薄头文件，只暴露插桩 API。
- 尽量不要频繁修改 `wzg_probe.h`，否则 include 它的 server 源文件会大量重编。
- 读取 `THD`、组装公共字段、写 JSON 的逻辑尽量放在 `.cc` 文件。
- 插桩点所在源码文件只负责提取该阶段真实存在的信息，并调用 `WZG_PROBE_EVENT`。

常用 API：

```cpp
WZG_PROBE_EVENT(thd, "optimizer.access_path")
    .message("最终选择了这张表的读取方式")
    .field("table", table_name)
    .field("access_method", access_method)
    .emit();
```

范围事件也支持：

```cpp
{
  WZG_PROBE_SCOPE(thd, "some.stage")
      .field("key", "value");
  // 原 MySQL 逻辑
}
```

## 当前阶段地图

### 连接和命令入口

主要目标：说明客户端发来了什么命令，Server 开始分发什么。

当前事件：

```text
connection.start
command.dispatch
sql.query_received
connection.end
```

注意：连接、授权、认证当前不是重点，只需要少量日志辅助串联流程。

### Parser 解析器

Parser 的定位：判断 SQL 文本在语法上能不能被 MySQL 理解，并生成内部语法结构。

它主要回答：

- SQL 是否通过语法解析。
- 它是哪类语句，例如 SELECT、INSERT、UPDATE。
- 是否出现子查询或嵌套查询块。
- 解析后会交给 resolver、optimizer 或对应执行阶段。

它不负责：

- 决定使用哪个索引。
- 决定表访问顺序。
- 决定子查询最终先执行哪一部分。
- 真实读取或修改数据。

当前事件：

```text
parser.start
parser.finish
parser.error
sql.parse_execute
```

推荐风格：

```jsonc
{
  "event_name": "parser.finish",
  "message": "SQL 语法解析完成，发现主查询中包含子查询结构",
  "sql_command": "select",
  "fields": {
    "parse_result": "success",
    "statement_type": "查询语句",
    "has_subquery": "true",
    "query_structure": "主查询包含子查询或其他嵌套查询块",
    "parser_output": "已生成主查询和子查询的内部结构，供后续优化器使用",
    "next_step": "进入优化器或执行阶段，由后续阶段决定改写方式、表访问顺序和索引选择",
    "note": "解析器识别语句结构，但不最终决定哪一部分先执行"
  }
}
```

### Resolver 名字解析和语义准备

Resolver 的定位：把 SQL 文本中的表名、字段名、表达式，变成 MySQL 后续阶段可以使用的内部对象。

它主要回答：

- 要访问哪些表。
- 字段是否存在。
- `SELECT *` 展开后有哪些真实字段。
- WHERE、HAVING 等条件表达式是否合法。
- 表达式类型是否能推导，例如 `id = 1` 是否能比较。
- 后面优化器拿到的查询结构已经具备哪些信息。

当前事件：

```text
resolver.start
resolver.tables_resolved
resolver.fields_resolved
resolver.conditions_resolved
resolver.finish
resolver.error
```

写日志时不要只说“完成绑定”。要写成用户能看懂的话，例如：

```text
已经确认要读取 orders 表，返回 user_id 字段，并按 id <= 2 过滤
```

### Optimizer 优化器

Optimizer 是本项目最重要的阶段之一。它的定位：在不改变 SQL 语义的前提下，选择更便宜的执行方式。

它主要回答：

- 表大概有多少行。
- 表上有哪些索引。
- WHERE 条件能不能变成索引候选。
- 范围条件、等值条件、IN、LIKE、IS NULL 等能否用于索引分析。
- 多表 JOIN 先读哪张表，后读哪张表。
- 子查询是否可以改写或选择特殊策略。
- ORDER BY、GROUP BY、DISTINCT 是否需要排序、临时表或去重。
- 最终每张表怎么读：const、ref、range、index scan、table scan 等。
- 预计成本、预计扫描行数、最终计划摘要。

当前事件：

```text
optimizer.start
optimizer.table_stats
optimizer.condition_analysis
optimizer.range_analysis
optimizer.condition_attach
optimizer.join_order
optimizer.access_path
optimizer.sort_group
optimizer.subquery_strategy
optimizer.index_condition_pushdown_attempt
optimizer.index_condition_pushdown_finish
optimizer.finish
```

优化器日志必须特别注意“具体”。不要只写“分析统计信息”，要写清楚：

- 操作谁：哪张表、哪个索引、哪个条件。
- 得到什么：估算行数、候选索引、可用/不可用原因。
- 意义是什么：后面为什么可能选择 range/ref/full scan。

推荐风格：

```jsonc
{
  "event_name": "optimizer.table_stats",
  "message": "读取 orders 表的统计信息和索引信息，用来估算不同读取方式的成本",
  "fields": {
    "table": "wzg_probe_test.orders",
    "table_rows_estimate": "5",
    "indexes": "PRIMARY(id), idx_user_id(user_id)",
    "usable_indexes": "PRIMARY",
    "stats_source": "Server 表对象和存储引擎统计信息",
    "note": "这里的行数是优化器估算值，不等于执行阶段实际读取行数"
  }
}
```

`optimizer.access_path` 要表达“最终怎么读这张表”，例如：

```text
用 PRIMARY 做 range 读取
用普通索引做 ref 查找
没有合适索引，选择全表扫描
```

### Executor 执行器

Executor 的定位：按照优化器选出的计划真正执行。它会创建 iterator，驱动表扫描、索引扫描、JOIN、过滤、排序、聚合、临时表、物化、去重、窗口函数、最终结果发送。

当前事件：

```text
executor.run_start
executor.iterator_create
executor.table_scan
executor.index_scan
executor.index_range_bounds
executor.ref_lookup_key
executor.filter_plan
executor.join_method
executor.aggregate
executor.sort
executor.sort_result
executor.limit_offset
executor.window
executor.materialize
executor.temp_table_create
executor.temp_table_result
executor.having
executor.distinct
executor.set_operation
executor.locking_read
executor.projection
executor.result_send_start
executor.finish
```

执行器日志要串起来看：

```text
executor.run_start
  -> executor.iterator_create
  -> executor.index_range_bounds / executor.ref_lookup_key / executor.table_scan / executor.index_scan
  -> executor.filter_plan
  -> executor.join_method
  -> executor.sort / executor.aggregate / executor.temp_table_create / executor.distinct / executor.window
  -> executor.projection
  -> executor.result_send_start
  -> executor.finish
```

不是每条 SQL 都会出现所有事件。简单 SQL 可能跳过排序、临时表、聚合、JOIN、窗口函数。

`executor.result_send_start` 的意义：Server 层开始把最终结果行按 MySQL 客户端协议逐行写给客户端。它只记录一次，不逐行记录具体值。它也不重新执行 ORDER BY、GROUP BY、DISTINCT、UNION 或窗口函数，这些如果需要，已经在前面的执行节点完成。

### Server 锁

当前已补 Server 层 MDL 日志：

```text
server.mdl_lock
```

它表示 metadata lock，也就是表定义/对象层面的元数据锁。

注意：

- MDL 是 Server 层。
- InnoDB 的意向锁、行锁、间隙锁、next-key lock 是存储引擎层。
- `SELECT ... FOR UPDATE` 这类 locking read 会在 Server 执行层先表达锁定读取意图，真实记录锁由 InnoDB 处理。

当前执行器里也有：

```text
executor.locking_read
```

用于说明 `FOR UPDATE`、`FOR SHARE`、`NOWAIT`、`SKIP LOCKED` 等锁定读取语义。

### Handler 和 InnoDB

Handler 是 Server 和存储引擎之间的接口层。Server 不直接读 InnoDB 页，它通过 handler 方法告诉存储引擎：

- 要读哪张表。
- 要用哪个索引。
- 要按什么 key 或 range 边界查。
- 要正向读、反向读，还是继续取下一条。

当前事件：

```text
handler.index_read_start
handler.index_read_finish
handler.range_read_start
handler.range_read_finish
innodb.index_read_start
innodb.index_read_finish
innodb.cursor_fetch_start
innodb.cursor_fetch_finish
innodb.index_condition_received
innodb.index_condition_check
```

这一层的日志重点是参数要讲清楚：

- handler 方法名是什么。
- 参数代表什么。
- key buffer 是什么意义。
- key length 是什么意义。
- find flag / search mode 是什么意义。
- 返回值类型是什么，0 表示什么，错误码表示什么。

InnoDB 后续还需要继续补：

- 事务 read view 和 MVCC。
- 聚簇索引和二级索引读取。
- 回表。
- undo log。
- redo log。
- buffer pool。
- 行锁、间隙锁、next-key lock、意向锁。

## MySQL 三大日志的边界

常见面试说的三大日志通常是：

- binlog：Server 层，和复制、恢复有关。
- redo log：InnoDB 层，保证崩溃恢复。
- undo log：InnoDB 层，支持回滚和 MVCC。

当前项目还没有系统性插三大日志。后续如果进入更新语句、事务提交、InnoDB 写路径，需要专门补这一块。

## 编译和运行约定

本项目已经做过首次完整构建。后续一般只编译 `mysqld`：

```bash
cmake --build _build/mysql-local --parallel 8 --target mysqld
```

不要开最大线程，给系统留资源。

重启本地实例的常用方式：

```bash
: > _runtime/mysql-local/logs/wzg-probe.jsonl

_install/mysql-local/bin/mysqladmin \
  --protocol=SOCKET \
  --socket="$PWD/_runtime/mysql-local/run/mysql.sock" \
  -uroot shutdown || true

WZG_PROBE_LOG="$PWD/_runtime/mysql-local/logs/wzg-probe.jsonl" \
_build/mysql-local/runtime_output_directory/mysqld --daemonize \
  --basedir="$PWD/_install/mysql-local" \
  --datadir="$PWD/_runtime/mysql-local/data" \
  --socket="$PWD/_runtime/mysql-local/run/mysql.sock" \
  --pid-file="$PWD/_runtime/mysql-local/run/mysqld.pid" \
  --log-error="$PWD/_runtime/mysql-local/logs/mysqld.err" \
  --tmpdir="$PWD/_runtime/mysql-local/tmp" \
  --port=3307 \
  --mysqlx=OFF
```

连接方式：

```bash
_install/mysql-local/bin/mysql \
  --protocol=SOCKET \
  --socket="$PWD/_runtime/mysql-local/run/mysql.sock" \
  -uroot
```

查看某类事件：

```bash
jq -c 'select(.event_name=="executor.result_send_start") | {event_name,raw_sql,fields}' \
  _runtime/mysql-local/logs/wzg-probe.jsonl
```

## 增量编译注意事项

影响范围：

- 只改 `sql/wzg_probe/*.cc`：重编 WZG Probe 小模块并重新链接 `mysqld`。
- 改 `sql/wzg_probe/wzg_probe.h`：所有 include 该头的插桩点会重编。
- 改 `sql/query_result.h` 这类公共头：可能触发大量 SQL 模块重编。
- 改 `sql/sql_optimizer.cc`：主要重编优化器所在目标并重新链接。
- 改 `sql/join_optimizer/access_path.cc`：主要影响执行器 iterator 创建逻辑。
- 改 `storage/innobase/handler/ha_innodb.cc`：会重编 InnoDB 相关目标。

后续写代码时优先选择局部 `.cc` 插桩。只有确实需要保存状态时，才改公共头或核心类。

## 后续继续插桩时的判断标准

补一个新日志前先问：

1. 这个阶段是不是 MySQL 真实存在的关键动作？
2. 这条日志能不能帮助人理解 SQL 为什么这样执行？
3. 能不能从源码真实结构里拿到具体数据？
4. 是否会进入高频逐行循环？
5. 如果会高频触发，能不能只记录开始、结束、摘要或第一次？
6. 日志文字是不是用户能看懂，而不是只有源码作者能看懂？
7. 有没有误导用户，以为这个阶段做了其实没做的事？

符合这些条件再写。

## 当前最重要的后续方向

Server 层已经覆盖了大部分读查询主路径。下一阶段重点是 InnoDB 存储引擎层，尤其是：

- 索引读取如何落到 InnoDB。
- 二级索引和聚簇索引的关系。
- 回表过程。
- MVCC 和 read view。
- 锁：意向锁、记录锁、间隙锁、next-key lock。
- undo log 和可见性判断。
- redo log 和事务提交写路径。

写 InnoDB 日志时依然保持同样风格：参数说清楚，动作说清楚，结果说清楚，不能为了通俗而改变事实。
