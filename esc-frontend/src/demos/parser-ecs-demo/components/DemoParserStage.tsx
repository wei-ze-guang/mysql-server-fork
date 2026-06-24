import { Cpu, DatabaseZap } from "lucide-react"
import type { ReactNode } from "react"

import { Badge } from "@/components/ui/badge"
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from "@/components/ui/card"
import { cn } from "@/lib/utils"

import type { DemoEvent, Entity, EntityId, ParentEdge, WorldState } from "../ecs/types"

type DemoParserStageProps = {
  sql: string
  world: WorldState
  selectedEvent: DemoEvent | null
  highlightedEntityIds: Set<EntityId>
  onSelectEntity: (entityId: EntityId) => void
}

type TokenView = {
  entity: Entity
  text: string
  kind: string
  start: number
  end: number
}

export function DemoParserStage({
  sql,
  world,
  selectedEvent,
  highlightedEntityIds,
  onSelectEntity,
}: DemoParserStageProps) {
  const parserState = world.components["parser:q1"]?.ParserStateComponent?.data
  const focusedEntityId = world.focusedEntityId
  const tokens = getTokens(world)
  const focusedToken = tokens.find((token) => token.entity.id === focusedEntityId)
  const astRootId = world.entities["ast:q1"] ? "ast:q1" : null
  const isTokenEmit = selectedEvent?.event_name === "mysql.parser.token.emit"

  return (
    <Card className="rounded-lg py-3">
      <CardHeader className="gap-1 px-3">
        <div className="flex items-center justify-between gap-2">
          <CardTitle className="text-[13px]">Visual Stage</CardTitle>
          <Badge variant="secondary" className="rounded-md text-[11px]">
            event → effect → world
          </Badge>
        </div>
        <CardDescription className="text-[12px]">
          SQL 像输入流进入 Parser，Parser 吐出 token，再组装成 AST。
        </CardDescription>
      </CardHeader>
      <CardContent className="grid gap-2 px-3">
        <StageBlock title="Raw SQL">
          <SqlStream sql={sql} focusedToken={focusedToken} />
        </StageBlock>

        <FlowArrow label="输入 SQL 文本" />

        <ParserMachine
          focusedEntityId={focusedEntityId}
          isTokenEmit={isTokenEmit}
          onSelectEntity={onSelectEntity}
          parserStage={String(parserState?.stage ?? "idle")}
        />

        <FlowArrow label="输出 token" />

        <StageBlock title="Token Strip">
          <TokenStrip
            focusedEntityId={focusedEntityId}
            highlightedEntityIds={highlightedEntityIds}
            onSelectEntity={onSelectEntity}
            tokens={tokens}
          />
        </StageBlock>

        <FlowArrow label="组装 AST" />

        <StageBlock title="AST Tree">
          {astRootId ? (
            <AstTree
              highlightedEntityIds={highlightedEntityIds}
              onSelectEntity={onSelectEntity}
              parentEdges={world.parentEdges}
              selectedEntityId={focusedEntityId}
              world={world}
              rootId={astRootId}
            />
          ) : (
            <div className="rounded-md border border-dashed px-2 py-3 text-center text-[12px] text-muted-foreground">
              AST 还没出现，继续 Step 或 Replay。
            </div>
          )}
        </StageBlock>
      </CardContent>
    </Card>
  )
}

function StageBlock({ title, children }: { title: string; children: ReactNode }) {
  return (
    <section className="grid gap-1.5 rounded-lg border bg-card/70 p-2">
      <div className="text-[11px] font-medium text-muted-foreground">{title}</div>
      {children}
    </section>
  )
}

function FlowArrow({ label }: { label: string }) {
  return (
    <div className="flex items-center justify-center gap-2 text-[11px] text-muted-foreground">
      <span className="h-px w-12 bg-border" />
      <span>↓ {label}</span>
      <span className="h-px w-12 bg-border" />
    </div>
  )
}

function SqlStream({ sql, focusedToken }: { sql: string; focusedToken?: TokenView }) {
  if (!focusedToken) {
    return (
      <div className="overflow-x-auto rounded-md bg-muted/40 px-2 py-1.5 font-mono text-[12px] leading-5">
        {sql}
      </div>
    )
  }

  const before = sql.slice(0, focusedToken.start)
  const active = sql.slice(focusedToken.start, focusedToken.end)
  const after = sql.slice(focusedToken.end)

  return (
    <div className="overflow-x-auto rounded-md bg-muted/40 px-2 py-1.5 font-mono text-[12px] leading-5">
      <span>{before}</span>
      <span className="rounded bg-primary px-1 text-primary-foreground">{active}</span>
      <span>{after}</span>
    </div>
  )
}

