# wzg.scene.v1 Schema

`wzg.scene.v1` is an event-sourcing protocol for replaying MySQL runtime behavior
into an ECS-style world.

The frontend must not hardcode MySQL event names. It should apply `effects`,
render generic entities/components/edges, and show MySQL meaning from data.

## Event

Required fields:

| Field | Meaning |
|---|---|
| `schema` | Fixed protocol name, currently `wzg.scene.v1`. |
| `event_id` | Stable monotonically increasing id inside one trace. |
| `trace_id` | One user-visible flow, usually one SQL statement. |
| `span_id` | A local span/action id. |
| `parent_event_id` | Parent event in the timeline tree, or `null`. |
| `caused_by_event_id` | Causal predecessor, or `null`. |
| `tick` | Logical replay tick. |
| `ts_ns` | Timestamp in nanoseconds. |
| `layer` | Broad layer, such as `mysql.executor` or `innodb.btree`. |
| `namespace` | Fine event namespace. |
| `event_name` | Concrete semantic event name. |
| `phase` | `begin`, `step`, `instant`, `end`, or `error`. |
| `actor` | Who performs the action. |
| `action` | What action is happening. |
| `object` | Primary object being acted on. |
| `result` | Result summary, if known. |
| `meaning` | Human-readable explanation for default UI. |
| `runtime_values` | Real runtime values or sample values from MySQL execution. |
| `source` | Source debug location. Hidden by default in UI detail. |
| `effects` | Patches that change the ECS world. |

## Entity

Entity ids must be stable and readable:

```text
query:q1
plan:q1
iterator:q1:idx_user_id
handler_call:q1:index_read:1
table:test.orders
index:test.orders.idx_user_id
btree:test.orders.idx_user_id
page:8:3
record:8:5:12
trx:101
read_view:q1
record_buffer:q1:orders
```

Entity fields:

| Field | Meaning |
|---|---|
| `entity_id` | Stable id. |
| `entity_type` | Generic type, such as `Record` or `BTreePage`. |
| `label` | Short display label. |
| `components` | Component map keyed by component name. |

## Component

A component is a typed state object attached to an entity.

Rules:

- Runtime values from MySQL should stay as explicit fields.
- Adapter-derived fields should be named clearly or placed under `derived`.
- Source function/file/line belongs in debug data, not in the main label.

## Edges

There are two edge classes.

`parent` edges are scene graph containment:

```text
Query -> Plan -> Iterator -> HandlerCall
Table -> Index -> BTree -> BTreePage -> Record
Transaction -> ReadView
```

`relation` edges are references or actions:

```text
query chooses index
handler_call uses search_tuple
cursor points_to record
record located_in page
iterator emits record
record_buffer receives record
read_view checks record
```

## Effect

Supported MVP operations:

| Operation | Meaning |
|---|---|
| `create_entity` | Ensure an entity exists. |
| `set_component` | Replace or set one component on an entity. |
| `add_edge` | Add parent or relation edge. |
| `append_timeline_marker` | Add a UI timeline marker. |
| `focus_entity` | Suggest a focused entity for this tick. |
| `mark_decision` | Highlight a decision. |
| `mark_wait` | Highlight waiting. Reserved for lock wait. |
| `mark_error` | Highlight an error. |

## RuntimeValue

`runtime_values` is for values that explain what happened at runtime. For the
first MVP, sample values are allowed; real MySQL hooks can later fill the same
fields.

Examples:

```json
{
  "search_key": {"user_id": 10},
  "space_id": 8,
  "page_no": 5,
  "heap_no": 12,
  "compare_result": "equal"
}
```

## SourceLocation

```json
{
  "source_namespace": "innodb.page",
  "source_file": "storage/innobase/page/page0page.cc",
  "source_line": null,
  "source_function": "page_cur_search_with_match"
}
```

## Snapshot

Reserved interface for later replay checkpoints:

```json
{
  "snapshot_id": "snapshot:q1:tick:10",
  "trace_id": "trace-select-orders-user-10",
  "tick": 10,
  "world_state_ref": "..."
}
```
