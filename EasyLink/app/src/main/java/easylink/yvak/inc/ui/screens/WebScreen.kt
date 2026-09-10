package easylink.yvak.inc.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import easylink.yvak.inc.Bridge
import easylink.yvak.inc.net.WebClient
import kotlinx.coroutines.launch

/**
 * Настройка веб-моста.
 *
 * Экран сознательно объясняет, что телефон здесь делает: люди
 * ожидают «ещё один чат», а на деле телефон работает мостом — и от
 * того, включён ли он, зависит, дойдут ли сообщения до тех, кто сидит
 * за компом.
 */
@Composable
fun WebScreen(nav: androidx.navigation.NavHostController) {
    val scope = rememberCoroutineScope()

    val enabled by Bridge.prefs.webEnabled.flow.collectAsState()
    val gateway by Bridge.prefs.webGateway.flow.collectAsState()
    val netName by Bridge.prefs.webNetName.flow.collectAsState()
    val memberName by Bridge.prefs.webMemberName.flow.collectAsState()
    val token by Bridge.prefs.webToken.flow.collectAsState()
    val state by Bridge.web.state.collectAsState()
    val note by Bridge.web.note.collectAsState()
    val airCall by Bridge.web.airCall.collectAsState()
    val radio by Bridge.device.radio.collectAsState()

    var url by rememberSaveable { mutableStateOf(Bridge.prefs.webUrl.value) }
    var uid by rememberSaveable { mutableStateOf(Bridge.prefs.webUid.value) }
    var password by rememberSaveable { mutableStateOf(Bridge.prefs.webPassword.value) }
    var busy by remember { mutableStateOf(false) }

    Column(
        Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(12.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Card {
            Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Веб-чат", style = MaterialTheme.typography.titleMedium)
                Text(
                    "Сообщения из эфира появятся в браузере, а написанное с компа " +
                        "уйдёт по LoRa тем, кого в вебе нет. Телефон при этом работает " +
                        "мостом: он передаёт эфир в интернет и обратно.",
                    style = MaterialTheme.typography.bodySmall,
                )

                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text("Включить", Modifier.weight(1f))
                    Switch(checked = enabled, onCheckedChange = {
                        Bridge.prefs.webEnabled.value = it
                    })
                }

                StatusLine(state, netName, memberName)
                if (note.isNotBlank()) {
                    Text(note, style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.error)
                }
                if (airCall) {
                    Text("Идёт звонок: голос из веба уходит в эфир",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.primary)
                }
            }
        }

        Card {
            Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Подключение", style = MaterialTheme.typography.titleMedium)
                Text(
                    "Адрес сервера и персональную ссылку выдаёт хост сети. " +
                        "Из ссылки вида /lora-chat/AbC123/chat нужна часть AbC123.",
                    style = MaterialTheme.typography.bodySmall,
                )

                OutlinedTextField(
                    value = url, onValueChange = { url = it },
                    label = { Text("Адрес сервера") },
                    placeholder = { Text("meetjimproviz.ru") },
                    singleLine = true, modifier = Modifier.fillMaxWidth(),
                )
                OutlinedTextField(
                    value = uid, onValueChange = { uid = it },
                    label = { Text("Ваша ссылка (uid)") },
                    singleLine = true, modifier = Modifier.fillMaxWidth(),
                )
                OutlinedTextField(
                    value = password, onValueChange = { password = it },
                    label = { Text("Пароль") },
                    visualTransformation = PasswordVisualTransformation(),
                    singleLine = true, modifier = Modifier.fillMaxWidth(),
                )

                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(
                        enabled = !busy && url.isNotBlank() &&
                            uid.isNotBlank() && password.isNotBlank(),
                        onClick = {
                            Bridge.prefs.webUrl.value = url.trim()
                            Bridge.prefs.webUid.value = uid.trim()
                            Bridge.prefs.webPassword.value = password
                            Bridge.prefs.webEnabled.value = true
                            busy = true
                            scope.launch {
                                Bridge.web.loginAndConnect()
                                busy = false
                            }
                        },
                    ) {
                        if (busy) {
                            CircularProgressIndicator(Modifier.padding(end = 8.dp))
                        }
                        Text(if (token.isBlank()) "Войти" else "Переподключиться")
                    }
                    if (token.isNotBlank()) {
                        OutlinedButton(onClick = { Bridge.web.logout() }) { Text("Выйти") }
                    }
                }
            }
        }

        Card {
            Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Шлюз в эфир", style = MaterialTheme.typography.titleMedium)
                Text(
                    "Ваш телефон будет выносить в LoRa сообщения, написанные из " +
                        "браузера тем, кого нет в вебе. Стоит выключать в роуминге " +
                        "и на последних процентах заряда: это постоянное соединение " +
                        "и радио в приёме.",
                    style = MaterialTheme.typography.bodySmall,
                )
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text("Работать шлюзом", Modifier.weight(1f))
                    Switch(checked = gateway, onCheckedChange = { Bridge.web.setGateway(it) })
                }

                // Наличие FSK решает, возможны ли звонки по эфиру. Молчать
                // об этом нельзя: иначе человек будет ждать звонка, который
                // физически невозможен.
                if (!radio.fsk) {
                    Text(
                        "У этой платы нет FSK" +
                            (if (radio.chip.isNotBlank()) " (${radio.chip})" else "") +
                            " — голос в эфир уйти не сможет. Сообщения ходят как обычно, " +
                            "а разговаривать получится только через веб.",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.error,
                    )
                }
            }
        }
    }
}

@Composable
private fun StatusLine(
    state: WebClient.State,
    netName: String,
    memberName: String,
) {
    val (text, color) = when (state) {
        is WebClient.State.Online -> {
            val who = listOfNotNull(
                netName.takeIf { it.isNotBlank() },
                memberName.takeIf { it.isNotBlank() },
            ).joinToString(" · ")
            (if (who.isBlank()) "На связи" else "На связи: $who") to
                MaterialTheme.colorScheme.primary
        }
        is WebClient.State.Connecting -> "Подключаюсь…" to MaterialTheme.colorScheme.onSurfaceVariant
        is WebClient.State.Failed -> "Нет связи: ${state.reason}" to MaterialTheme.colorScheme.error
        is WebClient.State.Offline -> "Отключено" to MaterialTheme.colorScheme.onSurfaceVariant
    }
    Text(text, style = MaterialTheme.typography.bodyMedium, color = color)
}
