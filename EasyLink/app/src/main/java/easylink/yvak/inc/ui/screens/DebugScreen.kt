package easylink.yvak.inc.ui.screens

import android.content.ContentValues
import android.content.Context
import android.os.Environment
import android.provider.MediaStore
import android.widget.Toast
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.rememberScrollState
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.Send
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.Download
import androidx.compose.material.icons.filled.NetworkPing
import androidx.compose.material.icons.filled.Pause
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Share
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import easylink.yvak.inc.Bridge
import easylink.yvak.inc.R
import easylink.yvak.inc.proto.LogDir
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

private val templates = listOf(
    """{"cmd":"diag"}""",
    """{"cmd":"status"}""",
    """{"cmd":"nodes"}""",
    """{"cmd":"discover"}""",
    """{"cmd":"get_radio"}""",
    """{"cmd":"ble_test"}""",
    """{"cmd":"send","seq":1,"text":"test"}""",
    """{"cmd":"setname","name":"Test"}""",
    """{"cmd":"speed","mode":"fast"}""",
    """{"cmd":"speed","mode":"slow"}""",
    """{"cmd":"set_tx_power","idx":0}""",
    """{"cmd":"set_antenna","type":"STOCK"}""",
    """{"cmd":"setpsk","pass":"secret"}""",
    """{"cmd":"blocknode","node_id":"0x00000000"}""",
    """{"cmd":"groupjoin","node_id":"0x0","group_id":"0x1","name":"Test"}""",
    """{"cmd":"groupleave","group_id":"0x1"}""",
    """{"cmd":"ptt_start","mode":1}""",
    """{"cmd":"ptt_stop"}""",
    """{"cmd":"sos"}""",
)

@Composable
fun DebugScreen() {
    val log by Bridge.debug.log.collectAsState()
    val paused by Bridge.debug.paused.collectAsState()
    val tx by Bridge.debug.txCount.collectAsState()
    val rx by Bridge.debug.rxCount.collectAsState()
    val err by Bridge.debug.errCount.collectAsState()
    var filter by remember { mutableStateOf<LogDir?>(null) }
    var cmd by remember { mutableStateOf("") }
    var showTemplates by remember { mutableStateOf(false) }
    val listState = rememberLazyListState()
    val ctx = LocalContext.current

    val shown = if (filter == null) log else log.filter { it.dir == filter }

    LaunchedEffect(shown.size) {
        if (shown.isNotEmpty() && !paused) listState.scrollToItem(shown.size - 1)
    }

    Column(Modifier.fillMaxSize().padding(8.dp)) {
        // ── Панель управления ──
        Row(
            Modifier.horizontalScroll(rememberScrollState()),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            FilterChip(filter == null, { filter = null },
                { Text(stringResource(R.string.dbg_filter_all)) })
            FilterChip(filter == LogDir.TX, { filter = LogDir.TX },
                { Text(stringResource(R.string.dbg_filter_tx)) })
            FilterChip(filter == LogDir.RX, { filter = LogDir.RX },
                { Text(stringResource(R.string.dbg_filter_rx)) })
            FilterChip(filter == LogDir.INFO, { filter = LogDir.INFO },
                { Text(stringResource(R.string.dbg_filter_info)) })

            IconButton(onClick = { Bridge.debug.paused.value = !paused }) {
                Icon(if (paused) Icons.Filled.PlayArrow else Icons.Filled.Pause,
                    stringResource(if (paused) R.string.dbg_resume else R.string.dbg_pause))
            }
            IconButton(onClick = { Bridge.debug.clear() }) {
                Icon(Icons.Filled.Delete, stringResource(R.string.dbg_clear))
            }
            IconButton(onClick = { Bridge.sendPing() }) {
                Icon(Icons.Filled.NetworkPing, stringResource(R.string.dbg_ping))
            }
            IconButton(onClick = { exportLog(ctx) }) {
                Icon(Icons.Filled.Download, stringResource(R.string.dbg_export))
            }
            // #У10 Поделиться файлом лога сессии (пишется всегда)
            IconButton(onClick = {
                Bridge.debug.currentLogFile()?.let {
                    shareFile(ctx, it, "text/plain")
                }
            }) {
                Icon(Icons.Filled.Share, stringResource(R.string.dbg_share_file))
            }
        }
        Text(
            stringResource(R.string.dbg_stats_fmt, tx, rx, err),
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )

        // ── Лог ──
        LazyColumn(
            Modifier.weight(1f).fillMaxWidth(),
            state = listState,
        ) {
            if (shown.isEmpty()) {
                item {
                    Text(stringResource(R.string.dbg_empty),
                        Modifier.padding(16.dp),
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }
            items(shown.size) { i ->
                val e = shown[i]
                val time = SimpleDateFormat("HH:mm:ss.SSS", Locale.US).format(Date(e.ts))
                val color = when (e.dir) {
                    LogDir.TX -> MaterialTheme.colorScheme.primary
                    LogDir.RX -> Color(0xFF2E7D32)
                    LogDir.ERR -> MaterialTheme.colorScheme.error
                    LogDir.INFO -> MaterialTheme.colorScheme.onSurfaceVariant
                }
                Text(
                    "$time ${e.dir.name.padEnd(4)} ${e.text}",
                    fontFamily = FontFamily.Monospace,
                    fontSize = 11.sp,
                    color = color,
                )
            }
        }

        // ── Ввод команды ──
        Row(verticalAlignment = Alignment.CenterVertically) {
            TextButton(onClick = { showTemplates = true }) {
                Text(stringResource(R.string.dbg_templates))
            }
            DropdownMenu(expanded = showTemplates, onDismissRequest = { showTemplates = false }) {
                for (t in templates) {
                    DropdownMenuItem(
                        text = { Text(t, fontFamily = FontFamily.Monospace, fontSize = 11.sp) },
                        onClick = { cmd = t; showTemplates = false },
                    )
                }
            }
            OutlinedTextField(
                value = cmd, onValueChange = { cmd = it },
                modifier = Modifier.weight(1f),
                placeholder = { Text(stringResource(R.string.dbg_cmd_hint)) },
                textStyle = androidx.compose.ui.text.TextStyle(
                    fontFamily = FontFamily.Monospace, fontSize = 12.sp),
                maxLines = 3,
            )
            IconButton(
                onClick = {
                    if (cmd.isNotBlank()) { Bridge.sendRaw(cmd.trim()) }
                },
                enabled = cmd.isNotBlank(),
            ) {
                Icon(Icons.AutoMirrored.Filled.Send, stringResource(R.string.send),
                    tint = MaterialTheme.colorScheme.primary)
            }
        }
    }
}

private fun exportLog(ctx: Context) {
    try {
        val name = "easylink_log_${System.currentTimeMillis()}.txt"
        val cv = ContentValues().apply {
            put(MediaStore.Downloads.DISPLAY_NAME, name)
            put(MediaStore.Downloads.MIME_TYPE, "text/plain")
            put(MediaStore.Downloads.RELATIVE_PATH, Environment.DIRECTORY_DOWNLOADS)
        }
        val uri = ctx.contentResolver.insert(
            MediaStore.Downloads.EXTERNAL_CONTENT_URI, cv) ?: return
        ctx.contentResolver.openOutputStream(uri)?.use {
            it.write(Bridge.debug.exportText().toByteArray())
        }
        Toast.makeText(ctx, "Downloads/$name", Toast.LENGTH_LONG).show()
    } catch (e: Exception) {
        Toast.makeText(ctx, e.message ?: "error", Toast.LENGTH_LONG).show()
    }
}
