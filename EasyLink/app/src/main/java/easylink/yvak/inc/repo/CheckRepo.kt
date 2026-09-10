package easylink.yvak.inc.repo

import easylink.yvak.inc.Prefs
import easylink.yvak.inc.ble.BleClient
import easylink.yvak.inc.proto.Proto
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlin.random.Random

/**
 * #Ф2 Групповой check-in «Все живы?».
 * Поверх обычного send текстовыми маркерами (в чат не попадают):
 *   запрос: "[CHK:xxxxxxxx]"   ответ: "[CHKOK:xxxxxxxx]"
 * Работает с любой прошивкой — это чистый app-уровень.
 */
class CheckRepo(
    private val ble: BleClient,
    private val prefs: Prefs,
    private val scope: CoroutineScope,
    private val nodes: NodeRepo,
) {
    data class Session(
        val id: String,
        val startedMs: Long,
        /** nodeId → имя всех, кого ждём (онлайн на момент запроса). */
        val expected: Map<Long, String>,
        /** nodeId ответивших. */
        val acks: Set<Long>,
    )

    data class Incoming(val id: String, val fromId: Long, val fromName: String)

    private val _session = MutableStateFlow<Session?>(null)
    val session: StateFlow<Session?> get() = _session

    private val _incoming = MutableStateFlow<Incoming?>(null)
    val incoming: StateFlow<Incoming?> get() = _incoming

    /** Уведомить (если приложение в фоне) — ставит Bridge. */
    var onIncomingNotify: ((String) -> Unit)? = null

    fun start() {
        val id = "%08x".format(Random.nextInt())
        val expected = nodes.nodes.value.values
            .filter { it.presence <= 1 }
            .associate { it.id to it.displayName }
        _session.value = Session(id, System.currentTimeMillis(), expected, emptySet())
        ble.sendJson(Proto.send(prefs.nextSeq(), "[CHK:$id]"))
        // повтор запроса через 20с для тех, кто пропустил
        scope.launch {
            delay(20_000)
            if (_session.value?.id == id) {
                ble.sendJson(Proto.send(prefs.nextSeq(), "[CHK:$id]"))
            }
            delay(10 * 60_000)
            if (_session.value?.id == id) _session.value = null   // авто-закрытие
        }
    }

    fun close() { _session.value = null }

    /** true = сообщение было маркером check-in (в чат не отдавать). */
    fun tryIntercept(nodeId: Long, nodeName: String, text: String): Boolean {
        val t = text.trim()
        if (t.startsWith("[CHK:") && t.endsWith("]")) {
            val id = t.removePrefix("[CHK:").removeSuffix("]")
            if (id.length in 4..16) {
                _incoming.value = Incoming(id, nodeId, nodeName)
                onIncomingNotify?.invoke(nodeName)
            }
            return true
        }
        if (t.startsWith("[CHKOK:") && t.endsWith("]")) {
            val id = t.removePrefix("[CHKOK:").removeSuffix("]")
            val s = _session.value
            if (s != null && s.id == id) {
                _session.value = s.copy(acks = s.acks + nodeId,
                    expected = if (s.expected.containsKey(nodeId)) s.expected
                    else s.expected + (nodeId to nodeName))
            }
            return true
        }
        return false
    }

    /** «Я в порядке» на входящий запрос. */
    fun ackIncoming() {
        val inc = _incoming.value ?: return
        ble.sendJson(Proto.send(prefs.nextSeq(), "[CHKOK:${inc.id}]"))
        _incoming.value = null
    }

    fun dismissIncoming() { _incoming.value = null }
}
