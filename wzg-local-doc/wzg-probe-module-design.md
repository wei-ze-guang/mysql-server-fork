# WZG Probe 模块设计记录

记录时间：2026-06-21

## 当前目标

先把 WZG Probe 作为 MySQL Server 内部模块编译进 `mysqld`，暂时不在 SQL 执行路径插桩。当前阶段只确定模块位置、字段结构、连接关联 ID 和最小 API，后续再逐步选择插桩点。

## 模块位置

模块放在：

```text
sql/wzg_probe/
```

当前文件：

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

`sql/CMakeLists.txt` 已经接入：

```cmake
ADD_SUBDIRECTORY(wzg_probe)

TARGET_LINK_LIBRARIES(mysqld
  sql_main sql_gis binlog rpl rpl_source rpl_replica sql_dd mysys
  minchassis mysql_binlog_event ext::icu wzg_probe)
```

## 依赖原则

`wzg_probe.h` 是薄头文件，只暴露插桩 API，不直接包含 `sql/sql_class.h`。读取 `THD` 的逻辑放在 `wzg_probe.cc`，这样后续改 JSON writer、sink、event 内部结构时，尽量不会触发大量 server 源文件重编。

当前策略：

- 公共 API 放 `wzg_probe.h`。
- `THD` 快照逻辑放 `wzg_probe.cc`。
- 每线程上下文放 `wzg_trace_context.*`。
- 原始事件结构放 `wzg_trace_event.*`。
- JSON Lines 输出放 `wzg_json_writer.*`。
- 输出 sink 放 `wzg_file_sink.*`。

## 原始事件字段设计

第一阶段不直接做 ECS 结构，只输出扁平 raw event。后续可以通过 adapter 转成 ECS 或可视化结构。

```jsonc
{
  "schema": "wzg.raw.v1",                  // 日志格式版本，后续字段升级时用来兼容解析
  "event_id": 1001,                        // 插桩模块生成的事件自增 ID，用于唯一标识一条事件

  "ts_ns": 123456789,                      // 事件发生时间，单位纳秒
  "duration_ns": 50000,                    // 事件耗时，单位纳秒；瞬时事件可以为 0

  "event_name": "sql.mysql_execute_command", // 事件完整名称，建议使用点分层级
  "event_phase": "end",                    // 事件阶段：instant、begin、end
  "event_level": "info",                   // 事件级别：debug、info、warn、error

  "thread_id": 8,                          // MySQL THD 线程 ID，来自 thd->thread_id()
  "connection_id": 8,                      // MySQL 连接 ID，第一版等同于 thd->thread_id()
  "connection_uuid": "conn-8-1",           // WZG 生成的连接生命周期 ID，用于连接层和后续线程事件关联
  "query_id": 42,                          // MySQL THD query_id，用于关联同一次 SQL 执行中的多条事件

  "user": "root",                          // 当前连接用户，来自 THD security_context
  "host": "localhost",                     // 当前连接来源主机，来自 THD security_context
  "db": "test",                            // 当前默认数据库，来自 thd->db()

  "command": "COM_QUERY",                  // MySQL 协议命令类型，后续在 dispatch_command 插桩时填充
  "sql_command": "SQLCOM_SELECT",          // SQL 语义命令类型，解析后才可能填充
  "query": "select 1",                     // 当前 SQL 文本；当前模块限制最多复制 4096 字节

  "error": false,                          // 当前事件记录时 THD 是否处于错误状态
  "error_code": 0,                         // MySQL 错误码；没有错误时为 0
  "warning_count": 0,                      // 当前语句 warning 数量

  "fields": {                              // 插桩点自定义字段，不同事件可以不同
    "packet_length": "8",                  // 示例：协议包长度；第一版 fields 值统一输出为字符串
    "table": "test.users",                 // 示例：涉及的表名
    "handler_method": "index_read",        // 示例：handler 层方法名
    "index": "PRIMARY"                     // 示例：访问的索引名
  }
}
```

## 连接 ID 关联方式

当前不修改 `THD` 类，使用独立 thread-local 上下文：

```text
thread_local TraceContext
```

`TraceContext` 内保存：

```text
connection_uuid
```

生成规则第一版为：

```text
conn-<mysql_thread_id>-<global_connection_seq>
```

例如：

```text
conn-8-1
```

连接层后续插桩时调用：

```cpp
wzg_probe::on_connection_start(thd);
wzg_probe::on_connection_end(thd);
```

SQL 层、handler 层、InnoDB 层的事件只要运行在同一服务端线程里，就能从 thread-local `TraceContext` 取到同一个 `connection_uuid`。

## 当前 API

瞬时事件：

```cpp
WZG_PROBE_EVENT(thd, "connection.start")
    .field("source", "accept")
    .emit();
```

范围事件：

```cpp
{
  WZG_PROBE_SCOPE(thd, "sql.mysql_execute_command")
      .field("sql_command", "SQLCOM_SELECT");
  // 原 MySQL 逻辑
}
```

`Scope` 析构时自动输出事件，并填充 `duration_ns`。

## 当前输出

当前 `wzg_file_sink.cc` 先输出到 `stderr`。因为模块还没有插到执行路径，所以正常启动不会产生 WZG 日志。后续接入运行路径后，再把 sink 改为项目内文件，例如：

```text
_runtime/mysql-local/logs/wzg-probe.jsonl
```

## 增量编译建议

后续一般只编译 `mysqld`：

```bash
cmake --build _build/mysql-local --parallel 8 --target mysqld
```

影响范围：

- 只改 `sql/wzg_probe/*.cc`：重编 WZG Probe 小模块并重新链接 `mysqld`。
- 改 `sql/wzg_probe/wzg_probe.h`：所有 include 该头的插桩点会重编。
- 改 `sql/sql_parse.cc` 插桩点：重编 `sql_parse.cc` 并重新链接 `mysqld`。
- 改 InnoDB 插桩点：重编对应 InnoDB 源文件及相关目标。

因此后续尽量保持 `wzg_probe.h` 稳定，把变化放在 `.cc` 文件里。

## 当前验证

已执行：

```bash
cmake -S . -B _build/mysql-local -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_INSTALL_PREFIX="$PWD/_install/mysql-local" \
  -DWITH_SSL=/usr/local/opt/openssl@3 \
  -DBISON_EXECUTABLE=/usr/local/opt/bison/bin/bison

cmake --build _build/mysql-local --parallel 8 --target mysqld
```

结果：`mysqld` 编译成功。

版本验证：

```bash
_build/mysql-local/runtime_output_directory/mysqld --version
```

输出：

```text
mysqld  Ver 8.4.10 for macos15.7 on x86_64 (Source distribution)
```
