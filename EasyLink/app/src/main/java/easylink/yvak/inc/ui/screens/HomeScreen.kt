package easylink.yvak.inc.ui.screens

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
import androidx.compose.material.icons.filled.Battery5Bar
import androidx.compose.material.icons.filled.Bluetooth
import androidx.compose.material.icons.filled.BluetoothDisabled
import androidx.compose.material.icons.filled.CellTower
import androidx.compose.material.icons.filled.Groups
import androidx.compose.material.icons.filled.Lock
import androidx.compose.material.icons.filled.LockOpen
import androidx.compose.material.icons.filled.RecordVoiceOver
import androidx.compose.material.icons.filled.Speed
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.navigation.NavHostController
import easylink.yvak.inc.Bridge
import easylink.yvak.inc.R
import easylink.yvak.inc.proto.ConnState

@Composable
fun HomeScreen(nav: NavHostController) {
    val connState by Bridge.ble.connState.collectAsState()
    val diag by Bridge.device.diag.collectAsState()
    val nodes by Bridge.nodes.nodes.collectAsState()
    val deviceName by Bridge.ble.deviceName.collectAsState()

    Column(
        Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(12.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        // ── Статус ──
        Card {
            Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    val connected = connState == ConnState.CONNECTED
                    Icon(
                        if (connected) Icons.Filled.Bluetooth else Icons.Filled.BluetoothDisabled,
                        null,
                        tint = if (connected) MaterialTheme.colorScheme.primary
                        else MaterialTheme.colorScheme.error,
                    )
                    Spacer(Modifier.width(8.dp))
                    Text(
                        when (connState) {
                            ConnState.CONNECTED -> deviceName.ifEmpty { stringResource(R.string.conn_connected) }
                            ConnState.CONNECTING -> stringResource(R.string.conn_connecting)
                            ConnState.RECONNECTING -> stringResource(R.string.conn_reconnecting)
                            else -> stringResource(R.string.conn_disconnected)
                        },
                        style = MaterialTheme.typography.titleMedium,
                        modifier = Modifier.weight(1f),
                    )
                    if (connState == ConnState.DISCONNECTED) {
                        Text(
                            stringResource(R.string.connect),
                            color = MaterialTheme.colorScheme.primary,
                            fontWeight = FontWeight.Bold,
                            modifier = Modifier.clickable { nav.navigate("scanner") },
                        )
                    }
                }
                if (connState == ConnState.CONNECTED) {
                    Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                        StatusChip(Icons.Filled.CellTower, "LoRa",
                            if (diag.loraOk) "OK" else "—",
                            ok = diag.loraOk)
                        StatusChip(Icons.Filled.Speed, stringResource(R.string.status_speed),
                            stringResource(if (diag.sf != 0) R.string.speed_fast else R.string.speed_slow))
                        StatusChip(Icons.Filled.Groups, stringResource(R.string.status_nodes),
                            "${diag.nodesOnline}")
                    }
                    Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                        StatusChip(Icons.Filled.Battery5Bar, stringResource(R.string.status_battery),
                            if (diag.batt in 0..100) "${diag.batt}%" else "—")
                        StatusChip(
                            if (diag.enc) Icons.Filled.Lock else Icons.Filled.LockOpen,
                            stringResource(R.string.status_encryption),
                            stringResource(if (diag.enc) R.string.enc_on else R.string.enc_off),
                            ok = diag.enc)
                        StatusChip(Icons.Filled.CellTower, "RSSI", "${diag.rssi}")
                    }
                }
            }
        }

        // ── SOS + звонок ──
        Text(stringResource(R.string.sos_disclaimer),
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant)
        Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            SosButton(Modifier.weight(1f))
            Card(
                Modifier
                    .weight(1f)
                    .height(96.dp)
                    .clickable { PttUi.requestCallDialog() },
                colors = CardDefaults.cardColors(
                    containerColor = MaterialTheme.colorScheme.primaryContainer),
            ) {
                Column(
                    Modifier.fillMaxSize(),
                    horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.Center,
                ) {
                    Icon(Icons.Filled.RecordVoiceOver, null,
                        modifier = Modifier.size(32.dp),
                        tint = MaterialTheme.colorScheme.onPrimaryContainer)
                    Text(stringResource(R.string.ptt_call),
                        color = MaterialTheme.colorScheme.onPrimaryContainer,
                        fontWeight = FontWeight.Bold)
                }
            }
        }

        // ── Перезвонить ──
        val lastCall by Bridge.ptt.lastCall.collectAsState()
        lastCall?.let { (id, name, mode) ->
            androidx.compose.material3.AssistChip(
                onClick = { PttUi.requestCallTo(id, name) },
                label = {
                    Text(stringResource(R.string.redial_fmt,
                        name.ifEmpty { "0x%08x".format(id) }))
                },
                leadingIcon = { Icon(Icons.Filled.RecordVoiceOver, null,
                    Modifier.size(16.dp)) },
            )
        }

        // ── Быстрые действия группы ──
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            androidx.compose.material3.AssistChip(
                onClick = { CheckInUi.open() },
                label = { Text(stringResource(R.string.checkin_title)) },
            )
            androidx.compose.material3.AssistChip(
                onClick = { nav.navigate("stats") },
                label = { Text(stringResource(R.string.stats_open)) },
            )
            androidx.compose.material3.AssistChip(
                onClick = { nav.navigate("batteries") },
                label = { Text(stringResource(R.string.batteries_title)) },
            )
        }

        // ── Карта ──
        Card(
            Modifier
                .fillMaxWidth()
                .height(320.dp),
        ) {
            Box(Modifier.fillMaxSize()) {
                MapContent(interactive = false, modifier = Modifier.fillMaxSize())
                // Прозрачный кликабельный слой поверх превью
                Box(
                    Modifier
                        .fillMaxSize()
                        .clickable { nav.navigate("map") })
                Text(
                    stringResource(R.string.map_expand),
                    Modifier
                        .align(Alignment.BottomEnd)
                        .padding(8.dp)
                        .clip(CircleShape)
                        .background(MaterialTheme.colorScheme.surface.copy(alpha = 0.85f))
                        .padding(horizontal = 12.dp, vertical = 6.dp),
                    style = MaterialTheme.typography.labelMedium,
                )
            }
        }

        // ── Узлы сети (онлайн сверху, как в чатах) ──
        val sorted = nodes.values.sortedWith(
            compareBy({ it.presence }, { -it.lastSeenMs }))
        val onlineCount = nodes.values.count { it.isOnline }
        if (sorted.isNotEmpty()) {
            Card {
                Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    Text(
                        stringResource(R.string.nodes_row_title) + " · " +
                            stringResource(R.string.online_count_fmt, onlineCount),
                        style = MaterialTheme.typography.titleSmall)
                    for (n in sorted.take(8)) {
                        Row(
                            verticalAlignment = Alignment.CenterVertically,
                            modifier = Modifier.clickable { nav.navigate("node/${n.idHex}") },
                        ) {
                            Box(
                                Modifier
                                    .size(8.dp)
                                    .clip(CircleShape)
                                    .background(presenceColor(n.presence)))
                            Spacer(Modifier.width(8.dp))
                            Text(n.displayName, Modifier.weight(1f))
                            Text(
                                stringResource(when (n.presence) {
                                    0 -> R.string.node_status_online
                                    1 -> R.string.node_status_stale
                                    else -> R.string.node_status_offline
                                }),
                                style = MaterialTheme.typography.labelSmall,
                                color = presenceColor(n.presence))
                            Spacer(Modifier.width(8.dp))
                            Text("${n.rssi} dBm",
                                style = MaterialTheme.typography.labelSmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant)
                            if (n.batt in 0..100) {
                                Spacer(Modifier.width(8.dp))
                                Text("${n.batt}%",
                                    style = MaterialTheme.typography.labelSmall,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant)
                            }
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun StatusChip(icon: androidx.compose.ui.graphics.vector.ImageVector,
                       label: String, value: String, ok: Boolean = true) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Icon(icon, null, Modifier.size(16.dp),
            tint = if (ok) MaterialTheme.colorScheme.primary
            else MaterialTheme.colorScheme.onSurfaceVariant)
        Spacer(Modifier.width(4.dp))
        Column {
            Text(label, fontSize = 10.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
            Text(value, fontSize = 13.sp, fontWeight = FontWeight.Bold)
        }
    }
}

@Composable
private fun SosButton(modifier: Modifier = Modifier) {
    var pressed by remember { mutableStateOf(false) }
    Card(
        modifier
            .height(96.dp)
            .pointerInput(Unit) {
                detectTapGestures(onPress = {
                    pressed = true
                    val t0 = System.currentTimeMillis()
                    tryAwaitRelease()
                    pressed = false
                    if (System.currentTimeMillis() - t0 >= 1500) {
                        Bridge.device.sos()
                    }
                })
            },
        colors = CardDefaults.cardColors(
            containerColor = if (pressed) MaterialTheme.colorScheme.error
            else MaterialTheme.colorScheme.errorContainer),
    ) {
        Column(
            Modifier.fillMaxSize(),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.Center,
        ) {
            Text(
                stringResource(R.string.sos_button),
                style = MaterialTheme.typography.headlineMedium,
                fontWeight = FontWeight.Black,
                color = if (pressed) MaterialTheme.colorScheme.onError
                else MaterialTheme.colorScheme.onErrorContainer,
            )
            Text(
                stringResource(R.string.sos_hold_hint),
                style = MaterialTheme.typography.labelSmall,
                color = if (pressed) MaterialTheme.colorScheme.onError
                else MaterialTheme.colorScheme.onErrorContainer,
            )
        }
    }
}
