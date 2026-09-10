package easylink.yvak.inc.ui.screens

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTapGestures
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
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Call
import androidx.compose.material.icons.filled.CallEnd
import androidx.compose.material.icons.filled.Mic
import androidx.compose.material.icons.filled.NotificationsOff
import androidx.compose.material.icons.filled.VolumeUp
import androidx.compose.material.icons.filled.Hearing
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.FilledTonalIconButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButtonDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.animation.core.animateFloat
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.res.stringResource
import androidx.compose.material3.Card
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import easylink.yvak.inc.Bridge
import easylink.yvak.inc.R
import easylink.yvak.inc.proto.PttState
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow

/** Запрос диалога звонка с любого экрана; target != null — адресат уже выбран. */
object PttUi {
    data class CallRequest(val targetId: Long = 0, val targetName: String = "",
                           val pickTarget: Boolean = true)
    val callRequest = MutableStateFlow<CallRequest?>(null)
    fun requestCallDialog() { callRequest.value = CallRequest() }
    fun requestCallTo(id: Long, name: String) {
        callRequest.value = CallRequest(id, name, pickTarget = false)
    }
}

@Composable
fun PttCallDialogHost() {
    val req by PttUi.callRequest.collectAsState()
    val r = req ?: return
    var target by remember(r) {
        mutableStateOf(if (r.pickTarget) null else (r.targetId to r.targetName))
    }
    val nodes by Bridge.nodes.nodes.collectAsState()

    val modes = listOf(
        Triple(0, stringResource(R.string.ptt_mode_long), stringResource(R.string.ptt_mode_long_desc)),
        Triple(1, stringResource(R.string.ptt_mode_std), stringResource(R.string.ptt_mode_std_desc)),
        Triple(2, stringResource(R.string.ptt_mode_hd), stringResource(R.string.ptt_mode_hd_desc)),
    )

    AlertDialog(
        onDismissRequest = { PttUi.callRequest.value = null },
        title = {
            Text(
                if (target == null) stringResource(R.string.ptt_call_to)
                else stringResource(R.string.ptt_choose_mode) +
                    (target?.second?.takeIf { it.isNotEmpty() }?.let { " · $it" } ?: ""))
        },
        text = {
            if (target == null) {
                // ── Шаг 1: адресат ──
                Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    Text(
                        stringResource(R.string.call_to_all),
                        Modifier
                            .fillMaxWidth()
                            .clickableRow { target = 0L to "" },
                        fontWeight = FontWeight.Bold,
                    )
                    for (n in nodes.values.sortedBy { it.presence }) {
                        Row(
                            Modifier
                                .fillMaxWidth()
                                .clickableRow { target = n.id to n.displayName },
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            Box(
                                Modifier
                                    .size(8.dp)
                                    .clip(CircleShape)
                                    .background(presenceColor(n.presence)))
                            Spacer(Modifier.size(8.dp))
                            Text(n.displayName)
                        }
                    }
                }
            } else {
                // ── Шаг 2: режим ──
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    for ((mode, name, desc) in modes) {
                        Button(
                            onClick = {
                                val t = target!!
                                PttUi.callRequest.value = null
                                Bridge.ptt.startCall(mode, t.first, t.second)
                            },
                            modifier = Modifier.height(56.dp),
                        ) {
                            Column {
                                Text(name, fontWeight = FontWeight.Bold)
                                Text(desc, fontSize = 11.sp)
                            }
                        }
                    }
                }
            }
        },
        confirmButton = {
            TextButton(onClick = { PttUi.callRequest.value = null }) {
                Text(stringResource(R.string.cancel))
            }
        },
    )
}

internal fun presenceColor(presence: Int): Color = when (presence) {
    0 -> Color(0xFF4CAF50)
    1 -> Color(0xFFFFA726)
    else -> Color(0xFF9E9E9E)
}

private fun Modifier.clickableRow(onClick: () -> Unit): Modifier =
    this.then(Modifier.clickable(onClick = onClick).padding(vertical = 10.dp))


