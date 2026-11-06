package com.msuiit.bca180.babymonitor

/**
 * Data model + pure UI-logic helpers for the Smart Baby Monitor companion app.
 *
 * Everything here is hardware- and Android-free (plain Kotlin) so the JSON
 * contract parsing and the derived-display logic can be unit tested on the
 * JVM (see ApiResponseParserTest). The wire shapes are the P5 backend REST
 * bridge over the frozen MQTT contract in src/mqtt_task.c:
 *
 *   GET /api/latest   -> {"reading":{"t":29.9,"h":75.4,"lux":257,"px":257,
 *                          "mg":1002,"dev_ts":167,"recv_ms":...},
 *                         "event":{...}|null,"online":true}
 *   GET /api/events   -> {"events":[{"type":"cry","on":true,"conf":86,
 *                          "dev_ts":..,"recv_ms":..},...]}
 *   GET /api/history  -> {"readings":[... newest first ...]}
 *   event types: "cry" | "env" | "fault" | "allclear"
 *
 * The firmware also emits "ts" (device uptime seconds) where the backend
 * contract uses "dev_ts"; the parser accepts either (defensive parsing).
 */

/** One sensor reading snapshot (fixed-point device telemetry). */
data class Reading(
    val tempC: Double?,
    val humidityPct: Double?,
    val lux: Int?,
    val proximity: Int?,
    val accelMg: Long?,
    val deviceTsSec: Long?,
    val recvMs: Long?
)

/** One edge-triggered event line from the device (cry/env/fault/allclear). */
data class BabyEvent(
    val type: String,
    val on: Boolean,
    val conf: Int?,
    val deviceTsSec: Long?,
    val recvMs: Long?
)

/** Full GET /api/latest response. */
data class LatestSnapshot(
    val reading: Reading?,
    val event: BabyEvent?,
    val online: Boolean?
)

data class EventsPage(val events: List<BabyEvent>)
data class HistoryPage(val readings: List<Reading>)

/** Big dashboard banner states: NORMAL green / ALERT red / FAULT amber. */
enum class BannerState { NORMAL, ALERT, FAULT }

data class Banner(val state: BannerState, val title: String, val subtitle: String)

object MonitorLogic {

    /**
     * Derive the banner from a /api/latest snapshot. The event field drives
     * the state (edge semantics from mqtt_task.c): an active cry or env edge
     * is ALERT, an active fault edge is FAULT, everything else (including
     * all-clear and cry-off edges, or a null event) is NORMAL. The
     * online/offline indicator is separate and does not mask the banner.
     */
    fun deriveBanner(snapshot: LatestSnapshot): Banner {
        val ev = snapshot.event
        val conf = ev?.conf
        return when {
            ev != null && ev.type == "cry" && ev.on ->
                Banner(BannerState.ALERT, "CRY DETECTED",
                    if (conf != null) "Classifier confidence $conf%" else "Cry classifier triggered")
            ev != null && ev.type == "env" && ev.on ->
                Banner(BannerState.ALERT, "ENVIRONMENT ALERT",
                    "Temperature/humidity limit exceeded")
            ev != null && ev.type == "fault" && ev.on ->
                Banner(BannerState.FAULT, "SENSOR FAULT",
                    "Device reports a fault condition")
            else ->
                Banner(BannerState.NORMAL, "NORMAL",
                    if (snapshot.online == false) "Device offline" else "All readings nominal")
        }
    }

    /**
     * Human age of an event: "just now" / "2 min ago" / "3 h ago" / "4 d ago",
     * based on recv_ms (backend reception epoch ms). Falls back to the device
     * uptime seconds when recv_ms is absent or implausible, and to a dash
     * when neither is usable.
     */
    fun relativeAgeText(recvMs: Long?, nowMs: Long, devTsSec: Long?): String {
        // recv_ms below ~2017 epoch ms is treated as missing/relative junk.
        if (recvMs == null || recvMs < 1_500_000_000_000L) {
            return if (devTsSec != null && devTsSec >= 0) "device uptime ${devTsSec}s" else "-"
        }
        val diff = (nowMs - recvMs).coerceAtLeast(0L)
        val s = diff / 1000L
        return when {
            s < 60L -> "just now"
            s < 3600L -> {
                val m = s / 60L
                if (m == 1L) "1 min ago" else "$m min ago"
            }
            s < 86_400L -> {
                val h = s / 3600L
                if (h == 1L) "1 h ago" else "$h h ago"
            }
            else -> {
                val d = s / 86_400L
                if (d == 1L) "1 d ago" else "$d d ago"
            }
        }
    }

    /** Compact mute-window text: "45s", "29m", "1h 59m", "4h 00m". */
    fun formatMuteRemaining(remainingMs: Long): String {
        if (remainingMs <= 0L) return "0s"
        val s = remainingMs / 1000L
        return when {
            s < 60L -> "${s}s"
            s < 3600L -> "${s / 60L}m"
            else -> {
                val h = s / 3600L
                val m = (s % 3600L) / 60L
                String.format(java.util.Locale.US, "%dh %02dm", h, m)
            }
        }
    }

    /** Deduplication key for one event line (device uptime + backend receipt). */
    fun eventKey(e: BabyEvent): String = "cry:${e.deviceTsSec ?: -1L}:${e.recvMs ?: -1L}"

    /**
     * Notification gate: notify exactly once per NEW cry rising edge
     * (type=cry, on=true) whose key differs from the last notified key.
     */
    fun shouldNotifyCry(e: BabyEvent?, lastNotifiedKey: String?): Boolean =
        e != null && e.type == "cry" && e.on && eventKey(e) != lastNotifiedKey

    /**
     * One-line summary of the reading history page (GET /api/history):
     * "N readings . Temp 21.3-24.9 C (avg 23.1) . RH 41-63%".
     * Returns a dash string when the page is empty.
     */
    fun summarizeReadings(readings: List<Reading>): String {
        val temps = readings.mapNotNull { it.tempC }
        val hums = readings.mapNotNull { it.humidityPct }
        if (temps.isEmpty() && hums.isEmpty()) {
            return "No readings in the requested window."
        }
        val sb = StringBuilder()
        sb.append(readings.size).append(" reading")
        if (readings.size != 1) sb.append("s")
        if (temps.isNotEmpty()) {
            val min = temps.min()
            val max = temps.max()
            val avg = temps.average()
            sb.append(String.format(java.util.Locale.US,
                " . Temp %.1f-%.1f C (avg %.1f)", min, max, avg))
        }
        if (hums.isNotEmpty()) {
            sb.append(String.format(java.util.Locale.US,
                " . RH %.0f-%.0f%%", hums.min(), hums.max()))
        }
        return sb.toString()
    }

    /**
     * Normalize the user-entered backend base URL: trim, add an http://
     * scheme when missing, and drop trailing slashes. Returns null for a
     * blank input. Example: " 192.168.1.50:8000/ " -> "http://192.168.1.50:8000"
     */
    fun normalizeBaseUrl(raw: String): String? {
        var url = raw.trim()
        if (url.isEmpty()) return null
        if (!url.contains("://")) url = "http://$url"
        while (url.endsWith("/")) url = url.dropLast(1)
        return url
    }
}
