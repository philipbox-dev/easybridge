package easylink.yvak.inc.repo

import android.content.Context
import android.location.Location
import easylink.yvak.inc.Prefs
import easylink.yvak.inc.db.Db
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import java.io.File

/**
 * #2 Запись трека: собственный GPS-трек по ходу движения.
 * Точка пишется, когда сместились дальше trackMinMeters от прошлой.
 * Экспорт в GPX для шаринга.
 */
class TrackRepo(
    private val context: Context,
    private val db: Db,
    private val prefs: Prefs,
    private val scope: CoroutineScope,
) {
    private val _points = MutableStateFlow<List<Db.TrackPoint>>(emptyList())
    val points: StateFlow<List<Db.TrackPoint>> get() = _points

    private val _distanceM = MutableStateFlow(0.0)
    val distanceM: StateFlow<Double> get() = _distanceM

    private var lastLat = Double.NaN
    private var lastLon = Double.NaN

    init {
        scope.launch(Dispatchers.IO) {
            val pts = db.trackPoints()
            _points.value = pts
            _distanceM.value = totalDistance(pts)
            pts.lastOrNull()?.let { lastLat = it.lat; lastLon = it.lon }
        }
    }

    fun onLocation(l: Location) {
        if (!prefs.trackEnabled.value) return
        val minM = prefs.trackMinMeters.value.coerceIn(5, 200)
        if (!lastLat.isNaN()) {
            val res = FloatArray(1)
            Location.distanceBetween(lastLat, lastLon, l.latitude, l.longitude, res)
            if (res[0] < minM) return
            _distanceM.value += res[0]
        }
        lastLat = l.latitude; lastLon = l.longitude
        val ts = System.currentTimeMillis()
        scope.launch(Dispatchers.IO) {
            db.addTrackPoint(ts, l.latitude, l.longitude, l.altitude)
            _points.value = _points.value + Db.TrackPoint(ts, l.latitude, l.longitude, l.altitude)
        }
    }

    fun clear() {
        scope.launch(Dispatchers.IO) {
            db.clearTrack()
            _points.value = emptyList()
            _distanceM.value = 0.0
            lastLat = Double.NaN; lastLon = Double.NaN
        }
    }

    private fun totalDistance(pts: List<Db.TrackPoint>): Double {
        var d = 0.0
        for (i in 1 until pts.size) {
            val r = FloatArray(1)
            Location.distanceBetween(pts[i - 1].lat, pts[i - 1].lon,
                pts[i].lat, pts[i].lon, r)
            d += r[0]
        }
        return d
    }

    /** GPX-файл для «Поделиться». */
    fun exportGpx(): File? {
        val pts = _points.value
        if (pts.isEmpty()) return null
        val sb = StringBuilder()
        sb.append("<?xml version=\"1.0\"?>\n")
        sb.append("<gpx version=\"1.1\" creator=\"EasyLink\"><trk><name>EasyLink track</name><trkseg>\n")
        for (p in pts) {
            sb.append(String.format(java.util.Locale.US,
                "<trkpt lat=\"%.7f\" lon=\"%.7f\"><ele>%.1f</ele></trkpt>\n",
                p.lat, p.lon, p.alt))
        }
        sb.append("</trkseg></trk></gpx>\n")
        return try {
            val dir = File(context.cacheDir, "share").apply { mkdirs() }
            val f = File(dir, "track_${System.currentTimeMillis()}.gpx")
            f.writeText(sb.toString())
            f
        } catch (_: Exception) { null }
    }
}
