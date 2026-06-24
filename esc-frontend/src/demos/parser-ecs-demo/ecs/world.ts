import type { WorldState } from "./types"

export function createEmptyWorld(): WorldState {
  return {
    entities: {},
    components: {},
    parentEdges: [],
    relationEdges: [],
    timelineMarkers: [],
    focusedEntityId: null,
  }
}
