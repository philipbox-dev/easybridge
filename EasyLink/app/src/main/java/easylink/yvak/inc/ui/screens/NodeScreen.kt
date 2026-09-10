package easylink.yvak.inc.ui.screens

import android.graphics.Paint
import android.location.Location
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.automirrored.filled.Chat
import androidx.compose.material.icons.filled.Block
import androidx.compose.material.icons.filled.Call
import androidx.compose.material.icons.filled.GroupAdd
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.nativeCanvas
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.navigation.NavHostController
import easylink.yvak.inc.Bridge
import easylink.yvak.inc.R
import easylink.yvak.inc.proto.ConnState
import easylink.yvak.inc.proto.Proto
import easylink.yvak.inc.proto.hwName
import kotlinx.coroutines.delay
import kotlin.math.cos
import kotlin.math.min
import kotlin.math.sin

/** Карточка узла: sigtrace (график RSSI) + прицел антенны + действия. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun NodeScreen(idHex: String, nav: NavHostController) {
    val id = idHex.removePrefix("0x").toLongOrNull(16) ?: 0L
    val nodes by Bridge.nodes.nodes.collectAsState()
    val hist by Bridge.nodes.rssiHist.collectAsState()
    val peers by Bridge.location.peers.collectAsState()
    val myLoc by Bridge.location.myLocation.collectAsState()
    val connState by Bridge.ble.connState.collectAsState()
    val node = nodes[id]
    val samples = hist[id] ?: emptyList()
    val peerLoc = peers[id]

    // Пока экран открыт — опрашиваем nodes для свежего RSSI.
    // НО НЕ во время звонка: экран остаётся жить под оверлеем звонка, а
    // 2КБ-дамп в очереди notify душил evt:ptt_audio (глох звук).
    LaunchedEffect(id) {
        while (true) {
            if (Bridge.ble.connState.value == ConnState.CONNECTED &&
                Bridge.ptt.state.value is easylink.yvak.inc.proto.PttState.Idle) {
                Bridge.ble.sendJson(Proto.nodes())
            }
            delay(5000)
        }
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(node?.displayName ?: idHex) },
                navigationIcon = {
                    IconButton(onClick = { nav.popBackStack() }) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, null)
                    }
                },
            )
        },
    ) { pad ->
        Column(
            Modifier
                .fillMaxSize()
                .padding(pad)
                .verticalScroll(rememberScrollState())
                .padding(12.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            // ── Шапка ──
            Card {
                Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Box(
                            Modifier
                                .size(10.dp)
                                .clip(CircleShape)
                                .background(presenceColor(node?.presence ?: 2)))
                        Spacer(Modifier.width(8.dp))
                        Text(node?.displayName ?: idHex,
                            style = MaterialTheme.typography.titleLarge,
                            fontWeight = FontWeight.Bold)
                    }
                    Text("$idHex · ${hwName(node?.hw ?: 0)}",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                    Row(horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                        Text(stringResource(R.string.last_rssi_fmt, node?.rssi ?: 0),
                            fontWeight = FontWeight.Bold)
                        if (node != null && node.batt in 0..100) Text("🔋 ${node.batt}%")
                        if (node != null && node.hops > 0)
                            Text(stringResource(R.string.node_hops_fmt, node.hops))
                    }
                }
            }

            // ── Действия ──
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(onClick = {
                    val key = Bridge.chat.createDm(id, node?.displayName ?: idHex)
                    nav.navigate("chat/$key")
                }) {
                    Icon(Icons.AutoMirrored.Filled.Chat, null)
                    Spacer(Modifier.width(6.dp))
                    Text(stringResource(R.string.write_dm))
                }
                Button(onClick = {
                    PttUi.requestCallTo(id, node?.displayName ?: idHex)
                }) {
                    Icon(Icons.Filled.Call, null)
                    Spacer(Modifier.width(6.dp))
                    Text(stringResource(R.string.call_action))
                }
                OutlinedButton(onClick = {
                    if (node?.blocked == true) Bridge.nodes.unblock(id)
                    else Bridge.nodes.block(id)
                }) {
                    Icon(Icons.Filled.Block, null,
                        tint = MaterialTheme.colorScheme.error)
                }
            }
            if (peerLoc != null) {
                OutlinedButton(onClick = { nav.navigate("route/$idHex") }) {
                    Text(stringResource(R.string.route_to_node))
                }
            }

            // ── Sigtrace ──
            Card {
                Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(stringResource(R.string.sigtrace_title),
                        style = MaterialTheme.typography.titleMedium)
                    if (samples.size < 2) {
                        Text(stringResource(R.string.sigtrace_empty),
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant)
                    } else {
                        SigTrace(samples, Modifier
                            .fillMaxWidth()
                            .height(160.dp))
                    }
                }
            }

            // ── Прицел ──
            Card {
                Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(stringResource(R.string.aim_title),
                        style = MaterialTheme.typography.titleMedium)
                    when {
                        peerLoc == null -> Text(stringResource(R.string.aim_no_node_gps),
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant)
                        myLoc == null -> Text(stringResource(R.string.aim_no_my_gps),
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant)
                        else -> {
                            val me = myLoc!!
                            val res = FloatArray(2)
                            Location.distanceBetween(me.latitude, me.longitude,
                                peerLoc.lat, peerLoc.lon, res)
                            AimCompass(bearing = res[1],
                                modifier = Modifier
                                    .fillMaxWidth()
                                    .height(220.dp))
                            val d = if (res[0] >= 1000) "%.1f км".format(res[0] / 1000)
                            else "${res[0].toInt()} м"
                            Text(stringResource(R.string.aim_dist_fmt, d),
                                fontWeight = FontWeight.Bold)
                            Text(stringResource(R.string.aim_hint),
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant)
                        }
                    }
                }
            }
        }
    }
}

/** График RSSI по времени. */
@Composable
private fun SigTrace(samples: List<Pair<Long, Int>>, modifier: Modifier) {
    val primary = MaterialTheme.colorScheme.primary
    val outline = MaterialTheme.colorScheme.outline
    Canvas(modifier) {
        val minR = (samples.minOf { it.second } - 5).coerceAtMost(-40)
        val maxR = (samples.maxOf { it.second } + 5).coerceAtLeast(-30)
        val t0 = samples.first().first
        val t1 = samples.last().first.coerceAtLeast(t0 + 1)

        fun x(ts: Long) = size.width * (ts - t0).toFloat() / (t1 - t0).toFloat()
        fun y(rssi: Int) = size.height * (1f - (rssi - minR).toFloat() / (maxR - minR).toFloat())

        // сетка
        val paint = Paint().apply {
            color = android.graphics.Color.GRAY; textSize = 26f; isAntiAlias = true
        }
        for (level in listOf(maxR, (maxR + minR) / 2, minR)) {
            val yy = y(level)
            drawLine(outline.copy(alpha = 0.3f), Offset(0f, yy), Offset(size.width, yy), 1.5f)
            drawContext.canvas.nativeCanvas.drawText("$level", 4f, yy - 6f, paint)
        }

        val path = Path()
        samples.forEachIndexed { i, (ts, rssi) ->
            if (i == 0) path.moveTo(x(ts), y(rssi)) else path.lineTo(x(ts), y(rssi))
        }
        drawPath(path, primary, style = Stroke(4f))
        samples.lastOrNull()?.let { (ts, rssi) ->
            drawCircle(primary, 8f, Offset(x(ts), y(rssi)))
        }
    }
}

