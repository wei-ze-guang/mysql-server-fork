export type EntityId = string
export type ComponentType = string

export type Entity = {
  id: EntityId
  type: string
  label: string
}

export type Component = {
  type: ComponentType
  data: Record<string, unknown>
}

export type ParentEdge = {
  id: string
  parent: EntityId
  child: EntityId
  label: string
}

export type RelationEdge = {
  id: string
  from: EntityId
  to: EntityId
  label: string
}

export type TimelineMarker = {
  eventId: number
  tick: number
  label: string
  entityIds: EntityId[]
}

export type RuntimeValue = {
  name: string
  value: string | number | boolean
  meaning: string
}

export type DemoEvent = {
  event_id: number
  tick: number
  namespace: string
  event_name: string
  phase: "start" | "instant" | "end"
  actor: string
  action: string
  object: string
  result: string
  meaning: string
  runtime_values: RuntimeValue[]
  effects: Effect[]
}

export type Effect =
  | {
      type: "create_entity"
      entity: Entity
    }
  | {
      type: "set_component"
      entityId: EntityId
      component: Component
    }
  | {
      type: "add_parent_edge"
      edge: ParentEdge
    }
  | {
      type: "add_relation_edge"
      edge: RelationEdge
    }
  | {
      type: "append_timeline_marker"
      marker: TimelineMarker
    }
  | {
      type: "focus_entity"
      entityId: EntityId
    }

export type WorldState = {
  entities: Record<EntityId, Entity>
  components: Record<EntityId, Record<ComponentType, Component>>
  parentEdges: ParentEdge[]
  relationEdges: RelationEdge[]
  timelineMarkers: TimelineMarker[]
  focusedEntityId: EntityId | null
}
