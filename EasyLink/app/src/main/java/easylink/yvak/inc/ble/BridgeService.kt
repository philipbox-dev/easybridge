package easylink.yvak.inc.ble

import android.annotation.SuppressLint
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.os.Build
import android.os.IBinder
import androidx.core.app.RemoteInput
import easylink.yvak.inc.Bridge
import easylink.yvak.inc.NotifHelper
import easylink.yvak.inc.proto.ConnState
import easylink.yvak.inc.proto.Proto
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

/**
 * Foreground-сервис: держит процесс с BLE-соединением живым,
 * шлёт GPS-маячки [LOC:], обрабатывает ответ из шторки.
 */
class BridgeService : Service() {

    companion object {
        const val ACTION_REPLY = "easylink.yvak.inc.REPLY"

        fun start(context: Context) {
            context.startForegroundService(Intent(context, BridgeService::class.java))
        }

        fun stop(context: Context) {
            context.stopService(Intent(context, BridgeService::class.java))
        }
    }

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private var locListener: LocationListener? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        startForegroundCompat()

        // Обновляем текст уведомления по состоянию соединения
        scope.launch {
            Bridge.ble.connState.collect { st ->
                val name = if (st == ConnState.CONNECTED)
                    Bridge.ble.deviceName.value else ""
                try {
                    startForegroundCompat(name)
                } catch (_: Exception) {}
            }
        }

        // GPS-маячок: включён в настройках + подключены
        scope.launch {
            combine(Bridge.prefs.locEnabled.flow, Bridge.ble.connState) { en, st ->
                en && st == ConnState.CONNECTED
            }.collect { active ->
                if (active) startLocation() else stopLocation()
            }
        }
        scope.launch { beaconLoop() }
    }

    private fun startForegroundCompat(deviceName: String = "") {
        val n = Bridge.notif.serviceNotification(deviceName)
        if (Build.VERSION.SDK_INT >= 34) {
            startForeground(NotifHelper.ID_SERVICE, n,
                ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE or
                    ServiceInfo.FOREGROUND_SERVICE_TYPE_LOCATION or
                    ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE)
        } else {
            startForeground(NotifHelper.ID_SERVICE, n)
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_REPLY) {
            val chatKey = intent.getStringExtra(NotifHelper.EXTRA_CHAT_KEY)
            val text = RemoteInput.getResultsFromIntent(intent)
                ?.getCharSequence(NotifHelper.KEY_REPLY)?.toString()
            if (!chatKey.isNullOrEmpty() && !text.isNullOrBlank()) {
                Bridge.chat.sendText(chatKey, text)
            }
        }
        return START_STICKY
    }

    // ── GPS ───────────────────────────────────────────────────
    @SuppressLint("MissingPermission")
    private fun startLocation() {
        if (locListener != null) return
        val lm = getSystemService(LOCATION_SERVICE) as LocationManager
        val listener = LocationListener { l: Location -> Bridge.location.updateMyLocation(l) }
        locListener = listener
        try {
            lm.requestLocationUpdates(LocationManager.GPS_PROVIDER, 5000L, 5f,
                listener, mainLooper)
            lm.getLastKnownLocation(LocationManager.GPS_PROVIDER)
                ?.let { Bridge.location.updateMyLocation(it) }
        } catch (_: Exception) {
            locListener = null
        }
    }

    private fun stopLocation() {
        val l = locListener ?: return
        locListener = null
        try {
            (getSystemService(LOCATION_SERVICE) as LocationManager).removeUpdates(l)
        } catch (_: Exception) {}
    }

    /** Периодическая отправка своей позиции в эфир (+ маяк #6, dead-man #10). */
    private suspend fun beaconLoop() {
        var lastSent = 0L
        var lastReminder = 0L
        var lastSentLat = Double.NaN
        var lastSentLon = Double.NaN
        while (scope.isActive) {
            delay(1000)
            val now = System.currentTimeMillis()

            // ── #10 Dead-man: не отметился вовремя → авто-SOS ──
            if (Bridge.prefs.deadmanEnabled.value) {
                val deadline = Bridge.prefs.deadmanDeadline.value
                if (deadline > 0 && now >= deadline) {
                    Bridge.fireDeadmanSos()
                } else if (deadline > 0 && deadline - now < 5 * 60_000 &&
                    now - lastReminder > 60_000) {
                    lastReminder = now
                    Bridge.notif.notifyDeadmanReminder((deadline - now) / 60_000 + 1)
                }
            }

            // ── Маяк-позиция ──
            // #6 «маяк потерялся» шлёт даже если обычный маячок выключен.
            val beaconMode = Bridge.prefs.beaconMode.value
            val locOn = Bridge.prefs.locEnabled.value || beaconMode
            if (!locOn) continue
            if (Bridge.ble.connState.value != ConnState.CONNECTED) continue
            if (Bridge.ptt.state.value != easylink.yvak.inc.proto.PttState.Idle) continue
            if (Bridge.image.txState.value is easylink.yvak.inc.repo.ImageRepo.TxState.Sending) continue
            var interval = if (beaconMode)
                Bridge.prefs.beaconEverySec.value.coerceIn(10, 120) * 1000L
            else Bridge.prefs.locIntervalSec.value.coerceIn(10, 60) * 1000L
            val loc = Bridge.location.myLocation.value ?: continue
            if (now - loc.time > 120_000) continue  // фикс протух
            // V1.7 (#У5): умный интервал — стоим на месте → шлём реже в 3 раза
            // (маяк «потерялся» не замедляем — там частота важнее батареи)
            if (!beaconMode && Bridge.prefs.locAdaptive.value && !lastSentLat.isNaN()) {
                val res = FloatArray(1)
                android.location.Location.distanceBetween(
                    lastSentLat, lastSentLon, loc.latitude, loc.longitude, res)
                if (res[0] < 20f) interval *= 3
            }
            if (now - lastSent < interval) continue
            lastSent = now
            lastSentLat = loc.latitude; lastSentLon = loc.longitude
            // #5: при включённом шаринге барометра добавляем высоту тегом
            var text = Bridge.location.formatLocText(loc)
            if (Bridge.prefs.baroShare.value) {
                Bridge.baro.reading.value?.let {
                    text += String.format(java.util.Locale.US, " [ALT:%.0f]", it.altitudeM)
                }
            }
            Bridge.ble.sendJson(Proto.send(Bridge.prefs.nextSeq(), text))
        }
    }

    override fun onDestroy() {
        stopLocation()
        scope.cancel()
        super.onDestroy()
    }
}
