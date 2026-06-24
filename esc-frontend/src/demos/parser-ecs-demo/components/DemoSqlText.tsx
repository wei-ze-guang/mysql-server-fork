import { Badge } from "@/components/ui/badge"
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from "@/components/ui/card"

import type { DemoEvent, EntityId, WorldState } from "../ecs/types"

type DemoSqlTextProps = {
  sql: string
  world: WorldState
  selectedEvent: DemoEvent | null
}

export function DemoSqlText({ sql, world, selectedEvent }: DemoSqlTextProps) {
  const focusedEntity = world.focusedEntityId
  const focusedToken = focusedEntity?.startsWith("token:")
    ? world.components[focusedEntity]?.TokenComponent?.data
    : null

  return (
    <Card className="rounded-lg py-3">
      <CardHeader className="gap-1 px-3">
        <div className="flex items-center justify-between gap-2">
          <CardTitle className="text-[13px]">SQL 输入</CardTitle>
          <Badge variant="outline" className="rounded-md text-[11px]">
            demo
          </Badge>
        </div>
        <CardDescription className="text-[12px]">
          这是一个小演示，不是正式项目页面。
        </CardDescription>
      </CardHeader>
      <CardContent className="flex flex-col gap-2 px-3">
        <div className="overflow-x-auto rounded-md border bg-muted/40 p-2 font-mono text-[12px] leading-5">
          {sql}
        </div>
        <div className="grid gap-2 text-[12px] text-muted-foreground">
          <CompactInfo label="当前焦点" value={focusedEntity ?? "none"} />
          <CompactInfo label="当前事件" value={selectedEvent?.event_name ?? "none"} />
          {focusedToken ? (
            <CompactInfo
              label="token 文本"
              value={String(focusedToken.text ?? "")}
            />
          ) : null}
        </div>
      </CardContent>
    </Card>
  )
}

function CompactInfo({ label, value }: { label: string; value: EntityId }) {
  return (
    <div className="flex min-w-0 items-center justify-between gap-2">
      <span className="shrink-0 text-muted-foreground">{label}</span>
      <span className="min-w-0 overflow-x-auto whitespace-nowrap rounded border bg-background px-1.5 py-0.5 font-mono text-[11px] text-foreground">
        {value}
      </span>
    </div>
  )
}