@Composable
fun PttOverlay() {
    val state by Bridge.ptt.state.collectAsState()
    when (val s = state) {
        is PttState.Idle -> return
        is PttState.Incoming -> IncomingCall(s)
        is PttState.Dialing -> DialingCall(s)
        is PttState.Active -> ActiveCall(s)
    }
}

/**
 * V1.7.3: живая диагностика звонка — вся цепочка эфир→BLE→декодер→динамик.
 * Обновляется 2 раза/с; данные платы приходят evt:ptt_stats раз в 2с.
 */
@Composable
private fun PttDebugPanel() {
    var rows by remember { mutableStateOf(Bridge.ptt.debugSnapshot()) }
    LaunchedEffect(Unit) {
        while (true) {
            rows = Bridge.ptt.debugSnapshot()
            delay(500)
        }
    }
    Card(Modifier.fillMaxWidth().padding(horizontal = 16.dp)) {
        Column(Modifier.padding(horizontal = 12.dp, vertical = 8.dp)) {
            for ((k, v) in rows) {
                Row(Modifier.fillMaxWidth()) {
                    Text(k, fontSize = 11.sp, modifier = Modifier.weight(1f),
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                    Text(v, fontSize = 11.sp, fontFamily = FontFamily.Monospace)
                }
            }
        }
    }
}

/** Кнопка-тумблер панели отладки. */
@Composable
private fun DebugToggle(show: Boolean, onToggle: () -> Unit) {
    TextButton(onClick = onToggle) {
        Text(
            if (show) stringResource(R.string.ptt_debug_hide)
            else stringResource(R.string.ptt_debug_show),
            fontSize = 12.sp,
            color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

@Composable
private fun DialingCall(s: PttState.Dialing) {
    var elapsed by remember { mutableLongStateOf(0L) }
    LaunchedEffect(s.startedMs) {
        while (true) {
            elapsed = (System.currentTimeMillis() - s.startedMs) / 1000
            delay(1000)
        }
    }
    val modeName = when (s.mode) {
        0 -> stringResource(R.string.ptt_mode_long)
        2 -> stringResource(R.string.ptt_mode_hd)
        else -> stringResource(R.string.ptt_mode_std)
    }
    Box(
        Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background),
        contentAlignment = Alignment.Center,
    ) {
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            // Пульсирующая иконка вызова
            val infinite = androidx.compose.animation.core.rememberInfiniteTransition(label = "dial")
            val alpha by infinite.animateFloat(
                initialValue = 0.35f, targetValue = 1f,
                animationSpec = androidx.compose.animation.core.infiniteRepeatable(
                    androidx.compose.animation.core.tween(700),
                    androidx.compose.animation.core.RepeatMode.Reverse),
                label = "a")
            Icon(Icons.Filled.Call, null, Modifier.size(72.dp).alpha(alpha),
                tint = MaterialTheme.colorScheme.primary)
            Spacer(Modifier.height(20.dp))
            Text(
                if (s.peerName.isNotEmpty()) s.peerName else stringResource(R.string.ptt_call),
                style = MaterialTheme.typography.headlineMedium,
                fontWeight = FontWeight.Bold)
            Spacer(Modifier.height(8.dp))
            Text(stringResource(R.string.ptt_dialing) + " · $modeName · %d:%02d".format(elapsed / 60, elapsed % 60),
                color = MaterialTheme.colorScheme.primary)
            Spacer(Modifier.height(6.dp))
            Text(stringResource(R.string.ptt_waiting_answer),
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                textAlign = TextAlign.Center,
                modifier = Modifier.padding(horizontal = 32.dp))
            var showDebug by remember { mutableStateOf(false) }
            DebugToggle(showDebug) { showDebug = !showDebug }
            if (showDebug) PttDebugPanel()
            Spacer(Modifier.height(24.dp))
            Button(
                onClick = { Bridge.ptt.end() },
                colors = ButtonDefaults.buttonColors(
                    containerColor = MaterialTheme.colorScheme.error),
            ) {
                Icon(Icons.Filled.CallEnd, null)
                Spacer(Modifier.size(8.dp))
                Text(stringResource(R.string.ptt_cancel_call))
            }
        }
    }
}

@Composable
private fun IncomingCall(s: PttState.Incoming) {
    Box(
        Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background.copy(alpha = 0.97f)),
        contentAlignment = Alignment.Center,
    ) {
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            Icon(Icons.Filled.Call, null, Modifier.size(64.dp),
                tint = MaterialTheme.colorScheme.primary)
            Spacer(Modifier.height(16.dp))
            Text(stringResource(R.string.ptt_incoming),
                style = MaterialTheme.typography.titleLarge)
            Spacer(Modifier.height(8.dp))
            Text(s.name.ifEmpty { "0x%08x".format(s.nodeId) },
                style = MaterialTheme.typography.headlineMedium,
                fontWeight = FontWeight.Bold)
            Spacer(Modifier.height(40.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(24.dp)) {
                Button(
                    onClick = { Bridge.notif.cancelPtt(); Bridge.ptt.accept() },
                    colors = ButtonDefaults.buttonColors(containerColor = Color(0xFF2E7D32)),
                ) {
                    Icon(Icons.Filled.Call, null)
                    Spacer(Modifier.size(8.dp))
                    Text(stringResource(R.string.ptt_accept))
                }
                Button(
                    onClick = { Bridge.notif.cancelPtt(); Bridge.ptt.decline() },
                    colors = ButtonDefaults.buttonColors(
                        containerColor = MaterialTheme.colorScheme.error),
                ) {
                    Icon(Icons.Filled.CallEnd, null)
                    Spacer(Modifier.size(8.dp))
                    Text(stringResource(R.string.ptt_decline))
                }
            }
        }
    }
}

@Composable
private fun ActiveCall(s: PttState.Active) {
    val activity by Bridge.ptt.activity.collectAsState()
    var elapsed by remember { mutableLongStateOf(0L) }
    LaunchedEffect(s.startedMs) {
        while (true) {
            elapsed = (System.currentTimeMillis() - s.startedMs) / 1000
            delay(1000)
        }
    }
    var talking by remember { mutableStateOf(false) }

    val modeName = when (s.mode) {
        0 -> stringResource(R.string.ptt_mode_long)
        2 -> stringResource(R.string.ptt_mode_hd)
        else -> stringResource(R.string.ptt_mode_std)
    }

    Box(
        Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background),
    ) {
        Column(
            Modifier
                .fillMaxSize()
                .padding(24.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Spacer(Modifier.height(32.dp))
            Text(
                if (s.peerName.isNotEmpty()) s.peerName else stringResource(R.string.ptt_call),
                style = MaterialTheme.typography.headlineSmall,
                fontWeight = FontWeight.Bold)
            Text("$modeName · %d:%02d".format(elapsed / 60, elapsed % 60),
                color = MaterialTheme.colorScheme.onSurfaceVariant)
            Spacer(Modifier.height(8.dp))

            val speakerOn by Bridge.ptt.speakerOn.collectAsState()
            FilledTonalIconButton(
                onClick = { Bridge.ptt.toggleSpeaker() },
                colors = if (speakerOn) IconButtonDefaults.filledTonalIconButtonColors()
                    else IconButtonDefaults.filledTonalIconButtonColors(
                        containerColor = MaterialTheme.colorScheme.surfaceVariant),
            ) {
                Icon(
                    if (speakerOn) Icons.Filled.VolumeUp else Icons.Filled.Hearing,
                    null)
            }
            Text(
                if (speakerOn) stringResource(R.string.ptt_speaker_on)
                else stringResource(R.string.ptt_speaker_off),
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
            Spacer(Modifier.height(8.dp))

            val (label, color) = when {
                talking -> stringResource(R.string.ptt_transmitting) to
                    MaterialTheme.colorScheme.error
                activity == 2 -> stringResource(R.string.ptt_receiving) to Color(0xFF2E7D32)
                else -> stringResource(R.string.ptt_on_air) to
                    MaterialTheme.colorScheme.onSurfaceVariant
            }
            Text(label, color = color, fontWeight = FontWeight.Bold, fontSize = 18.sp)

            var showDebug by remember { mutableStateOf(false) }
            DebugToggle(showDebug) { showDebug = !showDebug }
            if (showDebug) PttDebugPanel()

            Spacer(Modifier.weight(1f))

            // ── Большая кнопка «говори» ──
            Box(
                Modifier
                    .size(220.dp)
                    .clip(CircleShape)
                    .background(
                        if (talking) MaterialTheme.colorScheme.error
                        else MaterialTheme.colorScheme.primary)
                    .pointerInput(Unit) {
                        detectTapGestures(onPress = {
                            talking = true
                            Bridge.ptt.setTalk(true)
                            tryAwaitRelease()
                            talking = false
                            Bridge.ptt.setTalk(false)
                        })
                    },
                contentAlignment = Alignment.Center,
            ) {
                Column(horizontalAlignment = Alignment.CenterHorizontally) {
                    Icon(Icons.Filled.Mic, null, Modifier.size(56.dp),
                        tint = MaterialTheme.colorScheme.onPrimary)
                    Text(stringResource(R.string.ptt_talk),
                        color = MaterialTheme.colorScheme.onPrimary,
                        fontWeight = FontWeight.Black, fontSize = 22.sp)
                }
            }
            Text(
                if (talking) stringResource(R.string.ptt_release_to_listen)
                else stringResource(R.string.ptt_hold_to_talk),
                Modifier.padding(top = 12.dp),
                color = if (talking) MaterialTheme.colorScheme.error
                else MaterialTheme.colorScheme.onSurfaceVariant,
                fontWeight = FontWeight.Bold,
                textAlign = TextAlign.Center)
            Text(stringResource(R.string.ptt_halfduplex_hint),
                Modifier.padding(top = 4.dp, start = 24.dp, end = 24.dp),
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                textAlign = TextAlign.Center)

            Spacer(Modifier.weight(1f))

            Button(
                onClick = { Bridge.ptt.end() },
                colors = ButtonDefaults.buttonColors(
                    containerColor = MaterialTheme.colorScheme.error),
            ) {
                Icon(Icons.Filled.CallEnd, null)
                Spacer(Modifier.size(8.dp))
                Text(stringResource(R.string.ptt_end))
            }
            Spacer(Modifier.height(24.dp))
        }
    }
}

@Composable
fun SosOverlay() {
    val alert by Bridge.sosAlert.collectAsState()
    val a = alert ?: return
    val sirenOn by Bridge.siren.active.collectAsState()

    Box(
        Modifier
            .fillMaxSize()
            .background(Color(0xFFB71C1C)),
        contentAlignment = Alignment.Center,
    ) {
        Column(
            horizontalAlignment = Alignment.CenterHorizontally,
            modifier = Modifier.padding(24.dp),
        ) {
            Text("⚠️", fontSize = 72.sp)
            Text(
                stringResource(if (a.isPanic) R.string.panic_incoming else R.string.sos_incoming),
                style = MaterialTheme.typography.headlineMedium,
                fontWeight = FontWeight.Black, color = Color.White,
                textAlign = TextAlign.Center)
            Spacer(Modifier.height(12.dp))
            Text(stringResource(R.string.sos_from_fmt, a.name),
                style = MaterialTheme.typography.titleLarge, color = Color.White)
            if (a.text.isNotEmpty()) {
                Spacer(Modifier.height(8.dp))
                Text(a.text, color = Color.White, textAlign = TextAlign.Center)
            }
            Spacer(Modifier.height(8.dp))
            Text("RSSI ${a.rssi} dBm", color = Color.White.copy(alpha = 0.8f))
            Spacer(Modifier.height(40.dp))
            if (sirenOn) {
                Button(
                    onClick = { Bridge.siren.stop() },
                    colors = ButtonDefaults.buttonColors(containerColor = Color.White),
                ) {
                    Icon(Icons.Filled.NotificationsOff, null, tint = Color(0xFFB71C1C))
                    Spacer(Modifier.size(8.dp))
                    Text(stringResource(R.string.sos_mute), color = Color(0xFFB71C1C))
                }
                Spacer(Modifier.height(12.dp))
            }
            Button(
                onClick = { Bridge.dismissSos() },
                colors = ButtonDefaults.buttonColors(containerColor = Color.Black),
            ) {
                Text(stringResource(R.string.sos_dismiss), color = Color.White)
            }
        }
    }
}
