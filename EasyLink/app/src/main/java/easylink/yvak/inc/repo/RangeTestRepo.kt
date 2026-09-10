package easylink.yvak.inc.repo

import android.content.Context
import android.location.Location
import easylink.yvak.inc.Prefs
import easylink.yvak.inc.ble.BleClient
import easylink.yvak.inc.proto.Proto
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.io.File

/**
 * Режим замера дальности: вдвоём расходитесь, приложение пишет трек и
 * пары «расстояние ↔ RSSI» по мере удаления. Показывает максимум дальности,
 * на котором ещё была связь. Требует, чтобы оба слали позицию (маячок) —
 * при старте включаем частую локацию.
 */
class RangeTestRepo(
    private val context: Context,
    private val ble: BleClient,
    private val prefs: Prefs,
    private val scope: CoroutineScope,
    private val nodes: NodeRepo,
    private val location: LocationRepo,
) {
    data class Sample(val ts: Long, val distM: Float, val rssi: Int, val snr: Float,
                      val lat: Double, val lon: Double)

    data class Session(
        val peerId: Long,
        val peerName: String,
        val startedMs: Long,
        val samples: List<Sample>,
        val maxDistM: Float,       // макс дистанция при живой связи
        val lastRssi: Int,
        val lastDistM: Float,
        val connected: Boolean,    // слышали пир за последние N сек
    )

    private val _session = MutableStateFlow<Session?>(null)
    val session: StateFlow<Session?> get() = _session

    val running: Boolean get() = _session.value != null
    private var job: kotlinx.coroutines.Job? = null

    fun start(peerId: Long, peerName: String) {
        if (running) return
        // Включаем частую локацию, чтобы оба видели позицию и ловили RSSI
        prefs.locEnabled.value = true
        _session.value = Session(peerId, peerName, System.currentTimeMillis(),
            emptyList(), 0f, 0, 0f, false)
        job = scope.launch {
            var lastRssiSeen = 0L
            while (isActive) {
                val pingSec = prefs.rtPingEverySec.value.coerceIn(3, 60)
                // Пингуем пир, чтобы он ответил (PONG обновит RSSI) + шлём свою позицию
                location.myLocation.value?.let {
                    ble.sendJson(Proto.send(prefs.nextSeq(), location.formatLocText(it)))
                }
                sample(peerId)
                // считаем «в связи», если RSSI-история пира пополнялась недавно
                val hist = nodes.rssiHist.value[peerId]
                if (hist != null && hist.isNotEmpty()) lastRssiSeen = hist.last().first
                delay(pingSec * 1000L)
            }
        }
    }

    private fun sample(peerId: Long) {
        val me = location.myLocation.value ?: return
        val peer = location.peers.value[peerId]
        val hist = nodes.rssiHist.value[peerId]
        val rssi = hist?.lastOrNull()?.second ?: (nodes.nodes.value[peerId]?.rssi ?: 0)
        val node = nodes.nodes.value[peerId]
        val snr = 0f
        val dist: Float = if (peer != null) {
            val r = FloatArray(1)
            Location.distanceBetween(me.latitude, me.longitude, peer.lat, peer.lon, r)
            r[0]
        } else 0f

        val cur = _session.value ?: return
        val connected = node != null &&
            System.currentTimeMillis() - node.lastSeenMs < 30_000
        val s = Sample(System.currentTimeMillis(), dist, rssi, snr, me.latitude, me.longitude)
        val newSamples = cur.samples + s
        val newMax = if (connected && dist > cur.maxDistM) dist else cur.maxDistM
        _session.value = cur.copy(
            samples = newSamples, maxDistM = newMax,
            lastRssi = rssi, lastDistM = dist, connected = connected)
    }

    fun stop() {
        job?.cancel(); job = null
    }

    fun close() {
        stop()
        _session.value = null
    }

    /** CSV для «Поделиться»/анализа. */
    fun exportCsv(): File? {
        val s = _session.value ?: return null
        val sb = StringBuilder("ts,dist_m,rssi_dbm,lat,lon\n")
        for (x in s.samples) {
            sb.append(String.format(java.util.Locale.US,
                "%d,%.0f,%d,%.6f,%.6f\n", x.ts, x.distM, x.rssi, x.lat, x.lon))
        }
        return try {
            val dir = File(context.cacheDir, "share").apply { mkdirs() }
            val f = File(dir, "rangetest_${System.currentTimeMillis()}.csv")
            f.writeText(sb.toString())
            f
        } catch (_: Exception) { null }
    }
}
