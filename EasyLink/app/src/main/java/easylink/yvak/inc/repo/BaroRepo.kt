package easylink.yvak.inc.repo

import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import easylink.yvak.inc.Prefs
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

/**
 * Барометр телефона: высота (по давлению) и тренд давления (к погоде).
 * Не у всех телефонов есть датчик — тогда available=false.
 */
class BaroRepo(private val context: Context, private val prefs: Prefs) {

    data class Reading(
        val pressureHpa: Float,
        val altitudeM: Float,
        /** гПа за последний час: <-1 портится, >+1 улучшается. */
        val trendHpaPerHour: Float,
    )

    private val sm = context.getSystemService(Context.SENSOR_SERVICE) as SensorManager
    private val sensor: Sensor? = sm.getDefaultSensor(Sensor.TYPE_PRESSURE)
    val available: Boolean get() = sensor != null

    private val _reading = MutableStateFlow<Reading?>(null)
    val reading: StateFlow<Reading?> get() = _reading

    // (ts, pressure) для тренда
    private val history = ArrayDeque<Pair<Long, Float>>()

    private val listener = object : SensorEventListener {
        override fun onSensorChanged(e: SensorEvent) {
            val p = e.values[0]
            val alt = SensorManager.getAltitude(prefs.baroSeaLevelHpa.value, p)
            val now = System.currentTimeMillis()
            history.addLast(now to p)
            while (history.isNotEmpty() && now - history.first().first > 3_600_000) {
                history.removeFirst()
            }
            val trend = if (history.size >= 2) {
                val (t0, p0) = history.first()
                val dtH = (now - t0) / 3_600_000f
                if (dtH > 0.02f) (p - p0) / dtH else 0f
            } else 0f
            _reading.value = Reading(p, alt, trend)
        }
        override fun onAccuracyChanged(s: Sensor?, a: Int) {}
    }

    private var started = false
    fun start() {
        if (started || sensor == null) return
        started = true
        sm.registerListener(listener, sensor, SensorManager.SENSOR_DELAY_NORMAL)
    }

    fun stop() {
        if (!started) return
        started = false
        sm.unregisterListener(listener)
    }

    /** Строка тренда для UI. */
    fun trendLabel(trend: Float): String = when {
        trend < -1.5f -> "↓↓"
        trend < -0.4f -> "↓"
        trend > 1.5f -> "↑↑"
        trend > 0.4f -> "↑"
        else -> "→"
    }
}
