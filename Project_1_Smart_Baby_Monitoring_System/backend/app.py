"""P5 backend for the Smart Baby Monitoring System (BCA182 Project 1).

Single-file FastAPI service that:
  * ingests the board's MQTT telemetry (babymonitor/reading, babymonitor/event)
    into SQLite, tagging every row with a server-side UTC receive timestamp,
  * publishes {"cmd":"ack"} / {"cmd":"mute","sec":N} to babymonitor/cmd,
  * serves the REST API consumed by the P6 Android app.

Frozen wire format (see ../src/mqtt_task.c -- do not change firmware):
  reading: {"t":<temp C 1dp>,"h":<%RH 1dp>,"lux":<u16>,"px":<u16>,"mg":<int32>,"ts":<uptime s>}
  event:   {"type":"cry|env|fault|allclear","on":true|false,"conf":<0..100 cry only>,"ts":<uptime s>}
  cmd:     {"cmd":"ack"} | {"cmd":"mute","sec":<0..3600>}

Run (from this directory):
  python -m uvicorn app:app --host 0.0.0.0 --port 8000

Environment variables:
  MQTT_HOST      broker host  (default broker.hivemq.com)
  MQTT_PORT      broker port  (default 1883)
  BABYMONITOR_DB SQLite path  (default <this dir>/babymonitor.db)
"""

from __future__ import annotations

import atexit
import json
import logging
import os
import random
import sqlite3
import threading
import time
from contextlib import asynccontextmanager
from pathlib import Path
from typing import Any

import paho.mqtt.client as mqtt
from fastapi import FastAPI, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel

# ------------------------------------------------------------------ config

BASE_DIR = Path(__file__).resolve().parent
DB_PATH = Path(os.environ.get("BABYMONITOR_DB", str(BASE_DIR / "babymonitor.db")))

MQTT_HOST = os.environ.get("MQTT_HOST", "broker.hivemq.com")
MQTT_PORT = int(os.environ.get("MQTT_PORT", "1883"))

TOPIC_READING = "babymonitor/reading"
TOPIC_EVENT = "babymonitor/event"
TOPIC_CMD = "babymonitor/cmd"

ONLINE_WINDOW_MS = 15_000   # device-online heuristic: newest reading < 15 s old
HISTORY_MAX_MINUTES = 1440  # /api/history window cap (24 h)
HISTORY_MAX_ROWS = 2000     # /api/history response row cap
EVENTS_DEFAULT_LIMIT = 50
EVENTS_MAX_LIMIT = 1000
MUTE_SEC_MAX = 3600

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)-7s %(name)s: %(message)s",
)
if os.environ.get("MQTT_LOG_DEBUG"):
    logging.getLogger("paho").setLevel(logging.DEBUG)  # packet-level trace
log = logging.getLogger("babymonitor.backend")

# ------------------------------------------------------------------ sqlite
# Schema (frozen for P5): recv_ms = server UTC epoch ms at receive time,
# dev_ts = device uptime seconds from the payload's "ts" field.

_SCHEMA = """
CREATE TABLE IF NOT EXISTS readings (
    id      INTEGER PRIMARY KEY,
    recv_ms INTEGER NOT NULL,
    t       REAL,
    h       REAL,
    lux     INTEGER,
    px      INTEGER,
    mg      INTEGER,
    dev_ts  INTEGER
);
CREATE INDEX IF NOT EXISTS idx_readings_recv_ms ON readings(recv_ms);
CREATE TABLE IF NOT EXISTS events (
    id      INTEGER PRIMARY KEY,
    recv_ms INTEGER NOT NULL,
    type    TEXT,
    on_flag INTEGER,
    conf    INTEGER,
    dev_ts  INTEGER
);
CREATE INDEX IF NOT EXISTS idx_events_recv_ms ON events(recv_ms);
"""


def _db() -> sqlite3.Connection:
    """Short-lived per-call connection (WAL + busy timeout => thread-safe)."""
    conn = sqlite3.connect(DB_PATH, timeout=10.0)
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA busy_timeout=10000")
    conn.row_factory = sqlite3.Row
    return conn


def init_db() -> None:
    DB_PATH.parent.mkdir(parents=True, exist_ok=True)
    with _db() as conn:
        conn.executescript(_SCHEMA)


# Defensive numeric coercion: a malformed/missing field stores NULL instead
# of crashing the ingestion thread. The wire format is frozen, so in practice
# every field is present.

def _as_int(v: Any) -> int | None:
    if isinstance(v, bool):
        return int(v)
    if isinstance(v, int):
        return v
    if isinstance(v, float):
        return int(v)
    return None


def _as_float(v: Any) -> float | None:
    if isinstance(v, bool):
        return None
    if isinstance(v, (int, float)):
        return float(v)
    return None