/** Компас-стрелка на узел: вверх = куда смотрит телефон. */
@Composable
private fun AimCompass(bearing: Float, modifier: Modifier) {
    val azimuth = rememberAzimuth()
    val primary = MaterialTheme.colorScheme.primary
    val outline = MaterialTheme.colorScheme.outline

    Canvas(modifier) {
        val c = Offset(size.width / 2, size.height / 2)
        val r = min(size.width, size.height) / 2 - 20f
        drawCircle(outline.copy(alpha = 0.4f), r, c, style = Stroke(3f))

        // стрелка на узел (экранный угол: bearing - azimuth, 0 = вверх)
        val a = Math.toRadians((bearing - azimuth - 90f).toDouble())
        val tip = Offset(c.x + (r * 0.85f * cos(a)).toFloat(),
            c.y + (r * 0.85f * sin(a)).toFloat())
        val back = Math.toRadians((bearing - azimuth + 90f).toDouble())
        val tail = Offset(c.x + (r * 0.25f * cos(back)).toFloat(),
            c.y + (r * 0.25f * sin(back)).toFloat())
        val left = Math.toRadians((bearing - azimuth - 90f + 150f).toDouble())
        val right = Math.toRadians((bearing - azimuth - 90f - 150f).toDouble())
        val wingL = Offset(c.x + (r * 0.35f * cos(left)).toFloat(),
            c.y + (r * 0.35f * sin(left)).toFloat())
        val wingR = Offset(c.x + (r * 0.35f * cos(right)).toFloat(),
            c.y + (r * 0.35f * sin(right)).toFloat())

        val path = Path().apply {
            moveTo(tip.x, tip.y)
            lineTo(wingL.x, wingL.y)
            lineTo(tail.x, tail.y)
            lineTo(wingR.x, wingR.y)
            close()
        }
        drawPath(path, primary)
        drawCircle(primary.copy(alpha = 0.4f), 8f, c)
    }
}
