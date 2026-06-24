# wzg.scene.v1 MVP

This directory is the first minimal closed loop for the MySQL digital-twin viewer.
It does not require MySQL source instrumentation yet.

Flow:

```text
samples/select-secondary-index.scene.jsonl
  -> tools/replay_scene.py
  -> viewer/world-state.json
  -> viewer/index.html
```

The goal is to prove that one event stream can rebuild an ECS-style world state:

- timeline events
- entities
- components
- scene graph parent edges
- relation edges
- entity details
- event details

Open the viewer after generating the world state:

```bash
python3 wzg-local-doc/scene-v1/tools/replay_scene.py \
  wzg-local-doc/scene-v1/samples/select-secondary-index.scene.jsonl \
  wzg-local-doc/scene-v1/viewer/world-state.json

open wzg-local-doc/scene-v1/viewer/index.html
```

This MVP intentionally avoids real MySQL hooks, lock wait, redo/binlog/undo
commit flow, buffer-pool flushing, and deadlock detection. Those should be added
after the schema proves that replay works.
