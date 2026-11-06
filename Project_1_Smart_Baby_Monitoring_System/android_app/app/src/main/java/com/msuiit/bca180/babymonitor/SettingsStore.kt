package com.msuiit.bca180.babymonitor

import android.content.Context
import android.content.SharedPreferences

/**
 * Persistent app settings: only the backend base URL. The default uses the
 * PC-LAN placeholder documented in README.md; the user edits it in Settings
 * (IP:port of the machine running the P5 backend).
 */
class SettingsStore(context: Context) {

    private val prefs: SharedPreferences =
        context.getSharedPreferences("babymonitor_settings", Context.MODE_PRIVATE)

    companion object {
        const val DEFAULT_BASE_URL = "http://192.168.1.50:8000"
        private const val KEY_BASE_URL = "backend_base_url"
        private const val KEY_RATIONALE_SHOWN = "notification_rationale_shown"
    }

    fun baseUrl(): String = prefs.getString(KEY_BASE_URL, null) ?: DEFAULT_BASE_URL

    fun saveBaseUrl(url: String) {
        prefs.edit().putString(KEY_BASE_URL, url).apply()
    }

    fun notificationRationaleShown(): Boolean =
        prefs.getBoolean(KEY_RATIONALE_SHOWN, false)

    fun markNotificationRationaleShown() {
        prefs.edit().putBoolean(KEY_RATIONALE_SHOWN, true).apply()
    }
}
