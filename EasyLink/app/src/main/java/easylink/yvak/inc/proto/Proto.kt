package easylink.yvak.inc.proto

import org.json.JSONArray
import org.json.JSONObject

/**
 * BLE JSON-протокол EasyBridge (bridge.c v2.5+).
 * Команды телефон → устройство и разбор событий устройство → телефон.
 */
object Proto {

    // ── Команды ───────────────────────────────────────────────
    fun send(seq: Int, text: String, groupId: Long = 0): String {
        val o = JSONObject().put("cmd", "send").put("seq", seq).put("text", text)
        if (groupId != 0L) o.put("group_id", "0x%08x".format(groupId))
        return o.toString()
    }

    fun setName(name: String) = JSONObject().put("cmd", "setname").put("name", name).toString()
    fun status() = """{"cmd":"status"}"""
    fun discover() = """{"cmd":"discover"}"""
    fun diag() = """{"cmd":"diag"}"""
    fun nodes() = """{"cmd":"nodes"}"""
    fun bleTest() = """{"cmd":"ble_test"}"""
    fun sos() = """{"cmd":"sos"}"""
    fun getRadio() = """{"cmd":"get_radio"}"""
    fun speed(fast: Boolean) =
        JSONObject().put("cmd", "speed").put("mode", if (fast) "fast" else "slow").toString()
    fun setTxPower(idx: Int) = JSONObject().put("cmd", "set_tx_power").put("idx", idx).toString()
    fun setAntenna(type: String) = JSONObject().put("cmd", "set_antenna").put("type", type).toString()
    fun setPsk(pass: String) = JSONObject().put("cmd", "setpsk").put("pass", pass).toString()
    fun blockNode(id: Long) =
        JSONObject().put("cmd", "blocknode").put("node_id", "0x%08x".format(id)).toString()
    fun unblockNode(id: Long) =
        JSONObject().put("cmd", "unblocknode").put("node_id", "0x%08x".format(id)).toString()

    fun groupJoin(nodeId: Long, groupId: Long, name: String) = JSONObject()
        .put("cmd", "groupjoin")
        .put("node_id", "0x%08x".format(nodeId))
        .put("group_id", "0x%08x".format(groupId))
        .put("name", name)
        .toString()

    fun groupLeave(groupId: Long) = JSONObject()
        .put("cmd", "groupleave").put("group_id", "0x%08x".format(groupId)).toString()

    fun pttStart(mode: Int, nodeId: Long = 0) = JSONObject().put("cmd", "ptt_start")
        .put("mode", mode)
        .apply { if (nodeId != 0L) put("node_id", "0x%08x".format(nodeId)) }
        .toString()
    fun pttStop() = """{"cmd":"ptt_stop"}"""
    fun pttAccept() = """{"cmd":"ptt_accept"}"""
    fun pttAudio(b64: String) = JSONObject().put("cmd", "ptt_audio").put("d", b64).toString()

    /**
     * V2.9: сказать плате, кто участвует в звонке со стороны веба.
     * Зовётся ПОСЛЕ ptt_start: анонс уходит FSK-кадром внутри уже
     * начатой сессии — по LoRa он бы опоздал, участники к тому моменту
     * уже переключились на FSK.
     */
    fun pttWeb(names: String, count: Int) = JSONObject()
        .put("cmd", "ptt_web").put("names", names).put("n", count).toString()

    /** V2.9: команда автономному устройству (реле, «опросить сейчас»). */
    fun devCmd(nodeId: Long, id: Int, arg: Int) = JSONObject()
        .put("cmd", "devcmd")
        .put("node_id", "0x%08X".format(nodeId))
        .put("id", id).put("arg", arg).toString()

    fun voiceTx(seq: Int, idx: Int, total: Int, b64: String) = JSONObject()
        .put("cmd", "voice_tx").put("seq", seq).put("idx", idx).put("total", total)
        .put("data", b64).toString()

    fun imgTx(idx: Int, total: Int, prof: Int, nodeId: Long, b64: String) = JSONObject()
        .put("cmd", "img_tx").put("idx", idx).put("total", total).put("prof", prof)
        .apply { if (nodeId != 0L) put("node_id", "0x%08x".format(nodeId)) }
        .put("data", b64).toString()

