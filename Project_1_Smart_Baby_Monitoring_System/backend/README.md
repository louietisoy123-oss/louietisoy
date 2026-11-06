# P5 Backend — Smart Baby Monitoring System (BCA182 Project 1)

Single-file FastAPI service: ingests the board's MQTT telemetry into SQLite and
serves the REST API consumed by the P6 Android app. No Docker required —
stdlib + `fastapi` + `uvicorn` + `paho-mqtt` on Python 3.12.

## Run

```bash
pip install -r requirements.txt
python -m uvicorn app:app --host 0.0.0.0 --port 8000
```

(Or `python app.py` — same bind.) The server binds `0.0.0.0:8000`, so phones
and other machines on the same LAN reach it at `http://<PC-IP>:8000`. CORS is
open (allow all origins/methods/headers).

The broker defaults to `broker.hivemq.com:1883` — the board's currently
provisioned public broker — so this works today with zero config. Override via
environment variables (no code change):

```bash
# Git Bash
MQTT_HOST=localhost MQTT_PORT=1883 python -m uvicorn app:app --host 0.0.0.0 --port 8000
```

```bat
:: cmd.exe
set MQTT_HOST=localhost
set MQTT_PORT=1883
python -m uvicorn app:app --host 0.0.0.0 --port 8000
```

`BABYMONITOR_DB` overrides the SQLite path if needed (default: `babymonitor.db`
in this directory, WAL mode, git-ignored).

## Wire format (FROZEN — see `../src/mqtt_task.c`; firmware stays untouched)

| Topic | Direction | Payload |
|---|---|---|
| `babymonitor/reading` | board publishes, every 5 s, QoS 0 | `{"t":<temp C 1dp>,"h":<%RH 1dp>,"lux":<u16>,"px":<u16>,"mg":<int32>,"ts":<uptime s>}` |
| `babymonitor/event` | board publishes, edge-triggered, QoS 0 | `{"type":"cry\|env\|fault\|allclear","on":true\|false,"conf":<0..100 cry only>,"ts":<uptime s>}` |
| `babymonitor/cmd` | board subscribes, QoS 1 | `{"cmd":"ack"}` \| `{"cmd":"mute","sec":<0..3600>}` |

The backend subscribes both telemetry topics at QoS 1 and stores every message
with `recv_ms` (server UTC epoch ms at receive) alongside `dev_ts` (device
uptime seconds from `ts`).

## Storage

SQLite `babymonitor.db` in this directory:

- `readings(id INTEGER PK, recv_ms INTEGER, t REAL, h REAL, lux INTEGER, px INTEGER, mg INTEGER, dev_ts INTEGER)`
- `events(id INTEGER PK, recv_ms INTEGER, type TEXT, on_flag INTEGER, conf INTEGER, dev_ts INTEGER)`
  — `on_flag` is 0/1; `conf` is NULL for non-cry events (only cry carries it).
- Index on `recv_ms` in both tables.

## Endpoints

| Method | Path | Description |
|---|---|---|
| GET | `/api/latest` | `{"server_ms","online","reading","event"}` — newest reading and event (null until first data); `online` is true when the newest reading is < 15 s old. |
| GET | `/api/history?minutes=N` | Array of readings newest-first. Default 60 min, clamped 1..1440, max 2000 rows. |
| GET | `/api/events?limit=N` | Array of events newest-first. Default 50, clamped 1..1000. |
| POST | `/api/ack` | Publishes `{"cmd":"ack"}` to `babymonitor/cmd` → `{"ok":true}`. |
| POST | `/api/mute` | Body `{"sec":N}` (required), clamped 0..3600. Publishes `{"cmd":"mute","sec":N}` → `{"ok":true,"sec":N}`. |
| GET | `/api/health` | `{"mqtt_connected","broker","readings","events"}` — broker link flag + total row counts. |

`/api/ack` and `/api/mute` return **503** when the broker link is down. A
command publish is only reported `ok` after the broker's QoS 1 PUBACK
(waits up to 5 s). Command payloads use compact JSON (`{"cmd":"ack"}`, no
whitespace) so the firmware's hand-rolled scanner in `../src/mqtt_task.c`
matches them.
Quick smoke: `curl http://localhost:8000/api/health`.

## Switching to a local Mosquitto (later)

1. Install Mosquitto for Windows: https://mosquitto.org/download/
2. Add to `C:\Program Files\mosquitto\mosquitto.conf`:
   ```
   listener 1883
   allow_anonymous true
   ```
3. Start it: `net start mosquitto` (or run
   `"C:\Program Files\mosquitto\mosquitto.exe" -c "C:\Program Files\mosquitto\mosquitto.conf" -v`).
4. Start this backend with `MQTT_HOST=localhost` (commands above).
5. Re-provision the board to the PC's LAN IP, port 1883, via its Wi-Fi/broker
   portal — the portal-saved `wifi_storage` v2 broker record wins over
   `secrets.h` at (re)connect time (see `../src/mqtt_task.c`).

## Sanity check (offline, no broker needed)

```bash
python sanity_check.py
```

Hits the API via FastAPI `TestClient` against a throwaway temp DB with a
synthetic dataset: health counts, history window clamping/pagination, latest
online heuristic, events limit, mute input validation. Needs `httpx`
(`pip install httpx`) for `TestClient`.

## Live verification (2026-11-07)

Against the live board on `broker.hivemq.com:1883`: `/api/latest`,
`/api/history?minutes=5` and `/api/health` returned real sensor rows; the JSON
from `POST /api/ack` and `POST /api/mute {"sec":60}` was confirmed on
`babymonitor/cmd` with an independent paho subscriber.
