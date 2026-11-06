package com.msuiit.bca180.babymonitor

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Plain JVM unit tests (JUnit 4, no Robolectric) for the P5 backend JSON
 * response parsers and the pure display logic. JSON shapes are the exact
 * frozen contract exercised verbatim, plus defensive extra/missing-field
 * variants.
 */
class ApiResponseParserTest {

    /* ------------------------------------------------------------------ */
    /* GET /api/latest                                                     */
    /* ------------------------------------------------------------------ */

    @Test
    fun `parseLatest - exact contract shape with null event`() {
        val body = """
            {"reading":{"t":29.9,"h":75.4,"lux":257,"px":257,"mg":1002,
             "dev_ts":167,"recv_ms":1712345678901},"event":null,"online":true}
        """.trimIndent()

        val snap = ApiResponseParser.parseLatest(body)
        assertNotNull(snap)
        assertEquals(true, snap!!.online)

        val r = snap.reading!!
        assertEquals(29.9, r.tempC!!, 1e-9)
        assertEquals(75.4, r.humidityPct!!, 1e-9)
        assertEquals(257, r.lux)
        assertEquals(257, r.proximity)
        assertEquals(1002L, r.accelMg)
        assertEquals(167L, r.deviceTsSec)
        assertEquals(1712345678901L, r.recvMs)

        assertNull(snap.event)
    }

    @Test
    fun `parseLatest - nested cry event with unknown extra fields tolerated`() {
        // Extra fields ("fw":"1.2.0", "seq") must be ignored, not rejected.
        val body = """
            {"reading":{"t":22.5,"h":48.0,"lux":12,"px":900,"mg":-31,
             "dev_ts":402,"recv_ms":1712345678999,"fw":"1.2.0","seq":77},
             "event":{"type":"cry","on":true,"conf":86,"dev_ts":401,
             "recv_ms":1712345678800,"extra":1},"online":true}
        """.trimIndent()

        val snap = ApiResponseParser.parseLatest(body)
        assertNotNull(snap)
        val ev = snap!!.event!!
        assertEquals("cry", ev.type)
        assertEquals(true, ev.on)
        assertEquals(86, ev.conf)
        assertEquals(401L, ev.deviceTsSec)
        assertEquals(1712345678800L, ev.recvMs)
        assertEquals(-31L, snap.reading!!.accelMg)
    }

    @Test
    fun `parseLatest - missing fields map to nulls without throwing`() {
        val snap = ApiResponseParser.parseLatest("""{"online":false}""")
        assertNotNull(snap)
        assertEquals(false, snap!!.online)
        assertNull(snap.reading)
        assertNull(snap.event)
    }

    @Test
    fun `parseLatest - firmware ts alias accepted for dev_ts`() {
        // The frozen mqtt_task.c emits "ts" (uptime seconds); the backend
        // contract renames it "dev_ts". Both spellings must parse.
        val body = """{"reading":{"t":21.0,"h":40.0,"lux":5,"px":10,
            "mg":0,"ts":88},"event":null,"online":true}"""
        val snap = ApiResponseParser.parseLatest(body)
        assertNotNull(snap)
        assertEquals(88L, snap!!.reading!!.deviceTsSec)
    }

    @Test
    fun `parseLatest - malformed body returns null`() {
        assertNull(ApiResponseParser.parseLatest(""))
        assertNull(ApiResponseParser.parseLatest("<html>gateway timeout</html>"))
        assertNull(ApiResponseParser.parseLatest("{\"reading\":"))
    }

    @Test
    fun `parseLatest - missing online flag tolerated as unknown`() {
        val snap = ApiResponseParser.parseLatest("""{"reading":{},"event":null}""")
        assertNotNull(snap)
        assertNull(snap!!.online)
        val r = snap.reading!!
        assertNull(r.tempC)
        assertNull(r.humidityPct)
        assertNull(r.lux)
        assertNull(r.proximity)
        assertNull(r.accelMg)
        assertNull(r.deviceTsSec)
        assertNull(r.recvMs)
    }

    /* ------------------------------------------------------------------ */
    /* GET /api/events                                                     */
    /* ------------------------------------------------------------------ */

    @Test
    fun `parseEvents - exact contract shape newest first preserved`() {
        val body = """
            {"events":[
              {"type":"cry","on":true,"conf":86,"dev_ts":401,"recv_ms":1712345678800},
              {"type":"env","on":true,"dev_ts":350,"recv_ms":1712345600000},
              {"type":"fault","on":true,"dev_ts":300,"recv_ms":1712345500000},
              {"type":"allclear","on":true,"dev_ts":290,"recv_ms":1712345400000},
              {"type":"cry","on":false,"conf":86,"dev_ts":410,"recv_ms":1712345680000}
            ]}
        """.trimIndent()

        val page = ApiResponseParser.parseEvents(body)
        assertNotNull(page)
        assertEquals(5, page!!.events.size)
        // Order preserved exactly as delivered (backend sends newest first).
        assertEquals("cry", page.events[0].type)
        assertEquals(true, page.events[0].on)
        assertEquals(86, page.events[0].conf)
        assertEquals("env", page.events[1].type)
        assertEquals("fault", page.events[2].type)
        assertEquals("allclear", page.events[3].type)
        // A cry-off edge parses with on=false.
        assertEquals("cry", page.events[4].type)
        assertEquals(false, page.events[4].on)
    }