    // ── События ───────────────────────────────────────────────
    sealed class Event {
        data class Msg(val seq: Int, val rssi: Int, val groupId: Long, val nodeId: Long,
                       val hops: Int, val text: String) : Event()
        data class Ack(val seq: Int) : Event()
        data class Sent(val seq: Int) : Event()
        data class Error(val code: Int, val desc: String, val seq: Int) : Event()
        data class Voice(val nodeId: Long, val seq: Int, val idx: Int, val total: Int,
                         val dataB64: String) : Event()
        data class VoiceSent(val seq: Int) : Event()
        data class Image(val nodeId: Long, val idx: Int, val total: Int, val dataB64: String) : Event()
        data class ImageIncoming(val nodeId: Long, val name: String) : Event()
        data class ImageTxProgress(val pass: Int, val of: Int) : Event()
        data object ImageSent : Event()
        data class ImageFail(val got: Int, val total: Int) : Event()
        data class NodeSeen(val nodeId: Long, val name: String, val rssi: Int, val hops: Int,
                            val hw: Int, val batt: Int) : Event()
        data class Nodes(val list: List<NodeInfo>) : Event()
        data class Peer(val name: String, val online: Boolean, val rssi: Int) : Event()
        data class SosIn(val nodeId: Long, val seq: Int, val rssi: Int, val text: String) : Event()
        data class Panic(val nodeId: Long, val rssi: Int) : Event()
        data class SosSent(val count: Int) : Event()
        data class PttIncoming(val nodeId: Long, val name: String, val mode: Int) : Event()
        data class PttAudio(val b64: String) : Event()
        data class PttStateEvt(val active: Boolean) : Event()
        data object PttAnswered : Event()
        /** V1.7.3: статистика звонка с устройства (раз в 2с, для дебаг-панели). */
        data class PttStats(val fskRx: Int, val fskTx: Int, val txQueue: Int,
                            val txQueueDrops: Int, val notifyDrops: Int,
                            val notifyAborts: Int, val lastRxAgeMs: Int) : Event()
        data class ChanSwitch(val sf: Int, val inSec: Int) : Event()
        data class Speed(val sf: Int, val applied: Boolean, val inSec: Int) : Event()
        data class Diag(val info: DiagInfo) : Event()
        data class Hw(val loraOk: Boolean, val peerOnline: Boolean, val rssi: Int, val sf: Int,
                      val nodes: Int) : Event()
        data class Radio(val info: RadioInfo) : Event()
        data class NameSet(val name: String) : Event()
        data class Psk(val enabled: Boolean) : Event()
        data class Blocked(val nodeId: Long) : Event()
        data class Unblocked(val nodeId: Long) : Event()
        data object BlePong : Event()
        data class GroupJoined(val groupId: Long, val name: String, val from: Long) : Event()
        data class GroupLeft(val groupId: Long, val from: Long) : Event()
        data class TxPower(val idx: Int, val dbm: Int) : Event()
        data class AntennaSet(val type: String) : Event()
        // ── V2.9: автономные устройства ──
        /** Манифест: устройство рассказало, кто оно и что умеет. */
        data class DevHello(val nodeId: Long, val name: String, val devClass: Int,
                            val flags: Int, val intervalS: Int,
                            val fields: List<DevField>, val cmds: List<DevCmd>) : Event()
        /** Показания. Значения приходят со своим индексом и типом. */
        data class DevData(val nodeId: Long, val rssi: Int,
                           val values: List<DevValue>) : Event()
        /** Результат выполнения команды устройством. */
        data class DevAck(val nodeId: Long, val cmd: Int, val status: Int,
                          val desc: String) : Event()
        data class Unknown(val raw: String) : Event()
    }

    private fun JSONObject.hexId(key: String): Long {
        val s = optString(key, "")
        return try {
            if (s.startsWith("0x") || s.startsWith("0X")) s.substring(2).toLong(16)
            else if (s.isNotEmpty()) s.toLong()
            else optLong(key, 0)
        } catch (_: NumberFormatException) { 0L }
    }

