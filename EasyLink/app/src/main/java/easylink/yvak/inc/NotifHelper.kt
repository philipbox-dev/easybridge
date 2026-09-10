package easylink.yvak.inc

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import androidx.core.app.NotificationCompat
import androidx.core.app.RemoteInput
import easylink.yvak.inc.ble.BridgeService
import easylink.yvak.inc.proto.ChatMessage
import easylink.yvak.inc.proto.MsgKind
import easylink.yvak.inc.proto.SosAlert

class NotifHelper(private val context: Context) {
    companion object {
        const val CH_CONN = "conn"
        const val CH_MSG = "msg"
        const val CH_MSG_SILENT = "msg_silent"
        const val CH_SOS = "sos"
        const val CH_PTT = "ptt"
        const val ID_SERVICE = 1
        const val ID_SOS = 2
        const val ID_PTT = 3
        const val ID_MSG_BASE = 100
        const val KEY_REPLY = "reply_text"
        const val EXTRA_CHAT_KEY = "chat_key"
    }

    private val nm = context.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager

    fun createChannels() {
        nm.createNotificationChannel(NotificationChannel(CH_CONN,
            context.getString(R.string.notif_ch_conn), NotificationManager.IMPORTANCE_LOW))
        nm.createNotificationChannel(NotificationChannel(CH_MSG,
            context.getString(R.string.notif_ch_msg), NotificationManager.IMPORTANCE_HIGH)
            .apply { enableVibration(true) })
        // Тихий канал для «режима охоты»: уведомления есть, звука/вибры нет
        nm.createNotificationChannel(NotificationChannel(CH_MSG_SILENT,
            context.getString(R.string.notif_ch_msg) + " (silent)",
            NotificationManager.IMPORTANCE_LOW)
            .apply { enableVibration(false); setSound(null, null) })
        nm.createNotificationChannel(NotificationChannel(CH_SOS,
            context.getString(R.string.notif_ch_sos), NotificationManager.IMPORTANCE_HIGH)
            .apply { enableVibration(true); setBypassDnd(true) })
        nm.createNotificationChannel(NotificationChannel(CH_PTT,
            context.getString(R.string.notif_ch_ptt), NotificationManager.IMPORTANCE_HIGH)
            .apply { enableVibration(true) })
    }

    private fun openAppIntent(extra: String? = null): PendingIntent {
        val i = Intent(context, MainActivity::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP
            if (extra != null) putExtra("open", extra)
        }
        return PendingIntent.getActivity(context, extra?.hashCode() ?: 0, i,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)
    }

    fun serviceNotification(deviceName: String): Notification =
        NotificationCompat.Builder(context, CH_CONN)
            .setSmallIcon(R.drawable.ic_notif)
            .setContentTitle(context.getString(R.string.app_name))
            .setContentText(
                if (deviceName.isEmpty()) context.getString(R.string.conn_connecting)
                else context.getString(R.string.notif_service_fmt, deviceName))
            .setOngoing(true)
            .setContentIntent(openAppIntent())
            .build()

    fun notifyMessage(msg: ChatMessage) {
        val title = msg.nodeName.ifEmpty { context.getString(R.string.chat_general) }
        val text = when (msg.kind) {
            MsgKind.VOICE -> context.getString(R.string.voice_message)
            MsgKind.IMAGE -> context.getString(R.string.image_message)
            else -> msg.text
        }

        val replyIntent = Intent(context, BridgeService::class.java).apply {
            action = BridgeService.ACTION_REPLY
            putExtra(EXTRA_CHAT_KEY, msg.chatKey)
        }
        val replyPi = PendingIntent.getService(context, msg.chatKey.hashCode(), replyIntent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_MUTABLE)
        val remoteInput = RemoteInput.Builder(KEY_REPLY)
            .setLabel(context.getString(R.string.notif_reply)).build()
        val replyAction = NotificationCompat.Action.Builder(
            R.drawable.ic_notif, context.getString(R.string.notif_reply), replyPi)
            .addRemoteInput(remoteInput).build()

        val channel = if (Bridge.prefs.quietMode.value) CH_MSG_SILENT else CH_MSG
        val n = NotificationCompat.Builder(context, channel)
            .setSmallIcon(R.drawable.ic_notif)
            .setContentTitle(title)
            .setContentText(text)
            .setStyle(NotificationCompat.BigTextStyle().bigText(text))
            .setAutoCancel(true)
            .setContentIntent(openAppIntent("chat:${msg.chatKey}"))
            .addAction(replyAction)
            .build()
        nm.notify(ID_MSG_BASE + msg.chatKey.hashCode() % 1000, n)
    }

