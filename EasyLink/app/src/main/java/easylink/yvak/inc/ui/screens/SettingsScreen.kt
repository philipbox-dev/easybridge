package easylink.yvak.inc.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Warning
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import easylink.yvak.inc.App
import easylink.yvak.inc.Bridge
import easylink.yvak.inc.R
import easylink.yvak.inc.proto.ConnState

@Composable
fun SettingsScreen(nav: androidx.navigation.NavHostController) {
    val ctx = androidx.compose.ui.platform.LocalContext.current
    Column(
        Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(12.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        // ── Внешний вид ──
        SettingsCard(stringResource(R.string.set_appearance)) {
            val theme by Bridge.prefs.theme.flow.collectAsState()
            Text(stringResource(R.string.set_theme),
                style = MaterialTheme.typography.labelMedium)
            Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                FilterChip(theme == "system", { Bridge.prefs.theme.value = "system" },
                    { Text(stringResource(R.string.theme_system)) })
                FilterChip(theme == "light", { Bridge.prefs.theme.value = "light" },
                    { Text(stringResource(R.string.theme_light)) })
                FilterChip(theme == "dark", { Bridge.prefs.theme.value = "dark" },
                    { Text(stringResource(R.string.theme_dark)) })
            }

            val lang by Bridge.prefs.language.flow.collectAsState()
            Text(stringResource(R.string.set_language),
                style = MaterialTheme.typography.labelMedium)
            Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                FilterChip(lang == "system", {
                    Bridge.prefs.language.value = "system"; App.applyLanguage("system")
                }, { Text(stringResource(R.string.lang_system)) })
                FilterChip(lang == "ru", {
                    Bridge.prefs.language.value = "ru"; App.applyLanguage("ru")
                }, { Text(stringResource(R.string.lang_ru)) })
                FilterChip(lang == "en", {
                    Bridge.prefs.language.value = "en"; App.applyLanguage("en")
                }, { Text(stringResource(R.string.lang_en)) })
            }

            val fontScale by Bridge.prefs.fontScale.flow.collectAsState()
            Text(stringResource(R.string.font_scale) + " (%.0f%%)".format(fontScale * 100),
                style = MaterialTheme.typography.labelMedium)
            Slider(
                value = fontScale,
                onValueChange = { Bridge.prefs.fontScale.value = it },
                valueRange = 0.85f..1.4f,
                steps = 10,
            )
        }

        // ── Звуки ──
        SettingsCard(stringResource(R.string.set_sounds)) {
            SwitchRow(stringResource(R.string.snd_vibration), Bridge.prefs.vibration.flow.collectAsState().value) {
                Bridge.prefs.vibration.value = it
            }
            SwitchRow(stringResource(R.string.snd_sos_siren), Bridge.prefs.sosSiren.flow.collectAsState().value) {
                Bridge.prefs.sosSiren.value = it
            }
            // ── #Ф9 Тихий режим (охота) ──
            SwitchRow(stringResource(R.string.quiet_mode),
                Bridge.prefs.quietMode.flow.collectAsState().value) {
                Bridge.prefs.quietMode.value = it
            }
            Text(stringResource(R.string.quiet_hint),
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
            val vol by Bridge.prefs.sirenVolume.flow.collectAsState()
            Text(stringResource(R.string.siren_volume) + " (%.0f%%)".format(vol * 100),
                style = MaterialTheme.typography.labelMedium)
            Slider(
                value = vol,
                onValueChange = { Bridge.prefs.sirenVolume.value = it },
                valueRange = 0.1f..1f,
            )

            // ── Мелодия звонка ──
            val rtName by Bridge.prefs.ringtoneName.flow.collectAsState()
            Text(
                stringResource(R.string.ringtone_title) + ": " +
                    rtName.ifEmpty { stringResource(R.string.ringtone_default) },
                style = MaterialTheme.typography.labelMedium,
            )
            val pickAudio = androidx.activity.compose.rememberLauncherForActivityResult(
                androidx.activity.result.contract.ActivityResultContracts.OpenDocument()
            ) { uri ->
                if (uri != null) {
                    try {
                        ctx.contentResolver.takePersistableUriPermission(uri,
                            android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION)
                    } catch (_: Exception) {}
                    Bridge.prefs.ringtoneUri.value = uri.toString()
                    Bridge.prefs.ringtoneName.value =
                        uri.lastPathSegment?.substringAfterLast('/')
                            ?.substringAfterLast(':') ?: "audio"
                }
            }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedButton(onClick = {
                    Bridge.prefs.ringtoneUri.value = ""
                    Bridge.prefs.ringtoneName.value = ""
                }) { Text(stringResource(R.string.ringtone_default)) }
                Button(onClick = { pickAudio.launch(arrayOf("audio/*")) }) {
                    Text(stringResource(R.string.ringtone_pick))
                }
            }
        }

        // ── V1.7.5 Звонки: усиление звука ──
        // AGC некоторых телефонов (Huawei) душит микрофон в звонке —
        // программный гейн лечит; действует сразу, даже посреди звонка.
        SettingsCard(stringResource(R.string.set_calls)) {
            val mic by Bridge.prefs.pttMicGain.flow.collectAsState()
            Text(stringResource(R.string.call_mic_gain) + ": ×%.1f".format(mic) +
                if (mic == 1f) " (" + stringResource(R.string.call_gain_off) + ")" else "",
                style = MaterialTheme.typography.labelMedium)
            Slider(
                value = mic,
                onValueChange = {
                    Bridge.prefs.pttMicGain.value = (it * 2).toInt() / 2f  // шаг 0.5
                },
                valueRange = 1f..8f,
            )
            Text(stringResource(R.string.call_mic_gain_hint),
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant)

            val rx by Bridge.prefs.pttRxGain.flow.collectAsState()
            Text(stringResource(R.string.call_rx_gain) + ": ×%.1f".format(rx) +
                if (rx == 1f) " (" + stringResource(R.string.call_gain_off) + ")" else "",
                style = MaterialTheme.typography.labelMedium)
            Slider(
                value = rx,
                onValueChange = {
                    Bridge.prefs.pttRxGain.value = (it * 2).toInt() / 2f
                },
                valueRange = 1f..4f,
            )
            Text(stringResource(R.string.call_rx_gain_hint),
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
        }

        // ── Локация ──
        SettingsCard(stringResource(R.string.set_location)) {
            SwitchRow(stringResource(R.string.loc_enable), Bridge.prefs.locEnabled.flow.collectAsState().value) {
                Bridge.prefs.locEnabled.value = it
            }
            val interval by Bridge.prefs.locIntervalSec.flow.collectAsState()
            Text(
                stringResource(R.string.loc_interval) + ": " +
                    stringResource(R.string.loc_interval_fmt, interval),
                style = MaterialTheme.typography.labelMedium,
            )
            Slider(
                value = interval.toFloat(),
                onValueChange = { Bridge.prefs.locIntervalSec.value = it.toInt() },
                valueRange = 10f..60f,
                steps = 9,
            )
            // ── #У5 Умный интервал: стоим — шлём реже ──
            SwitchRow(stringResource(R.string.loc_adaptive),
                Bridge.prefs.locAdaptive.flow.collectAsState().value) {
                Bridge.prefs.locAdaptive.value = it
            }
        }

        // ── Чат ──
        SettingsCard(stringResource(R.string.set_chat)) {
            SwitchRow(stringResource(R.string.enter_to_send), Bridge.prefs.enterToSend.flow.collectAsState().value) {
                Bridge.prefs.enterToSend.value = it
            }
        }

        // ── #2 Трек ──
        SettingsCard(stringResource(R.string.set_track)) {
            SwitchRow(stringResource(R.string.track_enable),
                Bridge.prefs.trackEnabled.flow.collectAsState().value) {
                Bridge.prefs.trackEnabled.value = it
            }
            val tmin by Bridge.prefs.trackMinMeters.flow.collectAsState()
            Text(stringResource(R.string.track_min_dist) + ": " +
                stringResource(R.string.track_min_dist_fmt, tmin),
                style = MaterialTheme.typography.labelMedium)
            Slider(value = tmin.toFloat(),
                onValueChange = { Bridge.prefs.trackMinMeters.value = it.toInt() },
                valueRange = 5f..100f, steps = 18)
            OutlinedButton(onClick = { nav.navigate("track") }) {
                Text(stringResource(R.string.track_title))
            }
        }

        // ── V2.9: веб-мост ──
        SettingsCard("Веб-чат") {
            val webOn by Bridge.prefs.webEnabled.flow.collectAsState()
            val webNet by Bridge.prefs.webNetName.flow.collectAsState()
            val webState by Bridge.web.state.collectAsState()
            Text(
                if (!webOn) "Выключен"
                else if (webState is easylink.yvak.inc.net.WebClient.State.Online)
                    "На связи" + (if (webNet.isNotBlank()) ": $webNet" else "")
                else "Включён, связи нет",
                style = MaterialTheme.typography.labelMedium,
            )
            Text(
                "Зеркало сети в браузере. Телефон работает мостом: передаёт " +
                    "эфир в интернет и выносит в эфир написанное с компа.",
                style = MaterialTheme.typography.bodySmall,
            )
            OutlinedButton(onClick = { nav.navigate("web") }) {
                Text("Настроить веб-чат")
            }
        }

        // ── #5 Барометр ──
        SettingsCard(stringResource(R.string.set_baro)) {
            if (!Bridge.baro.available) {
                Text(stringResource(R.string.baro_unavailable),
                    style = MaterialTheme.typography.labelMedium,
                    color = MaterialTheme.colorScheme.error)
            }
            SwitchRow(stringResource(R.string.baro_enable),
                Bridge.prefs.baroEnabled.flow.collectAsState().value) {
                Bridge.prefs.baroEnabled.value = it
                if (it) Bridge.baro.start() else Bridge.baro.stop()
            }
            SwitchRow(stringResource(R.string.baro_share),
                Bridge.prefs.baroShare.flow.collectAsState().value) {
                Bridge.prefs.baroShare.value = it
            }
            val r by Bridge.baro.reading.collectAsState()
            r?.let {
                Text(stringResource(R.string.baro_alt_fmt, it.altitudeM) + " · " +
                    stringResource(R.string.baro_pressure_fmt, it.pressureHpa,
                        Bridge.baro.trendLabel(it.trendHpaPerHour)),
                    style = MaterialTheme.typography.titleMedium,
                    color = MaterialTheme.colorScheme.primary)
            }
        }

        // ── #6 Маяк ──
        SettingsCard(stringResource(R.string.beacon_mode)) {
            SwitchRow(stringResource(R.string.beacon_on),
                Bridge.prefs.beaconMode.flow.collectAsState().value) {
                Bridge.prefs.beaconMode.value = it
            }
            Text(stringResource(R.string.beacon_hint),
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
            val be by Bridge.prefs.beaconEverySec.flow.collectAsState()
            Text(stringResource(R.string.beacon_every) + ": " +
                stringResource(R.string.beacon_every_fmt, be),
                style = MaterialTheme.typography.labelMedium)
            Slider(value = be.toFloat(),
                onValueChange = { Bridge.prefs.beaconEverySec.value = it.toInt() },
                valueRange = 10f..120f, steps = 21)
        }

        // ── #10 Dead-man ──
        SettingsCard(stringResource(R.string.set_deadman)) {
            val dm by Bridge.prefs.deadmanEnabled.flow.collectAsState()
            SwitchRow(stringResource(R.string.deadman_enable), dm) { Bridge.deadmanArm(it) }
            Text(stringResource(R.string.deadman_hint),
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
            val di by Bridge.prefs.deadmanMinutes.flow.collectAsState()
            Text(stringResource(R.string.deadman_interval) + ": " +
                stringResource(R.string.deadman_interval_fmt, di),
                style = MaterialTheme.typography.labelMedium)
            Slider(value = di.toFloat(),
                onValueChange = { Bridge.prefs.deadmanMinutes.value = it.toInt() },
                valueRange = 5f..240f, steps = 46,
                onValueChangeFinished = { if (dm) Bridge.deadmanCheckIn() })
            if (dm) {
                val deadline by Bridge.prefs.deadmanDeadline.flow.collectAsState()
                val left = ((deadline - System.currentTimeMillis()) / 60000).coerceAtLeast(0)
                Button(onClick = { Bridge.deadmanCheckIn() }) {
                    Text(stringResource(R.string.deadman_checkin) +
                        "  ·  " + stringResource(R.string.deadman_left_fmt, left))
                }
            }
        }

        // ── #4 Outbox ──
        SettingsCard(stringResource(R.string.set_outbox)) {
            SwitchRow(stringResource(R.string.retry_enable),
                Bridge.prefs.retryEnabled.flow.collectAsState().value) {
                Bridge.prefs.retryEnabled.value = it
            }
            Text(stringResource(R.string.retry_hint),
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
            val rw by Bridge.prefs.retryWindowMin.flow.collectAsState()
            Text(stringResource(R.string.retry_window) + ": " +
                stringResource(R.string.retry_window_fmt, rw),
                style = MaterialTheme.typography.labelMedium)
            Slider(value = rw.toFloat(),
                onValueChange = { Bridge.prefs.retryWindowMin.value = it.toInt() },
                valueRange = 5f..240f, steps = 46)
        }

        // ── Безопасность ──
        SettingsCard(stringResource(R.string.set_security)) {
            val connState by Bridge.ble.connState.collectAsState()
            Text(stringResource(R.string.psk_title),
                style = MaterialTheme.typography.labelMedium)
            Text(stringResource(R.string.psk_hint),
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
            var psk by rememberSaveable { mutableStateOf("") }
            OutlinedTextField(
                value = psk, onValueChange = { if (it.length <= 60) psk = it },
                label = { Text(stringResource(R.string.psk_field)) },
                singleLine = true,
            )
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(
                    onClick = { Bridge.device.setPsk(psk); psk = "" },
                    enabled = connState == ConnState.CONNECTED && psk.isNotBlank(),
                ) { Text(stringResource(R.string.psk_set)) }
                OutlinedButton(
                    onClick = { Bridge.device.setPsk("") },
                    enabled = connState == ConnState.CONNECTED,
                ) { Text(stringResource(R.string.psk_clear)) }
            }
            if (connState != ConnState.CONNECTED) {
                Text(stringResource(R.string.connect_first),
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.error)
            }
        }

        // ── Часы Huawei ──
        SettingsCard(stringResource(R.string.watch_title)) {
            val watchOn by Bridge.prefs.watchEnabled.flow.collectAsState()
            val wState by Bridge.wear.state.collectAsState()
            SwitchRow(stringResource(R.string.watch_enable), watchOn) {
                Bridge.prefs.watchEnabled.value = it
            }
            Text(
                when (val s = wState) {
                    is easylink.yvak.inc.repo.WearRepo.State.Off ->
                        stringResource(R.string.watch_state_off)
                    is easylink.yvak.inc.repo.WearRepo.State.Connecting ->
                        stringResource(R.string.watch_state_connecting)
                    is easylink.yvak.inc.repo.WearRepo.State.Ready ->
                        stringResource(R.string.watch_state_ready_fmt, s.deviceName)
                    is easylink.yvak.inc.repo.WearRepo.State.Error ->
                        stringResource(R.string.watch_state_error_fmt, s.reason)
                },
                style = MaterialTheme.typography.labelMedium,
                color = if (wState is easylink.yvak.inc.repo.WearRepo.State.Ready)
                    MaterialTheme.colorScheme.primary
                else MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Text(stringResource(R.string.watch_hint),
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
        }

        // ── Экспертный режим ──
        SettingsCard(stringResource(R.string.set_expert)) {
            val expert by Bridge.prefs.expertMode.flow.collectAsState()
            var showWarn by remember { mutableStateOf(false) }
            SwitchRow(stringResource(R.string.expert_enable), expert) {
                if (it) showWarn = true else Bridge.prefs.expertMode.value = false
            }
            Text(
                stringResource(if (expert) R.string.expert_on else R.string.expert_off),
                style = MaterialTheme.typography.labelMedium,
                color = if (expert) MaterialTheme.colorScheme.error
                else MaterialTheme.colorScheme.primary)
            Text(stringResource(R.string.expert_hint),
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
            if (showWarn) {
                androidx.compose.material3.AlertDialog(
                    onDismissRequest = { showWarn = false },
                    icon = { androidx.compose.material3.Icon(
                        androidx.compose.material.icons.Icons.Filled.Warning, null,
                        tint = MaterialTheme.colorScheme.error) },
                    title = { Text(stringResource(R.string.expert_warn_title)) },
                    text = { Text(stringResource(R.string.expert_warn_body)) },
                    confirmButton = {
                        androidx.compose.material3.TextButton(onClick = {
                            Bridge.prefs.expertMode.value = true; showWarn = false
                        }) { Text(stringResource(R.string.ok)) }
                    },
                    dismissButton = {
                        androidx.compose.material3.TextButton(onClick = { showWarn = false }) {
                            Text(stringResource(R.string.cancel))
                        }
                    },
                )
            }
        }

        // ── Справка ──
        Card(onClick = { nav.navigate("help") }) {
            Row(
                Modifier
                    .fillMaxSize()
                    .padding(16.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text("📖", style = MaterialTheme.typography.titleLarge)
                Column(Modifier.padding(start = 10.dp)) {
                    Text(stringResource(R.string.help_title),
                        style = MaterialTheme.typography.titleMedium)
                    Text(stringResource(R.string.help_open),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }
        }

        // ── О приложении ──
        SettingsCard(stringResource(R.string.set_about)) {
            Text("EasyLink 1.7 · " + stringResource(R.string.about_company))
            Text(stringResource(R.string.about_desc),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
            Text(stringResource(R.string.about_site),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.primary)
        }
    }
}

@Composable
private fun SettingsCard(title: String, content: @Composable () -> Unit) {
    Card {
        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(title, style = MaterialTheme.typography.titleMedium)
            content()
        }
    }
}

@Composable
private fun SwitchRow(label: String, checked: Boolean, onChange: (Boolean) -> Unit) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(label, Modifier.weight(1f))
        Switch(checked = checked, onCheckedChange = onChange)
    }
}
