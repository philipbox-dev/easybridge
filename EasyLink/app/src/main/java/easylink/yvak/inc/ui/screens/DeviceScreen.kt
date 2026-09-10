package easylink.yvak.inc.ui.screens

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Block
import androidx.compose.material.icons.filled.BugReport
import androidx.compose.material.icons.filled.Lock
import androidx.compose.material.icons.filled.QrCode
import androidx.compose.material.icons.filled.QrCodeScanner
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Save
import androidx.compose.material.icons.filled.Straighten
import androidx.compose.material3.AssistChip
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.FilterChip
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.navigation.NavHostController
import easylink.yvak.inc.Bridge
import easylink.yvak.inc.R
import easylink.yvak.inc.proto.ConnState
import easylink.yvak.inc.proto.hwName

@Composable
fun DeviceScreen(nav: NavHostController) {
    val connState by Bridge.ble.connState.collectAsState()
    val deviceName by Bridge.ble.deviceName.collectAsState()
    val diag by Bridge.device.diag.collectAsState()
    val radio by Bridge.device.radio.collectAsState()
    val nodes by Bridge.nodes.nodes.collectAsState()
    val connected = connState == ConnState.CONNECTED

    Column(
        Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(12.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        // ── Профиль ──
        Card {
            Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(stringResource(R.string.profile_title),
                    style = MaterialTheme.typography.titleMedium)
                var name by rememberSaveable { mutableStateOf(Bridge.prefs.myName.value) }
                Row(verticalAlignment = Alignment.CenterVertically) {
                    OutlinedTextField(
                        value = name, onValueChange = { if (it.length <= 30) name = it },
                        label = { Text(stringResource(R.string.profile_name)) },
                        placeholder = { Text(stringResource(R.string.profile_name_hint)) },
                        singleLine = true,
                        modifier = Modifier.weight(1f),
                    )
                    IconButton(
                        onClick = {
                            Bridge.prefs.myName.value = name.trim()
                            if (connected) Bridge.device.setName(name.trim())
                        },
                        enabled = name.isNotBlank(),
                    ) { Icon(Icons.Filled.Save, stringResource(R.string.save)) }
                }

                // ── #Ф3 QR: мой контакт + сканер ──
                var showMyQr by remember { mutableStateOf(false) }
                val ctx = androidx.compose.ui.platform.LocalContext.current
                val scanLauncher = androidx.activity.compose.rememberLauncherForActivityResult(
                    com.journeyapps.barcodescanner.ScanContract()
                ) { result ->
                    val text = result.contents ?: return@rememberLauncherForActivityResult
                    when (val p = easylink.yvak.inc.repo.QrCodec.parse(text)) {
                        is easylink.yvak.inc.repo.QrCodec.Payload.Contact -> {
                            Bridge.chat.createDm(p.nodeId, p.name)
                            android.widget.Toast.makeText(ctx,
                                ctx.getString(R.string.qr_added_contact_fmt, p.name),
                                android.widget.Toast.LENGTH_LONG).show()
                        }
                        is easylink.yvak.inc.repo.QrCodec.Payload.Group -> {
                            Bridge.chat.onGroupJoined(p.groupId, p.name, 0L)
                            if (p.key.isNotEmpty()) {
                                Bridge.crypto.setKey(
                                    easylink.yvak.inc.proto.groupChatKey(p.groupId), p.key)
                            }
                            android.widget.Toast.makeText(ctx,
                                ctx.getString(R.string.qr_added_group_fmt, p.name),
                                android.widget.Toast.LENGTH_LONG).show()
                        }
                        null -> android.widget.Toast.makeText(ctx,
                            ctx.getString(R.string.qr_bad),
                            android.widget.Toast.LENGTH_SHORT).show()
                    }
                }
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedButton(onClick = { showMyQr = true }) {
                        Icon(Icons.Filled.QrCode, null)
                        Spacer(Modifier.width(6.dp))
                        Text(stringResource(R.string.qr_my))
                    }
                    OutlinedButton(onClick = {
                        val opts = com.journeyapps.barcodescanner.ScanOptions()
                            .setBeepEnabled(false)
                            .setOrientationLocked(true)
                        scanLauncher.launch(opts)
                    }) {
                        Icon(Icons.Filled.QrCodeScanner, null)
                        Spacer(Modifier.width(6.dp))
                        Text(stringResource(R.string.qr_scan))
                    }
                }
                if (showMyQr) {
                    val myId = Bridge.prefs.myNodeId.value.toLongOrNull() ?: 0L
                    androidx.compose.material3.AlertDialog(
                        onDismissRequest = { showMyQr = false },
                        title = { Text(stringResource(R.string.qr_my)) },
                        text = {
                            if (myId == 0L) {
                                Text(stringResource(R.string.qr_need_connect))
                            } else {
                                val bmp = remember {
                                    easylink.yvak.inc.repo.QrCodec.toBitmap(
                                        easylink.yvak.inc.repo.QrCodec.encodeContact(
                                            myId, Bridge.prefs.myName.value))
                                }
                                Column(horizontalAlignment = androidx.compose.ui.Alignment.CenterHorizontally) {
                                    bmp?.let {
                                        androidx.compose.foundation.Image(
                                            bitmap = it.asImageBitmap(),
                                            contentDescription = null,
                                            modifier = Modifier.fillMaxWidth())
                                    }
                                    Text(stringResource(R.string.qr_my_hint),
                                        style = MaterialTheme.typography.labelSmall,
                                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                                }
                            }
                        },
                        confirmButton = {
                            TextButton(onClick = { showMyQr = false }) {
                                Text(stringResource(R.string.close))
                            }
                        },
                    )
                }
            }
        }

        // ── Подключение ──
        Card {
            Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(stringResource(R.string.device_section),
                    style = MaterialTheme.typography.titleMedium)
                Text(
                    when (connState) {
                        ConnState.CONNECTED -> "${stringResource(R.string.conn_connected)}: $deviceName"
                        ConnState.CONNECTING -> stringResource(R.string.conn_connecting)
                        ConnState.RECONNECTING -> stringResource(R.string.conn_reconnecting)
                        else -> stringResource(R.string.conn_disconnected)
                    },
                    color = if (connected) MaterialTheme.colorScheme.primary
                    else MaterialTheme.colorScheme.onSurfaceVariant,
                    fontWeight = FontWeight.Bold,
                )
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    if (connState == ConnState.DISCONNECTED) {
                        Button(onClick = { nav.navigate("scanner") }) {
                            Text(stringResource(R.string.connect))
                        }
                        val lastAddr = Bridge.prefs.lastDeviceAddr.value
                        if (lastAddr.isNotEmpty()) {
                            OutlinedButton(onClick = {
                                Bridge.connect(lastAddr, Bridge.prefs.lastDeviceName.value)
                            }) {
                                Text(Bridge.prefs.lastDeviceName.value.ifEmpty { lastAddr })
                            }
                        }
                    } else {
                        OutlinedButton(onClick = { Bridge.disconnect() }) {
                            Text(stringResource(R.string.disconnect))
                        }
                    }
                }
            }
        }

        // ── Диагностика ──
        if (connected) {
            Card {
                Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text(stringResource(R.string.device_info),
                            style = MaterialTheme.typography.titleMedium,
                            modifier = Modifier.weight(1f))
                        IconButton(onClick = { Bridge.device.refresh() }) {
                            Icon(Icons.Filled.Refresh, stringResource(R.string.nodes_refresh))
                        }
                    }
                    DiagRow(stringResource(R.string.diag_node_id), diag.nodeId)
                    DiagRow(stringResource(R.string.diag_board), diag.board)
                    DiagRow(stringResource(R.string.diag_hw), hwName(diag.hw))
                    DiagRow("LoRa", if (diag.loraOk) "OK" else "✗")
                    DiagRow(stringResource(R.string.status_speed),
                        stringResource(if (diag.sf != 0) R.string.speed_fast else R.string.speed_slow))
                    DiagRow("RSSI / SNR", "${diag.rssi} dBm / %.1f".format(diag.snr))
                    DiagRow(stringResource(R.string.diag_tx) + " / " +
                        stringResource(R.string.diag_rx), "${diag.txCount} / ${diag.rxCount}")
                    DiagRow(stringResource(R.string.status_nodes), "${diag.nodesOnline}")
                    DiagRow(stringResource(R.string.status_battery),
                        if (diag.batt in 0..100) "${diag.batt}%" else "—")
                    DiagRow(stringResource(R.string.status_encryption),
                        stringResource(if (diag.enc) R.string.enc_on else R.string.enc_off))
                }
            }

            // ── Радио ──
            Card {
                Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(stringResource(R.string.radio_section),
                        style = MaterialTheme.typography.titleMedium)

                    Text(stringResource(R.string.tx_power),
                        style = MaterialTheme.typography.labelMedium)
                    val expert by Bridge.prefs.expertMode.flow.collectAsState()
                    Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                        // idx 0=30, 1=27 dBm — высокая мощность, только в эксперт-режиме
                        val dbm = listOf(30, 27, 24, 21)
                        for ((idx, d) in dbm.withIndex()) {
                            val high = idx <= 1
                            FilterChip(
                                selected = radio.txIdx == idx,
                                enabled = expert || !high,
                                onClick = { Bridge.device.setTxPower(idx) },
                                label = { Text("$d dBm") },
                                leadingIcon = if (high && !expert) {
                                    { Icon(Icons.Filled.Lock, null,
                                        Modifier.size(14.dp)) }
                                } else null,
                            )
                        }
                    }
                    Text(
                        stringResource(if (expert) R.string.tx_power_legal_hint
                        else R.string.tx_power_locked),
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant)

                    Text(stringResource(R.string.antenna_type),
                        style = MaterialTheme.typography.labelMedium)
                    Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                        for (a in listOf("STOCK", "YAGI", "DIPOLE", "WHIP")) {
                            FilterChip(
                                selected = radio.antenna.equals(a, ignoreCase = true),
                                onClick = { Bridge.device.setAntenna(a) },
                                label = { Text(a) },
                            )
                        }
                    }

                    Text(stringResource(R.string.speed_title),
                        style = MaterialTheme.typography.labelMedium)
                    Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                        FilterChip(
                            selected = diag.sf == 0,
                            onClick = { Bridge.device.setSpeed(false) },
                            label = { Text(stringResource(R.string.speed_slow)) },
                        )
                        FilterChip(
                            selected = diag.sf != 0,
                            onClick = { Bridge.device.setSpeed(true) },
                            label = { Text(stringResource(R.string.speed_fast)) },
                        )
                    }
                    Text(stringResource(R.string.speed_hint),
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant)

                    // ── Качество FSK-фото ──
                    val imgProf by Bridge.prefs.imgProfile.flow.collectAsState()
                    Text(stringResource(R.string.img_profile_title),
                        style = MaterialTheme.typography.labelMedium)
                    Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                        FilterChip(imgProf == 0, { Bridge.prefs.imgProfile.value = 0 },
                            { Text(stringResource(R.string.ptt_mode_long)) })
                        FilterChip(imgProf == 1, { Bridge.prefs.imgProfile.value = 1 },
                            { Text(stringResource(R.string.ptt_mode_std)) })
                        FilterChip(imgProf == 2, { Bridge.prefs.imgProfile.value = 2 },
                            { Text(stringResource(R.string.ptt_mode_hd)) })
                    }
                }
            }
        }

        // ── Узлы ──
        Card {
            Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(stringResource(R.string.nodes_title),
                        style = MaterialTheme.typography.titleMedium,
                        modifier = Modifier.weight(1f))
                    if (connected) {
                        IconButton(onClick = { Bridge.nodes.refresh() }) {
                            Icon(Icons.Filled.Refresh, stringResource(R.string.nodes_refresh))
                        }
                    }
                }
                val list = nodes.values.sortedByDescending { it.lastSeenMs }
                if (list.isEmpty()) {
                    Text(stringResource(R.string.nodes_empty),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
                for (n in list) {
                    HorizontalDivider()
                    Column(
                        Modifier
                            .clickable { nav.navigate("node/${n.idHex}") }
                            .padding(vertical = 6.dp)) {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Column(Modifier.weight(1f)) {
                                Text(n.displayName, fontWeight = FontWeight.Bold)
                                Text(
                                    "${n.idHex} · ${hwName(n.hw)}",
                                    style = MaterialTheme.typography.labelSmall,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                                )
                                Text(
                                    "${n.rssi} dBm · " +
                                        (if (n.hops > 0)
                                            stringResource(R.string.node_hops_fmt, n.hops)
                                        else stringResource(R.string.node_direct)) +
                                        (if (n.batt in 0..100) " · ${n.batt}%" else ""),
                                    style = MaterialTheme.typography.labelSmall,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                                )
                            }
                            if (n.blocked) {
                                AssistChip(
                                    onClick = { Bridge.nodes.unblock(n.id) },
                                    label = { Text(stringResource(R.string.node_unblock)) },
                                )
                            } else {
                                IconButton(onClick = { Bridge.nodes.block(n.id) }) {
                                    Icon(Icons.Filled.Block,
                                        stringResource(R.string.node_block),
                                        tint = MaterialTheme.colorScheme.error)
                                }
                            }
                        }
                    }
                }
            }
        }

        // ── Инструменты: замер дальности ──
        Card(onClick = { nav.navigate("rangetest") }) {
            Row(
                Modifier
                    .fillMaxWidth()
                    .padding(16.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Icon(Icons.Filled.Straighten, null,
                    tint = MaterialTheme.colorScheme.primary)
                Spacer(Modifier.width(8.dp))
                Text(stringResource(R.string.rangetest_open), Modifier.weight(1f))
                Text("→", color = MaterialTheme.colorScheme.primary,
                    fontWeight = FontWeight.Bold)
            }
        }

        // ── Дебаг ──
        Card(onClick = { nav.navigate("debug") }) {
            Row(
                Modifier
                    .fillMaxWidth()
                    .padding(16.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Icon(Icons.Filled.BugReport, null,
                    tint = MaterialTheme.colorScheme.primary)
                Spacer(Modifier.width(8.dp))
                Text(stringResource(R.string.debug_console), Modifier.weight(1f))
                Text("→", color = MaterialTheme.colorScheme.primary,
                    fontWeight = FontWeight.Bold)
            }
        }
    }
}

@Composable
private fun DiagRow(label: String, value: String) {
    Row {
        Text(label, Modifier.weight(1f),
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            style = MaterialTheme.typography.bodySmall)
        Text(value, style = MaterialTheme.typography.bodySmall,
            fontWeight = FontWeight.Bold)
    }
}
