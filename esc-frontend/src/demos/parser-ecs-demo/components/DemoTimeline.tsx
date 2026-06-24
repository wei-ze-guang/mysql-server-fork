import { Badge } from "@/components/ui/badge"
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from "@/components/ui/card"
import { ScrollArea } from "@/components/ui/scroll-area"
import { cn } from "@/lib/utils"

import type { DemoEvent } from "../ecs/types"

type DemoTimelineProps = {
  events: DemoEvent[]
  appliedCount: number
  selectedEventId: number | null
  onSelectEvent: (event: DemoEvent) => void
}

export function DemoTimeline({
  events,
  appliedCount,
  selectedEventId,
  onSelectEvent,
}: DemoTimelineProps) {
  return (
    <Card className="rounded-lg py-3">
      <CardHeader className="gap-1 px-3">
        <CardTitle className="text-[13px]">Timeline</CardTitle>
        <CardDescription className="text-[12px]">事件一步步推动 world 变化。</CardDescription>
      </CardHeader>
      <CardContent className="px-3">
        <ScrollArea className="h-[340px] rounded-md border">
          <div className="flex flex-col gap-1.5 p-2">
            {events.map((event, index) => {
              const isApplied = index < appliedCount
              const isSelected = event.event_id === selectedEventId

              return (
                <button
                  className={cn(
                    "flex min-w-0 flex-col gap-1 rounded-md border px-2 py-1.5 text-left text-[12px] transition-colors",
                    isApplied ? "bg-card" : "bg-muted/30 text-muted-foreground",
                    isSelected && "border-primary bg-primary/5"
                  )}
                  key={event.event_id}
                  onClick={() => onSelectEvent(event)}
                  type="button"
                >
                  <div className="flex items-center justify-between gap-2">
                    <span className="font-mono text-[11px]">#{event.event_id}</span>
                    <Badge variant={isApplied ? "secondary" : "outline"} className="rounded text-[10px]">
                      tick {event.tick}
                    </Badge>
                  </div>
                  <span className="break-all font-mono text-[11px] leading-4">{event.event_name}</span>
                  <span className="line-clamp-2 text-muted-foreground">{event.meaning}</span>
                </button>
              )
            })}
          </div>
        </ScrollArea>
      </CardContent>
    </Card>
  )
}
