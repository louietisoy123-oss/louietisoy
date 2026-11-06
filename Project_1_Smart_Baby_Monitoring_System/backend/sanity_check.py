"""Offline sanity check for the P5 backend API (no broker, no real network).

Runs the FastAPI app under TestClient against a throwaway temp SQLite DB
seeded with synthetic rows, then asserts:

  * /api/health row counts
  * /api/history default window, minute clamping, newest-first, row cap
  * /api/latest online heuristic (fresh vs stale data)
  * /api/events limit clamping and newest-first ordering
  * /api/mute input validation (422 on missing field)

Usage:  python sanity_check.py      (needs: fastapi, httpx)

Note: this imports app.py with MQTT_HOST pointed at an unroutable link-local
address, so the background MQTT loop just retries silently and never touches
a real broker. All HTTP calls are in-process TestClient.
"""

from __future__ import annotations

import os
import sqlite3
import sys
import tempfile
import time
from pathlib import Path

os.environ["MQTT_HOST"] = "192.0.2.255"  # TEST-NET-1, unroutable: no real broker
os.environ["MQTT_PORT"] = "1"

_tmpdir = tempfile.TemporaryDirectory(prefix="babymonitor-sanity-")
os.environ["BABYMONITOR_DB"] = str(Path(_tmpdir.name) / "sanity.db")

sys.path.insert(0, str(Path(__file__).resolve().parent))
import app  # noqa: E402  (must import AFTER the env vars above)

from fastapi.testclient import TestClient  # noqa: E402

NOW_MS = int(time.time() * 1000)
FAILURES: list[str] = []


def check(name: str, cond: bool, detail: str = "") -> None:
    status = "PASS" if cond else "FAIL"
    print(f"[{status}] {name}" + (f" -- {detail}" if detail and not cond else ""))
    if not cond:
        FAILURES.append(name)


def seed() -> None:
    """Synthetic dataset: 20 readings 1 min apart (newest ~5 s old) + events."""
    conn = sqlite3.connect(os.environ["BABYMONITOR_DB"])
    for i in range(20):
        recv_ms = NOW_MS - (5_000 + (19 - i) * 60_000)
        conn.execute(
            "INSERT INTO readings (recv_ms, t, h, lux, px, mg, dev_ts) "
            "VALUES (?, ?, ?, ?, ?, ?, ?)",
            (recv_ms, 25.0 + i * 0.1, 65.0 + i * 0.2, 100 + i, 50 + i, 1000 + i, 100 + i),
        )
    for i in range(5):
        conn.execute(
            "INSERT INTO events (recv_ms, type, on_flag, conf, dev_ts) "
            "VALUES (?, ?, ?, ?, ?)",
            (NOW_MS - 60_000 * (i + 1), "cry", 1, 80 - i, 200 + i),
        )
    conn.commit()
    conn.close()


def main() -> int:
    app.init_db()
    seed()

    with TestClient(app.app) as client:
        # --- /api/health -------------------------------------------------
        r = client.get("/api/health")
        check("health status 200", r.status_code == 200)
        h = r.json()
        check("health counts", h["readings"] == 20 and h["events"] == 5, str(h))
        check("health broker fields", h["broker"] == "192.0.2.255:1" and h["mqtt_connected"] is False)

        # --- /api/history ------------------------------------------------
        r = client.get("/api/history")  # default 60 min -> all 20 synthetic rows
        rows = r.json()
        check("history default = 20 rows", isinstance(rows, list) and len(rows) == 20, str(len(rows)))
        check("history newest-first", rows[0]["recv_ms"] >= rows[-1]["recv_ms"])
        check("history newest age ~5 s", 0 <= NOW_MS - rows[0]["recv_ms"] < 10_000)
        check("history field set", set(rows[0]) == {"id", "recv_ms", "t", "h", "lux", "px", "mg", "dev_ts"})

        r = client.get("/api/history?minutes=2")  # window 120 s -> 2 rows (5 s + 65 s old)
        check("history minutes=2 -> 2 rows", len(r.json()) == 2, str(len(r.json())))

        r = client.get("/api/history?minutes=999999")  # clamp to 1440
        check("history window clamped to 1440 min", len(r.json()) == 20)

        # Row cap: insert 30 extra stale-free rows, still capped at 2000.
        conn = sqlite3.connect(os.environ["BABYMONITOR_DB"])
        conn.executemany(
            "INSERT INTO readings (recv_ms, t, h, lux, px, mg, dev_ts) VALUES (?,?,?,?,?,?,?)",
            [(NOW_MS - 1000 - i, 26.0, 70.0, 10, 10, 1000, i) for i in range(2100)],
        )
        conn.commit()
        conn.close()
        r = client.get("/api/history?minutes=1440")
        check("history capped at 2000 rows", len(r.json()) == 2000, str(len(r.json())))

        # --- /api/latest -------------------------------------------------
        r = client.get("/api/latest")
        l = r.json()
        check("latest online=True (1 s old)", l["online"] is True and l["reading"] is not None)
        check("latest newest event = cry", l["event"]["type"] == "cry")

        conn = sqlite3.connect(os.environ["BABYMONITOR_DB"])
        conn.execute("DELETE FROM readings")  # make data stale
        conn.commit()
        conn.close()
        r = client.get("/api/latest")
        check("latest online=False (stale)", r.json()["online"] is False)
        check("latest reading null after delete", r.json()["reading"] is None)

        # --- /api/events -------------------------------------------------
        r = client.get("/api/events?limit=2")
        ev = r.json()
        check("events limit=2", len(ev) == 2)
        check("events newest-first", ev[0]["recv_ms"] >= ev[1]["recv_ms"])
        r = client.get("/api/events?limit=0")  # clamp up to 1
        check("events limit clamped to >=1", len(r.json()) == 1)

        # --- /api/mute validation (offline -> 503 after validation) ------
        r = client.post("/api/mute")  # missing body
        check("mute missing body -> 422", r.status_code == 422)
        r = client.post("/api/mute", json={"sec": "abc"})
        check("mute non-int -> 422", r.status_code == 422)
        r = client.post("/api/mute", json={"sec": 60})  # valid, but broker down
        check("mute broker down -> 503", r.status_code == 503)
        r = client.post("/api/ack")
        check("ack broker down -> 503", r.status_code == 503)

    _tmpdir.cleanup()
    print()
    if FAILURES:
        print(f"SANITY CHECK FAILED: {len(FAILURES)} failure(s): {FAILURES}")
        return 1
    print("SANITY CHECK PASSED: all assertions green")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
