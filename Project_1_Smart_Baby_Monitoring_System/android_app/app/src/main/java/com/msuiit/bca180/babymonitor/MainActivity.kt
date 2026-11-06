package com.msuiit.bca180.babymonitor

import android.Manifest
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import android.view.View
import android.widget.PopupMenu
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.google.android.material.button.MaterialButtonToggleGroup
import com.google.android.material.card.MaterialCardView
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.textfield.TextInputEditText
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors

/**
 * ==============================================================================
 * Project 1 - P6: Smart Baby Monitor Companion App (single-Activity, Views)
 * ==============================================================================
 * Talks to the P5 PC backend (REST bridge over the frozen MQTT contract):
 *   GET /api/latest every 5 s while foregrounded -> dashboard + banner
 *   GET /api/events, /api/history                -> Events screen
 *   POST /api/ack, /api/mute                     -> dashboard controls
 *   GET /api/health                              -> Settings connection test
 *
 * State holder: this Activity + DashboardPoller (simple state-holder pattern;
 * no ViewModel, matching the LAB_3 house style of one plain Activity).
 */
class MainActivity : AppCompatActivity() {

    private lateinit var settings: SettingsStore
    private lateinit var api: ApiClient
    private lateinit var notifier: CryNotifier
    private lateinit var poller: DashboardPoller
    private val ioExecutor: ExecutorService = Executors.newSingleThreadExecutor()

    // ---- Views ---------------------------------------------------------------
    private lateinit var tvOnlineDot: TextView
    private lateinit var toggleScreens: MaterialButtonToggleGroup
    private lateinit var sectionDashboard: View
    private lateinit var sectionEvents: View
    private lateinit var sectionSettings: View

    private lateinit var cardBanner: MaterialCardView
    private lateinit var tvBannerState: TextView
    private lateinit var tvBannerSubtitle: TextView
    private lateinit var tvMuteChip: TextView
    private lateinit var tvTemp: TextView
    private lateinit var tvHumidity: TextView
    private lateinit var tvLux: TextView
    private lateinit var tvProximity: TextView
    private lateinit var tvAccel: TextView
    private lateinit var btnAck: View
    private lateinit var btnMute: View
    private lateinit var tvLastUpdated: TextView

    private lateinit var btnRefreshEvents: View
    private lateinit var tvReadingSummary: TextView
    private lateinit var recyclerEvents: RecyclerView
    private lateinit var eventAdapter: EventAdapter

    private lateinit var etBaseUrl: TextInputEditText
    private lateinit var btnTestConnection: View
    private lateinit var tvTestResult: TextView
    private lateinit var tvNotificationNote: TextView
    private lateinit var tvCryHint: TextView

    // ---- Runtime state -------------------------------------------------------
    private var lastNotifiedCryKey: String? = null
    private var muteUntilElapsedMs: Long = 0L

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        settings = SettingsStore(this)
        api = ApiClient()
        notifier = CryNotifier(this)
        notifier.ensureChannel()
        poller = DashboardPoller(
            api,
            onSnapshot = ::onSnapshot,
            onError = ::onPollError
        )

        bindViews()
        etBaseUrl.setText(settings.baseUrl())

        toggleScreens.addOnButtonCheckedListener { _, checkedId, isChecked ->
            if (!isChecked) return@addOnButtonCheckedListener
            when (checkedId) {
                R.id.btnTabDashboard -> showSection(sectionDashboard)
                R.id.btnTabEvents -> {
                    showSection(sectionEvents)
                    loadEvents()
                }
                R.id.btnTabSettings -> showSection(sectionSettings)
            }
        }

        btnAck.setOnClickListener { sendAck() }
        btnMute.setOnClickListener { showMuteMenu() }
        btnTestConnection.setOnClickListener { testConnection() }
        btnRefreshEvents.setOnClickListener { loadEvents() }
        tvCryHint.text = getString(R.string.cry_playback_hint)

