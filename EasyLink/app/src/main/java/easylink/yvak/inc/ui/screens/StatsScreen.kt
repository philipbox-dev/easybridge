package easylink.yvak.inc.ui.screens

import android.content.Intent
import android.location.Location
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Share
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.navigation.NavHostController
import easylink.yvak.inc.Bridge
import easylink.yvak.inc.R
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.util.Calendar

/** #Ф4 Итоги дня: трек, сообщения, узлы. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun StatsScreen(nav: NavHostController) {
    val ctx = LocalContext.current
    var distM by remember { mutableStateOf(0.0) }
    var elevM by remember { mutableStateOf(0.0) }
    var durMin by remember { mutableStateOf(0L) }
    var msgs by remember { mutableStateOf(0) }
    var delivered by remember { mutableStateOf(0) }
    var nodesToday by remember { mutableStateOf(0) }
    var hasData by remember { mutableStateOf(false) }

    LaunchedEffect(Unit) {
        withContext(Dispatchers.IO) {
            val dayStart = Calendar.getInstance().apply {
                set(Calendar.HOUR_OF_DAY, 0); set(Calendar.MINUTE, 0)
                set(Calendar.SECOND, 0); set(Calendar.MILLISECOND, 0)
            }.timeInMillis

            val pts = Bridge.track.points.value.filter { it.ts >= dayStart }
            var d = 0.0; var e = 0.0
            for (i in 1 until pts.size) {
                val r = FloatArray(1)
                Location.distanceBetween(pts[i - 1].lat, pts[i - 1].lon,
                    pts[i].lat, pts[i].lon, r)
                d += r[0]
                val dAlt = pts[i].alt - pts[i - 1].alt
                if (dAlt > 2) e += dAlt
            }
            distM = d; elevM = e
            durMin = if (pts.size >= 2) (pts.last().ts - pts.first().ts) / 60000 else 0

            val (t, del) = Bridge.db.outStatsSince(dayStart)
            msgs = t; delivered = del
            nodesToday = Bridge.nodes.nodes.value.values.count { it.lastSeenMs >= dayStart }
            hasData = pts.isNotEmpty() || t > 0 || nodesToday > 0
        }
    }

    fun summaryText(): String {
        val km = "%.2f".format(distM / 1000)
        val pct = if (msgs > 0) (delivered * 100 / msgs) else 0
        return "EasyLink · ${ctx.getString(R.string.stats_title)}\n" +
            "🥾 $km км · ⛰ +${elevM.toInt()} м · ⏱ ${durMin / 60}ч ${durMin % 60}м\n" +
            "✉️ $msgs (${ctx.getString(R.string.stats_delivery)} $pct%) · " +
            "📡 ${ctx.getString(R.string.stats_nodes)}: $nodesToday"
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(stringResource(R.string.stats_title)) },
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
                .padding(12.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            if (!hasData) {
                Text(stringResource(R.string.stats_empty),
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            StatRow("🥾", stringResource(R.string.stats_dist),
                if (distM >= 1000) "%.2f км".format(distM / 1000)
                else "${distM.toInt()} м")
            StatRow("⛰", stringResource(R.string.stats_elev), "+${elevM.toInt()} м")
            StatRow("⏱", stringResource(R.string.stats_time),
                "%dч %02dм".format(durMin / 60, durMin % 60))
            StatRow("✉️", stringResource(R.string.stats_msgs), "$msgs")
            StatRow("✅", stringResource(R.string.stats_delivery),
                if (msgs > 0) "${delivered * 100 / msgs}%" else "—")
            StatRow("📡", stringResource(R.string.stats_nodes), "$nodesToday")

            Spacer(Modifier.weight(1f))
            Button(
                onClick = {
                    val i = Intent(Intent.ACTION_SEND).apply {
                        type = "text/plain"
                        putExtra(Intent.EXTRA_TEXT, summaryText())
                    }
                    ctx.startActivity(Intent.createChooser(i, null))
                },
                modifier = Modifier.fillMaxWidth(),
            ) {
                Icon(Icons.Filled.Share, null)
                Spacer(Modifier.width(8.dp))
                Text(stringResource(R.string.stats_share))
            }
        }
    }
}

@Composable
private fun StatRow(emoji: String, label: String, value: String) {
    Card {
        Row(
            Modifier
                .fillMaxWidth()
                .padding(14.dp),
        ) {
            Text(emoji)
            Spacer(Modifier.width(10.dp))
            Text(label, Modifier.weight(1f))
            Text(value, fontWeight = FontWeight.Bold,
                color = MaterialTheme.colorScheme.primary)
        }
    }
}
