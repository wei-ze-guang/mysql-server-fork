import { Card, CardContent, CardDescription, CardHeader, CardTitle } from "@/components/ui/card"
import { ScrollArea } from "@/components/ui/scroll-area"
import { cn } from "@/lib/utils"

import type { EntityId, WorldState } from "../ecs/types"

type DemoRelationGraphProps = {
  world: WorldState
  highlightedEntityIds: Set<EntityId>
}

export function DemoRelationGraph({ world, highlightedEntityIds }: DemoRelationGraphProps) {
  return (
    <Card className="rounded-lg py-3">
      <CardHeader className="gap-1 px-3">
        <CardTitle className="text-[13px]">Relation Graph</CardTitle>
        <CardDescription className="text-[12px]">这里展示 token、parser、AST 之间的临时关系。</CardDescription>
      </CardHeader>
      <CardContent className="px-3">
        <ScrollArea className="h-[260px] rounded-md border">
          <div className="flex flex-col gap-1.5 p-2 text-[12px]">
            {world.relationEdges.length > 0 ? (
              world.relationEdges.map((edge) => {
                const active = highlightedEntityIds.has(edge.from) || highlightedEntityIds.has(edge.to)

                return (
                  <div
                    className={cn(
                      "grid gap-1 rounded-md border px-2 py-1.5",
                      active && "border-primary/50 bg-primary/5"
                    )}
                    key={edge.id}
                  >
                    <span className="font-mono text-[11px]">{edge.label}</span>
                    <div className="grid gap-1 text-[11px] text-muted-foreground">
                      <span className="overflow-x-auto whitespace-nowrap">{edge.from}</span>
                      <span>↓</span>
                      <span className="overflow-x-auto whitespace-nowrap">{edge.to}</span>
                    </div>
                  </div>
                )
              })
            ) : (
              <span className="text-muted-foreground">还没有 relation edge。</span>
            )}
          </div>
        </ScrollArea>
      </CardContent>
    </Card>
  )
}