    @Test
    fun `parseEvents - missing conf and on default defensively`() {
        // env/fault/allclear lines carry no conf per the firmware emitter.
        val body = """{"events":[{"type":"env","dev_ts":9}]}"""
        val page = ApiResponseParser.parseEvents(body)
        assertNotNull(page)
        val ev = page!!.events[0]
        assertEquals("env", ev.type)
        assertTrue(ev.on) // edges default to rising
        assertNull(ev.conf)
        assertEquals(9L, ev.deviceTsSec)
        assertNull(ev.recvMs)
    }

    @Test
    fun `parseEvents - malformed body returns null`() {
        assertNull(ApiResponseParser.parseEvents("""{"events":}"""))
        assertNull(ApiResponseParser.parseEvents("""{"readings":[]}"""))
        assertNull(ApiResponseParser.parseEvents("not json"))
    }

    @Test
    fun `parseEvents - empty list parses to empty page`() {
        val page = ApiResponseParser.parseEvents("""{"events":[]}""")
        assertNotNull(page)
        assertEquals(0, page!!.events.size)
    }

    /* ------------------------------------------------------------------ */
    /* GET /api/history                                                    */
    /* ------------------------------------------------------------------ */

    @Test
    fun `parseHistory - exact contract shape newest first preserved`() {
        val body = """
            {"readings":[
              {"t":29.9,"h":75.4,"lux":257,"px":257,"mg":1002,"dev_ts":167,"recv_ms":1712345678901},
              {"t":29.7,"h":75.1,"lux":250,"px":251,"mg":998,"dev_ts":162,"recv_ms":1712345673901}
            ]}
        """.trimIndent()

        val page = ApiResponseParser.parseHistory(body)
        assertNotNull(page)
        assertEquals(2, page!!.readings.size)
        assertEquals(29.9, page.readings[0].tempC!!, 1e-9)
        assertEquals(29.7, page.readings[1].tempC!!, 1e-9)
        assertEquals(1712345678901L, page.readings[0].recvMs)
    }

    @Test
    fun `parseHistory - malformed body returns null`() {
        assertNull(ApiResponseParser.parseHistory("[]"))
        assertNull(ApiResponseParser.parseHistory("""{"events":[]}"""))
    }

    /* ------------------------------------------------------------------ */
    /* POST /api/ack and /api/mute                                         */
    /* ------------------------------------------------------------------ */

    @Test
    fun `parseOk - contract ack and mute responses`() {
        assertTrue(ApiResponseParser.parseOk("""{"ok":true}"""))
        assertFalse(ApiResponseParser.parseOk("""{"ok":false}"""))
        assertFalse(ApiResponseParser.parseOk("{}"))
        assertFalse(ApiResponseParser.parseOk("garbage"))
    }

    /* ------------------------------------------------------------------ */
    /* Banner derivation                                                   */
    /* ------------------------------------------------------------------ */

    private fun snap(event: BabyEvent?, online: Boolean? = true) =
        LatestSnapshot(reading = null, event = event, online = online)

    @Test
    fun `banner - cry on is ALERT with CRY DETECTED`() {
        val b = MonitorLogic.deriveBanner(
            snap(BabyEvent("cry", true, 86, 401L, null)))
        assertEquals(BannerState.ALERT, b.state)
        assertEquals("CRY DETECTED", b.title)
        assertTrue(b.subtitle.contains("86%"))
    }

    @Test
    fun `banner - env on is ALERT, fault on is FAULT`() {
        val env = MonitorLogic.deriveBanner(
            snap(BabyEvent("env", true, null, 350L, null)))
        assertEquals(BannerState.ALERT, env.state)

        val fault = MonitorLogic.deriveBanner(
            snap(BabyEvent("fault", true, null, 300L, null)))
        assertEquals(BannerState.FAULT, fault.state)
    }

    @Test
    fun `banner - allclear, cry-off edge, null event are NORMAL`() {
        assertEquals(BannerState.NORMAL,
            MonitorLogic.deriveBanner(snap(BabyEvent("allclear", true, null, 1L, null))).state)
        assertEquals(BannerState.NORMAL,
            MonitorLogic.deriveBanner(snap(BabyEvent("cry", false, 80, 1L, null))).state)
        assertEquals(BannerState.NORMAL, MonitorLogic.deriveBanner(snap(null)).state)
    }

    @Test
    fun `banner - offline does not mask an active cry event`() {
        val b = MonitorLogic.deriveBanner(
            snap(BabyEvent("cry", true, 90, 1L, null), online = false))
        assertEquals(BannerState.ALERT, b.state)
    }

