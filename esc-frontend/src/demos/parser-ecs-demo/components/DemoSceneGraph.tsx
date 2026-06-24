import { Card, CardContent, CardDescription, CardHeader, CardTitle } from "@/components/ui/card"
import { ScrollArea } from "@/components/ui/scroll-area"
import { cn } from "@/lib/utils"

import type { EntityId, ParentEdge, WorldState } from "../ecs/types"

type DemoSceneGraphProps = {
  world: WorldState
  selectedEntityId: EntityId | null
  highlightedEntityIds: Set<EntityId>
  onSelectEntity: (entityId: EntityId) => void
}

export function DemoSceneGraph({
  world,
  selectedEntityId,
  highlightedEntityIds,
  onSelectEntity,
}: DemoSceneGraphProps) {
  const roots = Object.keys(world.entities).filter(
    (entityId) => !world.parentEdges.some((edge) => edge.child === entityId)
  )

  return (
    <Card className="rounded-lg py-3">
      <CardHeader className="gap-1 px-3">
        <CardTitle className="text-[13px]">Scene Graph</CardTitle>
        <CardDescription className="text-[12px]">这里只展示 parent edge，也就是包含关系。</CardDescription>
      </CardHeader>
      <CardContent className="px-3">
        <ScrollArea className="h-[300px] rounded-md border">
          <div className="flex flex-col gap-1 p-2 text-[12px]">
            {roots.length > 0 ? (
              roots.map((rootId) => (
                <GraphNode
                  depth={0}
                  highlightedEntityIds={highlightedEntityIds}
                  key={rootId}
                  onSelectEntity={onSelectEntity}
                  parentEdges={world.parentEdges}
                  selectedEntityId={selectedEntityId}
                  world={world}
                  entityId={rootId}
                />
              ))
            ) : (
              <span className="text-muted-foreground">还没有 parent edge。</span>
            )}
          </div>
        </ScrollArea>
      </CardContent>
    </Card>
  )
}

function GraphNode({
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
  const children = parentEdges.filter((edge) => edge.parent === entityId)

  if (!entity) {
    return null
  }

  return (
    <div className="flex flex-col gap-1">
      <button
        className={cn(
          "min-w-0 rounded-md border px-2 py-1 text-left",
          selectedEntityId === entityId && "border-primary bg-primary/5",
          selectedEntityId !== entityId && highlightedEntityIds.has(entityId) && "border-primary/50 bg-primary/5"
        )}
        onClick={() => onSelectEntity(entityId)}
        style={{ marginLeft: depth * 12 }}
        type="button"
      >
        <span className="block truncate font-medium">{entity.label}</span>
        <span className="block overflow-x-auto whitespace-nowrap font-mono text-[11px] text-muted-foreground">
          {entity.id}
        </span>
      </button>
      {children.map((edge) => (
        <GraphNode
          depth={depth + 1}
          highlightedEntityIds={highlightedEntityIds}
          key={edge.id}
          onSelectEntity={onSelectEntity}
          parentEdges={parentEdges}
          selectedEntityId={selectedEntityId}
          world={world}
          entityId={edge.child}
        />
      ))}
    </div>
  )
}
