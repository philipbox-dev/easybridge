package easylink.yvak.inc.proto

/** Состояние соединения с устройством EasyBridge. */
enum class ConnState { DISCONNECTED, CONNECTING, CONNECTED, RECONNECTING }

/** Узел LoRa-сети. */
data class NodeInfo(
    val id: Long,                 // uint32 node_id
    val name: String = "",
    val rssi: Int = 0,
    val hops: Int = 0,
    val batt: Int = 255,          // 255 = нет батареи/неизвестно
    val hw: Int = 0,
    val status: Int = 0,          // 0 online, 1 stale, 2 offline
    val lastSeenMs: Long = 0L,
    val blocked: Boolean = false,
) {
    val idHex: String get() = "0x%08x".format(id)
    val displayName: String get() = name.ifEmpty { idHex }

    /** 0 онлайн (<5 мин), 1 давно не слышно (<30 мин), 2 оффлайн. */
    val presence: Int get() {
        if (lastSeenMs <= 0) return 2
        val age = System.currentTimeMillis() - lastSeenMs
        return when {
            age < 5 * 60_000 -> 0
            age < 30 * 60_000 -> 1
            else -> 2
        }
    }
    val isOnline: Boolean get() = presence == 0
}

/** Известные hw_id прошивки (main/boards/). */
fun hwName(hw: Int): String = when (hw) {
    1 -> "ESP32-S3 + E220"
    2 -> "ESP32-C6 + E220"
    3 -> "LilyGO T3 (SX1276)"
    4 -> "ESP32-S3 + E22 (SX1268)"
    else -> "HW $hw"
}

enum class MsgDir { IN, OUT }
enum class MsgKind { TEXT, VOICE, SOS, SYSTEM, IMAGE }
enum class MsgStatus { SENDING, SENT, DELIVERED, FAILED }

const val CHAT_GENERAL = "general"
fun groupChatKey(groupId: Long) = "g%08x".format(groupId)
fun dmChatKey(peerId: Long) = "dm%08x".format(peerId)
fun dmPeerOf(chatKey: String): Long =
    if (chatKey.startsWith("dm")) chatKey.removePrefix("dm").toLongOrNull(16) ?: 0L else 0L

/** group_id личного чата: симметричен для обеих сторон, старший бит = метка ЛС. */
fun dmGroupId(a: Long, b: Long): Long = ((a xor b) or 0x80000000L) and 0xFFFFFFFFL
fun isDmGroupId(groupId: Long): Boolean = (groupId and 0x80000000L) != 0L

data class ChatMessage(
    val id: Long = 0,
    val chatKey: String,
    val dir: MsgDir,
    val kind: MsgKind,
    val text: String = "",
    val voicePath: String = "",
    val seq: Int = -1,
    val status: MsgStatus = MsgStatus.SENDING,
    val nodeId: Long = 0,
    val nodeName: String = "",
    val rssi: Int = 0,
    val hops: Int = 0,
    val ts: Long = System.currentTimeMillis(),
)

data class GroupInfo(val id: Long, val name: String) {
    val idHex: String get() = "0x%08x".format(id)
    val chatKey: String get() = groupChatKey(id)
}

data class ChatInfo(
    val key: String,
    val title: String,
    val isGroup: Boolean,
    val groupId: Long = 0,
    val peerId: Long = 0,          // ≠0 → личный чат
    val lastMessage: ChatMessage? = null,
    val unread: Int = 0,
) {
    val isDm: Boolean get() = peerId != 0L
}

data class PeerLocation(
    val nodeId: Long,
    val name: String,
    val lat: Double,
    val lon: Double,
    val ts: Long,
)

data class DiagInfo(
    val loraOk: Boolean = false,
    val peerOk: Boolean = false,
    val rssi: Int = 0,
    val snr: Double = 0.0,
    val sf: Int = 0,
    val txCount: Int = 0,
    val rxCount: Int = 0,
    val nodeId: String = "",
    val nodesOnline: Int = 0,
    val hw: Int = 0,
    val batt: Int = 255,
    val board: String = "",
    val enc: Boolean = false,
)

data class RadioInfo(
    val txIdx: Int = 0,
    val txDbm: Int = 30,
    val antenna: String = "STOCK",
    // ── V2.9: железо стало настраиваемым, и приложению нужно знать,
    // что именно припаяно. От fsk напрямую зависит, показывать ли
    // кнопку звонка: у E220 доступа к GFSK нет.
    val chip: String = "",
    val band: String = "",
    val freqHz: Long = 0,
    val maxDbm: Int = 0,
    val fsk: Boolean = false,
    val profile: String = "",
)

enum class LogDir { TX, RX, INFO, ERR }
data class LogEntry(val ts: Long, val dir: LogDir, val text: String)

/** Состояние PTT-звонка. */
sealed class PttState {
    data object Idle : PttState()
    data class Incoming(val nodeId: Long, val name: String, val mode: Int) : PttState()
    /** Звоним и ждём, пока абонент возьмёт трубку (только исходящий). */
    data class Dialing(
        val peerName: String,
        val mode: Int,
        val startedMs: Long = System.currentTimeMillis(),
    ) : PttState()
    /** Сессия активна (рация в FSK). */
    data class Active(
        val peerName: String,
        val mode: Int,
        val outgoing: Boolean,
        val startedMs: Long = System.currentTimeMillis(),
    ) : PttState()
}

data class SosAlert(
    val nodeId: Long,
    val name: String,
    val text: String,
    val rssi: Int,
    val isPanic: Boolean = false,
    val ts: Long = System.currentTimeMillis(),
)

// ── V2.9: автономные устройства ──────────────────────────────
// Датчик или реле без телефона. Схему полей мы узнаём из его же
// манифеста, поэтому заранее знать, что это за железка, не нужно.

/** Поле показаний: индекс в манифесте, подпись, тип, единица, порядок. */
data class DevField(
    val index: Int,
    val name: String,
    val type: Int,
    val unit: String,
    /** Значение показывается как v × 10^scale. */
    val scale: Int,
)

/** Команда, которую устройство принимает. */
data class DevCmd(
    val id: Int,
    val name: String,
    val action: Int,
)

/** Одно значение из посылки. Тип дублируется в каждом значении:
 *  тот, кто пропустил манифест, всё равно покажет числа. */
data class DevValue(
    val index: Int,
    val type: Int,
    val raw: Long,
)