    @Test
    fun `banner - normal subtitle reflects offline device`() {
        val b = MonitorLogic.deriveBanner(snap(null, online = false))
        assertEquals(BannerState.NORMAL, b.state)
        assertTrue(b.subtitle.contains("offline"))
    }

    /* ------------------------------------------------------------------ */
    /* Relative ages, mute formatting, URL normalization, notify gate      */
    /* ------------------------------------------------------------------ */

    @Test
    fun `relativeAgeText - just now, minutes, hours, days`() {
        val now = 1_700_000_000_000L
        assertEquals("just now",
            MonitorLogic.relativeAgeText(now - 30_000L, now, 10L))
        assertEquals("2 min ago",
            MonitorLogic.relativeAgeText(now - 120_000L, now, 10L))
        assertEquals("1 min ago",
            MonitorLogic.relativeAgeText(now - 60_000L, now, 10L))
        assertEquals("3 h ago",
            MonitorLogic.relativeAgeText(now - 3 * 3_600_000L, now, 10L))
        assertEquals("4 d ago",
            MonitorLogic.relativeAgeText(now - 4 * 86_400_000L, now, 10L))
    }

    @Test
    fun `relativeAgeText - future timestamp clamps to just now`() {
        val now = 1_700_000_000_000L
        assertEquals("just now",
            MonitorLogic.relativeAgeText(now + 500_000L, now, 10L))
    }

    @Test
    fun `relativeAgeText - implausible recv_ms falls back to device uptime`() {
        assertEquals("device uptime 167s",
            MonitorLogic.relativeAgeText(null, 1_700_000_000_000L, 167L))
        assertEquals("device uptime 167s",
            MonitorLogic.relativeAgeText(1234L, 1_700_000_000_000L, 167L))
        assertEquals("-", MonitorLogic.relativeAgeText(null, 1_700_000_000_000L, null))
    }

    @Test
    fun `formatMuteRemaining - seconds, minutes, hours`() {
        assertEquals("45s", MonitorLogic.formatMuteRemaining(45_000L))
        assertEquals("29m", MonitorLogic.formatMuteRemaining(29 * 60_000L + 12_000L))
        assertEquals("1h 59m", MonitorLogic.formatMuteRemaining((3600 + 59 * 60) * 1000L))
        assertEquals("4h 00m", MonitorLogic.formatMuteRemaining(14400_000L))
        assertEquals("0s", MonitorLogic.formatMuteRemaining(0L))
        assertEquals("0s", MonitorLogic.formatMuteRemaining(-5L))
    }

    @Test
    fun `normalizeBaseUrl - trims, schemes, trailing slashes`() {
        assertEquals("http://192.168.1.50:8000",
            MonitorLogic.normalizeBaseUrl(" 192.168.1.50:8000/ "))
        assertEquals("http://192.168.1.50:8000",
            MonitorLogic.normalizeBaseUrl("192.168.1.50:8000///"))
        assertEquals("http://192.168.1.50:8000",
            MonitorLogic.normalizeBaseUrl("http://192.168.1.50:8000"))
        assertNull(MonitorLogic.normalizeBaseUrl("   "))
    }

    @Test
    fun `shouldNotifyCry - only new cry rising edges notify`() {
        val cryOn1 = BabyEvent("cry", true, 86, 401L, 1712345678800L)
        val cryOn1Dup = BabyEvent("cry", true, 86, 401L, 1712345678800L)
        val cryOn2 = BabyEvent("cry", true, 90, 512L, 1712345999000L)
        val cryOff = BabyEvent("cry", false, 86, 410L, 1712345680000L)
        val envOn = BabyEvent("env", true, null, 350L, null)

        val key1 = MonitorLogic.eventKey(cryOn1)
        assertTrue(MonitorLogic.shouldNotifyCry(cryOn1, null))
        // Same edge seen again on the next 5 s poll: suppressed.
        assertFalse(MonitorLogic.shouldNotifyCry(cryOn1Dup, key1))
        // New rising edge: notifies.
        assertTrue(MonitorLogic.shouldNotifyCry(cryOn2, key1))
        // Cry-off and env edges never notify.
        assertFalse(MonitorLogic.shouldNotifyCry(cryOff, null))
        assertFalse(MonitorLogic.shouldNotifyCry(envOn, null))
        assertFalse(MonitorLogic.shouldNotifyCry(null, null))
    }

    @Test
    fun `summarizeReadings - min max avg summary`() {
        val readings = listOf(
            Reading(21.0, 41.0, 5, null, null, 1L, null),
            Reading(24.9, 63.0, 9, null, null, 2L, null),
            Reading(23.1, 50.0, 7, null, null, 3L, null)
        )
        val s = MonitorLogic.summarizeReadings(readings)
        assertTrue(s.contains("3 readings"))
        assertTrue(s.contains("Temp 21.0-24.9 C (avg 23.0)"))
        assertTrue(s.contains("RH 41-63%"))
        assertEquals("No readings in the requested window.",
            MonitorLogic.summarizeReadings(emptyList()))
    }
}
