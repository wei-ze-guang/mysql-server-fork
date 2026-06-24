import { useEffect, useMemo, useState } from "react"
import { ArrowLeft, Play, RotateCcw, StepForward } from "lucide-react"

import { Badge } from "@/components/ui/badge"
import { Button } from "@/components/ui/button"
import { Card, CardContent } from "@/components/ui/card"
import { Separator } from "@/components/ui/separator"

import { DemoEntityDetail } from "./components/DemoEntityDetail"
import { DemoEntityList } from "./components/DemoEntityList"
import { DemoParserStage } from "./components/DemoParserStage"
import { DemoRelationGraph } from "./components/DemoRelationGraph"
import { DemoSceneGraph } from "./components/DemoSceneGraph"
import { DemoSqlText } from "./components/DemoSqlText"
import { DemoTimeline } from "./components/DemoTimeline"
import { demoSql, parserSampleEvents } from "./data/parserSampleEvents"
import { replayEvents } from "./ecs/replay"
import type { DemoEvent, EntityId } from "./ecs/types"

export function ParserEcsDemoPage() {
  const [appliedCount, setAppliedCount] = useState(0)
  const [selectedEventId, setSelectedEventId] = useState<number | null>(null)
  const [selectedEntityId, setSelectedEntityId] = useState<EntityId | null>(null)
  const [isReplaying, setIsReplaying] = useState(false)

  const world = useMemo(() => replayEvents(parserSampleEvents, appliedCount), [appliedCount])
  const selectedEvent = parserSampleEvents.find((event) => event.event_id === selectedEventId) ?? null

  const highlightedEntityIds = useMemo(() => {
    const ids = new Set<EntityId>()
    selectedEvent?.effects.forEach((effect) => {
      if (effect.type === "create_entity") ids.add(effect.entity.id)
      if (effect.type === "set_component") ids.add(effect.entityId)
      if (effect.type === "add_parent_edge") {
        ids.add(effect.edge.parent)
        ids.add(effect.edge.child)
      }
      if (effect.type === "add_relation_edge") {
        ids.add(effect.edge.from)
        ids.add(effect.edge.to)
      }
      if (effect.type === "focus_entity") ids.add(effect.entityId)
      if (effect.type === "append_timeline_marker") {
        effect.marker.entityIds.forEach((entityId) => ids.add(entityId))
      }
    })

    if (world.focusedEntityId) {
      ids.add(world.focusedEntityId)
    }

    return ids
  }, [selectedEvent, world.focusedEntityId])

  useEffect(() => {
    if (!isReplaying) {
      return
    }

    if (appliedCount >= parserSampleEvents.length) {
      setIsReplaying(false)
      return
    }

    const timer = window.setTimeout(() => {
      setAppliedCount((count) => count + 1)
      setSelectedEventId(parserSampleEvents[appliedCount]?.event_id ?? null)
    }, 520)

    return () => window.clearTimeout(timer)
  }, [appliedCount, isReplaying])

  function resetDemo() {
    setIsReplaying(false)
    setAppliedCount(0)
    setSelectedEventId(null)
    setSelectedEntityId(null)
  }

  function stepDemo() {
    setIsReplaying(false)
    setAppliedCount((count) => {
      const nextCount = Math.min(count + 1, parserSampleEvents.length)
      setSelectedEventId(parserSampleEvents[nextCount - 1]?.event_id ?? null)
      return nextCount
    })
  }

  function selectEvent(event: DemoEvent) {
    const eventIndex = parserSampleEvents.findIndex((item) => item.event_id === event.event_id)

    setIsReplaying(false)
    setSelectedEventId(event.event_id)
    setAppliedCount(Math.max(appliedCount, eventIndex + 1))
  }

  return (
    <main className="min-h-screen bg-background p-3 text-[13px] text-foreground [font-family:-apple-system,BlinkMacSystemFont,'SF_Pro_Text','SF_Pro_Display','Segoe_UI',sans-serif]">
      <div className="mx-auto flex w-full max-w-[1480px] flex-col gap-3">
        <header className="flex flex-col gap-2 rounded-lg border bg-card px-3 py-2">
          <div className="flex flex-col gap-2 md:flex-row md:items-center md:justify-between">
            <div className="flex min-w-0 items-center gap-2">
              <Button asChild size="sm" variant="ghost">
                <a href="/">
                  <ArrowLeft data-icon="inline-start" aria-hidden="true" />
                  首页
                </a>
              </Button>
              <Separator className="hidden h-5 md:block" orientation="vertical" />
              <div className="min-w-0">
                <div className="flex flex-wrap items-center gap-2">
                  <h1 className="text-[20px] font-semibold tracking-normal">Parser ECS 小演示</h1>
                  <Badge variant="secondary" className="rounded-md text-[11px]">
                    不是正式项目页面
                  </Badge>
                </div>
                <p className="text-[12px] text-muted-foreground">
                  这是一个隔离 demo，用来观察 SQL 文本、token、AST 和事件回放如何组成一个 world。
                </p>
              </div>
            </div>

            <div className="flex flex-wrap gap-2">
              <Button onClick={() => setIsReplaying(true)} size="sm">
                <Play data-icon="inline-start" aria-hidden="true" />
                Replay
              </Button>
              <Button onClick={stepDemo} size="sm" variant="outline">
                <StepForward data-icon="inline-start" aria-hidden="true" />
                Step
              </Button>
              <Button onClick={resetDemo} size="sm" variant="secondary">
                <RotateCcw data-icon="inline-start" aria-hidden="true" />
                Reset
              </Button>
            </div>
          </div>
          <Card className="rounded-md py-2 shadow-none">
            <CardContent className="px-3">
              <div className="overflow-x-auto font-mono text-[12px]">{demoSql}</div>
            </CardContent>
          </Card>
        </header>

        <section className="grid gap-3 xl:grid-cols-[360px_minmax(0,1fr)_380px]">
          <div className="flex min-w-0 flex-col gap-3">
            <DemoSqlText sql={demoSql} world={world} selectedEvent={selectedEvent} />
            <DemoTimeline
              appliedCount={appliedCount}
              events={parserSampleEvents}
              onSelectEvent={selectEvent}
              selectedEventId={selectedEventId}
            />
          </div>

          <div className="grid min-w-0 gap-3">
            <DemoParserStage
              highlightedEntityIds={highlightedEntityIds}
              onSelectEntity={setSelectedEntityId}
              selectedEvent={selectedEvent}
              sql={demoSql}
              world={world}
            />
            <DemoSceneGraph
              highlightedEntityIds={highlightedEntityIds}
              onSelectEntity={setSelectedEntityId}
              selectedEntityId={selectedEntityId}
              world={world}
            />
            <DemoRelationGraph highlightedEntityIds={highlightedEntityIds} world={world} />
          </div>

          <div className="flex min-w-0 flex-col gap-3">
            <DemoEntityList
              highlightedEntityIds={highlightedEntityIds}
              onSelectEntity={setSelectedEntityId}
              selectedEntityId={selectedEntityId}
              world={world}
            />
            <DemoEntityDetail
              selectedEntityId={selectedEntityId}
              selectedEvent={selectedEvent}
              world={world}
            />
          </div>
        </section>
      </div>
    </main>
  )
}