    /** Разбор одного JSON-события от устройства. null = не распарсилось. */
    fun parseEvent(json: String): Event? {
        val o = try { JSONObject(json) } catch (_: Exception) { return null }
        return when (o.optString("evt")) {
            "msg" -> Event.Msg(
                seq = o.optInt("seq"), rssi = o.optInt("rssi"),
                groupId = o.hexId("group_id"), nodeId = o.hexId("node_id"),
                hops = o.optInt("hops"), text = o.optString("text"))
            "ack" -> Event.Ack(o.optInt("seq"))
            "sent" -> Event.Sent(o.optInt("seq"))
            "error" -> Event.Error(o.optInt("code", -1), o.optString("desc"), o.optInt("seq", -1))
            "voice" -> Event.Voice(o.hexId("node_id"), o.optInt("seq"), o.optInt("idx"),
                o.optInt("total"), o.optString("data"))
            "voice_sent" -> Event.VoiceSent(o.optInt("seq"))
            "image" -> Event.Image(o.hexId("node_id"), o.optInt("idx"), o.optInt("total"),
                o.optString("data"))
            "image_incoming" -> Event.ImageIncoming(o.hexId("node_id"), o.optString("name"))
            "image_tx" -> Event.ImageTxProgress(o.optInt("pass"), o.optInt("of"))
            "image_sent" -> Event.ImageSent
            "image_fail" -> Event.ImageFail(o.optInt("got"), o.optInt("total"))
            "node_seen" -> Event.NodeSeen(o.hexId("node_id"), o.optString("name"),
                o.optInt("rssi"), o.optInt("hops"), o.optInt("hw"), o.optInt("batt", 255))
            "nodes" -> {
                val arr: JSONArray = o.optJSONArray("list") ?: JSONArray()
                val list = ArrayList<NodeInfo>(arr.length())
                for (i in 0 until arr.length()) {
                    val n = arr.optJSONObject(i) ?: continue
                    list.add(NodeInfo(
                        id = n.hexId("id"), name = n.optString("name"),
                        rssi = n.optInt("rssi"), hops = n.optInt("hops"),
                        batt = n.optInt("batt", 255), hw = n.optInt("hw"),
                        status = n.optInt("status"),
                        lastSeenMs = System.currentTimeMillis()))
                }
                Event.Nodes(list)
            }
            "peer" -> Event.Peer(o.optString("name"), o.optBoolean("online"), o.optInt("rssi"))
            "sos_in" -> Event.SosIn(o.hexId("node_id"), o.optInt("seq"), o.optInt("rssi"),
                o.optString("text"))
            "panic" -> Event.Panic(o.hexId("node_id"), o.optInt("rssi"))
            "sos_sent" -> Event.SosSent(o.optInt("count"))
            "ptt_incoming" -> Event.PttIncoming(o.hexId("node_id"), o.optString("name"),
                o.optInt("mode", 1))
            "ptt_audio" -> Event.PttAudio(o.optString("d"))
            "ptt_state" -> Event.PttStateEvt(o.optString("state") == "active")
            "ptt_answered" -> Event.PttAnswered
            "ptt_stats" -> Event.PttStats(o.optInt("rx"), o.optInt("tx"), o.optInt("txq"),
                o.optInt("qd"), o.optInt("nd"), o.optInt("na"), o.optInt("age"))
            "chan_switch" -> Event.ChanSwitch(o.optInt("sf"), o.optInt("in"))
            "speed" -> Event.Speed(o.optInt("sf"), o.optBoolean("applied"), o.optInt("in", 0))
            "diag" -> Event.Diag(DiagInfo(
                loraOk = o.optBoolean("lora_ok"), peerOk = o.optBoolean("peer_ok"),
                rssi = o.optInt("rssi"), snr = o.optDouble("snr", 0.0), sf = o.optInt("sf"),
                txCount = o.optInt("tx_count"), rxCount = o.optInt("rx_count"),
                nodeId = o.optString("node_id"), nodesOnline = o.optInt("nodes_online"),
                hw = o.optInt("hw"), batt = o.optInt("batt", 255),
                board = o.optString("board"), enc = o.optBoolean("enc")))
            "hw" -> Event.Hw(o.optBoolean("lora"), o.optBoolean("peer_online"),
                o.optInt("rssi"), o.optInt("sf"), o.optInt("nodes"))
            "radio" -> Event.Radio(RadioInfo(
                txIdx = o.optInt("tx_idx"), txDbm = o.optInt("tx_dbm", 30),
                antenna = o.optString("antenna", "STOCK"),
                chip = o.optString("chip"), band = o.optString("band"),
                freqHz = o.optLong("freq"), maxDbm = o.optInt("max_dbm"),
                // Старая прошивка этих полей не шлёт: fsk останется false,
                // и звонки просто не предложатся — это безопасный дефолт.
                fsk = o.optBoolean("fsk"), profile = o.optString("profile")))
            "name_set" -> Event.NameSet(o.optString("name"))
            "psk" -> Event.Psk(o.optBoolean("enabled"))
            "blocked" -> Event.Blocked(o.hexId("node_id"))
            "unblocked" -> Event.Unblocked(o.hexId("node_id"))
            "ble_pong" -> Event.BlePong
            "group_joined" -> Event.GroupJoined(o.hexId("group_id"), o.optString("name"),
                o.hexId("from"))
            "group_left" -> Event.GroupLeft(o.hexId("group_id"), o.hexId("from"))
            "dev_hello" -> {
                val fa = o.optJSONArray("fields") ?: JSONArray()
                val fields = ArrayList<DevField>(fa.length())
                for (i in 0 until fa.length()) {
                    val f = fa.optJSONObject(i) ?: continue
                    fields.add(DevField(f.optInt("i"), f.optString("name"),
                        f.optInt("type"), f.optString("unit"), f.optInt("scale")))
                }
                val ca = o.optJSONArray("cmds") ?: JSONArray()
                val cmds = ArrayList<DevCmd>(ca.length())
                for (i in 0 until ca.length()) {
                    val c = ca.optJSONObject(i) ?: continue
                    cmds.add(DevCmd(c.optInt("id"), c.optString("name"), c.optInt("action")))
                }
                Event.DevHello(o.hexId("node_id"), o.optString("name"),
                    o.optInt("class"), o.optInt("flags"), o.optInt("interval", 60),
                    fields, cmds)
            }
            "dev_data" -> {
                val va = o.optJSONArray("v") ?: JSONArray()
                val vals = ArrayList<DevValue>(va.length())
                for (i in 0 until va.length()) {
                    val v = va.optJSONObject(i) ?: continue
                    vals.add(DevValue(v.optInt("i"), v.optInt("t"), v.optLong("v")))
                }
                Event.DevData(o.hexId("node_id"), o.optInt("rssi"), vals)
            }
            "dev_ack" -> Event.DevAck(o.hexId("node_id"), o.optInt("cmd"),
                o.optInt("status"), o.optString("desc"))
            "tx_power" -> Event.TxPower(o.optInt("idx"), o.optInt("dbm"))
            "antenna_set" -> Event.AntennaSet(o.optString("type"))
            else -> Event.Unknown(json)
        }
    }
}

