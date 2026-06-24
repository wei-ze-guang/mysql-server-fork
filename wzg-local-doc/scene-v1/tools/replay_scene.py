#!/usr/bin/env python3
"""Replay wzg.scene.v1 JSONL events into an ECS-style world-state JSON.

This adapter is intentionally thin. It validates basic event shape and applies
effects. MySQL-specific meaning stays in event data, not in this script.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any, Dict, Iterable, List


REQUIRED_EVENT_FIELDS = [
    "schema",
    "event_id",
    "trace_id",
    "span_id",
    "parent_event_id",
    "caused_by_event_id",
    "tick",
    "ts_ns",
    "layer",
    "namespace",
    "event_name",
    "phase",
    "actor",
    "action",
    "object",
    "result",
    "meaning",
    "runtime_values",
    "source",
    "effects",
]


SUPPORTED_EFFECTS = {
    "create_entity",
    "set_component",
    "add_edge",
    "append_timeline_marker",
    "focus_entity",
    "mark_decision",
    "mark_wait",
    "mark_error",
}


def load_jsonl(path: Path) -> Iterable[Dict[str, Any]]:
    with path.open("r", encoding="utf-8") as fh:
        for line_no, line in enumerate(fh, 1):
            stripped = line.strip()
            if not stripped:
                continue
            try:
                event = json.loads(stripped)
            except json.JSONDecodeError as exc:
                raise ValueError(f"{path}:{line_no}: invalid JSON: {exc}") from exc
            validate_event(event, path, line_no)
            yield event


def validate_event(event: Dict[str, Any], path: Path, line_no: int) -> None:
    missing = [field for field in REQUIRED_EVENT_FIELDS if field not in event]
    if missing:
        raise ValueError(f"{path}:{line_no}: missing fields: {', '.join(missing)}")
    if event["schema"] != "wzg.scene.v1":
        raise ValueError(f"{path}:{line_no}: unsupported schema {event['schema']!r}")
    if not isinstance(event["effects"], list):
        raise ValueError(f"{path}:{line_no}: effects must be a list")
    for idx, effect in enumerate(event["effects"]):
        op = effect.get("op")
        if op not in SUPPORTED_EFFECTS:
            raise ValueError(f"{path}:{line_no}: unsupported effect #{idx}: {op!r}")


def empty_world() -> Dict[str, Any]:
    return {
        "schema": "wzg.scene.v1.world",
        "generated_by": "tools/replay_scene.py",
        "events": [],
        "entities": {},
        "edges": {},
        "timeline": [],
        "lanes": {},
        "focus_history": [],
        "markers": [],
        "errors": [],
    }


def ensure_entity(world: Dict[str, Any], entity_id: str, entity_type: str = "Unknown",
                  label: str | None = None) -> Dict[str, Any]:
    entities = world["entities"]
    if entity_id not in entities:
        entities[entity_id] = {
            "entity_id": entity_id,
            "entity_type": entity_type,
            "label": label or entity_id,
            "components": {},
            "created_by_event_id": None,
            "updated_by_event_ids": [],
        }
    entity = entities[entity_id]
    if entity_type != "Unknown":
        entity["entity_type"] = entity_type
    if label:
        entity["label"] = label
    return entity


def apply_effect(world: Dict[str, Any], event: Dict[str, Any], effect: Dict[str, Any]) -> None:
    op = effect["op"]

    if op == "create_entity":
        entity = ensure_entity(
            world,
            effect["entity_id"],
            effect.get("entity_type", "Unknown"),
            effect.get("label"),
        )
        if entity["created_by_event_id"] is None:
            entity["created_by_event_id"] = event["event_id"]
        entity["updated_by_event_ids"].append(event["event_id"])
        return

    if op == "set_component":
        entity = ensure_entity(world, effect["entity_id"])
        entity["components"][effect["component"]] = effect.get("value", {})
        entity["updated_by_event_ids"].append(event["event_id"])
        return

    if op == "add_edge":
        edge_id = effect.get("edge_id") or f"edge:{effect['from']}:{effect['to']}:{effect.get('label', '')}"
        world["edges"][edge_id] = {
            "edge_id": edge_id,
            "edge_type": effect.get("edge_type", "relation"),
            "from": effect["from"],
            "to": effect["to"],
            "label": effect.get("label", ""),
            "created_by_event_id": event["event_id"],
        }
        return

    if op == "append_timeline_marker":
        marker = {
            "event_id": event["event_id"],
            "tick": event["tick"],
            "lane": effect.get("lane", event["namespace"]),
            "label": effect.get("label", event["event_name"]),
            "entity_id": effect.get("entity_id"),
        }
        world["markers"].append(marker)
        return

    if op == "focus_entity":
        world["focus_history"].append({
            "event_id": event["event_id"],
            "tick": event["tick"],
            "entity_id": effect["entity_id"],
        })
        return

    if op in {"mark_decision", "mark_wait", "mark_error"}:
        world["timeline"].append({
            "kind": op,
            "event_id": event["event_id"],
            "tick": event["tick"],
            "entity_id": effect.get("entity_id"),
            "label": effect.get("label", ""),
            "value": effect.get("value", {}),
        })
        if op == "mark_error":
            world["errors"].append({
                "event_id": event["event_id"],
                "entity_id": effect.get("entity_id"),
                "label": effect.get("label", ""),
                "value": effect.get("value", {}),
            })
        return

    raise AssertionError(f"unsupported effect already validated: {op}")


def compact_event(event: Dict[str, Any]) -> Dict[str, Any]:
    return {
        "event_id": event["event_id"],
        "trace_id": event["trace_id"],
        "span_id": event["span_id"],
        "parent_event_id": event["parent_event_id"],
        "caused_by_event_id": event["caused_by_event_id"],
        "tick": event["tick"],
        "ts_ns": event["ts_ns"],
        "layer": event["layer"],
        "namespace": event["namespace"],
        "event_name": event["event_name"],
        "phase": event["phase"],
        "actor": event["actor"],
        "action": event["action"],
        "object": event["object"],
        "result": event["result"],
        "meaning": event["meaning"],
        "runtime_values": event["runtime_values"],
        "source": event["source"],
        "effect_count": len(event["effects"]),
    }


def replay(events: Iterable[Dict[str, Any]]) -> Dict[str, Any]:
    world = empty_world()
    for event in sorted(events, key=lambda item: (item["tick"], item["event_id"])):
        compact = compact_event(event)
        world["events"].append(compact)
        lane = event["namespace"]
        world["lanes"].setdefault(lane, []).append(event["event_id"])
        world["timeline"].append({
            "kind": "event",
            "event_id": event["event_id"],
            "tick": event["tick"],
            "lane": lane,
            "event_name": event["event_name"],
            "meaning": event["meaning"],
        })
        for effect in event["effects"]:
            apply_effect(world, event, effect)

    world["summary"] = {
        "event_count": len(world["events"]),
        "entity_count": len(world["entities"]),
        "edge_count": len(world["edges"]),
        "lane_count": len(world["lanes"]),
        "marker_count": len(world["markers"]),
    }
    return world


def main(argv: List[str]) -> int:
    if len(argv) != 3:
        print("usage: replay_scene.py <input.scene.jsonl> <output.world-state.json>", file=sys.stderr)
        return 2

    input_path = Path(argv[1])
    output_path = Path(argv[2])
    events = list(load_jsonl(input_path))
    world = replay(events)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", encoding="utf-8") as fh:
        json.dump(world, fh, ensure_ascii=False, indent=2)
        fh.write("\n")
    print(
        f"wrote {output_path} "
        f"events={world['summary']['event_count']} "
        f"entities={world['summary']['entity_count']} "
        f"edges={world['summary']['edge_count']} "
        f"lanes={world['summary']['lane_count']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
