package easylink.yvak.inc.widget

import android.app.PendingIntent
import android.appwidget.AppWidgetManager
import android.appwidget.AppWidgetProvider
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.widget.RemoteViews
import easylink.yvak.inc.Bridge
import easylink.yvak.inc.MainActivity
import easylink.yvak.inc.R
import easylink.yvak.inc.proto.ConnState

/** #Ф5 Виджет: статус сети + последнее сообщение, тап открывает приложение. */
class EasyLinkWidget : AppWidgetProvider() {

    override fun onUpdate(context: Context, mgr: AppWidgetManager, ids: IntArray) {
        for (id in ids) mgr.updateAppWidget(id, build(context))
    }

    companion object {
        /** Дёргается из Bridge при изменениях состояния. */
        fun updateAll(context: Context) {
            try {
                val mgr = AppWidgetManager.getInstance(context)
                val ids = mgr.getAppWidgetIds(
                    ComponentName(context, EasyLinkWidget::class.java))
                if (ids.isNotEmpty()) {
                    for (id in ids) mgr.updateAppWidget(id, build(context))
                }
            } catch (_: Exception) {}
        }

        private fun build(context: Context): RemoteViews {
            val rv = RemoteViews(context.packageName, R.layout.widget_easylink)
            val connected = try {
                Bridge.ble.connState.value == ConnState.CONNECTED
            } catch (_: Exception) { false }
            val nodesOnline = try {
                Bridge.nodes.nodes.value.values.count { it.isOnline }
            } catch (_: Exception) { 0 }

            rv.setTextViewText(R.id.w_status,
                if (connected) context.getString(R.string.conn_connected)
                else context.getString(R.string.conn_disconnected))
            rv.setTextViewText(R.id.w_nodes,
                context.getString(R.string.online_count_fmt, nodesOnline))
            val last = try {
                Bridge.chat.chats.value.firstOrNull()?.lastMessage
            } catch (_: Exception) { null }
            rv.setTextViewText(R.id.w_last,
                last?.let { m ->
                    (m.nodeName.ifEmpty { "" }.let { if (it.isEmpty()) "" else "$it: " }) +
                        m.text.take(60)
                } ?: "")

            val pi = PendingIntent.getActivity(context, 0,
                Intent(context, MainActivity::class.java),
                PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)
            rv.setOnClickPendingIntent(R.id.w_root, pi)
            return rv
        }
    }
}
