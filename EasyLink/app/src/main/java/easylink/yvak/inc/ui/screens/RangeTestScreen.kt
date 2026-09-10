package easylink.yvak.inc.ui.screens

import android.graphics.Paint
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
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
import androidx.compose.material.icons.filled.Share
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.nativeCanvas
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.navigation.NavHostController
import easylink.yvak.inc.Bridge
import easylink.yvak.inc.R
import kotlin.math.min

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun RangeTestScreen(nav: NavHostController) {
    val session by Bridge.rangeTest.session.collectAsState()
    val nodes by Bridge.nodes.nodes.collectAsState()
    val ctx = LocalContext.current
    var picked by remember { mutableStateOf<Pair<Long, String>?>(null) }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(stringResource(R.string.rangetest_title)) },
                navigationIcon = {
                    IconButton(onClick = {
                        Bridge.rangeTest.close(); nav.popBackStack()
                    }) { Icon(Icons.AutoMirrored.Filled.ArrowBack, null) }
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
            val s = session
            if (s == null) {
                // ── Мастер настройки ──
                Card {
                    Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        Text(stringResource(R.string.rangetest_intro),
                            style = MaterialTheme.typography.bodyMedium)
                        Text(stringResource(R.string.rangetest_pick),
                            style = MaterialTheme.typography.titleSmall)
                        if (nodes.isEmpty()) {
                            Text(stringResource(R.string.rangetest_need_peer),
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant)
                        }
                        for (n in nodes.values.sortedBy { it.presence }) {
                            Row(
                                Modifier
                                    .fillMaxWidth()
                                    .clickable { picked = n.id to n.displayName }
                                    .padding(vertical = 8.dp),
                                verticalAlignment = Alignment.CenterVertically,
                            ) {
                                Box(
                                    Modifier
                                        .size(10.dp)
                                        .clip(CircleShape)
                                        .background(
                                            if (picked?.first == n.id)
                                                MaterialTheme.colorScheme.primary
                                            else presenceColor(n.presence)))
                                Spacer(Modifier.width(8.dp))
                                Text(n.displayName,
                                    fontWeight = if (picked?.first == n.id)
                                        FontWeight.Bold else FontWeight.Normal)
                            }
                        }

                        val ping by Bridge.prefs.rtPingEverySec.flow.collectAsState()
                        Text(stringResource(R.string.rangetest_ping) + ": " +
                            stringResource(R.string.rangetest_ping_fmt, ping),
                            style = MaterialTheme.typography.labelMedium)
                        Slider(value = ping.toFloat(),
                            onValueChange = { Bridge.prefs.rtPingEverySec.value = it.toInt() },
                            valueRange = 3f..60f, steps = 56)

                        Button(
                            onClick = { picked?.let { Bridge.rangeTest.start(it.first, it.second) } },
                            enabled = picked != null,
                            modifier = Modifier.fillMaxWidth(),
                        ) { Text(stringResource(R.string.rangetest_start)) }
                    }
                }
            } else {
                // ── Живой замер ──
                Card {
                    Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                        Text(s.peerName, style = MaterialTheme.typography.titleLarge,
                            fontWeight = FontWeight.Bold)
                        Text(
                            stringResource(if (s.connected) R.string.rangetest_connected
                            else R.string.rangetest_lost),
                            color = if (s.connected) Color(0xFF2E7D32)
                            else MaterialTheme.colorScheme.error,
                            fontWeight = FontWeight.Bold)
                        Text(stringResource(R.string.rangetest_cur_fmt,
                            fmtDist(s.lastDistM), s.lastRssi),
                            style = MaterialTheme.typography.titleMedium)
                        Text(stringResource(R.string.rangetest_max_fmt, fmtDist(s.maxDistM)),
                            style = MaterialTheme.typography.titleMedium,
                            color = MaterialTheme.colorScheme.primary,
                            fontWeight = FontWeight.Bold)
                        Text(stringResource(R.string.rangetest_samples_fmt, s.samples.size),
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                }

                Card {
                    Column(Modifier.padding(12.dp)) {
                        Text("dBm ↔ м", style = MaterialTheme.typography.titleSmall)
                        RangeChart(s.samples, Modifier
                            .fillMaxWidth()
                            .height(200.dp))
                    }
                }

                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(
                        onClick = { Bridge.rangeTest.stop() },
                        colors = ButtonDefaults.buttonColors(
                            containerColor = MaterialTheme.colorScheme.error),
                    ) { Text(stringResource(R.string.rangetest_stop)) }
                    OutlinedButton(onClick = {
                        Bridge.rangeTest.exportCsv()?.let { shareFile(ctx, it, "text/csv") }
                    }) {
                        Icon(Icons.Filled.Share, null)
                        Spacer(Modifier.width(6.dp))
                        Text(stringResource(R.string.rangetest_export))
                    }
                }
            }
        }
    }
}

private fun fmtDist(m: Float): String =
    if (m >= 1000) "%.2f км".format(m / 1000) else "${m.toInt()} м"

@Composable
private fun RangeChart(
    samples: List<easylink.yvak.inc.repo.RangeTestRepo.Sample>,
    modifier: Modifier,
) {
    val primary = MaterialTheme.colorScheme.primary
    val outline = MaterialTheme.colorScheme.outline
    if (samples.size < 2) {
        Box(modifier, contentAlignment = Alignment.Center) {
            Text("…", color = outline)
        }
        return
    }
    Canvas(modifier.padding(top = 8.dp)) {
        val pts = samples.filter { it.distM > 0 && it.rssi != 0 }
        if (pts.size < 2) return@Canvas
        val maxDist = pts.maxOf { it.distM }.coerceAtLeast(50f)
        val minRssi = (pts.minOf { it.rssi } - 5).coerceAtMost(-120)
        val maxRssi = (pts.maxOf { it.rssi } + 5).coerceAtLeast(-30)

        fun x(d: Float) = size.width * (d / maxDist)
        fun y(r: Int) = size.height * (1f - (r - minRssi).toFloat() / (maxRssi - minRssi))

        val paint = Paint().apply {
            color = android.graphics.Color.GRAY; textSize = 24f; isAntiAlias = true
        }
        // оси-подписи
        drawContext.canvas.nativeCanvas.drawText("$maxRssi", 4f, 20f, paint)
        drawContext.canvas.nativeCanvas.drawText("$minRssi", 4f, size.height - 6f, paint)
        drawContext.canvas.nativeCanvas.drawText(fmtDist(maxDist),
            size.width - 90f, size.height - 6f, paint)

        // линия по времени (точки уже по возрастанию времени = удаление)
        val sorted = pts.sortedBy { it.distM }
        val path = androidx.compose.ui.graphics.Path()
        sorted.forEachIndexed { i, p ->
            val px = x(p.distM); val py = y(p.rssi)
            if (i == 0) path.moveTo(px, py) else path.lineTo(px, py)
        }
        drawPath(path, primary, style = Stroke(3f))
        for (p in pts) drawCircle(primary.copy(alpha = 0.5f), 4f, Offset(x(p.distM), y(p.rssi)))
    }
}

fun shareFile(ctx: android.content.Context, f: java.io.File, mime: String) {
    try {
        val uri = androidx.core.content.FileProvider.getUriForFile(
            ctx, ctx.packageName + ".fileprovider", f)
        val i = android.content.Intent(android.content.Intent.ACTION_SEND).apply {
            type = mime
            putExtra(android.content.Intent.EXTRA_STREAM, uri)
            addFlags(android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        ctx.startActivity(android.content.Intent.createChooser(i, null)
            .addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK))
    } catch (_: Exception) {}
}