        refreshMuteChip()
        maybeRequestNotificationPermission()
    }

    override fun onStart() {
        super.onStart()
        poller.start { liveBaseUrl() }
    }

    override fun onStop() {
        poller.stop()
        super.onStop()
    }

    override fun onPause() {
        // Persist the server URL field as-is (normalized on use).
        settings.saveBaseUrl(etBaseUrl.text?.toString()?.trim() ?: "")
        super.onPause()
    }

    /** Live URL wins over the persisted one while the user is editing. */
    private fun liveBaseUrl(): String =
        MonitorLogic.normalizeBaseUrl(etBaseUrl.text?.toString() ?: "") ?: ""

    private fun bindViews() {
        tvOnlineDot = findViewById(R.id.tvOnlineDot)
        toggleScreens = findViewById(R.id.toggleScreens)
        sectionDashboard = findViewById(R.id.sectionDashboard)
        sectionEvents = findViewById(R.id.sectionEvents)
        sectionSettings = findViewById(R.id.sectionSettings)

        cardBanner = findViewById(R.id.cardBanner)
        tvBannerState = findViewById(R.id.tvBannerState)
        tvBannerSubtitle = findViewById(R.id.tvBannerSubtitle)
        tvMuteChip = findViewById(R.id.tvMuteChip)
        tvTemp = findViewById(R.id.tvTemp)
        tvHumidity = findViewById(R.id.tvHumidity)
        tvLux = findViewById(R.id.tvLux)
        tvProximity = findViewById(R.id.tvProximity)
        tvAccel = findViewById(R.id.tvAccel)
        btnAck = findViewById(R.id.btnAck)
        btnMute = findViewById(R.id.btnMute)
        tvLastUpdated = findViewById(R.id.tvLastUpdated)

        btnRefreshEvents = findViewById(R.id.btnRefreshEvents)
        tvReadingSummary = findViewById(R.id.tvReadingSummary)
        recyclerEvents = findViewById(R.id.recyclerEvents)
        eventAdapter = EventAdapter()
        recyclerEvents.layoutManager = LinearLayoutManager(this)
        recyclerEvents.adapter = eventAdapter
        recyclerEvents.isNestedScrollingEnabled = false

        etBaseUrl = findViewById(R.id.etBaseUrl)
        btnTestConnection = findViewById(R.id.btnTestConnection)
        tvTestResult = findViewById(R.id.tvTestResult)
        tvNotificationNote = findViewById(R.id.tvNotificationNote)
        tvCryHint = findViewById(R.id.tvCryHint)
    }

    private fun showSection(section: View) {
        sectionDashboard.visibility = if (section == sectionDashboard) View.VISIBLE else View.GONE
        sectionEvents.visibility = if (section == sectionEvents) View.VISIBLE else View.GONE
        sectionSettings.visibility = if (section == sectionSettings) View.VISIBLE else View.GONE
    }

    /* ---------------------------------------------------------------------- */
    /* Dashboard polling                                                      */
    /* ---------------------------------------------------------------------- */

    private fun onSnapshot(snapshot: LatestSnapshot) {
        val banner = MonitorLogic.deriveBanner(snapshot)
        val stateColor = when (banner.state) {
            BannerState.NORMAL -> R.color.normal_green
            BannerState.ALERT -> R.color.alert_red
            BannerState.FAULT -> R.color.fault_amber
        }
        cardBanner.setCardBackgroundColor(ContextCompat.getColor(this, stateColor))
        tvBannerState.setTextColor(ContextCompat.getColor(this, R.color.background_dark))
        tvBannerSubtitle.setTextColor(ContextCompat.getColor(this, R.color.background_dark))

        // Big title line: state text (CRY DETECTED / NORMAL / ...).
        tvBannerState.text = banner.title
        tvBannerSubtitle.text = banner.subtitle

        // Online dot.
        when (snapshot.online) {
            true -> {
                tvOnlineDot.text = getString(R.string.dot_online)
                tvOnlineDot.setTextColor(ContextCompat.getColor(this, R.color.normal_green))
            }
            false -> {
                tvOnlineDot.text = getString(R.string.dot_offline)
                tvOnlineDot.setTextColor(ContextCompat.getColor(this, R.color.text_secondary))
            }
            else -> {
                tvOnlineDot.text = getString(R.string.dot_unknown)
                tvOnlineDot.setTextColor(ContextCompat.getColor(this, R.color.text_secondary))
            }
        }

        // Sensor cards (defensive: "--" when a field is missing).
        val r = snapshot.reading
        tvTemp.text = r?.tempC?.let { String.format("%.1f °C", it) } ?: "--"
        tvHumidity.text = r?.humidityPct?.let { String.format("%.1f %%", it) } ?: "--"
        tvLux.text = r?.lux?.let { "$it lx" } ?: "--"
        tvProximity.text = r?.proximity?.let { "$it" } ?: "--"
        tvAccel.text = r?.accelMg?.let { "$it mg" } ?: "--"

        tvLastUpdated.text = getString(R.string.updated_line, liveBaseUrl())

        // One high-priority notification per NEW cry rising edge.
        val cryCandidate = snapshot.event?.let { if (it.type == "cry") it else null }
        if (cryCandidate != null &&
            MonitorLogic.shouldNotifyCry(cryCandidate, lastNotifiedCryKey)
        ) {
            lastNotifiedCryKey = MonitorLogic.eventKey(cryCandidate)
            notifier.notifyCry(cryCandidate)
        }
        refreshMuteChip()
    }

    private fun onPollError(message: String) {
        tvOnlineDot.text = getString(R.string.dot_unreachable)
        tvOnlineDot.setTextColor(ContextCompat.getColor(this, R.color.fault_amber))
        tvLastUpdated.text = getString(R.string.poll_failed_line, message)
    }

    /* ---------------------------------------------------------------------- */
    /* Controls: ACK + Mute                                                   */
    /* ---------------------------------------------------------------------- */

    private fun sendAck() {
        val url = liveBaseUrl()
        if (url.isEmpty()) {
            toast(getString(R.string.err_no_url)); return
        }
        ioExecutor.execute {
            try {
                val ok = api.ack(url)
                runOnUiThread {
                    if (ok) {
                        toast(getString(R.string.ack_ok))
                        // Firmware mirrors an ack into a 60 s mute window
                        // (baby_logic ALERT_MUTE_MS, see mqtt_task.c).
                        muteUntilElapsedMs = SystemClock.elapsedRealtime() + 60_000L
                        refreshMuteChip()
                        pollerNow()
                    } else {
                        toast(getString(R.string.ack_bad_response))
                    }
                }
            } catch (e: Exception) {
                runOnUiThread { toast(getString(R.string.ack_failed, e.message ?: "?")) }
            }
        }
    }

    private fun showMuteMenu() {
        val popup = PopupMenu(this, btnMute)
        val options = listOf(
            getString(R.string.mute_30m) to 1800,
            getString(R.string.mute_1h) to 3600,
            getString(R.string.mute_2h) to 7200,
            getString(R.string.mute_4h) to 14400
        )
        options.forEachIndexed { i, (label, _) -> popup.menu.add(0, i, i, label) }
        popup.setOnMenuItemClickListener { item ->
            val seconds = options[item.itemId].second
            sendMute(seconds)
            true
        }
        popup.show()
    }

    private fun sendMute(seconds: Int) {
        val url = liveBaseUrl()
        if (url.isEmpty()) {
            toast(getString(R.string.err_no_url)); return
        }
        ioExecutor.execute {
            try {
                val ok = api.mute(url, seconds)
                runOnUiThread {
                    if (ok) {
                        toast(getString(R.string.mute_ok, seconds / 60))
                        muteUntilElapsedMs = SystemClock.elapsedRealtime() + seconds * 1000L
                        refreshMuteChip()
                        pollerNow()
                    } else {
                        toast(getString(R.string.mute_bad_response))
                    }
                }
            } catch (e: Exception) {
                runOnUiThread { toast(getString(R.string.mute_failed, e.message ?: "?")) }
            }
        }
    }

    /** Client-side mirror of the requested mute window (contract has no mute field). */
    private fun refreshMuteChip() {
        val remaining = muteUntilElapsedMs - SystemClock.elapsedRealtime()
        if (remaining > 0) {
            tvMuteChip.visibility = View.VISIBLE
            tvMuteChip.text = getString(
                R.string.mute_chip,
                MonitorLogic.formatMuteRemaining(remaining)
            )
        } else {
            tvMuteChip.visibility = View.GONE
        }
    }

    private fun pollerNow() {
        // Immediate refresh so the dashboard reflects the command outcome fast.
        if (poller.isRunning()) {
            poller.stop()
            poller.start { liveBaseUrl() }
        }
    }

    /* ---------------------------------------------------------------------- */
    /* Events screen                                                          */
    /* ---------------------------------------------------------------------- */

    private fun loadEvents() {
        val url = liveBaseUrl()
        if (url.isEmpty()) {
            tvReadingSummary.text = getString(R.string.err_no_url)
            return
        }
        tvReadingSummary.text = getString(R.string.loading)
        ioExecutor.execute {
            try {
                val events = api.fetchEvents(url, limit = 30)
                val history = api.fetchHistory(url, minutes = 60)
                runOnUiThread {
                    eventAdapter.submit(events.events)
                    tvReadingSummary.text = getString(
                        R.string.reading_summary_prefix,
                        MonitorLogic.summarizeReadings(history.readings)
                    )
                }
            } catch (e: Exception) {
                runOnUiThread {
                    tvReadingSummary.text = getString(
                        R.string.events_failed, e.message ?: "?")
                }
            }
        }
    }

    /* ---------------------------------------------------------------------- */
    /* Settings screen                                                        */
    /* ---------------------------------------------------------------------- */

    private fun testConnection() {
        val raw = etBaseUrl.text?.toString() ?: ""
        val url = MonitorLogic.normalizeBaseUrl(raw)
        if (url == null) {
            tvTestResult.text = getString(R.string.err_no_url)
            return
        }
        settings.saveBaseUrl(raw.trim())
        tvTestResult.text = getString(R.string.testing)
        ioExecutor.execute {
            try {
                val result = api.health(url)
                runOnUiThread { tvTestResult.text = result }
            } catch (e: Exception) {
                runOnUiThread {
                    tvTestResult.text = getString(R.string.test_failed, e.message ?: "?")
                }
            }
        }
    }

    /* ---------------------------------------------------------------------- */
    /* Notifications permission (the app's one runtime permission)            */
    /* ---------------------------------------------------------------------- */

    private fun maybeRequestNotificationPermission() {
        tvNotificationNote.visibility =
            if (notifier.notificationsGranted()) View.GONE else View.VISIBLE

        if (Build.VERSION.SDK_INT < 33) return
        if (notifier.notificationsGranted()) return
        if (settings.notificationRationaleShown()) return

        settings.markNotificationRationaleShown()
        MaterialAlertDialogBuilder(this)
            .setTitle(R.string.notif_dialog_title)
            .setMessage(R.string.notif_dialog_rationale)
            .setPositiveButton(R.string.notif_dialog_allow) { _, _ ->
                ActivityCompat.requestPermissions(
                    this,
                    arrayOf(Manifest.permission.POST_NOTIFICATIONS),
                    CryNotifier.REQUEST_POST_NOTIFICATIONS
                )
            }
            .setNegativeButton(R.string.notif_dialog_later, null)
            .show()
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == CryNotifier.REQUEST_POST_NOTIFICATIONS) {
            val granted = grantResults.isNotEmpty() &&
                grantResults[0] == android.content.pm.PackageManager.PERMISSION_GRANTED
            tvNotificationNote.visibility =
                if (granted) View.GONE else View.VISIBLE
            if (!granted) {
                toast(getString(R.string.notif_denied_toast))
            }
        }
    }

    private fun toast(msg: String) {
        Toast.makeText(this, msg, Toast.LENGTH_SHORT).show()
    }
}
