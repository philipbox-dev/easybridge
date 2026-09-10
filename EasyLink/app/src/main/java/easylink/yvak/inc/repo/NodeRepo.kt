package easylink.yvak.inc.repo

import easylink.yvak.inc.ble.BleClient
import easylink.yvak.inc.proto.NodeInfo
import easylink.yvak.inc.proto.Proto
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

/** Узлы LoRa-сети + чёрный список. */
class NodeRepo(private val ble: BleClient) {

    private val _nodes = MutableStateFlow<Map<Long, NodeInfo>>(emptyMap())
    val nodes: StateFlow<Map<Long, NodeInfo>> get() = _nodes

    private val blockedIds = MutableStateFlow<Set<Long>>(emptySet())
    val blocked: StateFlow<Set<Long>> get() = blockedIds

    /** История RSSI по узлам (sigtrace): последние ~180 замеров (ts, rssi). */
    private val _rssiHist = MutableStateFlow<Map<Long, List<Pair<Long, Int>>>>(emptyMap())
    val rssiHist: StateFlow<Map<Long, List<Pair<Long, Int>>>> get() = _rssiHist

    private fun record(id: Long, rssi: Int) {
        if (rssi == 0) return
        val cur = _rssiHist.value
        val list = (cur[id] ?: emptyList()) + (System.currentTimeMillis() to rssi)
        _rssiHist.value = cur + (id to list.takeLast(180))
    }

    /** Узел подал признак жизни (пришло сообщение/голос) — обновить презенс. */
    fun touch(id: Long, rssi: Int, hops: Int) {
        if (id == 0L) return
        val cur = _nodes.value.toMutableMap()
        val old = cur[id]
        cur[id] = (old ?: NodeInfo(id)).copy(
            rssi = if (rssi != 0) rssi else old?.rssi ?: 0,
            hops = hops,
            lastSeenMs = System.currentTimeMillis(),
        )
        _nodes.value = cur
        record(id, rssi)
    }

    fun onNodeSeen(id: Long, name: String, rssi: Int, hops: Int, hw: Int, batt: Int) {
        if (id == 0L) return
        val cur = _nodes.value.toMutableMap()
        val old = cur[id]
        cur[id] = NodeInfo(
            id = id,
            name = name.ifEmpty { old?.name ?: "" },
            rssi = rssi, hops = hops,
            hw = if (hw != 0) hw else old?.hw ?: 0,
            batt = if (batt != 255) batt else old?.batt ?: 255,
            status = 0,
            lastSeenMs = System.currentTimeMillis(),
            blocked = blockedIds.value.contains(id),
        )
        _nodes.value = cur
        record(id, rssi)
    }

    fun onNodesDump(list: List<NodeInfo>) {
        val cur = _nodes.value.toMutableMap()
        for (n in list) {
            val old = cur[n.id]
            cur[n.id] = n.copy(
                name = n.name.ifEmpty { old?.name ?: "" },
                lastSeenMs = old?.lastSeenMs ?: System.currentTimeMillis(),
                blocked = blockedIds.value.contains(n.id),
            )
            record(n.id, n.rssi)
        }
        _nodes.value = cur
    }

    fun refresh() {
        ble.sendJson(Proto.nodes())
        ble.sendJson(Proto.discover())
    }

    fun block(id: Long) {
        ble.sendJson(Proto.blockNode(id))
    }

    fun unblock(id: Long) {
        ble.sendJson(Proto.unblockNode(id))
    }

    fun onBlocked(id: Long, isBlocked: Boolean) {
        blockedIds.value =
            if (isBlocked) blockedIds.value + id else blockedIds.value - id
        _nodes.value = _nodes.value.mapValues { (k, v) ->
            if (k == id) v.copy(blocked = isBlocked) else v
        }
    }

    fun name(id: Long): String = _nodes.value[id]?.displayName ?: "0x%08x".format(id)

    fun clear() { _nodes.value = emptyMap() }
}
