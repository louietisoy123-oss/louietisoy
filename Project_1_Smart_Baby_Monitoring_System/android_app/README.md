# P6: Smart Baby Monitor - Android Companion App

Kotlin companion app for the Project 1 Smart Baby Monitoring System. It polls
the **P5 PC backend** (the REST bridge over the frozen MQTT contract in
`src/mqtt_task.c`) and presents the monitor state on a dark, LCD-style
dashboard matching the LAB_3 companion-app look (Material Components, same
palette, package convention `com.msuiit.bca180.*`).

## Features

- **Dashboard** - big state banner (NORMAL green / CRY DETECTED and
  ENVIRONMENT ALERT red / SENSOR FAULT amber), five sensor cards
  (Temperature, Humidity, Light, Proximity, Accel), an online/offline dot,
  and a mute countdown chip while a mute window is active.
- **Polling** - `GET /api/latest` every 5 s via OkHttp, foregrounded only
  (starts in `onStart()`, stops in `onStop()`; a request already in flight
  when backgrounding is allowed to finish).
- **Notifications** - a high-priority "Baby cry detected" notification is
  posted for every *new* cry rising edge (`type=cry`, `on=true`) the poller
  observes, foreground or background. Stated policy: banner + notification
  redundancy is deliberate for a baby monitor; duplicates are suppressed by
  an event-key check. Because polling is foreground-gated, a cry that occurs
  while the app is fully backgrounded is caught up (and notified) by the
  first foreground poll.
- **Events screen** - last 30 events (`GET /api/events?limit=30`) with
  relative times ("2 min ago") and a color/symbol badge per type
  (cry/env/fault/allclear), plus a one-line summary of the last 60 min of
  readings (`GET /api/history?minutes=60`).
- **Controls** - **Acknowledge** (`POST /api/ack`) and a **Mute** dropdown of
  30 min / 1 h / 2 h / 4 h (`POST /api/mute {"sec":N}`); both toast the
  result. The mute countdown mirrors the requested window client-side (the
  backend contract exposes no mute field); a successful ack mirrors the
  firmware's 60 s post-ack mute window.
- **Settings** - editable backend base URL (persisted in SharedPreferences),
  a **Test connection** button (`GET /api/health`), and a hint on exercising
  the cry alert path.

## Build

Requirements: Android SDK (the project reads `sdk.dir` from a git-ignored
`local.properties`; on the dev machine this is `D:\Android_SDK`), JDK 17+.

```bash
cd android_app
./gradlew assembleDebug          # APK: app/build/outputs/apk/debug/
./gradlew testDebugUnitTest      # JVM unit tests (JSON parsers + logic)
```

Toolchain matches the LAB_3 app: Gradle 8.9, AGP 8.5.2, Kotlin 1.9.24,
compileSdk/targetSdk 34, minSdk 24.

## Install & configure

1. Install `app-debug.apk` on the phone (sideload; "install unknown apps").
2. Connect the phone to the **same LAN** as the PC running the P5 backend.
3. Open **Settings** and set the base URL to the PC's address, e.g.
   `http://192.168.1.50:8000` (replace with the actual PC LAN IP -
   `ipconfig` on the PC shows it). The URL is saved automatically; the live
   field wins while editing.
4. Tap **Test connection** - expect `HTTP 200` from `/api/health`.

## Permissions (exactly one runtime permission)

- `INTERNET` (normal permission, granted at install) for the HTTP polling.
- `POST_NOTIFICATIONS` (Android 13+, **the one runtime permission**): cry
  alerts are delivered as high-priority notifications, so they are useless
  if notifications are blocked. The app requests it contextually on first
  launch with a rationale dialog ("allow notifications so alerts are never
  missed"). Everything else is permission-free: no location, no storage, no
  microphone access - audio classification happens on the device, not here.

## Data contract (frozen, `mqtt_task.c` via the P5 REST bridge)

```
GET  /api/latest           {"reading":{"t":29.9,"h":75.4,"lux":257,"px":257,
                            "mg":1002,"dev_ts":167,"recv_ms":...},
                            "event":{...}|null,"online":true}
GET  /api/events?limit=N   {"events":[{"type":"cry","on":true,"conf":86,
                            "dev_ts":..,"recv_ms":..},...]}   (newest first)
GET  /api/history?minutes=N{"readings":[...]}                (newest first)
POST /api/ack              {} -> {"ok":true}
POST /api/mute             {"sec":N} -> {"ok":true}   (1800/3600/7200/14400)
GET  /api/health           connection test
```

Parsing is defensive (`org.json`, null-safe optionals): unknown fields are
ignored, missing fields become Kotlin nulls (displayed as `--`), and the
firmware's `"ts"` spelling of the device timestamp is accepted alongside the
backend's `"dev_ts"`. All parsers are covered by plain JVM unit tests
(`app/src/test/.../ApiResponseParserTest.kt`, JUnit 4 + `org.json:json`,
no Robolectric).