def _as_str(v: Any) -> str | None:
    return v if isinstance(v, str) else None


def _store_reading(recv_ms: int, p: dict) -> None:
    with _db() as conn:
        conn.execute(
            "INSERT INTO readings (recv_ms, t, h, lux, px, mg, dev_ts) "
            "VALUES (?, ?, ?, ?, ?, ?, ?)",
            (
                recv_ms,
                _as_float(p.get("t")),
                _as_float(p.get("h")),
                _as_int(p.get("lux")),
                _as_int(p.get("px")),
                _as_int(p.get("mg")),
                _as_int(p.get("ts")),
            ),
        )


def _store_event(recv_ms: int, p: dict) -> None:
    with _db() as conn:
        conn.execute(
            "INSERT INTO events (recv_ms, type, on_flag, conf, dev_ts) "
            "VALUES (?, ?, ?, ?, ?)",
            (
                recv_ms,
                _as_str(p.get("type")),
                _as_int(p.get("on")),
                _as_int(p.get("conf")),  # cry only; NULL otherwise
                _as_int(p.get("ts")),
            ),
        )

# -------------------------------------------------------------------- mqtt

_mqtt_connected = threading.Event()
_client: mqtt.Client | None = None


def _on_connect(client, userdata, flags, reason_code, properties) -> None:
    if getattr(reason_code, "is_failure", False):
        log.error("MQTT connect refused: %s", reason_code)
        return
    _mqtt_connected.set()
    client.subscribe([(TOPIC_READING, 1), (TOPIC_EVENT, 1)])
    log.info(
        "MQTT connected to %s:%d; subscribed reading+event at QoS 1",
        MQTT_HOST,
        MQTT_PORT,
    )


def _on_disconnect(client, userdata, *args) -> None:
    _mqtt_connected.clear()
    log.warning("MQTT disconnected; auto-reconnect in background")


def _on_message(client, userdata, msg: mqtt.MQTTMessage) -> None:
    recv_ms = int(time.time() * 1000)
    try:
        payload = json.loads(msg.payload.decode("utf-8", "replace"))
        if not isinstance(payload, dict):
            raise ValueError("payload is not a JSON object")
    except (ValueError, UnicodeDecodeError) as exc:
        log.warning("Dropped malformed payload on %s: %s", msg.topic, exc)
        return

    try:
        if msg.topic == TOPIC_READING:
            _store_reading(recv_ms, payload)
            log.info("reading recv_ms=%d %s", recv_ms, payload)
        elif msg.topic == TOPIC_EVENT:
            _store_event(recv_ms, payload)
            log.info("event   recv_ms=%d %s", recv_ms, payload)
        else:
            log.debug("ignored topic %s", msg.topic)
    except sqlite3.Error as exc:
        log.error("DB insert failed for %s: %s", msg.topic, exc)


def start_mqtt() -> None:
    """Start the background paho network loop (idempotent).

    connect_async + loop_start => the loop connects and keeps retrying with
    backoff until the broker is reachable, and auto-reconnects after any
    drop. Subscriptions are re-established in on_connect on every reconnect.
    """
    global _client
    if _client is not None:
        return
    client_id = "babymonitor-backend-%04x" % random.randint(0, 0xFFFF)
    client = mqtt.Client(
        mqtt.CallbackAPIVersion.VERSION2,
        client_id=client_id,
        clean_session=True,
        protocol=mqtt.MQTTv311,
    )
    client.on_connect = _on_connect
    client.on_disconnect = _on_disconnect
    client.on_message = _on_message
    client.reconnect_delay_set(min_delay=1, max_delay=30)
    _client = client
    client.connect_async(MQTT_HOST, MQTT_PORT, keepalive=30)
    client.loop_start()
    log.info("MQTT client starting: %s:%d (id=%s)", MQTT_HOST, MQTT_PORT, client_id)


def publish_cmd(payload: dict) -> bool:
    """Publish a command JSON to babymonitor/cmd at QoS 1 and wait (up to
    5 s) for the broker PUBACK.

    Compact separators: the firmware cmd scanner matches the literal
    '"cmd":"ack"' / '"cmd":"mute"' via strstr (see ../src/mqtt_task.c), so
    the payload must carry no whitespace after ':' or ','.

    Returns True only when the broker acknowledged the publish (API maps
    False to 503).
    """
    if _client is None or not _mqtt_connected.is_set():
        log.warning("cmd %s dropped: broker not connected", payload)
        return False
    wire = json.dumps(payload, separators=(",", ":"))
    info = _client.publish(TOPIC_CMD, wire, qos=1)
    try:
        info.wait_for_publish(timeout=5)
    except (RuntimeError, ValueError) as exc:
        log.error("cmd %s failed: %s", wire, exc)
        return False
    published = info.is_published()
    log.info("cmd %s mid=%s rc=%s pubacked=%s", wire, info.mid, info.rc, published)
    return published


