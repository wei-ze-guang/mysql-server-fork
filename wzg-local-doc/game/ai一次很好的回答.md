你现在有 **ECS + scene graph**，这已经是骨架了。但如果按“无帧游戏引擎 / 数字孪生 MySQL”来做，还差几类非常关键的概念。

我会这样补：

**1. Event Stream**
这是最核心的。因为我们没有帧，世界变化只能靠事件流驱动。

```text
MySQL 日志 = event stream
前端世界 = event stream replay 出来的状态
```

ECS 和 scene graph 本身只是当前状态。真正让它动起来的是：

```text
event_id / tick / parent_event_id / caused_by_event_id
```

没有 event stream，你只能看到静态图；有 event stream，才能播放 SQL 执行。

**2. Timeline / Tick**
无帧不代表没有时间。你需要逻辑时间：

```text
tick = event_id
time = ts_ns
lane = mysql.optimizer / innodb.btree / innodb.lock
```

前端可以：
- 播放
- 暂停
- 单步
- 回退
- 跳到某个事件
- 按 lane 看局部链路

所以你还缺：

```text
Timeline
LogicalTick
Lane
PlaybackCursor
```

**3. Command / Action System**
ECS 里实体有状态，但“发生了什么动作”需要单独建模。

比如：

```text
Action: choose_index
Actor: optimizer
Object: idx_abc
Result: chosen
```

这不是 entity，也不是 parent-child，而是事件里的动作语义。

你需要：

```text
Actor
Action
Object
Decision
Effect
```

这会让前端知道：这个事件不是普通状态更新，而是“优化器选择了索引”。

**4. Effects / Patches**
游戏里系统每 tick 产生 state changes。你的日志也应该像 patch：

```json
"effects": [
  {
    "op": "create_entity",
    "entity": "index:wzg.t.idx_abc"
  },
  {
    "op": "set_component",
    "entity": "index:wzg.t.idx_abc",
    "component": "OptimizerState",
    "value": {
      "candidate_state": "chosen"
    }
  },
  {
    "op": "add_edge",
    "from": "query:23",
    "to": "index:wzg.t.idx_abc",
    "type": "chooses_index"
  }
]
```

如果只有日志文本，前端要猜怎么改世界。  
如果有 effects，前端直接 apply。

**5. Causality Graph**
Scene graph 是空间/归属关系，但 MySQL 执行还有因果关系：

```text
optimizer 选择 idx_abc
  导致 handler 传入 idx_abc key buffer
    导致 InnoDB 构建 search_tuple
      导致 B+Tree 搜索 page
```

这不是父子关系，是因果链。

你需要：

```text
caused_by_event_id
parent_event_id
trace_id
span_id
```

甚至可以有：

```text
edge.type = caused
```

这样前端能画“为什么发生”。

**6. State Snapshot**
Event stream 可以 replay，但有时候前端不能每次从头播。需要 snapshot：

```text
snapshot at tick 500
```

比如保存：
- 当前 query scene
- 当前 plan
- 当前事务状态
- 当前锁等待图
- 当前 B+Tree 展开节点

否则日志大了之后回放会慢。

所以你可能还缺：

```text
Snapshot
Checkpoint
ReplayFromSnapshot
```

**7. Lanes / Layers**
MySQL 不是一个平面。你需要 lane 分层展示：

```text
client/protocol
server.parser
server.optimizer
server.executor
handler.api
innodb.handler
innodb.btree
innodb.page
innodb.mvcc
innodb.lock
innodb.trx
innodb.undo
innodb.redo
innodb.buffer_pool
```

这不是 scene graph 的父子关系，而是**时间轴泳道**。

**8. System Boundary**
必须显式表示“跨层调用边界”。

比如：

```text
mysql.handler.api.index_read.call
  crosses boundary:
server.executor -> handler api -> innodb.handler
```

这个很重要，因为你想解释“发生在哪里”。

你需要概念：

```text
Boundary
CrossLayerCall
CallReturn
```

例如：

```json
"boundary": {
  "from": "mysql.executor",
  "to": "mysql.handler.api",
  "contract": "handler index_read returns int status code"
}
```

**9. Contract**
跨边界时要有契约：

```text
handler.index_read:
输入：key buffer, keypart_map, find_flag, record buffer
输出：int return code
副作用：成功时写入 table->record[0]
```

这和游戏里 system API contract 很像。

你需要：

```text
Contract
Input
Output
SideEffect
ReturnCode
```

**10. Identity Registry**
你刚刚说“任何东西有父亲就能找到父亲”，这需要 ID 注册表。

前端需要维护：

```text
entity_id -> entity
parent_id -> parent
placeholder entity
```

如果日志先出现 record，再出现 page，也没关系，先创建 placeholder。

所以你缺：

```text
EntityRegistry
PlaceholderEntity
IdentityResolver
```

**11. Semantic Zoom**
这个非常重要。MySQL 细节太多，不可能一屏全部展示。

你需要：

```text
level 0: 一条 SQL 总览
level 1: Server / Handler / InnoDB 三段
level 2: Optimizer / Executor / BTree / MVCC / Lock
level 3: page / record / undo / redo
level 4: 字节级结构
```

这叫语义缩放。没有这个，前端会变成一坨节点。

**12. Focus / Camera**
游戏思维里还需要 camera。

对于 MySQL 可视化：

```text
camera.focus = 当前事件 object
camera.follow = cursor / transaction / query
camera.mode = optimizer / btree / lock_wait / transaction
```

比如播放到 `innodb.page.directory.binary_search`，镜头应该聚焦到 page directory。

所以你缺：

```text
Camera
Focus
Selection
FollowTarget
```

**13. Queries Over The Twin**
既然是数字孪生，用户应该能问：

```text
这个查询为什么没用 idx_abc？
这个 trx 持有哪些锁？
这条记录为什么不可见？
这个 undo record 被谁创建？
这次回表从哪个二级索引跳到哪个 PRIMARY？
```

这需要前端/adapter 有查询能力：

```text
Graph Query
Entity Search
Event Filter
Causal Trace
```

**14. Invariants**
为了保证日志质量，要有规则检查：

```text
每个 entity 必须有 parent
每个 edge 的 from/to 必须能解析
每个 handler.call 必须有 handler.return
每个 lock.wait 必须有 blocker
每个 transaction.commit 必须有 trx id
```

你缺：

```text
Validator
InvariantChecker
SchemaLint
```

这个对你后续很重要，因为日志会越来越多。

**15. Diff / State Change**
不是每条事件都要完整状态，有些只需要 diff：

```text
before: trx.active
after: trx.committed
```

或者：

```text
old_cursor_page = 4
new_cursor_page = 7
```

你需要：

```text
StateDiff
BeforeAfter
```

**总结：你现在已有**
```text
ECS
Scene Graph
```

**我建议补上的核心概念**
```text
Event Stream
Timeline / Tick
Actor-Action-Object
Effects / Patches
Causality Graph
Snapshot / Checkpoint
Lane / Layer
Boundary / Contract
Identity Registry
Semantic Zoom
Camera / Focus
Graph Query
Invariant Checker
State Diff
```

如果按优先级，先补这 6 个：

```text
1. Event Stream
2. Identity Registry
3. Effects / Patches
4. Causality Graph
5. Lanes
6. Semantic Zoom
```

一句话：

> ECS 是“世界有什么”，scene graph 是“东西挂在哪里”，event stream 是“世界怎么动”，causality graph 是“为什么动”，effects 是“怎么更新世界”，semantic zoom 是“人怎么看得懂”。

你现在最缺的不是 ECS，而是 **event stream + causality + effects** 这三件套。