package easylink.yvak.inc

import android.content.Context
import easylink.yvak.inc.audio.Siren
import easylink.yvak.inc.audio.VoicePlayer
import easylink.yvak.inc.audio.VoiceRecorder
import easylink.yvak.inc.ble.BleClient
import easylink.yvak.inc.ble.BridgeService
import easylink.yvak.inc.db.Db
import easylink.yvak.inc.proto.ConnState
import easylink.yvak.inc.proto.LogDir
import easylink.yvak.inc.proto.Proto
import easylink.yvak.inc.proto.SosAlert
import easylink.yvak.inc.repo.ChatRepo
import easylink.yvak.inc.repo.DebugRepo
import easylink.yvak.inc.repo.DeviceRepo
import easylink.yvak.inc.repo.LocationRepo
import easylink.yvak.inc.repo.NodeRepo
import easylink.yvak.inc.repo.PttRepo
import easylink.yvak.inc.repo.UiNote
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.launch

/**
 * Композиционный корень: BLE-клиент, репозитории, маршрутизация событий.
 * Живёт в процессе (инициализируется в App), сервис лишь держит процесс.
 */
object Bridge {
    lateinit var appContext: Context
        private set
    lateinit var prefs: Prefs
    lateinit var db: Db
    lateinit var ble: BleClient
    lateinit var debug: DebugRepo
    lateinit var nodes: NodeRepo
    lateinit var location: LocationRepo
    lateinit var chat: ChatRepo
    lateinit var device: DeviceRepo
    lateinit var ptt: PttRepo
    lateinit var image: easylink.yvak.inc.repo.ImageRepo
    lateinit var crypto: easylink.yvak.inc.repo.CryptoRepo
    lateinit var baro: easylink.yvak.inc.repo.BaroRepo
    lateinit var track: easylink.yvak.inc.repo.TrackRepo
    lateinit var rangeTest: easylink.yvak.inc.repo.RangeTestRepo
    lateinit var check: easylink.yvak.inc.repo.CheckRepo
    lateinit var recorder: VoiceRecorder
    lateinit var player: VoicePlayer
    lateinit var siren: Siren
    lateinit var notif: NotifHelper
    lateinit var wear: easylink.yvak.inc.repo.WearRepo
    /** V2.9: мост в веб — зеркалит эфир и выносит веб-сообщения в LoRa. */
    lateinit var web: easylink.yvak.inc.repo.WebRepo

