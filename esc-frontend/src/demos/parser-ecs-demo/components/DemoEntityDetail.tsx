import { Card, CardContent, CardDescription, CardHeader, CardTitle } from "@/components/ui/card"
import { ScrollArea } from "@/components/ui/scroll-area"

import type { DemoEvent, EntityId, WorldState } from "../ecs/types"

type DemoEntityDetailProps = {
  world: WorldState
  selectedEntityId: EntityId | null
  selectedEvent: DemoEvent | null
}

export function DemoEntityDetail({
  world,
  selectedEntityId,
  selectedEvent,
}: DemoEntityDetailProps) {
  const entity = selectedEntityId ? world.entities[selectedEntityId] : null
  const components = selectedEntityId ? world.components[selectedEntityId] : null

  return (
    <div className="grid gap-2">
      <Card className="rounded-lg py-3">
        <CardHeader className="gap-1 px-3">
          <CardTitle className="text-[13px]">Entity Detail</CardTitle>
          <CardDescription className="text-[12px]">
            点击实体后查看它身上的 components。
          </CardDescription>
        </CardHeader>
        <CardContent className="px-3">
          <ScrollArea className="h-[210px] rounded-md border">
            <pre className="p-2 text-[11px] leading-5">
              {entity
                ? JSON.stringify({ entity, components: components ?? {} }, null, 2)
                : "请选择一个 entity"}
            </pre>
          </ScrollArea>
        </CardContent>
      </Card>

      <Card className="rounded-lg py-3">
        <CardHeader className="gap-1 px-3">
          <CardTitle className="text-[13px]">Event Detail</CardTitle>
          <CardDescription className="text-[12px]">
            点击 timeline event 后查看原因和 effects。
          </CardDescription>
        </CardHeader>
        <CardContent className="px-3">
          <ScrollArea className="h-[260px] rounded-md border">
            <pre className="p-2 text-[11px] leading-5">
              {selectedEvent ? JSON.stringify(selectedEvent, null, 2) : "请选择一个 event"}
            </pre>
          </ScrollArea>
        </CardContent>
      </Card>
    </div>
  )
}