/**
 * Склейка BLE-нотификаций в целые JSON.
 * Прошивка режет длинные события на чанки по 180 байт — собираем по балансу
 * фигурных скобок с учётом строк и экранирования.
 */
class NotifyAssembler(private val onJson: (String) -> Unit) {
    // Работаем с БАЙТАМИ: чанк по 180 байт может разрезать UTF-8 символ,
    // поэтому в строку конвертируем только целый собранный JSON.
    // Структурные символы ({}"\) — ASCII, байты многобайтовых UTF-8
    // последовательностей всегда ≥ 0x80 и с ними не совпадают.
    private val buf = java.io.ByteArrayOutputStream()
    private var depth = 0
    private var inString = false
    private var escaped = false
    private var lastFeedMs = 0L

    /** Сколько раз сборщик сбрасывался из-за рассинхрона (дебаг-панель). */
    @Volatile var resyncCount = 0
        private set

    fun feed(bytes: ByteArray) {
        // V1.7.3: если прошивка оборвала JSON на середине (потерянный чанк),
        // buf навсегда оставался с незакрытой скобкой и глотал ВСЁ дальше
        // (лечилось только переполнением 16КБ ~ секунды глухоты в звонке).
        // Устройство шлёт чанки одного сообщения с зазором ≤ десятков мс —
        // недособранный буфер старше секунды это точно обрывок, выкидываем.
        val now = System.currentTimeMillis()
        if (buf.size() > 0 && now - lastFeedMs > 1000) {
            resyncCount++; reset()
        }
        lastFeedMs = now
        for (b in bytes) {
            val c = b.toInt() and 0xFF
            if (depth == 0 && buf.size() == 0 && c != '{'.code) continue  // мусор между JSON
            buf.write(c)
            when {
                escaped -> escaped = false
                inString && c == '\\'.code -> escaped = true
                c == '"'.code -> inString = !inString
                !inString && c == '{'.code -> depth++
                !inString && c == '}'.code -> {
                    depth--
                    if (depth == 0) {
                        onJson(buf.toString(Charsets.UTF_8.name()))
                        buf.reset()
                    }
                }
            }
        }
        // Одно сообщение устройства ≤ 2048Б; больше = рассинхрон, ресинк
        if (buf.size() > 4096) { resyncCount++; reset() }
    }

    fun reset() {
        buf.reset(); depth = 0; inString = false; escaped = false
    }
}
