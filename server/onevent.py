#!/usr/bin/env python3
"""librespot playback hook: merge each event into the bridge's state file.

Track events carry NAME/ARTISTS/ALBUM; play/pause events carry none, so the
last known track is preserved and only the playing flag moves.
"""
import json
import os
from pathlib import Path

STATE = Path(os.environ.get("BRIDGE_STATE", "/tmp/3ds-spotify-now.json"))

state = {"title": "", "artist": "", "album": "", "playing": False}
try:
    state.update(json.loads(STATE.read_text()))
except Exception:
    pass

event = os.environ.get("PLAYER_EVENT", "")
if os.environ.get("NAME"):
    state["title"] = os.environ["NAME"]
    state["artist"] = os.environ.get("ARTISTS", "")
    state["album"] = os.environ.get("ALBUM", "")
if event in ("playing", "started", "loading"):
    state["playing"] = True
elif event in ("paused", "stopped", "end_of_track"):
    state["playing"] = False
state["event"] = event

tmp = STATE.with_suffix(".tmp")
tmp.write_text(json.dumps(state))
tmp.replace(STATE)