    fun notifyCheckIn(fromName: String) {
        val n = NotificationCompat.Builder(context, CH_SOS)
            .setSmallIcon(R.drawable.ic_notif)
            .setContentTitle(context.getString(R.string.checkin_title))
            .setContentText(context.getString(R.string.checkin_incoming_fmt, fromName))
            .setPriority(NotificationCompat.PRIORITY_HIGH)
            .setAutoCancel(true)
            .setContentIntent(openAppIntent("checkin"))
            .build()
        nm.notify(ID_SOS + 7, n)
    }

    fun notifyMissed(name: String) {
        val n = NotificationCompat.Builder(context,
            if (Bridge.prefs.quietMode.value) CH_MSG_SILENT else CH_PTT)
            .setSmallIcon(R.drawable.ic_notif)
            .setContentTitle(context.getString(R.string.missed_call_title))
            .setContentText(name)
            .setAutoCancel(true)
            .setContentIntent(openAppIntent())
            .build()
        nm.notify(ID_PTT + 10, n)
    }

    fun notifySos(alert: SosAlert) {
        val n = NotificationCompat.Builder(context, CH_SOS)
            .setSmallIcon(R.drawable.ic_notif)
            .setContentTitle(context.getString(
                if (alert.isPanic) R.string.panic_incoming else R.string.notif_sos_title))
            .setContentText(context.getString(R.string.sos_from_fmt, alert.name) +
                if (alert.text.isNotEmpty()) " · ${alert.text}" else "")
            .setPriority(NotificationCompat.PRIORITY_MAX)
            .setCategory(NotificationCompat.CATEGORY_ALARM)
            .setAutoCancel(true)
            .setContentIntent(openAppIntent("sos"))
            .setFullScreenIntent(openAppIntent("sos"), true)
            .build()
        nm.notify(ID_SOS, n)
    }

    fun cancelSos() = nm.cancel(ID_SOS)

    fun notifyPtt(callerName: String) {
        val n = NotificationCompat.Builder(context, CH_PTT)
            .setSmallIcon(R.drawable.ic_notif)
            .setContentTitle(context.getString(R.string.ptt_incoming))
            .setContentText(context.getString(R.string.notif_ptt_fmt, callerName))
            .setPriority(NotificationCompat.PRIORITY_MAX)
            .setCategory(NotificationCompat.CATEGORY_CALL)
            .setAutoCancel(true)
            .setContentIntent(openAppIntent("ptt"))
            .setFullScreenIntent(openAppIntent("ptt"), true)
            .build()
        nm.notify(ID_PTT, n)
    }

    fun cancelPtt() = nm.cancel(ID_PTT)

    fun notifyDeadmanReminder(minsLeft: Long) {
        val n = NotificationCompat.Builder(context, CH_SOS)
            .setSmallIcon(R.drawable.ic_notif)
            .setContentTitle(context.getString(R.string.deadman_reminder_title))
            .setContentText(context.getString(R.string.deadman_reminder_fmt, minsLeft))
            .setPriority(NotificationCompat.PRIORITY_HIGH)
            .setContentIntent(openAppIntent("deadman"))
            .setAutoCancel(true)
            .build()
        nm.notify(ID_SOS + 5, n)
    }
}
