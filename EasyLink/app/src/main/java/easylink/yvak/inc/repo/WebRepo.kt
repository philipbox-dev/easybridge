package easylink.yvak.inc.repo

import easylink.yvak.inc.Prefs
import easylink.yvak.inc.audio.WebAudioBridge
import easylink.yvak.inc.ble.BleClient
import easylink.yvak.inc.net.WebClient
import easylink.yvak.inc.proto.Proto
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import org.json.JSONObject

/**
 * Веб-мост. Телефон здесь не клиент чата, а **мост** между эфиром и
 * интернетом, и работы у него три:
 *
 *  1. зеркалить в веб то, что услышало радио;
 *  2. быть шлюзом: выносить в эфир сообщения, написанные из браузера
 *     тем, кого в вебе нет;
 *  3. перекодировать звук во время звонка (см. WebAudioBridge).
 *
 * Полная спецификация — docs/WEB_BRIDGE.md в репозитории прошивки.
 */
class WebRepo(
    private val prefs: Prefs,
    private val ble: BleClient,
    private val scope: CoroutineScope,
) {
    private val client = WebClient()
    private var bridge: WebAudioBridge? = null

    private val _state = MutableStateFlow<WebClient.State>(WebClient.State.Offline)
    val state: StateFlow<WebClient.State> get() = _state

    private val _session = MutableStateFlow<WebClient.Session?>(null)
    val session: StateFlow<WebClient.Session?> get() = _session

    /** Идёт ли звонок с эфирным плечом (для интерфейса). */
    private val _airCall = MutableStateFlow(false)
    val airCall: StateFlow<Boolean> get() = _airCall

    /** Последняя проблема, о которой сказал сервер. */
    private val _note = MutableStateFlow("")
    val note: StateFlow<String> get() = _note

    /** Что показывать в железе устройства: профиль и наличие FSK. */
    var hwProfile: () -> String = { "" }
    var hasFsk: () -> Boolean = { false }
    var myNodeId: () -> String = { "0x0" }

    /** Сообщение, пришедшее из веба (чтобы показать его в чате). */
    var onWebMessage: ((from: String, text: String, kind: String) -> Unit)? = null

    private var audioSeq = 0
    private var callTargets = ""
    private var callTargetCount = 0

    init {
        client.onState = { st ->
            _state.value = st
            if (st is WebClient.State.Online) onSocketOpen()
        }
        client.onJson = ::onServerJson
        client.onAudio = ::onServerAudio
    }

    // ── Жизненный цикл ──────────────────────────────────────
    fun start() {
        if (!prefs.webEnabled.value) return
        val url = prefs.webUrl.value
        val token = prefs.webToken.value
        if (url.isBlank()) return
        if (token.isBlank()) {
            scope.launch { loginAndConnect() }
            return
        }
        client.connect(url, token)
    }

    fun stop() {
        client.disconnect()
        endAirLeg()
        _session.value = null
    }

    /** Вход по паролю. Токен сохраняется и живёт до смены пароля. */
    suspend fun loginAndConnect(): Boolean {
        val url = prefs.webUrl.value
        val uid = prefs.webUid.value
        val pass = prefs.webPassword.value
        if (url.isBlank() || uid.isBlank() || pass.isBlank()) {
            _note.value = "Заполните адрес, ссылку и пароль"
            return false
        }
        _state.value = WebClient.State.Connecting
        val res = client.login(
            baseUrl = url, uid = uid, password = pass,
            nodeId = myNodeId(),
            deviceName = android.os.Build.MODEL ?: "Телефон",
            hwProfile = hwProfile(),
            // Врать здесь нельзя: сервер по этому флагу решает, можно ли
            // вообще выводить звонок в эфир.
            hasFsk = hasFsk(),
            canGateway = prefs.webGateway.value,
        )
        val session = res.getOrElse {
            _state.value = WebClient.State.Failed(client.lastError)
            _note.value = client.lastError
            return false
        }
        prefs.webToken.value = session.token
        prefs.webNetName.value = session.netName
        prefs.webMemberName.value = session.memberName
        _session.value = session
        client.connect(url, session.token)
        return true
    }

    /** Забыть вход: токен и пароль стираются, сокет закрывается. */
    fun logout() {
        stop()
        prefs.webToken.value = ""
        prefs.webPassword.value = ""
        prefs.webNetName.value = ""
        prefs.webMemberName.value = ""
    }

    private fun onSocketOpen() {
        _note.value = ""
        client.send {
            put("op", "gateway")
            put("on", prefs.webGateway.value && hasFsk())
        }
    }

    fun setGateway(on: Boolean) {
        prefs.webGateway.value = on
        client.send { put("op", "gateway"); put("on", on) }
    }

    // ── Эфир → веб ──────────────────────────────────────────
    /**
     * Всё, что услышало радио, уходит на сервер. packet_id обязателен:
     * по паре (узел, packet_id) сервер отбрасывает повторы — один пакет
     * слышат все телефоны в радиусе.
     */
    fun mirrorMessage(nodeId: Long, name: String, text: String, seq: Int, kind: String = "text") {
        if (!online()) return
        client.send {
            put("op", "lora_rx")
            put("node_id", hex(nodeId))
            put("packet_id", seq)
            put("kind", kind)
            put("text", text)
            put("name", name)
        }
    }

    fun mirrorSos(nodeId: Long, name: String, text: String, seq: Int) =
        mirrorMessage(nodeId, name, text.ifBlank { "SOS" }, seq, "sos")

    /** Манифест автономного устройства. */
    fun mirrorDevHello(e: Proto.Event.DevHello) {
        if (!online()) return
        client.send {
            put("op", "dev_hello")
            put("node_id", hex(e.nodeId))
            put("name", e.name)
            put("class", e.devClass)
            put("flags", e.flags)
            put("interval", e.intervalS)
            put("fields", org.json.JSONArray().apply {
                e.fields.forEach {
                    put(JSONObject()
                        .put("i", it.index).put("name", it.name)
                        .put("type", it.type).put("unit", it.unit)
                        .put("scale", it.scale))
                }
            })
            put("cmds", org.json.JSONArray().apply {
                e.cmds.forEach {
                    put(JSONObject().put("id", it.id).put("name", it.name)
                        .put("action", it.action))
                }
            })
        }
    }

    fun mirrorDevData(e: Proto.Event.DevData) {
        if (!online()) return
        client.send {
            put("op", "dev_data")
            put("node_id", hex(e.nodeId))
            put("rssi", e.rssi)
            put("v", org.json.JSONArray().apply {
                e.values.forEach {
                    put(JSONObject().put("i", it.index).put("t", it.type).put("v", it.raw))
                }
            })
        }
    }

    fun mirrorDevAck(e: Proto.Event.DevAck) {
        if (!online()) return
        client.send {
            put("op", "dev_ack")
            put("node_id", hex(e.nodeId))
            put("cmd", e.cmd)
            put("status", e.status)
            put("desc", e.desc)
        }
    }

    /** Кадры голоса из эфира во время звонка — на перекодирование. */
    fun onAirAudio(amr: ByteArray) {
        bridge?.feedFromAir(amr)
    }

    // ── Веб → эфир ──────────────────────────────────────────
    private fun onServerJson(o: JSONObject) {
        when (o.optString("op")) {
            "hello_ok" -> _note.value = ""

            "lora_tx" -> handleLoraTx(o)

            "dev_tx" -> {
                // Команда автономному устройству, нажатая в браузере.
                val node = o.optString("node_id")
                ble.sendJson(Proto.devCmd(parseHex(node), o.optInt("id"), o.optInt("arg")))
            }

            "call_air_start" -> handleAirStart(o)
            "call_air_end" -> endAirLeg()

            "msg" -> {
                val m = o.optJSONObject("msg") ?: return
                // Своё же сообщение обратно не показываем.
                if (m.optInt("from") == _session.value?.memberId) return
                onWebMessage?.invoke(m.optString("from_name"), m.optString("text"),
                    m.optString("kind", "text"))
            }

            "error" -> _note.value = o.optString("desc").ifBlank { o.optString("code") }
        }
    }

    private fun handleLoraTx(o: JSONObject) {
        val msgId = o.optInt("msg_id")
        val text = o.optString("text")
        val from = o.optString("from_name")
        // Пометка WEB нужна тому, у кого телефона под рукой нет: на
        // мештастике он увидит «[WEB] Philip: …» и поймёт, откуда
        // сообщение и почему оно короткое.
        val body = if (o.optBoolean("web_flag")) "[WEB] $from: $text" else "$from: $text"

        val targets = o.optJSONArray("targets")
        var sent = false
        if (targets != null && targets.length() > 0) {
            for (i in 0 until targets.length()) {
                val t = targets.optJSONObject(i) ?: continue
                val node = parseHex(t.optString("node"))
                if (node == 0L) continue
                ble.sendJson(Proto.send(prefs.nextSeq(), body, groupId = 0))
                sent = true
            }
        } else {
            ble.sendJson(Proto.send(prefs.nextSeq(), body, groupId = 0))
            sent = true
        }

        // Отчёт нужен, чтобы в браузере погасло «передаётся».
        client.send {
            put("op", "lora_tx_result")
            put("msg_id", msgId)
            put("ok", sent)
        }
    }

    // ── Звонок: эфирное плечо ───────────────────────────────
    private fun handleAirStart(o: JSONObject) {
        if (!hasFsk()) {
            _note.value = "У платы нет FSK — голос в эфир не уйдёт"
            return
        }
        val mode = o.optInt("mode", 1)
        val targets = o.optJSONArray("targets") ?: org.json.JSONArray()
        val names = ArrayList<String>()
        var firstNode = 0L
        for (i in 0 until targets.length()) {
            val t = targets.optJSONObject(i) ?: continue
            names.add(t.optString("name"))
            if (firstNode == 0L) firstNode = parseHex(t.optString("node"))
        }
        callTargets = names.joinToString(", ")
        callTargetCount = names.size

        val b = WebAudioBridge(
            onAmrToAir = { amr ->
                ble.sendJson(Proto.pttAudio(
                    android.util.Base64.encodeToString(amr, android.util.Base64.NO_WRAP)))
            },
            onPcmToWeb = { pcm ->
                audioSeq = (audioSeq + 1) and 0xFFFF
                client.sendAudio(pcm, audioSeq, WebClient.CODEC_PCM16, fromAir = true)
            },
        )
        if (!b.start()) {
            _note.value = "Не удалось поднять кодеки"
            return
        }
        bridge = b
        _airCall.value = true

        scope.launch {
            ble.sendJson(Proto.pttStart(mode, firstNode))
            // Анонс уходит FSK-кадром внутри уже начатой сессии, поэтому
            // строго после ptt_start и с небольшой паузой на переход.
            delay(600)
            val who = _session.value?.memberName.orEmpty()
            ble.sendJson(Proto.pttWeb(who.ifBlank { "веб" }, callTargetCount + 1))
        }
    }

    private fun endAirLeg() {
        if (!_airCall.value && bridge == null) return
        bridge?.stop()
        bridge = null
        _airCall.value = false
        // Забыть про ptt_stop нельзя: плата останется в FSK и оглохнет
        // для обычного LoRa до срабатывания watchdog.
        ble.sendJson(Proto.pttStop())
    }

    private fun onServerAudio(data: ByteArray) {
        val f = client.parseAudio(data) ?: return
        if (f.fromAir) return                       // это наше же эхо
        if (f.codec != WebClient.CODEC_PCM16) {
            // Мост умеет только PCM: сервер переводит звонок на него,
            // как только появляется эфирное плечо.
            return
        }
        bridge?.feedFromWeb(f.payload)
    }

    // ── Мелочи ──────────────────────────────────────────────
    private fun online() = _state.value is WebClient.State.Online

    private fun hex(id: Long) = "0x%08X".format(id)

    private fun parseHex(s: String): Long = try {
        val t = s.removePrefix("0x").removePrefix("0X")
        if (t.isEmpty()) 0L else t.toLong(16)
    } catch (_: NumberFormatException) { 0L }

    fun debugSnapshot(): List<Pair<String, String>> = listOf(
        "web state" to _state.value.toString(),
        "web net" to (prefs.webNetName.value.ifBlank { "—" }),
        "gateway" to prefs.webGateway.value.toString(),
        "air call" to _airCall.value.toString(),
        "audio dropped" to (bridge?.dropped?.toString() ?: "—"),
        "note" to _note.value.ifBlank { "—" },
    )
}
