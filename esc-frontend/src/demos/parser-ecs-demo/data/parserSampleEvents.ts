import type { DemoEvent, Effect, EntityId } from "../ecs/types"

export const demoSql = "SELECT id, name FROM users WHERE id = 1;"

function createBaseEvent(
  event_id: number,
  event_name: string,
  meaning: string,
  effects: Effect[],
  entityIds: EntityId[],
  runtime_values: DemoEvent["runtime_values"] = []
): DemoEvent {
  return {
    event_id,
    tick: event_id,
    namespace: "mysql.parser",
    event_name,
    phase: "instant",
    actor: "Parser",
    action: event_name.split(".").at(-1) ?? event_name,
    object: "SELECT id, name FROM users WHERE id = 1",
    result: "ok",
    meaning,
    runtime_values,
    effects: [
      ...effects,
      {
        type: "append_timeline_marker",
        marker: {
          eventId: event_id,
          tick: event_id,
          label: event_name,
          entityIds,
        },
      },
    ],
  }
}

const tokenSpecs = [
  { id: "token:q1:select", text: "SELECT", kind: "keyword", start: 0, end: 6 },
  { id: "token:q1:id", text: "id", kind: "identifier", start: 7, end: 9 },
  { id: "token:q1:name", text: "name", kind: "identifier", start: 11, end: 15 },
  { id: "token:q1:from", text: "FROM", kind: "keyword", start: 16, end: 20 },
  { id: "token:q1:users", text: "users", kind: "identifier", start: 21, end: 26 },
  { id: "token:q1:where", text: "WHERE", kind: "keyword", start: 27, end: 32 },
  { id: "token:q1:condition", text: "id = 1", kind: "condition", start: 33, end: 39 },
] as const

function tokenEffects(index: number): Effect[] {
  const token = tokenSpecs[index]

  return [
    {
      type: "create_entity",
      entity: {
        id: token.id,
        type: "Token",
        label: token.text,
      },
    },
    {
      type: "set_component",
      entityId: token.id,
      component: {
        type: "TokenComponent",
        data: {
          text: token.text,
          kind: token.kind,
          start: token.start,
          end: token.end,
        },
      },
    },
    {
      type: "set_component",
      entityId: "parser:q1",
      component: {
        type: "ParserStateComponent",
        data: {
          stage: "lexing",
          current_token: token.id,
          emitted_token_count: index + 1,
        },
      },
    },
    {
      type: "add_relation_edge",
      edge: {
        id: `rel:parser-focus:${token.id}`,
        from: "parser:q1",
        to: token.id,
        label: "currently_focus",
      },
    },
    {
      type: "focus_entity",
      entityId: token.id,
    },
  ]
}

