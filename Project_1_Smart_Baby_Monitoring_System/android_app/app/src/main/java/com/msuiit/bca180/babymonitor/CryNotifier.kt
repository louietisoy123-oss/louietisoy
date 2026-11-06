package com.msuiit.bca180.babymonitor

import android.Manifest
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import androidx.core.content.ContextCompat

/**
 * High-priority "Baby cry detected" notifications.
 *
 * Policy (stated): a notification is posted for every NEW cry rising edge
 * the poller observes, whether the app is foregrounded or not - for a baby
 * monitor, alert redundancy between the banner and the notification shade
 * is a feature, not noise. Duplicate edges are suppressed by the
 * MonitorLogic.shouldNotifyCry() key check in the Activity.
 *
 * Because the poller is foreground-gated (stops in onStop), backgrounded
 * detection is limited to in-flight requests finishing just after
 * backgrounding; a cry that arrives while the app is fully backgrounded is
 * picked up (and notified) by the first foreground poll, and the dashboard
 * banner is the immediate in-app signal.
 */
class CryNotifier(private val context: Context) {

    companion object {
        const val CHANNEL_ID = "cry_alerts"
        const val NOTIFICATION_TAG = "cry"
        const val REQUEST_POST_NOTIFICATIONS = 7
    }

    /** Idempotent channel creation (API 26+); harmless to call every launch. */
    fun ensureChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
        val manager = context.getSystemService(NotificationManager::class.java)
            ?: return
        val channel = NotificationChannel(
            CHANNEL_ID,
            "Cry alerts",
            NotificationManager.IMPORTANCE_HIGH
        ).apply {
            description = "Alerts when the monitor detects infant crying"
        }
        manager.createNotificationChannel(channel)
    }

    fun notificationsGranted(): Boolean =
        Build.VERSION.SDK_INT < 33 ||
            ContextCompat.checkSelfPermission(
                context, Manifest.permission.POST_NOTIFICATIONS
            ) == PackageManager.PERMISSION_GRANTED

    /**
     * Post one cry notification. Returns false when notifications are not
     * permitted (SecurityException on 33+) - callers treat that as "skipped".
     */
    fun notifyCry(event: BabyEvent): Boolean {
        val title = "Baby cry detected"
        val detail = buildString {
            append(event.conf?.let { "Classifier confidence $it%. " } ?: "")
            MonitorLogic.relativeAgeText(event.recvMs, System.currentTimeMillis(),
                event.deviceTsSec).let { if (it != "-") append("Event $it.") }
            append(" Tap to open the monitor.")
        }

        val intent = Intent(context, MainActivity::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP
        }
        val pending = PendingIntent.getActivity(
            context, 0, intent,
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        )

        val notification = NotificationCompat.Builder(context, CHANNEL_ID)
            .setSmallIcon(android.R.drawable.ic_dialog_alert)
            .setContentTitle(title)
            .setContentText(detail)
            .setStyle(NotificationCompat.BigTextStyle().bigText(detail))
            .setPriority(NotificationCompat.PRIORITY_MAX)
            .setCategory(NotificationCompat.CATEGORY_ALARM)
            .setContentIntent(pending)
            .setAutoCancel(true)
            .build()

        val id = ((event.recvMs ?: event.deviceTsSec ?: 0L) and 0x7FFFFFFFL).toInt()
        return try {
            NotificationManagerCompat.from(context).notify(NOTIFICATION_TAG, id, notification)
            true
        } catch (_: SecurityException) {
            false
        }
    }
}