atexit.register(lambda: _client.loop_stop() if _client is not None else None)

# --------------------------------------------------------------------- api


class MuteRequest(BaseModel):
    sec: int


@asynccontextmanager
async def lifespan(_: FastAPI):
    init_db()
    start_mqtt()
    yield


app = FastAPI(
    title="Smart Baby Monitor Backend (P5)",
    version="1.0.0",
    lifespan=lifespan,  # runs init_db() + start_mqtt() on startup
)

# The Android app / browser tests hit this from the LAN: allow all origins.
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)


def _row_dict(row: sqlite3.Row | None) -> dict | None:
    return None if row is None else {k: row[k] for k in row.keys()}


@app.get("/")
def root() -> dict:
    return {
        "service": "smart-baby-monitor-backend",
        "phase": "P5",
        "endpoints": [
            "GET /api/latest",
            "GET /api/history?minutes=N",
            "GET /api/events?limit=N",
            "POST /api/ack",
            "POST /api/mute {\"sec\":N}",
            "GET /api/health",
        ],
    }


@app.get("/api/latest")
def api_latest() -> dict:
    """Newest reading + newest event + device-online heuristic.

    online = newest stored reading arrived within the last 15 s
    (the board publishes every 5 s).
    """
    now_ms = int(time.time() * 1000)
    with _db() as conn:
        reading = conn.execute(
            "SELECT * FROM readings ORDER BY recv_ms DESC, id DESC LIMIT 1"
        ).fetchone()
        event = conn.execute(
            "SELECT * FROM events ORDER BY recv_ms DESC, id DESC LIMIT 1"
        ).fetchone()
    online = reading is not None and (now_ms - reading["recv_ms"]) <= ONLINE_WINDOW_MS
    return {
        "server_ms": now_ms,
        "online": bool(online),
        "reading": _row_dict(reading),
        "event": _row_dict(event),
    }


@app.get("/api/history")
def api_history(minutes: int = 60) -> list[dict]:
    """Readings newest-first from the last `minutes` minutes.

    Default 60, window clamped to 1..1440; response capped at 2000 rows.
    """
    minutes = max(1, min(minutes, HISTORY_MAX_MINUTES))
    since_ms = int(time.time() * 1000) - minutes * 60_000
    with _db() as conn:
        rows = conn.execute(
            "SELECT * FROM readings WHERE recv_ms >= ? "
            "ORDER BY recv_ms DESC, id DESC LIMIT ?",
            (since_ms, HISTORY_MAX_ROWS),
        ).fetchall()
    return [_row_dict(r) for r in rows]


@app.get("/api/events")
def api_events(limit: int = EVENTS_DEFAULT_LIMIT) -> list[dict]:
    """Events newest-first. Default limit 50, clamped to 1..1000."""
    limit = max(1, min(limit, EVENTS_MAX_LIMIT))
    with _db() as conn:
        rows = conn.execute(
            "SELECT * FROM events ORDER BY recv_ms DESC, id DESC LIMIT ?",
            (limit,),
        ).fetchall()
    return [_row_dict(r) for r in rows]


@app.post("/api/ack")
def api_ack() -> dict:
    """Publish {"cmd":"ack"} to babymonitor/cmd (board refreshes its mute
    window exactly like the physical KEY0 path)."""
    if not publish_cmd({"cmd": "ack"}):
        raise HTTPException(status_code=503, detail="MQTT broker not connected")
    return {"ok": True}


@app.post("/api/mute")
def api_mute(req: MuteRequest) -> dict:
    """Publish {"cmd":"mute","sec":N} to babymonitor/cmd. sec clamped 0..3600
    (0 requests unmute, matching the firmware clamp)."""
    sec = max(0, min(req.sec, MUTE_SEC_MAX))
    if not publish_cmd({"cmd": "mute", "sec": sec}):
        raise HTTPException(status_code=503, detail="MQTT broker not connected")
    return {"ok": True, "sec": sec}


@app.get("/api/health")
def api_health() -> dict:
    """Broker link flag + total stored row counts."""
    with _db() as conn:
        readings = conn.execute("SELECT COUNT(*) AS n FROM readings").fetchone()["n"]
        events = conn.execute("SELECT COUNT(*) AS n FROM events").fetchone()["n"]
    return {
        "mqtt_connected": _mqtt_connected.is_set(),
        "broker": f"{MQTT_HOST}:{MQTT_PORT}",
        "readings": readings,
        "events": events,
    }


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(app, host="0.0.0.0", port=8000)
