import { Badge } from "@/components/ui/badge"
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from "@/components/ui/card"
import { ScrollArea } from "@/components/ui/scroll-area"
import { cn } from "@/lib/utils"

import type { Entity, EntityId, WorldState } from "../ecs/types"

type DemoEntityListProps = {
  world: WorldState
  selectedEntityId: EntityId | null
  highlightedEntityIds: Set<EntityId>
  onSelectEntity: (entityId: EntityId) => void
}

export function DemoEntityList({
  world,
  selectedEntityId,
  highlightedEntityIds,
  onSelectEntity,
}: DemoEntityListProps) {
  const entities = Object.values(world.entities).sort((a, b) => a.id.localeCompare(b.id))

  return (
    <Card className="rounded-lg py-3">
      <CardHeader className="gap-1 px-3">
        <CardTitle className="text-[13px]">Entity List</CardTitle>
        <CardDescription className="text-[12px]">{entities.length} 个实体已进入 world。</CardDescription>
      </CardHeader>
      <CardContent className="px-3">
        <ScrollArea className="h-[210px] rounded-md border">
          <div className="flex flex-col gap-1.5 p-2">
            {entities.map((entity) => (
              <EntityRow
                entity={entity}
                isHighlighted={highlightedEntityIds.has(entity.id)}
                isSelected={entity.id === selectedEntityId}
                key={entity.id}
                onSelectEntity={onSelectEntity}
              />
            ))}
          </div>
        </ScrollArea>
      </CardContent>
    </Card>
  )
}

function EntityRow({
  entity,
  isHighlighted,
  isSelected,
  onSelectEntity,
}: {
  entity: Entity
  isHighlighted: boolean
  isSelected: boolean
  onSelectEntity: (entityId: EntityId) => void
}) {
  return (
    <button
      className={cn(
        "flex min-w-0 items-center justify-between gap-2 rounded-md border px-2 py-1.5 text-left text-[12px]",
        isSelected && "border-primary bg-primary/5",
        !isSelected && isHighlighted && "border-primary/50 bg-primary/5"
      )}
      onClick={() => onSelectEntity(entity.id)}
      type="button"
    >
      <span className="min-w-0">
        <span className="block truncate font-medium">{entity.label}</span>
        <span className="block overflow-x-auto whitespace-nowrap font-mono text-[11px] text-muted-foreground">
          {entity.id}
        </span>
      </span>
      <Badge variant="outline" className="shrink-0 rounded text-[10px]">
        {entity.type}
      </Badge>
    </button>
  )
}