export const parserSampleEvents: DemoEvent[] = [
  createBaseEvent(
    1,
    "mysql.parser.query.receive",
    "SQL 文本进入解析器，先作为一个 Query 和 SQL Text 放进世界。",
    [
      {
        type: "create_entity",
        entity: { id: "query:q1", type: "Query", label: "Query q1" },
      },
      {
        type: "create_entity",
        entity: { id: "sql_text:q1", type: "SqlText", label: "SQL Text" },
      },
      {
        type: "create_entity",
        entity: { id: "parser:q1", type: "Parser", label: "Parser" },
      },
      {
        type: "set_component",
        entityId: "sql_text:q1",
        component: {
          type: "TextComponent",
          data: { text: demoSql },
        },
      },
      {
        type: "set_component",
        entityId: "parser:q1",
        component: {
          type: "ParserStateComponent",
          data: { stage: "received", current_token: null },
        },
      },
      {
        type: "add_parent_edge",
        edge: {
          id: "parent:query:q1:sql_text",
          parent: "query:q1",
          child: "sql_text:q1",
          label: "contains",
        },
      },
      {
        type: "add_parent_edge",
        edge: {
          id: "parent:query:q1:parser",
          parent: "query:q1",
          child: "parser:q1",
          label: "contains",
        },
      },
      { type: "focus_entity", entityId: "sql_text:q1" },
    ],
    ["query:q1", "sql_text:q1", "parser:q1"],
    [{ name: "sql", value: demoSql, meaning: "本次演示使用的固定 SQL" }]
  ),
  createBaseEvent(
    2,
    "mysql.parser.lexer.start",
    "词法分析开始，解析器准备把 SQL 文本切成 token。",
    [
      {
        type: "set_component",
        entityId: "parser:q1",
        component: {
          type: "ParserStateComponent",
          data: { stage: "lexing", current_token: null },
        },
      },
      { type: "focus_entity", entityId: "parser:q1" },
    ],
    ["parser:q1"],
    [{ name: "stage", value: "lexing", meaning: "当前正在做词法分析" }]
  ),
  ...tokenSpecs.map((token, index) =>
    createBaseEvent(
      3 + index,
      "mysql.parser.token.emit",
      `解析器识别出 token：${token.text}`,
      tokenEffects(index),
      ["parser:q1", token.id],
      [
        { name: "token_text", value: token.text, meaning: "刚识别出的 token 文本" },
        { name: "token_kind", value: token.kind, meaning: "token 的粗略类型" },
      ]
    )
  ),
  createBaseEvent(
    10,
    "mysql.parser.syntax.select_statement.enter",
    "语法分析进入 SELECT 语句，开始建立 AST 根节点。",
    [
      {
        type: "create_entity",
        entity: { id: "ast:q1", type: "Ast", label: "AST q1" },
      },
      {
        type: "create_entity",
        entity: {
          id: "ast:q1:select_statement",
          type: "AstNode",
          label: "SelectStatement",
        },
      },
      {
        type: "set_component",
        entityId: "ast:q1:select_statement",
        component: {
          type: "AstNodeComponent",
          data: { node_kind: "SelectStatement", status: "building" },
        },
      },
      {
        type: "add_parent_edge",
        edge: {
          id: "parent:query:q1:ast",
          parent: "query:q1",
          child: "ast:q1",
          label: "contains",
        },
      },
      {
        type: "add_parent_edge",
        edge: {
          id: "parent:ast:q1:select_statement",
          parent: "ast:q1",
          child: "ast:q1:select_statement",
          label: "contains",
        },
      },
      {
        type: "add_relation_edge",
        edge: {
          id: "rel:select-token-to-statement",
          from: "token:q1:select",
          to: "ast:q1:select_statement",
          label: "belongs_to",
        },
      },
      { type: "focus_entity", entityId: "ast:q1:select_statement" },
    ],
    ["ast:q1", "ast:q1:select_statement", "token:q1:select"]
  ),
  createBaseEvent(
    11,
    "mysql.parser.syntax.select_list.build",
    "SELECT 后面的字段列表被归到 SelectList 节点下面。",
    [
      {
        type: "create_entity",
        entity: { id: "ast:q1:select_list", type: "AstNode", label: "SelectList" },
      },
      {
        type: "set_component",
        entityId: "ast:q1:select_list",
        component: {
          type: "AstNodeComponent",
          data: { node_kind: "SelectList", columns: ["id", "name"] },
        },
      },
      {
        type: "add_parent_edge",
        edge: {
          id: "parent:select_statement:select_list",
          parent: "ast:q1:select_statement",
          child: "ast:q1:select_list",
          label: "contains",
        },
      },
      {
        type: "add_relation_edge",
        edge: {
          id: "rel:token-id-to-select-list",
          from: "token:q1:id",
          to: "ast:q1:select_list",
          label: "belongs_to",
        },
      },
      {
        type: "add_relation_edge",
        edge: {
          id: "rel:token-name-to-select-list",
          from: "token:q1:name",
          to: "ast:q1:select_list",
          label: "belongs_to",
        },
      },
      { type: "focus_entity", entityId: "ast:q1:select_list" },
    ],
    ["ast:q1:select_list", "token:q1:id", "token:q1:name"]
  ),
  createBaseEvent(
    12,
    "mysql.parser.syntax.from_clause.build",
    "FROM 后面的表名被归到 FromClause 节点下面。",
    [
      {
        type: "create_entity",
        entity: { id: "ast:q1:from_clause", type: "AstNode", label: "FromClause" },
      },
      {
        type: "set_component",
        entityId: "ast:q1:from_clause",
        component: {
          type: "AstNodeComponent",
          data: { node_kind: "FromClause", table: "users" },
        },
      },
      {
        type: "add_parent_edge",
        edge: {
          id: "parent:select_statement:from_clause",
          parent: "ast:q1:select_statement",
          child: "ast:q1:from_clause",
          label: "contains",
        },
      },
      {
        type: "add_relation_edge",
        edge: {
          id: "rel:token-users-to-from-clause",
          from: "token:q1:users",
          to: "ast:q1:from_clause",
          label: "belongs_to",
        },
      },
      { type: "focus_entity", entityId: "ast:q1:from_clause" },
    ],
    ["ast:q1:from_clause", "token:q1:users"]
  ),
  createBaseEvent(
    13,
    "mysql.parser.syntax.where_clause.build",
    "WHERE 条件被归到 WhereClause 节点下面。",
    [
      {
        type: "create_entity",
        entity: { id: "ast:q1:where_clause", type: "AstNode", label: "WhereClause" },
      },
      {
        type: "create_entity",
        entity: { id: "ast:q1:condition", type: "AstNode", label: "Condition" },
      },
      {
        type: "set_component",
        entityId: "ast:q1:where_clause",
        component: {
          type: "AstNodeComponent",
          data: { node_kind: "WhereClause" },
        },
      },
      {
        type: "set_component",
        entityId: "ast:q1:condition",
        component: {
          type: "AstNodeComponent",
          data: { node_kind: "Condition", expression: "id = 1" },
        },
      },
      {
        type: "add_parent_edge",
        edge: {
          id: "parent:select_statement:where_clause",
          parent: "ast:q1:select_statement",
          child: "ast:q1:where_clause",
          label: "contains",
        },
      },
      {
        type: "add_parent_edge",
        edge: {
          id: "parent:where_clause:condition",
          parent: "ast:q1:where_clause",
          child: "ast:q1:condition",
          label: "contains",
        },
      },
      {
        type: "add_relation_edge",
        edge: {
          id: "rel:token-condition-to-condition-node",
          from: "token:q1:condition",
          to: "ast:q1:condition",
          label: "belongs_to",
        },
      },
      { type: "focus_entity", entityId: "ast:q1:condition" },
    ],
    ["ast:q1:where_clause", "ast:q1:condition", "token:q1:condition"]
  ),
  createBaseEvent(
    14,
    "mysql.parser.ast.node.create",
    "AST 关键节点都已经创建，token 和语法节点也建立了关系。",
    [
      {
        type: "set_component",
        entityId: "ast:q1",
        component: {
          type: "AstNodeComponent",
          data: { node_kind: "AstRoot", node_count: 5 },
        },
      },
      {
        type: "add_relation_edge",
        edge: {
          id: "rel:ast-produced-by-parser",
          from: "ast:q1",
          to: "parser:q1",
          label: "produced_by",
        },
      },
      { type: "focus_entity", entityId: "ast:q1" },
    ],
    ["ast:q1", "parser:q1"]
  ),
  createBaseEvent(
    15,
    "mysql.parser.ast.complete",
    "解析完成，最终 AST 可以交给后面的优化器阶段。",
    [
      {
        type: "set_component",
        entityId: "parser:q1",
        component: {
          type: "ParserStateComponent",
          data: { stage: "complete", current_token: null },
        },
      },
      {
        type: "set_component",
        entityId: "ast:q1:select_statement",
        component: {
          type: "AstNodeComponent",
          data: { node_kind: "SelectStatement", status: "complete" },
        },
      },
      { type: "focus_entity", entityId: "ast:q1:select_statement" },
    ],
    ["parser:q1", "ast:q1:select_statement"]
  ),
]
