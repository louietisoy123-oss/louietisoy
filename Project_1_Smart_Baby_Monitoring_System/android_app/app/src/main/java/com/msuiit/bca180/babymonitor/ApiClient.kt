package com.msuiit.bca180.babymonitor

import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import java.io.IOException
import java.util.concurrent.TimeUnit

/**
 * Thin OkHttp client over the P5 backend REST contract. All calls are
 * synchronous and BLOCKING (callers run them on background executors).
 *
 * Endpoints (base URL = user-configured, e.g. http://192.168.1.50:8000):
 *   GET  /api/latest          -> LatestSnapshot
 *   GET  /api/events?limit=N  -> EventsPage
 *   GET  /api/history?minutes=N -> HistoryPage
 *   GET  /api/health          -> raw status text (connection test)
 *   POST /api/ack  (empty)    -> {"ok":true}
 *   POST /api/mute {"sec":N}  -> {"ok":true}
 */
class ApiClient(
    private val http: OkHttpClient = OkHttpClient.Builder()
        .connectTimeout(4, TimeUnit.SECONDS)
        .readTimeout(5, TimeUnit.SECONDS)
        .writeTimeout(4, TimeUnit.SECONDS)
        .build()
) {

    /** GET and return the body as a string; non-2xx throws IOException. */
    private fun get(url: String): String {
        val request = Request.Builder().url(url).get().build()
        http.newCall(request).execute().use { resp ->
            val body = resp.body?.string().orEmpty()
            if (!resp.isSuccessful) {
                throw IOException("HTTP ${resp.code}")
            }
            return body
        }
    }

    /** POST with the given body; returns the response body (2xx expected). */
    private fun post(url: String, body: String): String {
        val mediaType = "application/json; charset=utf-8".toMediaType()
        val request = Request.Builder().url(url)
            .post(body.toRequestBody(mediaType))
            .build()
        http.newCall(request).execute().use { resp ->
            val respBody = resp.body?.string().orEmpty()
            if (!resp.isSuccessful) {
                throw IOException("HTTP ${resp.code}")
            }
            return respBody
        }
    }

    fun fetchLatest(baseUrl: String): LatestSnapshot =
        ApiResponseParser.parseLatest(get("$baseUrl/api/latest"))
            ?: throw IOException("Malformed /api/latest response")

    fun fetchEvents(baseUrl: String, limit: Int = 30): EventsPage =
        ApiResponseParser.parseEvents(get("$baseUrl/api/events?limit=$limit"))
            ?: throw IOException("Malformed /api/events response")

    fun fetchHistory(baseUrl: String, minutes: Int = 60): HistoryPage =
        ApiResponseParser.parseHistory(get("$baseUrl/api/history?minutes=$minutes"))
            ?: throw IOException("Malformed /api/history response")

    /** Connection test: report the status line plus a short body snippet. */
    fun health(baseUrl: String): String {
        val request = Request.Builder().url("$baseUrl/api/health").get().build()
        http.newCall(request).execute().use { resp ->
            val body = resp.body?.string().orEmpty().trim()
            val snippet = if (body.length > 120) body.take(120) + "..." else body
            return "HTTP ${resp.code}" + if (snippet.isNotEmpty()) " - $snippet" else ""
        }
    }

    /** POST /api/ack with an empty body (contract: {"ok":true}). */
    fun ack(baseUrl: String): Boolean =
        ApiResponseParser.parseOk(post("$baseUrl/api/ack", ""))

    /** POST /api/mute with {"sec":N} (contract: {"ok":true}). */
    fun mute(baseUrl: String, seconds: Int): Boolean =
        ApiResponseParser.parseOk(
            post("$baseUrl/api/mute", "{\"sec\":$seconds}")
        )
}
