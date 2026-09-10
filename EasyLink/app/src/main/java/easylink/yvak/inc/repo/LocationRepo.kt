package easylink.yvak.inc.repo

import android.location.Location
import easylink.yvak.inc.db.Db
import easylink.yvak.inc.proto.PeerLocation
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch

/**
 * Локации: чужие [LOC:lat,lon] с эфира → карта (мимо чата),
 * своя позиция от GPS (обновляет сервис).
 */
class LocationRepo(private val db: Db, private val scope: CoroutineScope) {

    // [LOC:lat,lon] где угодно в строке (может тянуться хвост [ALT:...])
    private val locRegex = Regex("""\[LOC:(-?\d+(?:\.\d+)?),(-?\d+(?:\.\d+)?)]""")
    // V1.7.6: старые версии (<= 1.7.5) на русской локали слали дробную часть
    // через ЗАПЯТУЮ — [LOC:43,6833567,40,2704983] — 4 группы вместо 2х.
    // Приёмник не парсил, и маячок валился в чат спамом. Терпим legacy-формат.
    private val locRegexComma = Regex("""\[LOC:(-?\d+),(\d+),(-?\d+),(\d+)]""")

    private val _peers = MutableStateFlow<Map<Long, PeerLocation>>(emptyMap())
    val peers: StateFlow<Map<Long, PeerLocation>> get() = _peers

    private val _myLocation = MutableStateFlow<Location?>(null)
    val myLocation: StateFlow<Location?> get() = _myLocation

    /** Хвосты позиций узлов за 24ч (для карты). */
    private val _peerTrails = MutableStateFlow<Map<Long, List<easylink.yvak.inc.db.Db.TrackPoint>>>(emptyMap())
    val peerTrails: StateFlow<Map<Long, List<easylink.yvak.inc.db.Db.TrackPoint>>> get() = _peerTrails

    init {
        scope.launch(Dispatchers.IO) {
            _peers.value = db.locations().associateBy { it.nodeId }
            _peerTrails.value = db.locHist(System.currentTimeMillis() - 24 * 3_600_000L)
        }
    }

    /** true = это была локация, в чат не отдавать. */
    fun tryParseLoc(nodeId: Long, name: String, text: String): Boolean {
        // отдаём в карту только если строка — по сути тег локации (не текст с координатой внутри)
        if (!text.trimStart().startsWith("[LOC:")) return false
        val m = locRegex.find(text)
        val (latS, lonS) = if (m != null) {
            m.groupValues[1] to m.groupValues[2]
        } else {
            val mc = locRegexComma.find(text) ?: return false
            "${mc.groupValues[1]}.${mc.groupValues[2]}" to
                "${mc.groupValues[3]}.${mc.groupValues[4]}"
        }
        val lat = latS.toDoubleOrNull() ?: return true
        val lon = lonS.toDoubleOrNull() ?: return true
        val loc = PeerLocation(nodeId, name, lat, lon, System.currentTimeMillis())
        _peers.value = _peers.value + (nodeId to loc)
        // хвост позиций для карты
        val tp = easylink.yvak.inc.db.Db.TrackPoint(loc.ts, lat, lon, 0.0)
        _peerTrails.value = _peerTrails.value +
            (nodeId to ((_peerTrails.value[nodeId] ?: emptyList()) + tp).takeLast(100))
        scope.launch(Dispatchers.IO) {
            db.upsertLocation(loc)
            db.addLocHist(nodeId, lat, lon, loc.ts)
        }
        return true
    }

    fun updateMyLocation(l: Location) {
        val cur = _myLocation.value
        if (cur == null || l.time >= cur.time) _myLocation.value = l
    }

    // Locale.US обязателен: дефолтный format() на ru-локали ставит запятую
    // в дробях — эфирный формат ломался (см. locRegexComma выше)
    fun formatLocText(l: Location): String =
        String.format(java.util.Locale.US, "[LOC:%.7f,%.7f]", l.latitude, l.longitude)

    // ── UI-подписка на GPS: пока приложение на экране (для карты/радара/прицела),
    // независимо от маячка в сервисе ──
    private var uiListener: android.location.LocationListener? = null

    @android.annotation.SuppressLint("MissingPermission")
    fun startUi(context: android.content.Context) {
        if (uiListener != null) return
        val lm = context.getSystemService(android.content.Context.LOCATION_SERVICE)
            as android.location.LocationManager
        val listener = android.location.LocationListener { l: Location -> updateMyLocation(l) }
        uiListener = listener
        try {
            lm.requestLocationUpdates(android.location.LocationManager.GPS_PROVIDER,
                3000L, 2f, listener, android.os.Looper.getMainLooper())
            if (lm.isProviderEnabled(android.location.LocationManager.NETWORK_PROVIDER)) {
                lm.requestLocationUpdates(android.location.LocationManager.NETWORK_PROVIDER,
                    10_000L, 10f, listener, android.os.Looper.getMainLooper())
            }
            lm.getLastKnownLocation(android.location.LocationManager.GPS_PROVIDER)
                ?.let { updateMyLocation(it) }
            lm.getLastKnownLocation(android.location.LocationManager.NETWORK_PROVIDER)
                ?.let { updateMyLocation(it) }
        } catch (_: Exception) {
            uiListener = null
        }
    }

    fun stopUi(context: android.content.Context) {
        val l = uiListener ?: return
        uiListener = null
        try {
            val lm = context.getSystemService(android.content.Context.LOCATION_SERVICE)
                as android.location.LocationManager
            lm.removeUpdates(l)
        } catch (_: Exception) {}
    }
}
