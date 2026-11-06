package com.msuiit.bca180.babymonitor

import org.json.JSONArray
import org.json.JSONObject

/**
 * Defensive parsers for the P5 backend REST responses (frozen contract).
 *
 * Rules:
 *  - Unknown/extra fields are ignored (JSONObject semantics).
 *  - Missing or null fields map to Kotlin nulls, never exceptions.
 *  - Malformed bodies return null from the top-level entry points instead of
 *    throwing; UI code treats null as "bad response".
 *  - The firmware emits "ts" (device uptime seconds) where the backend
 *    contract uses "dev_ts"; both spellings are accepted.
 */
object ApiResponseParser {

    private fun JSONObject.longOrNull(name: String): Long? =
        if (has(name) && !isNull(name)) optLong(name) else null

    private fun JSONObject.intOrNull(name: String): Int? =
        if (has(name) && !isNull(name)) optInt(name) else null

    private fun JSONObject.doubleOrNull(name: String): Double? {
        if (!has(name) || isNull(name)) return null
        val d = optDouble(name)
        return if (d.isNaN()) null else d
    }

    /** Reading object; "dev_ts" preferred, firmware-style "ts" accepted. */
    fun parseReading(obj: JSONObject): Reading {
        val devTs = obj.longOrNull("dev_ts") ?: obj.longOrNull("ts")
        return Reading(
            tempC = obj.doubleOrNull("t"),
            humidityPct = obj.doubleOrNull("h"),
            lux = obj.intOrNull("lux"),
            proximity = obj.intOrNull("px"),
            accelMg = obj.longOrNull("mg"),
            deviceTsSec = devTs,
            recvMs = obj.longOrNull("recv_ms")
        )
    }

    /** Event object; "on" defaults to true (edges are rising by contract). */
    fun parseEvent(obj: JSONObject): BabyEvent = BabyEvent(
        type = obj.optString("type", ""),
        on = obj.optBoolean("on", true),
        conf = obj.intOrNull("conf"),
        deviceTsSec = obj.longOrNull("dev_ts") ?: obj.longOrNull("ts"),
        recvMs = obj.longOrNull("recv_ms")
    )

    /** GET /api/latest -> LatestSnapshot, or null when the body is not JSON. */
    fun parseLatest(body: String): LatestSnapshot? = try {
        val root = JSONObject(body)
        val readingObj = root.optJSONObject("reading")
        // "event": null (JSON literal) and a missing key both yield null here.
        val eventObj = root.optJSONObject("event")
        LatestSnapshot(
            reading = readingObj?.let(::parseReading),
            event = eventObj?.let(::parseEvent),
            online = if (root.has("online") && !root.isNull("online"))
                root.optBoolean("online") else null
        )
    } catch (_: Exception) {
        null
    }

    /** GET /api/events?limit=N -> EventsPage (newest first, order preserved). */
    fun parseEvents(body: String): EventsPage? = try {
        val root = JSONObject(body)
        val arr: JSONArray = root.getJSONArray("events")
        EventsPage((0 until arr.length()).map { parseEvent(arr.getJSONObject(it)) })
    } catch (_: Exception) {
        null
    }

    /** GET /api/history?minutes=N -> HistoryPage (newest first, order preserved). */
    fun parseHistory(body: String): HistoryPage? = try {
        val root = JSONObject(body)
        val arr: JSONArray = root.getJSONArray("readings")
        HistoryPage((0 until arr.length()).map { parseReading(arr.getJSONObject(it)) })
    } catch (_: Exception) {
        null
    }

    /**
     * Generic {"ok":true} command response (POST /api/ack, POST /api/mute).
     * Returns true only when the body parses and ok == true.
     */
    fun parseOk(body: String): Boolean = try {
        JSONObject(body).optBoolean("ok", false)
    } catch (_: Exception) {
        false
    }
}
