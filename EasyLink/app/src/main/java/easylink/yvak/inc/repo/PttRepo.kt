package easylink.yvak.inc.repo

import android.content.Context
import android.util.Base64
import easylink.yvak.inc.audio.PttAudioEngine
import easylink.yvak.inc.audio.RingtonePlayer
import easylink.yvak.inc.ble.BleClient
import easylink.yvak.inc.proto.Proto
import easylink.yvak.inc.proto.PttState
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

/** Итог звонка — для записи в историю чата. */
enum class CallOutcome { ENDED, DECLINED, MISSED, CANCELLED }

/**
 * PTT-звонок (рация). С патченной прошивкой V2.9.1 звонок можно адресовать
 * конкретному узлу (node_id в ptt_start); старая прошивка зовёт всех.
 */
class PttRepo(
    private val context: Context,
    private val ble: BleClient,
    private val ringtone: RingtonePlayer,
    private val ringtoneUri: () -> String,
) {
    private val _state = MutableStateFlow<PttState>(PttState.Idle)
    val state: StateFlow<PttState> get() = _state

    /** 0 тишина, 1 передача, 2 приём — для индикации. */
    private val _activity = MutableStateFlow(0)
    val activity: StateFlow<Int> get() = _activity
    private var lastRxMs = 0L

    private var engine: PttAudioEngine? = null

    /** Кому был адресован текущий звонок (0 = всем) — для истории. */
    private var callPeerId = 0L
    private var callPeerName = ""

    /** Колбэк истории: (peerId, peerName, durationSec (-1 = не состоялся), outcome). */
    var onCallEnded: ((Long, String, Long, CallOutcome) -> Unit)? = null

    /** V1.7: последний собеседник — для «Перезвонить» (id, name, mode). */
    private val _lastCall = MutableStateFlow<Triple<Long, String, Int>?>(null)
    val lastCall: StateFlow<Triple<Long, String, Int>?> get() = _lastCall

    /** Выключить рингтон в тихом режиме (охота). */
    var quietMode: () -> Boolean = { false }

    /** V1.7.5: усиление микрофона/динамика из настроек (живое). */
    var micGain: () -> Float = { 1f }
    var rxGain: () -> Float = { 1f }

    /** Динамик/наушник во время звонка (по умолчанию — динамик, звонок держат не у уха). */
    private val _speakerOn = MutableStateFlow(true)
    val speakerOn: StateFlow<Boolean> get() = _speakerOn

    fun toggleSpeaker() {
        val on = !_speakerOn.value
        _speakerOn.value = on
        engine?.setSpeakerphone(on)
    }

    // ── V1.7.3: диагностика звонка ────────────────────────────
    /** Последняя статистика с устройства (evt:ptt_stats, раз в 2с). */
    private val _deviceStats = MutableStateFlow<Proto.Event.PttStats?>(null)
    val deviceStats: StateFlow<Proto.Event.PttStats?> get() = _deviceStats

    /** Счётчик evt:ptt_audio, дошедших до приложения. */
    @Volatile private var evtAudioCount = 0

    fun onDeviceStats(s: Proto.Event.PttStats) { _deviceStats.value = s }

    /**
     * Снимок всей цепочки для дебаг-панели: эфир FSK (устройство) →
     * очередь notify → BLE → декодер → динамик. Дёргается UI-тикером.
     */
    fun debugSnapshot(): List<Pair<String, String>> {
        val e = engine
        val d = _deviceStats.value
        val rxAge = System.currentTimeMillis() - lastRxMs
        return listOf(
            "FSK rx/tx (плата)" to (d?.let { "${it.fskRx} / ${it.fskTx}" } ?: "нет данных"),
            "FSK rx давность" to (d?.let { "${it.lastRxAgeMs} мс" } ?: "—"),
            "txq глубина/дропы" to (d?.let { "${it.txQueue} / ${it.txQueueDrops}" } ?: "—"),
            "notify дроп/обрыв" to (d?.let { "${it.notifyDrops} / ${it.notifyAborts}" } ?: "—"),
            "BLE ресинк JSON" to "${ble.assemblerResyncs}",
            "evt:audio в апп" to "$evtAudioCount",
            "звук давность" to if (lastRxMs == 0L) "ещё не было" else "$rxAge мс",
            "декодер пкт/кадр" to (e?.let { "${it.rxPackets} / ${it.framesDecoded}" } ?: "—"),
            "мусор/реанимаций" to (e?.let { "${it.garbageFrames} / ${it.decoderRecoveries}" } ?: "—"),
            "оч.декодера дроп" to (e?.rxQueueDrops?.toString() ?: "—"),
            "underruns/ошибки" to (e?.let { "${it.trackUnderruns} / ${it.decodeErrors}" } ?: "—"),
            "передано пачек" to (e?.txPacketsSent?.toString() ?: "—"),
        )
    }

    fun startCall(mode: Int, targetId: Long, targetName: String) {
        if (_state.value !is PttState.Idle) return
        callPeerId = targetId
        callPeerName = targetName
        _lastCall.value = Triple(targetId, targetName, mode)
        ble.sendJson(Proto.pttStart(mode, targetId))
        // Ждём, пока абонент возьмёт трубку (ptt_answered или первый звук).
        _state.value = PttState.Dialing(targetName, mode)
        startEngine()
    }

    /** Абонент взял трубку: устройство прислало ptt_answered (FSK ACCEPT). */
    fun onAnswered() {
        val s = _state.value as? PttState.Dialing ?: return
        _state.value = PttState.Active(s.peerName, s.mode, outgoing = true, startedMs = System.currentTimeMillis())
    }

    fun onIncoming(nodeId: Long, name: String, mode: Int) {
        if (_state.value is PttState.Active) return
        callPeerId = nodeId
        callPeerName = name
        _lastCall.value = Triple(nodeId, name, mode)
        _state.value = PttState.Incoming(nodeId, name, mode)
        if (!quietMode()) ringtone.start(ringtoneUri())
    }

    fun accept() {
        val s = _state.value as? PttState.Incoming ?: return
        ringtone.stop()
        // Сообщаем устройству — оно отправит FSK ACCEPT звонящему,
        // чтобы у него «ожидание ответа» сменилось на «соединено».
        ble.sendJson(Proto.pttAccept())
        _state.value = PttState.Active(s.name, s.mode, outgoing = false)
        startEngine()
    }

    fun decline() {
        ble.sendJson(Proto.pttStop())
        finish(CallOutcome.DECLINED)
    }

    fun end() {
        ble.sendJson(Proto.pttStop())
        finish(if (_state.value is PttState.Active) CallOutcome.ENDED
        else CallOutcome.CANCELLED)
    }

    /** Устройство сообщило state=idle (вторая сторона повесила / watchdog). */
    fun onDeviceState(active: Boolean) {
        if (active) return
        finish(when (_state.value) {
            is PttState.Incoming -> CallOutcome.MISSED
            is PttState.Active -> CallOutcome.ENDED
            // Звонили, но никто не ответил (watchdog устройства ~30с или отбой)
            is PttState.Dialing -> CallOutcome.CANCELLED
            else -> return
        })
    }

    fun onAudio(b64: String) {
        evtAudioCount++
        val data = try { Base64.decode(b64, Base64.NO_WRAP) } catch (_: Exception) { return }
        // Пришёл звук от абонента, а мы ещё «ждём ответа» — значит трубку
        // взяли (fallback, если FSK ACCEPT потерялся в горах).
        if (_state.value is PttState.Dialing) onAnswered()
        engine?.feedRx(data)
        lastRxMs = System.currentTimeMillis()
        if (_activity.value != 1) _activity.value = 2
    }

    fun setTalk(pressed: Boolean) {
        engine?.txActive = pressed
        _activity.value = when {
            pressed -> 1
            System.currentTimeMillis() - lastRxMs < 1000 -> 2
            else -> 0
        }
    }

    private fun startEngine() {
        if (engine != null) return
        _speakerOn.value = true   // движок включает динамик по умолчанию
        // Живой звук — ~20 evt/с; на дефолтном BLE-интервале это душит
        // notify-очередь устройства за пару секунд (глухо после начала).
        ble.requestHighPriority(true)
        engine = PttAudioEngine(context) { frames ->
            val b64 = Base64.encodeToString(frames, Base64.NO_WRAP)
            ble.sendJson(Proto.pttAudio(b64))
        }.also {
            it.micGain = micGain
            it.rxGain = rxGain
            if (!it.start()) {
                engine = null
                end()
            }
        }
    }

    private fun finish(outcome: CallOutcome) {
        ringtone.stop()
        engine?.stop()
        engine = null
        ble.requestHighPriority(false)
        _deviceStats.value = null
        evtAudioCount = 0
        lastRxMs = 0L
        _activity.value = 0
        val st = _state.value
        if (st == PttState.Idle) return
        _state.value = PttState.Idle
        val duration = if (st is PttState.Active)
            (System.currentTimeMillis() - st.startedMs) / 1000 else -1L
        onCallEnded?.invoke(callPeerId, callPeerName, duration, outcome)
        callPeerId = 0L
        callPeerName = ""
    }
}
