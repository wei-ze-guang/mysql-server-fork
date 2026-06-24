import { createEmptyWorld } from "./world"
import type { DemoEvent, Effect, WorldState } from "./types"

function upsertById<T extends { id: string }>(items: T[], nextItem: T) {
  const index = items.findIndex((item) => item.id === nextItem.id)

  if (index === -1) {
    return [...items, nextItem]
  }

  return items.map((item) => (item.id === nextItem.id ? nextItem : item))
}

function upsertMarker(
  items: WorldState["timelineMarkers"],
  nextMarker: WorldState["timelineMarkers"][number]
) {
  const index = items.findIndex((item) => item.eventId === nextMarker.eventId)

  if (index === -1) {
    return [...items, nextMarker]
  }

  return items.map((item) => (item.eventId === nextMarker.eventId ? nextMarker : item))
}

export function applyEffect(world: WorldState, effect: Effect): WorldState {
  switch (effect.type) {
    case "create_entity":
      return {
        ...world,
        entities: {
          ...world.entities,
          [effect.entity.id]: effect.entity,
        },
      }
    case "set_component":
      return {
        ...world,
        components: {
          ...world.components,
          [effect.entityId]: {
            ...world.components[effect.entityId],
            [effect.component.type]: effect.component,
          },
        },
      }
    case "add_parent_edge":
      return {
        ...world,
        parentEdges: upsertById(world.parentEdges, effect.edge),
      }
    case "add_relation_edge":
      return {
        ...world,
        relationEdges: upsertById(world.relationEdges, effect.edge),
      }
    case "append_timeline_marker":
      return {
        ...world,
        timelineMarkers: upsertMarker(world.timelineMarkers, effect.marker),
      }
    case "focus_entity":
      return {
        ...world,
        focusedEntityId: effect.entityId,
      }
  }
}

export function applyEvent(world: WorldState, event: DemoEvent): WorldState {
  return event.effects.reduce(applyEffect, world)
}

export function replayEvents(events: DemoEvent[], count: number): WorldState {
  // 先按事件顺序回放，避免后面的状态被旧事件覆盖。
  return events
    .slice(0, count)
    .sort((a, b) => a.tick - b.tick || a.event_id - b.event_id)
    .reduce(applyEvent, createEmptyWorld())
}
