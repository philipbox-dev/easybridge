package easylink.yvak.inc.ui.screens

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import easylink.yvak.inc.Bridge
import easylink.yvak.inc.R
import kotlinx.coroutines.flow.MutableStateFlow

/** #Ф2 UI check-in «Все живы?»: сессия организатора + входящий запрос. */
object CheckInUi {
    val dialogOpen = MutableStateFlow(false)
    fun open() { dialogOpen.value = true }
}

@Composable
fun CheckInOverlays() {
    // ── Входящий запрос: «X спрашивает — все живы?» ──
    val incoming by Bridge.check.incoming.collectAsState()
    incoming?.let { inc ->
        AlertDialog(
            onDismissRequest = { Bridge.check.dismissIncoming() },
            title = { Text(stringResource(R.string.checkin_title)) },
            text = { Text(stringResource(R.string.checkin_incoming_fmt, inc.fromName)) },
            confirmButton = {
                Button(onClick = { Bridge.check.ackIncoming() }) {
                    Text(stringResource(R.string.checkin_ok))
                }
            },
            dismissButton = {
                TextButton(onClick = { Bridge.check.dismissIncoming() }) {
                    Text(stringResource(R.string.close))
                }
            },
        )
    }

    // ── Диалог организатора ──
    val open by CheckInUi.dialogOpen.collectAsState()
    if (!open) return
    val session by Bridge.check.session.collectAsState()

    AlertDialog(
        onDismissRequest = { CheckInUi.dialogOpen.value = false },
        title = { Text(stringResource(R.string.checkin_title)) },
        text = {
            val s = session
            if (s == null) {
                Text(stringResource(R.string.rangetest_need_peer))
            } else {
                Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    Text(stringResource(R.string.checkin_status_fmt,
                        s.acks.size, s.expected.size),
                        fontWeight = FontWeight.Bold,
                        color = MaterialTheme.colorScheme.primary)
                    for ((id, name) in s.expected) {
                        val ok = s.acks.contains(id)
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Box(
                                Modifier
                                    .size(10.dp)
                                    .clip(CircleShape)
                                    .background(
                                        if (ok) Color(0xFF4CAF50) else Color(0xFF9E9E9E)))
                            Spacer(Modifier.width(8.dp))
                            Text(name, Modifier.weight(1f))
                            Text(
                                stringResource(if (ok) R.string.checkin_answered
                                else R.string.checkin_waiting),
                                style = MaterialTheme.typography.labelSmall,
                                color = if (ok) Color(0xFF4CAF50)
                                else MaterialTheme.colorScheme.onSurfaceVariant)
                        }
                    }
                }
            }
        },
        confirmButton = {
            Button(onClick = { Bridge.check.start() }) {
                Text(stringResource(R.string.checkin_send))
            }
        },
        dismissButton = {
            TextButton(onClick = {
                CheckInUi.dialogOpen.value = false
            }) { Text(stringResource(R.string.checkin_close)) }
        },
    )
}
