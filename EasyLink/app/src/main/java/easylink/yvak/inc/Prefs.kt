package easylink.yvak.inc

import android.content.Context
import android.content.SharedPreferences
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

/**
 * Все настройки приложения. SharedPreferences + StateFlow для Compose.
 */
class Prefs(context: Context) {
    private val sp: SharedPreferences =
        context.getSharedPreferences("easylink", Context.MODE_PRIVATE)

    // ── Внешний вид ──
    val theme = pref("theme", "system")            // system | light | dark
    val language = pref("language", "system")      // system | ru | en
    val fontScale = prefFloat("font_scale", 1.0f)

    // ── Звуки ──
    val soundMessage = prefBool("snd_message", true)
    val vibration = prefBool("vibration", true)
    val sosSiren = prefBool("sos_siren", true)
    val sirenVolume = prefFloat("siren_volume", 1.0f)

    // ── Локация ──
    val locEnabled = prefBool("loc_enabled", false)
    val locIntervalSec = prefInt("loc_interval", 30)   // 10..60

    // ── Чат ──
    val enterToSend = prefBool("enter_to_send", false)

    // ── Звонки ──
    /** URI выбранной мелодии звонка ("" = системная по умолчанию). */
    val ringtoneUri = pref("ringtone_uri", "")
    val ringtoneName = pref("ringtone_name", "")

    /** Наш node_id (из diag) — нужен для group_id личных чатов. */
    val myNodeId = pref("my_node_id", "0")

    // ── Часы Huawei (Wear Engine) ──
    val watchEnabled = prefBool("watch_enabled", false)

    // ── FSK-картинки: профиль эфира 0 дальнобой / 1 стандарт / 2 HD ──
    val imgProfile = prefInt("img_profile", 1)

    // ── #2 Трек ──
    val trackEnabled = prefBool("track_enabled", false)
    val trackMinMeters = prefInt("track_min_m", 15)     // мин. смещение для точки

    // ── #4 Outbox: досылка до ACK ──
    val retryEnabled = prefBool("retry_enabled", true)
    val retryWindowMin = prefInt("retry_window_min", 30)   // сколько досылать
    val retryEverySec = prefInt("retry_every_sec", 45)

    // ── #5 Барометр ──
    val baroEnabled = prefBool("baro_enabled", false)
    val baroShare = prefBool("baro_share", false)          // класть высоту в маячок
    val baroSeaLevelHpa = prefFloat("baro_sea_hpa", 1013.25f)

    // ── #6 Маяк «потерялся» ──
    val beaconMode = prefBool("beacon_mode", false)         // runtime-режим
    val beaconEverySec = prefInt("beacon_every_sec", 20)

    // ── #10 Dead-man таймер ──
    val deadmanEnabled = prefBool("deadman_enabled", false)
    val deadmanMinutes = prefInt("deadman_min", 60)
    val deadmanDeadline = prefLong("deadman_deadline", 0L)  // ts когда сработает

    // ── Range test ──
    val rtPingEverySec = prefInt("rt_ping_sec", 10)

    // ── Экспертный режим / правовое ──
    val expertMode = prefBool("expert_mode", false)      // разблокирует высокую мощность
    val disclaimerAccepted = prefBool("disclaimer_ok", false)

    // ── V1.7 ──
    val onboardingDone = prefBool("onboarding_done", false)
    val locAdaptive = prefBool("loc_adaptive", true)     // умный интервал маячка
    val quietMode = prefBool("quiet_mode", false)        // тихий режим (охота)
    /** Закреплённые сообщения: JSON {chatKey: text}. */
    val pinnedJson = pref("pinned_json", "{}")

    // ── V1.7.5: усиление звука в звонках (Huawei душит микрофон AGC) ──
    /** Усиление микрофона в звонке, 1.0 = как есть, до 8.0. */
    val pttMicGain = prefFloat("ptt_mic_gain", 1f)
    /** Усиление входящего звука в звонке, 1.0 = как есть, до 4.0. */
    val pttRxGain = prefFloat("ptt_rx_gain", 1f)

    // ── V2.9: веб-мост ──
    // Телефон здесь не клиент чата, а мост: зеркалит эфир в веб и
    // выносит в эфир то, что написали из браузера.
    val webEnabled = prefBool("web_enabled", false)
    /** Адрес сервера, например https://meetjimproviz.ru */
    val webUrl = pref("web_url", "")
    /** Персональный uid из ссылки /lora-chat/<uid>/chat. */
    val webUid = pref("web_uid", "")
    val webPassword = pref("web_password", "")
    /** Токен устройства, выданный сервером. Живёт до смены пароля. */
    val webToken = pref("web_token", "")
    /** Имя сети и участника — только для показа в интерфейсе. */
    val webNetName = pref("web_net_name", "")
    val webMemberName = pref("web_member_name", "")
    /**
     * Работать шлюзом в эфир: выносить чужие сообщения из веба в LoRa.
     * Стоит выключать в роуминге и на последних процентах заряда —
     * это постоянный сокет и радио в приёме.
     */
    val webGateway = prefBool("web_gateway", true)

    // ── Профиль/устройство ──
    val myName = pref("my_name", "")
    val lastDeviceAddr = pref("last_addr", "")
    val lastDeviceName = pref("last_name", "")
    val autoReconnect = prefBool("auto_reconnect", true)

    /** Сквозной счётчик seq для команд send/voice_tx. */
    @Synchronized
    fun nextSeq(): Int {
        val v = sp.getInt("app_seq", 1)
        sp.edit().putInt("app_seq", if (v >= 0x7FFFFF) 1 else v + 1).apply()
        return v
    }

    // ── Обёртки ──
    inner class PrefStr(private val key: String, default: String) {
        private val state = MutableStateFlow(sp.getString(key, default) ?: default)
        val flow: StateFlow<String> get() = state
        var value: String
            get() = state.value
            set(v) { sp.edit().putString(key, v).apply(); state.value = v }
    }
    inner class PrefBool(private val key: String, default: Boolean) {
        private val state = MutableStateFlow(sp.getBoolean(key, default))
        val flow: StateFlow<Boolean> get() = state
        var value: Boolean
            get() = state.value
            set(v) { sp.edit().putBoolean(key, v).apply(); state.value = v }
    }
    inner class PrefInt(private val key: String, default: Int) {
        private val state = MutableStateFlow(sp.getInt(key, default))
        val flow: StateFlow<Int> get() = state
        var value: Int
            get() = state.value
            set(v) { sp.edit().putInt(key, v).apply(); state.value = v }
    }
    inner class PrefFloat(private val key: String, default: Float) {
        private val state = MutableStateFlow(sp.getFloat(key, default))
        val flow: StateFlow<Float> get() = state
        var value: Float
            get() = state.value
            set(v) { sp.edit().putFloat(key, v).apply(); state.value = v }
    }
    inner class PrefLong(private val key: String, default: Long) {
        private val state = MutableStateFlow(sp.getLong(key, default))
        val flow: StateFlow<Long> get() = state
        var value: Long
            get() = state.value
            set(v) { sp.edit().putLong(key, v).apply(); state.value = v }
    }

    private fun pref(key: String, default: String) = PrefStr(key, default)
    private fun prefBool(key: String, default: Boolean) = PrefBool(key, default)
    private fun prefInt(key: String, default: Int) = PrefInt(key, default)
    private fun prefFloat(key: String, default: Float) = PrefFloat(key, default)
    private fun prefLong(key: String, default: Long) = PrefLong(key, default)
}
