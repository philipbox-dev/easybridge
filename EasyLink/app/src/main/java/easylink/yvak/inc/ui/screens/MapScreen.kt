package easylink.yvak.inc.ui.screens

import android.content.Context
import android.graphics.Paint
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.location.Location
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Download
import androidx.compose.material.icons.filled.MyLocation
import androidx.compose.material3.FloatingActionButton
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.nativeCanvas
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.compose.ui.zIndex
import easylink.yvak.inc.Bridge
import easylink.yvak.inc.R
import easylink.yvak.inc.proto.PeerLocation
import org.osmdroid.tileprovider.tilesource.TileSourceFactory
import org.osmdroid.util.GeoPoint
import org.osmdroid.views.MapView
import org.osmdroid.views.overlay.Marker
import kotlin.math.cos
import kotlin.math.min
import kotlin.math.sin

@Composable
fun MapScreen() {
    var radar by remember { mutableStateOf(false) }
    val myLoc by Bridge.location.myLocation.collectAsState()
    val mapRef = remember { mutableStateOf<MapView?>(null) }

    Box(Modifier.fillMaxSize()) {
        if (radar) RadarView(Modifier.fillMaxSize())
        else MapContent(interactive = true, modifier = Modifier.fillMaxSize(), mapRef = mapRef)

        // ── Тоггл поверх карты ──
        Surface(
            Modifier
                .align(Alignment.TopCenter)
                .padding(top = 10.dp)
                .zIndex(2f),
            shape = MaterialTheme.shapes.extraLarge,
            tonalElevation = 4.dp,
            shadowElevation = 4.dp,
        ) {
            SingleChoiceSegmentedButtonRow(Modifier.padding(6.dp)) {
                SegmentedButton(
                    selected = !radar, onClick = { radar = false },
                    shape = SegmentedButtonDefaults.itemShape(0, 2),
                ) { Text(stringResource(R.string.map_mode)) }
                SegmentedButton(
                    selected = radar, onClick = { radar = true },
                    shape = SegmentedButtonDefaults.itemShape(1, 2),
                ) { Text(stringResource(R.string.radar_mode)) }
            }
        }

        // ── Кнопки: моя локация + скачать область ──
        if (!radar) {
            FloatingActionButton(
                onClick = {
                    val l = myLoc ?: return@FloatingActionButton
                    mapRef.value?.controller?.animateTo(
                        GeoPoint(l.latitude, l.longitude), 16.0, 700L)
                },
                modifier = Modifier
                    .align(Alignment.BottomEnd)
                    .padding(16.dp)
                    .zIndex(2f),
            ) {
                Icon(Icons.Filled.MyLocation, stringResource(R.string.map_my_location))
            }
            OfflineDownloadFab(
                mapRef = mapRef,
                modifier = Modifier
                    .align(Alignment.BottomStart)
                    .padding(16.dp)
                    .zIndex(2f),
            )
        }
    }
}

/** #Ф1 Скачивание тайлов видимой области для офлайна. */
@Composable
private fun OfflineDownloadFab(
    mapRef: androidx.compose.runtime.MutableState<MapView?>,
    modifier: Modifier = Modifier,
) {
    var confirm by remember { mutableStateOf(false) }
    var progress by remember { mutableStateOf(-1) }   // -1 нет, 0..100 идёт
    var resultMsg by remember { mutableStateOf<Int?>(null) }

    Box(modifier) {
        androidx.compose.material3.SmallFloatingActionButton(onClick = { confirm = true }) {
            Icon(Icons.Filled.Download, stringResource(R.string.map_download))
        }
    }

    if (confirm) {
        androidx.compose.material3.AlertDialog(
            onDismissRequest = { confirm = false },
            title = { Text(stringResource(R.string.map_download)) },
            text = { Text(stringResource(R.string.map_download_confirm)) },
            confirmButton = {
                androidx.compose.material3.TextButton(onClick = {
                    confirm = false
                    val map = mapRef.value ?: return@TextButton
                    try {
                        val cm = org.osmdroid.tileprovider.cachemanager.CacheManager(map)
                        val bb = map.boundingBox
                        val zMin = map.zoomLevelDouble.toInt().coerceIn(8, 15)
                        val zMax = (zMin + 3).coerceAtMost(16)
                        progress = 0
                        cm.downloadAreaAsyncNoUI(map.context, bb, zMin, zMax,
                            object : org.osmdroid.tileprovider.cachemanager.CacheManager.CacheManagerCallback {
                                override fun onTaskComplete() {
                                    progress = -1; resultMsg = R.string.map_download_done
                                }
                                override fun onTaskFailed(errors: Int) {
                                    progress = -1
                                    resultMsg = if (errors > 0) R.string.map_download_fail
                                    else R.string.map_download_done
                                }
                                override fun updateProgress(p: Int, currentZoomLevel: Int,
                                                            zoomMin: Int, zoomMax: Int) {
                                    progress = p
                                }
                                override fun downloadStarted() {}
                                override fun setPossibleTilesInArea(total: Int) {}
                            })
                    } catch (_: Exception) {
                        progress = -1; resultMsg = R.string.map_download_fail
                    }
                }) { Text(stringResource(R.string.ok)) }
            },
            dismissButton = {
                androidx.compose.material3.TextButton(onClick = { confirm = false }) {
                    Text(stringResource(R.string.cancel))
                }
            },
        )
    }

    if (progress >= 0) {
        Surface(
            shape = MaterialTheme.shapes.large,
            tonalElevation = 6.dp,
            modifier = Modifier.padding(top = 56.dp),
        ) {
            Text(stringResource(R.string.map_download_progress_fmt, progress),
                Modifier.padding(10.dp),
                style = MaterialTheme.typography.labelMedium)
        }
    }
    resultMsg?.let {
        androidx.compose.material3.AlertDialog(
            onDismissRequest = { resultMsg = null },
            text = { Text(stringResource(it)) },
            confirmButton = {
                androidx.compose.material3.TextButton(onClick = { resultMsg = null }) {
                    Text(stringResource(R.string.ok))
                }
            },
        )
    }
}