function ParserMachine({
  parserStage,
  focusedEntityId,
  isTokenEmit,
  onSelectEntity,
}: {
  parserStage: string
  focusedEntityId: EntityId | null
  isTokenEmit: boolean
  onSelectEntity: (entityId: EntityId) => void
}) {
  return (
    <button
      className="grid min-h-[92px] gap-2 rounded-lg border bg-muted/30 p-2 text-left transition-colors hover:bg-muted/50"
      onClick={() => onSelectEntity("parser:q1")}
      type="button"
    >
      <div className="flex items-center justify-between gap-2">
        <div className="flex items-center gap-2">
          <div className="flex size-8 items-center justify-center rounded-md border bg-background">
            <Cpu aria-hidden="true" />
          </div>
          <div>
            <div className="text-[13px] font-semibold">Parser 组件</div>
            <div className="text-[11px] text-muted-foreground">把文本变成结构</div>
          </div>
        </div>
        <Badge variant={parserStage === "complete" ? "secondary" : "outline"} className="rounded-md text-[11px]">
          {parserStage}
        </Badge>
      </div>
      <div className="grid gap-1 rounded-md border bg-background px-2 py-1.5 text-[11px]">
        <div className="flex min-w-0 justify-between gap-2">
          <span className="text-muted-foreground">focus</span>
          <span className="min-w-0 overflow-x-auto whitespace-nowrap font-mono">
            {focusedEntityId ?? "none"}
          </span>
        </div>
        <div className="flex items-center gap-1.5 text-muted-foreground">
          <DatabaseZap aria-hidden="true" className="size-3.5" />
          <span>{isTokenEmit ? "正在吐出 token" : "等待下一步事件"}</span>
        </div>
      </div>
    </button>
  )
}

function TokenStrip({
  tokens,
  focusedEntityId,
  highlightedEntityIds,
  onSelectEntity,
}: {
  tokens: TokenView[]
  focusedEntityId: EntityId | null
  highlightedEntityIds: Set<EntityId>
  onSelectEntity: (entityId: EntityId) => void
}) {
  if (tokens.length === 0) {
    return (
      <div className="rounded-md border border-dashed px-2 py-3 text-center text-[12px] text-muted-foreground">
        token 还没吐出来。
      </div>
    )
  }

  return (
    <div className="w-full overflow-x-auto rounded-md border">
      <div className="flex min-w-max gap-1.5 p-2">
        {tokens.map((token) => {
          const active = token.entity.id === focusedEntityId || highlightedEntityIds.has(token.entity.id)

          return (
            <button
              className={cn(
                "grid min-h-[48px] min-w-[76px] gap-1 rounded-md border px-2 py-1 text-left transition-colors",
                active ? "border-primary bg-primary/5" : "bg-background"
              )}
              key={token.entity.id}
              onClick={() => onSelectEntity(token.entity.id)}
              type="button"
            >
              <span className="font-mono text-[12px] font-semibold">{token.text}</span>
              <span className="text-[10px] text-muted-foreground">{token.kind}</span>
            </button>
          )
        })}
      </div>
    </div>
  )
}

function AstTree({
  world,
  parentEdges,
  rootId,
  selectedEntityId,
  highlightedEntityIds,
  onSelectEntity,
}: {
  world: WorldState
  parentEdges: ParentEdge[]
  rootId: EntityId
  selectedEntityId: EntityId | null
  highlightedEntityIds: Set<EntityId>
  onSelectEntity: (entityId: EntityId) => void
}) {
  return (
    <div className="grid gap-1 text-[12px]">
      <AstNode
        depth={0}
        highlightedEntityIds={highlightedEntityIds}
        onSelectEntity={onSelectEntity}
        parentEdges={parentEdges}
        selectedEntityId={selectedEntityId}
        world={world}
        entityId={rootId}
      />
    </div>
  )
}

function AstNode({
  world,
  parentEdges,
  entityId,
  depth,
  selectedEntityId,
  highlightedEntityIds,
  onSelectEntity,
}: {
  world: WorldState
  parentEdges: ParentEdge[]
  entityId: EntityId
  depth: number
  selectedEntityId: EntityId | null
  highlightedEntityIds: Set<EntityId>
  onSelectEntity: (entityId: EntityId) => void
}) {
  const entity = world.entities[entityId]
  const astComponent = world.components[entityId]?.AstNodeComponent?.data
  const children = parentEdges
    .filter((edge) => edge.parent === entityId)
    .map((edge) => edge.child)
    .filter((childId) => Boolean(world.components[childId]?.AstNodeComponent))

  if (!entity) {
    return null
  }

  return (
    <div className="grid gap-1">
      <button
        className={cn(
          "min-h-[34px] rounded-md border bg-background px-2 py-1 text-left transition-colors",
          (selectedEntityId === entityId || highlightedEntityIds.has(entityId)) && "border-primary bg-primary/5"
        )}
        onClick={() => onSelectEntity(entityId)}
        style={{ marginLeft: depth * 14 }}
        type="button"
      >
        <span className="block truncate text-[12px] font-medium">
          {String(astComponent?.node_kind ?? entity.label)}
        </span>
        <span className="block overflow-x-auto whitespace-nowrap font-mono text-[10px] text-muted-foreground">
          {entity.id}
        </span>
      </button>
      {children.map((childId) => (
        <AstNode
          depth={depth + 1}
          highlightedEntityIds={highlightedEntityIds}
          key={childId}
          onSelectEntity={onSelectEntity}
          parentEdges={parentEdges}
          selectedEntityId={selectedEntityId}
          world={world}
          entityId={childId}
        />
      ))}
    </div>
  )
}

function getTokens(world: WorldState): TokenView[] {
  return Object.values(world.entities)
    .map((entity) => {
      const token = world.components[entity.id]?.TokenComponent?.data

      if (!token) {
        return null
      }

      return {
        entity,
        text: String(token.text ?? entity.label),
        kind: String(token.kind ?? "unknown"),
        start: Number(token.start ?? 0),
        end: Number(token.end ?? 0),
      }
    })
    .filter((token): token is TokenView => token !== null)
    .sort((a, b) => a.start - b.start)
}