    val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)

    /** Активная SOS-тревога (оверлей + сирена). */
    val sosAlert = MutableStateFlow<SosAlert?>(null)

    /** Приложение на переднем плане? (для решения о уведомлениях) */
    @Volatile var appVisible = false

    private var initialized = false

    fun init(context: Context) {
        if (initialized) return
        initialized = true
        appContext = context.applicationContext
        prefs = Prefs(appContext)
        db = Db(appContext)
        ble = BleClient(appContext)
        debug = DebugRepo(appContext)
        nodes = NodeRepo(ble)
        location = LocationRepo(db, scope)
        recorder = VoiceRecorder(appContext)
        player = VoicePlayer()
        siren = Siren(appContext)
        crypto = easylink.yvak.inc.repo.CryptoRepo(appContext)
        chat = ChatRepo(db, prefs, ble, scope, recorder, location, crypto)
        device = DeviceRepo(ble)
        baro = easylink.yvak.inc.repo.BaroRepo(appContext, prefs)
        track = easylink.yvak.inc.repo.TrackRepo(appContext, db, prefs, scope)
        rangeTest = easylink.yvak.inc.repo.RangeTestRepo(
            appContext, ble, prefs, scope, nodes, location)
        check = easylink.yvak.inc.repo.CheckRepo(ble, prefs, scope, nodes)
        chat.onServiceText = { id, name, text -> check.tryIntercept(id, name, text) }
        check.onIncomingNotify = { name ->
            if (!appVisible) notif.notifyCheckIn(name)
        }
        image = easylink.yvak.inc.repo.ImageRepo(appContext, ble, scope)
        ptt = PttRepo(appContext, ble, easylink.yvak.inc.audio.RingtonePlayer(appContext)) {
            prefs.ringtoneUri.value
        }
        ptt.quietMode = { prefs.quietMode.value }
        ptt.micGain = { prefs.pttMicGain.value }
        ptt.rxGain = { prefs.pttRxGain.value }
        notif = NotifHelper(appContext)

        // ── V2.9: веб-мост ──
        web = easylink.yvak.inc.repo.WebRepo(prefs, ble, scope)
        // Железо сервер спрашивает у нас: по has_fsk он решает, можно ли
        // выводить звонок в эфир, а профиль показывает людям.
        web.hasFsk = { device.radio.value?.fsk ?: false }
        web.hwProfile = { device.radio.value?.profile.orEmpty() }
        web.myNodeId = { prefs.myNodeId.value }
        web.onWebMessage = { from, text, kind ->
            chat.insertWebMessage(easylink.yvak.inc.proto.CHAT_GENERAL, from, text, kind)
        }
        scope.launch {
            prefs.webEnabled.flow.collect { on -> if (on) web.start() else web.stop() }
        }

        // История звонков → системное сообщение в ЛС (или общий канал)
        ptt.onCallEnded = { peerId, peerName, durationSec, outcome ->
            val time = java.text.SimpleDateFormat("HH:mm", java.util.Locale.getDefault())
                .format(java.util.Date())
            val text = when (outcome) {
                easylink.yvak.inc.repo.CallOutcome.ENDED ->
                    appContext.getString(R.string.call_log_ended_fmt,
                        "%d:%02d".format(durationSec.coerceAtLeast(0) / 60,
                            durationSec.coerceAtLeast(0) % 60), time)
                easylink.yvak.inc.repo.CallOutcome.DECLINED ->
                    appContext.getString(R.string.call_log_declined)
                easylink.yvak.inc.repo.CallOutcome.MISSED ->
                    appContext.getString(R.string.call_log_missed)
                easylink.yvak.inc.repo.CallOutcome.CANCELLED ->
                    appContext.getString(R.string.call_log_cancelled)
            }
            chat.insertCallLog(peerId, peerName, text)
            if (outcome == easylink.yvak.inc.repo.CallOutcome.MISSED) {
                notif.notifyMissed(peerName)
            }
        }

        // ── Часы Huawei ──
        wear = easylink.yvak.inc.repo.WearRepo(appContext)
        wear.onCommand = { cmd ->
            when (cmd.optString("t")) {
                "send" -> {
                    val text = cmd.optString("text")
                    if (text.isNotBlank()) chat.sendText(easylink.yvak.inc.proto.CHAT_GENERAL, text)
                }
                "sos" -> device.sos()
                "get" -> pushWatchStatus()
            }
        }
        scope.launch {
            prefs.watchEnabled.flow.collect { on ->
                if (on) wear.start() else wear.stop()
            }
        }
        scope.launch {
            chat.incoming.collect { msg ->
                if (prefs.watchEnabled.value) {
                    wear.pushMessage(
                        msg.nodeName.ifEmpty { "?" },
                        if (msg.kind == easylink.yvak.inc.proto.MsgKind.VOICE)
                            "🎤 голосовое" else msg.text)
                }
            }
        }
        scope.launch {
            device.diag.collect { pushWatchStatus() }
        }
        // Своя локация → запись трека + барометр высота (для шаринга)
        scope.launch {
            location.myLocation.collect { l -> if (l != null) track.onLocation(l) }
        }
        if (prefs.baroEnabled.value) baro.start()

        // Принятая по FSK картинка → в общий чат
        scope.launch {
            image.received.collect { r ->
                chat.insertImage(easylink.yvak.inc.proto.CHAT_GENERAL, r.path,
                    easylink.yvak.inc.proto.MsgDir.IN, r.nodeId, r.nodeName)
            }
        }

        // V1.7: автоподключение к последнему устройству при старте приложения
        scope.launch {
            kotlinx.coroutines.delay(1200)
            val addr = prefs.lastDeviceAddr.value
            if (prefs.autoReconnect.value && addr.isNotEmpty() &&
                ble.connState.value == ConnState.DISCONNECTED) {
                try { connect(addr, prefs.lastDeviceName.value) } catch (_: Exception) {}
            }
        }

        // Виджет: обновление при смене состояния (мягкий троттлинг внутри)
        scope.launch { ble.connState.collect { pokeWidget() } }
        scope.launch { chat.incoming.collect { pokeWidget(force = true) } }

        scope.launch { ble.sentLog.collect { debug.add(LogDir.TX, it) } }
        scope.launch { ble.events.collect { onRawEvent(it) } }
        scope.launch {
            ble.connState.collect { st ->
                debug.add(LogDir.INFO, "conn: $st")
                if (st == ConnState.CONNECTED) onConnected()
            }
        }
        scope.launch {
            chat.incoming.collect { msg ->
                if (!appVisible || chat.currentChatKey != msg.chatKey) {
                    notif.notifyMessage(msg)
                }
            }
        }
    }

    // ── Подключение ───────────────────────────────────────────
    fun connect(addr: String, name: String) {
        prefs.lastDeviceAddr.value = addr
        prefs.lastDeviceName.value = name
        BridgeService.start(appContext)
        ble.connect(addr, name)
    }

    fun disconnect() {
        ble.disconnect()
        BridgeService.stop(appContext)
    }

    private suspend fun onConnected() {
        delay(400)
        val name = prefs.myName.value
        if (name.isNotBlank()) ble.sendJson(Proto.setName(name))
        delay(150)
        ble.sendJson(Proto.diag())
        delay(150)
        ble.sendJson(Proto.getRadio())
        delay(150)
        ble.sendJson(Proto.nodes())
        delay(150)
        ble.sendJson(Proto.discover())
    }

    // ── Маршрутизация событий устройства ──────────────────────
    private fun onRawEvent(json: String) {
        debug.add(LogDir.RX, json)
        when (val e = Proto.parseEvent(json) ?: return) {
            is Proto.Event.Msg -> {
                nodes.touch(e.nodeId, e.rssi, e.hops)
                chat.onIncomingText(
                    e.seq, e.rssi, e.groupId, e.nodeId, e.hops, e.text, nodes.name(e.nodeId))
                // Зеркалим в веб всё, что услышало радио: для человека
                // за компом это может быть единственная копия.
                web.mirrorMessage(e.nodeId, nodes.name(e.nodeId), e.text, e.seq)
            }
            is Proto.Event.Ack -> chat.onAck(e.seq)
            is Proto.Event.Sent -> chat.onSent(e.seq)
            is Proto.Event.VoiceSent -> chat.onVoiceSent(e.seq)
            is Proto.Event.Error -> {
                debug.add(LogDir.ERR, json)
                chat.onSendError(e.seq)
                device.note(UiNote.DeviceError(e.desc))
            }
            is Proto.Event.Voice -> chat.onVoiceChunk(
                e.nodeId, e.seq, e.idx, e.total, e.dataB64, nodes.name(e.nodeId))
            is Proto.Event.NodeSeen ->
                nodes.onNodeSeen(e.nodeId, e.name, e.rssi, e.hops, e.hw, e.batt)
            is Proto.Event.Nodes -> nodes.onNodesDump(e.list)
            // ── V2.9: автономные устройства → в веб ──
            is Proto.Event.DevHello -> web.mirrorDevHello(e)
            is Proto.Event.DevData -> web.mirrorDevData(e)
            is Proto.Event.DevAck -> web.mirrorDevAck(e)
            is Proto.Event.Peer -> { /* legacy peer status — узлы уже в node_seen */ }
            is Proto.Event.SosIn -> onSos(SosAlert(e.nodeId,
                nodes.name(e.nodeId), e.text, e.rssi))
            is Proto.Event.Panic -> onSos(SosAlert(e.nodeId,
                nodes.name(e.nodeId), "", e.rssi, isPanic = true))
            is Proto.Event.SosSent -> device.note(UiNote.SosSent(e.count))
            is Proto.Event.PttIncoming -> {
                ptt.onIncoming(e.nodeId, e.name, e.mode)
                if (!appVisible) notif.notifyPtt(e.name)
            }
            is Proto.Event.PttAudio -> {
                ptt.onAudio(e.b64)
                // Во время звонка с веб-плечом тот же поток уходит на
                // перекодирование: люди в браузере должны слышать эфир.
                if (web.airCall.value) {
                    runCatching {
                        web.onAirAudio(android.util.Base64.decode(
                            e.b64, android.util.Base64.NO_WRAP))
                    }
                }
            }
            is Proto.Event.PttStateEvt -> ptt.onDeviceState(e.active)
            is Proto.Event.PttAnswered -> ptt.onAnswered()
            is Proto.Event.PttStats -> ptt.onDeviceStats(e)
            is Proto.Event.Image -> {
                nodes.touch(e.nodeId, 0, 0)
                image.onImageChunk(e.nodeId, e.idx, e.total, e.dataB64, nodes.name(e.nodeId))
            }
            is Proto.Event.ImageIncoming -> image.onIncomingStart(e.name)
            is Proto.Event.ImageTxProgress -> image.onTxProgress(e.pass, e.of)
            is Proto.Event.ImageSent -> image.onSent()
            is Proto.Event.ImageFail -> image.onImageFail(e.got, e.total)
            is Proto.Event.ChanSwitch ->
                device.note(UiNote.SpeedSwitch(e.sf != 0, e.inSec))
            is Proto.Event.Speed -> {
                device.onSpeed(e.sf, e.applied)
                if (e.applied) device.note(UiNote.SpeedApplied(e.sf != 0))
                else if (e.inSec > 0) device.note(UiNote.SpeedSwitch(e.sf != 0, e.inSec))
            }
            is Proto.Event.Diag -> {
                device.onDiag(e.info)
                // Наш node_id — нужен для group_id личных чатов
                val id = e.info.nodeId.removePrefix("0x").toLongOrNull(16) ?: 0L
                if (id != 0L) prefs.myNodeId.value = id.toString()
            }
            is Proto.Event.Hw -> device.onHw(e.loraOk, e.rssi, e.sf, e.nodes)
            is Proto.Event.Radio -> device.onRadio(e.info)
            is Proto.Event.NameSet -> device.note(UiNote.NameSaved)
            is Proto.Event.Psk -> device.note(UiNote.Psk(e.enabled))
            is Proto.Event.Blocked -> nodes.onBlocked(e.nodeId, true)
            is Proto.Event.Unblocked -> nodes.onBlocked(e.nodeId, false)
            is Proto.Event.BlePong -> {
                debug.onPong()
                device.note(UiNote.Ping(debug.lastPingMs.value))
            }
            is Proto.Event.GroupJoined -> {
                chat.onGroupJoined(e.groupId, e.name, e.from)
                device.note(UiNote.GroupJoined(e.name))
            }
            is Proto.Event.GroupLeft ->
                chat.insertSystem("g%08x".format(e.groupId),
                    "← ${nodes.name(e.from)}")
            is Proto.Event.TxPower -> device.onRadio(
                device.radio.value.copy(txIdx = e.idx, txDbm = e.dbm))
            is Proto.Event.AntennaSet -> device.onRadio(
                device.radio.value.copy(antenna = e.type))
            is Proto.Event.Unknown -> debug.add(LogDir.INFO, "unknown evt: $json")
        }
    }

    @Volatile private var lastWidgetMs = 0L
    private fun pokeWidget(force: Boolean = false) {
        val now = System.currentTimeMillis()
        if (!force && now - lastWidgetMs < 5000) return
        lastWidgetMs = now
        easylink.yvak.inc.widget.EasyLinkWidget.updateAll(appContext)
    }

    private fun pushWatchStatus() {
        if (!prefs.watchEnabled.value) return
        wear.pushStatus(
            ble.connState.value == ConnState.CONNECTED,
            nodes.nodes.value.values.count { it.isOnline })
    }

    private fun onSos(alert: SosAlert) {
        sosAlert.value = alert
        if (prefs.watchEnabled.value) wear.pushSos(alert.name)
        chat.insertSos(alert.nodeId, alert.name,
            if (alert.isPanic) "PANIC" else alert.text, alert.rssi)
        if (prefs.sosSiren.value) {
            siren.start(prefs.sirenVolume.value, prefs.vibration.value)
        }
        if (!appVisible) notif.notifySos(alert)
    }

    fun dismissSos() {
        siren.stop()
        sosAlert.value = null
        notif.cancelSos()
    }

    // ── #10 Dead-man таймер ───────────────────────────────────
    /** Отметиться «я в порядке» — сдвинуть дедлайн. */
    fun deadmanCheckIn() {
        if (!prefs.deadmanEnabled.value) return
        prefs.deadmanDeadline.value =
            System.currentTimeMillis() + prefs.deadmanMinutes.value.coerceIn(5, 720) * 60_000L
    }

    fun deadmanArm(enabled: Boolean) {
        prefs.deadmanEnabled.value = enabled
        prefs.deadmanDeadline.value =
            if (enabled) System.currentTimeMillis() + prefs.deadmanMinutes.value.coerceIn(5, 720) * 60_000L
            else 0L
    }

    /** Дедлайн истёк — авто-SOS с последней позицией. */
    fun fireDeadmanSos() {
        // Обезоруживаем, чтобы не повторялось каждую секунду
        prefs.deadmanDeadline.value = 0L
        prefs.deadmanEnabled.value = false
        val loc = location.myLocation.value
        val name = prefs.myName.value.ifEmpty { "я" }
        val alert = SosAlert(prefs.myNodeId.value.toLongOrNull() ?: 0L, name,
            "DEADMAN" + (loc?.let {
                String.format(java.util.Locale.US, " %.5f,%.5f", it.latitude, it.longitude)
            } ?: ""), 0)
        sosAlert.value = alert
        if (prefs.sosSiren.value) siren.start(prefs.sirenVolume.value, prefs.vibration.value)
        notif.notifySos(alert)
        // Пробуем и в эфир, если подключены
        if (ble.connState.value == ConnState.CONNECTED) device.sos()
    }

    /** Отправить фото по FSK из чата: сжать, показать у себя, блнуть на устройство. */
    fun sendImage(chatKey: String, uri: android.net.Uri) {
        scope.launch(Dispatchers.IO) {
            val prepared = image.prepare(uri)
            if (prepared == null) {
                device.note(UiNote.DeviceError("image compress failed"))
                return@launch
            }
            val (file, bytes) = prepared
            chat.insertImage(chatKey, file.absolutePath, easylink.yvak.inc.proto.MsgDir.OUT)
            val peer = easylink.yvak.inc.proto.dmPeerOf(chatKey)
            image.blast(bytes, peer, prefs.imgProfile.value)
        }
    }

    /** Пинг с замером времени. */
    fun sendPing() {
        debug.pingSentAt = System.currentTimeMillis()
        ble.sendJson(Proto.bleTest())
    }

    /** Произвольная команда из дебаг-консоли. */
    fun sendRaw(json: String) {
        ble.sendJson(json)
    }
}