/** Карта OSM с узлами. Общая для превью на Home и полного экрана. */
@Composable
fun MapContent(
    interactive: Boolean,
    modifier: Modifier = Modifier,
    mapRef: androidx.compose.runtime.MutableState<MapView?>? = null,
) {
    val peers by Bridge.location.peers.collectAsState()
    val myLoc by Bridge.location.myLocation.collectAsState()
    var centeredOnce by remember { mutableStateOf(false) }

    // Автоцентр на первый GPS-фикс
    LaunchedEffect(myLoc != null) {
        val l = myLoc
        if (l != null && !centeredOnce) {
            centeredOnce = true
            mapRef?.value?.controller?.animateTo(GeoPoint(l.latitude, l.longitude), 15.0, 500L)
        }
    }

    AndroidView(
        modifier = modifier,
        factory = { ctx ->
            MapView(ctx).apply {
                setTileSource(TileSourceFactory.MAPNIK)
                setMultiTouchControls(interactive)
                zoomController.setVisibility(
                    org.osmdroid.views.CustomZoomButtonsController.Visibility.NEVER)
                controller.setZoom(if (myLoc != null) 15.0 else 13.0)
                val start = myLoc?.let { GeoPoint(it.latitude, it.longitude) }
                    ?: peers.values.firstOrNull()?.let { GeoPoint(it.lat, it.lon) }
                    ?: GeoPoint(55.751, 37.618)
                controller.setCenter(start)
                if (myLoc != null) centeredOnce = true
                if (!interactive) {
                    setOnTouchListener { _, _ -> true }
                }
                mapRef?.value = this
            }
        },
        update = { map ->
            map.overlays.clear()
            // #У2 Хвосты позиций узлов (последние точки за 24ч)
            for ((_, trail) in Bridge.location.peerTrails.value) {
                if (trail.size < 2) continue
                val line = org.osmdroid.views.overlay.Polyline()
                line.setPoints(trail.takeLast(50).map { GeoPoint(it.lat, it.lon) })
                line.outlinePaint.color = android.graphics.Color.argb(110, 13, 98, 184)
                line.outlinePaint.strokeWidth = 6f
                map.overlays.add(line)
            }
            myLoc?.let { l ->
                val m = Marker(map)
                m.position = GeoPoint(l.latitude, l.longitude)
                m.setAnchor(Marker.ANCHOR_CENTER, Marker.ANCHOR_BOTTOM)
                m.title = map.context.getString(R.string.map_me)
                map.overlays.add(m)
            }
            for (p in peers.values) {
                val m = Marker(map)
                m.position = GeoPoint(p.lat, p.lon)
                m.setAnchor(Marker.ANCHOR_CENTER, Marker.ANCHOR_BOTTOM)
                m.title = p.name.ifEmpty { "0x%08x".format(p.nodeId) }
                m.snippet = ageText(map.context, p.ts)
                map.overlays.add(m)
            }
            map.invalidate()
        },
    )
}

private fun ageText(ctx: Context, ts: Long): String {
    val s = (System.currentTimeMillis() - ts) / 1000
    val txt = when {
        s < 60 -> "${s}s"
        s < 3600 -> "${s / 60}m"
        else -> "${s / 3600}h"
    }
    return ctx.getString(R.string.loc_age_fmt, txt)
}

/** Азимут устройства (градусы от севера) с датчика rotation vector. */
@Composable
fun rememberAzimuth(): Float {
    val ctx = LocalContext.current
    var azimuth by remember { mutableFloatStateOf(0f) }
    DisposableEffect(Unit) {
        val sm = ctx.getSystemService(Context.SENSOR_SERVICE) as SensorManager
        val sensor = sm.getDefaultSensor(Sensor.TYPE_ROTATION_VECTOR)
        val listener = object : SensorEventListener {
            private val rot = FloatArray(9)
            private val orient = FloatArray(3)
            override fun onSensorChanged(e: SensorEvent) {
                SensorManager.getRotationMatrixFromVector(rot, e.values)
                SensorManager.getOrientation(rot, orient)
                azimuth = Math.toDegrees(orient[0].toDouble()).toFloat()
            }
            override fun onAccuracyChanged(s: Sensor?, a: Int) {}
        }
        if (sensor != null) {
            sm.registerListener(listener, sensor, SensorManager.SENSOR_DELAY_UI)
        }
        onDispose { sm.unregisterListener(listener) }
    }
    return azimuth
}

