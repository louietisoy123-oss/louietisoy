package com.msuiit.bca180.babymonitor

import android.os.Handler
import android.os.Looper
import java.io.IOException
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Foreground-gated dashboard polling loop: GET /api/latest every 5 s.
 *
 * Lifecycle contract:
 *  - start(baseUrlProvider): schedules an immediate tick, then re-arms itself
 *    PERIOD_MS after each completed request (fixed-delay, never overlapping).
 *  - stop(): clears the pending tick. An in-flight HTTP call may still
 *    complete; its callback is dropped when the poller is stopped.
 *  - The Activity calls start()/stop() from onStart()/onStop(), so polling
 *    only runs while the app is foregrounded (README documents this).
 *
 * The base URL provider is evaluated on the MAIN thread at tick time, so it
 * may safely read the settings text field (the live URL wins over the
 * persisted one while the user is editing).
 */
class DashboardPoller(
    private val api: ApiClient,
    private val onSnapshot: (LatestSnapshot) -> Unit,
    private val onError: (String) -> Unit,
    private val periodMs: Long = PERIOD_MS
) {
    private val mainHandler = Handler(Looper.getMainLooper())
    private val executor: ExecutorService = Executors.newSingleThreadExecutor()
    private val running = AtomicBoolean(false)

    @Volatile
    private var baseUrlProvider: (() -> String)? = null

    private val tick = Runnable { doTick() }

    fun start(baseUrlProvider: () -> String) {
        this.baseUrlProvider = baseUrlProvider
        if (!running.compareAndSet(false, true)) return
        mainHandler.post(tick)
    }

    fun stop() {
        running.set(false)
        mainHandler.removeCallbacks(tick)
    }

    fun isRunning(): Boolean = running.get()

    private fun doTick() {
        if (!running.get()) return
        val url = baseUrlProvider?.invoke().orEmpty()
        if (url.isBlank()) {
            // No backend configured yet: keep the loop alive, retry next period.
            if (running.get()) mainHandler.postDelayed(tick, periodMs)
            return
        }
        executor.execute {
            try {
                val snapshot = api.fetchLatest(url)
                mainHandler.post {
                    if (running.get()) onSnapshot(snapshot)
                }
            } catch (e: IOException) {
                mainHandler.post {
                    if (running.get()) onError(e.message ?: "network error")
                }
            } catch (e: Exception) {
                mainHandler.post {
                    if (running.get()) onError("Unexpected: ${e.message}")
                }
            } finally {
                // Fixed-delay re-arm regardless of success/failure.
                if (running.get()) {
                    mainHandler.postDelayed(tick, periodMs)
                }
            }
        }
    }

    companion object {
        const val PERIOD_MS = 5000L
    }
}