/** Круглые шаги масштаба радара. */
private val RADAR_SCALES = floatArrayOf(
    150f, 300f, 600f, 1500f, 3000f, 6000f, 15000f, 30000f, 60000f)

/**
 * Радар: узлы относительно меня, крутится по компасу (куда смотришь — то сверху),
 * масштаб подбирается автоматически под самый дальний узел.
 */
@Composable
fun RadarView(modifier: Modifier = Modifier) {
    val peers by Bridge.location.peers.collectAsState()
    val myLoc by Bridge.location.myLocation.collectAsState()
    val azimuth = rememberAzimuth()
    val primary = MaterialTheme.colorScheme.primary
    val onSurface = MaterialTheme.colorScheme.onSurface
    val outline = MaterialTheme.colorScheme.outline

    val me = myLoc
    if (me == null) {
        Box(modifier, contentAlignment = Alignment.Center) {
            Text(stringResource(R.string.radar_need_gps))
        }
        return
    }

    data class P(val name: String, val distM: Float, val bearing: Float)
    val pts = peers.values.map { p: PeerLocation ->
        val res = FloatArray(2)
        Location.distanceBetween(me.latitude, me.longitude, p.lat, p.lon, res)
        P(p.name.ifEmpty { "0x%08x".format(p.nodeId) }, res[0], res[1])
    }
    val maxDist = pts.maxOfOrNull { it.distM } ?: 0f
    val scale = RADAR_SCALES.firstOrNull { it >= maxDist * 1.1f } ?: RADAR_SCALES.last()

    Canvas(modifier.padding(16.dp)) {
        val c = Offset(size.width / 2, size.height / 2)
        val r = min(size.width, size.height) / 2 - 40f

        val paint = Paint().apply {
            color = android.graphics.Color.GRAY
            textSize = 28f
            isAntiAlias = true
        }
        // кольца с подписями дистанции
        for (i in 1..3) {
            val rr = r * i / 3f
            drawCircle(outline.copy(alpha = 0.5f), rr, c, style = Stroke(2f))
            val distLabel = scale * i / 3f
            val label = if (distLabel >= 1000) "%.1f км".format(distLabel / 1000)
            else "${distLabel.toInt()} м"
            drawContext.canvas.nativeCanvas.drawText(label, c.x + 8f, c.y - rr + 32f, paint)
        }

        // Компас: экран повёрнут так, что «куда смотрю» = вверх.
        // Мировой угол → экранный: screen = bearing - azimuth - 90°.
        fun screenXY(bearingDeg: Float, rr: Float): Offset {
            val a = Math.toRadians((bearingDeg - azimuth - 90f).toDouble())
            return Offset(c.x + (rr * cos(a)).toFloat(), c.y + (rr * sin(a)).toFloat())
        }

        // стороны света на ободе
        val nPaint = Paint().apply {
            textSize = 34f; isAntiAlias = true; textAlign = Paint.Align.CENTER
            color = android.graphics.Color.rgb(
                (primary.red * 255).toInt(), (primary.green * 255).toInt(),
                (primary.blue * 255).toInt())
        }
        val gray = Paint(nPaint).apply { color = android.graphics.Color.GRAY }
        for ((label, deg, p) in listOf(
            Triple("N", 0f, nPaint), Triple("E", 90f, gray),
            Triple("S", 180f, gray), Triple("W", 270f, gray))) {
            val pos = screenXY(deg, r + 26f)
            drawContext.canvas.nativeCanvas.drawText(label, pos.x, pos.y + 10f, p)
        }

        // курс — луч вверх
        drawLine(primary.copy(alpha = 0.35f), c, Offset(c.x, c.y - r), 3f)

        // я в центре
        drawCircle(primary, 14f, c)

        // узлы
        val namePaint = Paint().apply {
            textSize = 32f
            isAntiAlias = true
            color = android.graphics.Color.rgb(
                (onSurface.red * 255).toInt(), (onSurface.green * 255).toInt(),
                (onSurface.blue * 255).toInt())
        }
        for (p in pts) {
            val rr = r * (p.distM / scale).coerceAtMost(1f)
            val pos = screenXY(p.bearing, rr)
            drawCircle(Color(0xFFFF7043), 12f, pos)
            drawContext.canvas.nativeCanvas.drawText(p.name, pos.x + 16f, pos.y + 10f, namePaint)
            val d = if (p.distM >= 1000) "%.1fкм".format(p.distM / 1000)
            else "${p.distM.toInt()}м"
            drawContext.canvas.nativeCanvas.drawText(d, pos.x + 16f, pos.y + 44f, paint)
        }
    }
}
