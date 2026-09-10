package easylink.yvak.inc.repo

import android.util.Base64
import easylink.yvak.inc.Prefs
import easylink.yvak.inc.audio.VoiceRecorder
import easylink.yvak.inc.ble.BleClient
import easylink.yvak.inc.db.Db
import easylink.yvak.inc.proto.CHAT_GENERAL
import easylink.yvak.inc.proto.ChatInfo
import easylink.yvak.inc.proto.ChatMessage
import easylink.yvak.inc.proto.GroupInfo
import easylink.yvak.inc.proto.MsgDir
import easylink.yvak.inc.proto.MsgKind
import easylink.yvak.inc.proto.MsgStatus
import easylink.yvak.inc.proto.Proto
import easylink.yvak.inc.proto.dmChatKey
import easylink.yvak.inc.proto.dmGroupId
import easylink.yvak.inc.proto.dmPeerOf
import easylink.yvak.inc.proto.groupChatKey
import easylink.yvak.inc.proto.isDmGroupId
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlin.random.Random

/**
 * Чаты: общий канал + личные (ЛС) + группы.
 *
 * ЛС ездят поверх group_id: id = (my ^ peer) | 0x80000000 — симметричен,
 * старший бит отличает ЛС от обычных групп. Эфир broadcast, чужие ЛС
 * отбрасываются проверкой ожидаемого id (защита от показа — не шифрование,
 * приватность канала даёт PSK).
 */
class ChatRepo(
    private val db: Db,
    private val prefs: Prefs,
    private val ble: BleClient,
    private val scope: CoroutineScope,
    private val recorder: VoiceRecorder,
    private val locRepo: LocationRepo,
    private val crypto: CryptoRepo,
) {
    /** Открытый сейчас чат (его сообщения не считаем непрочитанными). */
    @Volatile var currentChatKey: String? = null

    private val _chats = MutableStateFlow<List<ChatInfo>>(emptyList())
    val chats: StateFlow<List<ChatInfo>> get() = _chats

    private val unread = HashMap<String, Int>()
    private val messageFlows = HashMap<String, MutableStateFlow<List<ChatMessage>>>()
    private val dbMutex = Mutex()

    /** Свежепринятые сообщения — для уведомлений. */
    val incoming = kotlinx.coroutines.flow.MutableSharedFlow<ChatMessage>(extraBufferCapacity = 32)

    /** Сервисные текст-маркеры ([CHK] и т.п.): true = перехвачено, в чат не отдавать. */
    var onServiceText: ((Long, String, String) -> Boolean)? = null

    // ── #У7 Закреп: chatKey → текст (в prefs) ─────────────────
    private val _pinned = MutableStateFlow<Map<String, String>>(loadPinned())
    val pinned: StateFlow<Map<String, String>> get() = _pinned

    private fun loadPinned(): Map<String, String> = try {
        val o = org.json.JSONObject(prefs.pinnedJson.value)
        buildMap { o.keys().forEach { put(it, o.getString(it)) } }
    } catch (_: Exception) { emptyMap() }

    fun pin(chatKey: String, text: String) {
        val m = _pinned.value.toMutableMap()
        if (text.isBlank()) m.remove(chatKey) else m[chatKey] = text.take(300)
        val o = org.json.JSONObject(); m.forEach { (k, v) -> o.put(k, v) }
        prefs.pinnedJson.value = o.toString()
        _pinned.value = m
    }

    init {
        scope.launch(Dispatchers.IO) { rebuildChats() }
        scope.launch(Dispatchers.IO) { retryLoop() }
    }

    /** #4 Outbox: досылаем сообщения, которые не ушли в эфир (статус SENDING). */
    private suspend fun retryLoop() {
        while (true) {
            val everySec = prefs.retryEverySec.value.coerceIn(15, 300)
            delay(everySec * 1000L)
            if (!prefs.retryEnabled.value) continue
            if (ble.connState.value != easylink.yvak.inc.proto.ConnState.CONNECTED) continue
            val windowMs = prefs.retryWindowMin.value.coerceIn(1, 720) * 60_000L
            val since = System.currentTimeMillis() - windowMs
            val pending = db.pendingUnsent(since).filter {
                System.currentTimeMillis() - it.ts > everySec * 1000L  // не трогаем «только что»
            }
            for (m in pending) {
                val gid = _chats.value.firstOrNull { it.key == m.chatKey }?.groupId ?: 0L
                val peer = dmPeerOf(m.chatKey)
                val wireGid = if (peer != 0L) {
                    val me = myId(); if (me != 0L) dmGroupId(me, peer) else continue
                } else gid
                val wire = crypto.encryptForChat(m.chatKey, m.text)
                ble.sendJson(Proto.send(m.seq, wire, wireGid))
                delay(400)
            }
        }
    }

    private fun myId(): Long = prefs.myNodeId.value.toLongOrNull() ?: 0L

    fun messagesFlow(chatKey: String): StateFlow<List<ChatMessage>> {
        synchronized(messageFlows) {
            return messageFlows.getOrPut(chatKey) {
                val f = MutableStateFlow<List<ChatMessage>>(emptyList())
                scope.launch(Dispatchers.IO) { f.value = db.messages(chatKey) }
                f
            }
        }
    }

    private suspend fun reload(chatKey: String) {
        val flow = synchronized(messageFlows) { messageFlows[chatKey] }
        if (flow != null) flow.value = db.messages(chatKey)
        rebuildChats()
    }

    private suspend fun rebuildChats() = dbMutex.withLock {
        val groups = db.groups()
        val dms = db.dms()
        val list = ArrayList<ChatInfo>(groups.size + dms.size + 1)
        list.add(ChatInfo(CHAT_GENERAL, "", isGroup = false,
            lastMessage = db.lastMessage(CHAT_GENERAL),
            unread = unread[CHAT_GENERAL] ?: 0))
        for ((peerId, name) in dms) {
            val key = dmChatKey(peerId)
            list.add(ChatInfo(key, name, isGroup = false, peerId = peerId,
                lastMessage = db.lastMessage(key),
                unread = unread[key] ?: 0))
        }
        for (g in groups) {
            list.add(ChatInfo(g.chatKey, g.name, isGroup = true, groupId = g.id,
                lastMessage = db.lastMessage(g.chatKey),
                unread = unread[g.chatKey] ?: 0))
        }
        // свежие сверху (общий канал всегда первым)
        _chats.value = listOf(list.first()) + list.drop(1)
            .sortedByDescending { it.lastMessage?.ts ?: 0L }
    }

    fun openChat(chatKey: String) {
        currentChatKey = chatKey
        unread.remove(chatKey)
        scope.launch(Dispatchers.IO) { rebuildChats() }
    }

    fun closeChat() { currentChatKey = null }

    /** group_id для отправки в этот чат (0 = общий канал). */
    private fun wireGroupId(chatKey: String): Long {
        val peer = dmPeerOf(chatKey)
        if (peer != 0L) {
            val me = myId()
            return if (me != 0L) dmGroupId(me, peer) else -1L  // -1 = нельзя отправить
        }
        return _chats.value.firstOrNull { it.key == chatKey }?.groupId ?: 0L
    }

    // ── Отправка ──────────────────────────────────────────────
    fun sendText(chatKey: String, text: String) {
        if (text.isBlank()) return
        val groupId = wireGroupId(chatKey)
        val seq = prefs.nextSeq()
        val msg = ChatMessage(
            chatKey = chatKey, dir = MsgDir.OUT, kind = MsgKind.TEXT,
            text = text, seq = seq,
            status = if (groupId < 0) MsgStatus.FAILED else MsgStatus.SENDING)
        scope.launch(Dispatchers.IO) {
            db.insertMessage(msg)   // храним/показываем открытый текст у себя
            reload(chatKey)
            if (groupId >= 0) {
                val wire = crypto.encryptForChat(chatKey, text)   // шифруем в эфир, если есть ключ
                ble.sendJson(Proto.send(seq, wire, groupId))
            }
        }
    }

    /** Голосовое: байты AMR → base64-чанки voice_tx. */
    fun sendVoice(chatKey: String, filePath: String, bytes: ByteArray) {
        val groupId = wireGroupId(chatKey)  // голосовые в эфире без group_id,
        if (groupId < 0) return             // но ЛС без myId не отправляем вовсе
        val seq = prefs.nextSeq()
        val msg = ChatMessage(
            chatKey = chatKey, dir = MsgDir.OUT, kind = MsgKind.VOICE,
            voicePath = filePath, seq = seq, status = MsgStatus.SENDING)
        scope.launch(Dispatchers.IO) {
            db.insertMessage(msg)
            reload(chatKey)
            val overhead = 80
            val b64Budget = (ble.maxWriteBytes - overhead).coerceAtLeast(60)
            val rawChunk = (b64Budget / 4) * 3
            val total = (bytes.size + rawChunk - 1) / rawChunk
            for (i in 0 until total) {
                val from = i * rawChunk
                val to = minOf(from + rawChunk, bytes.size)
                val b64 = Base64.encodeToString(bytes, from, to - from, Base64.NO_WRAP)
                ble.sendJson(Proto.voiceTx(seq, i, total, b64))
                delay(30)
            }
        }
    }

    // ── События от устройства ─────────────────────────────────
    fun onSent(seq: Int) = updateStatus(seq, MsgStatus.SENT, onlyIfBelow = MsgStatus.DELIVERED)
    fun onAck(seq: Int) = updateStatus(seq, MsgStatus.DELIVERED, onlyIfBelow = null)
    fun onVoiceSent(seq: Int) = updateStatus(seq, MsgStatus.SENT, onlyIfBelow = MsgStatus.DELIVERED)
    fun onSendError(seq: Int) {
        if (seq >= 0) updateStatus(seq, MsgStatus.FAILED, onlyIfBelow = null)
    }

    private fun updateStatus(seq: Int, status: MsgStatus, onlyIfBelow: MsgStatus?) {
        scope.launch(Dispatchers.IO) {
            if (db.updateStatusBySeq(seq, status, onlyIfBelow) > 0) {
                val keys = synchronized(messageFlows) { messageFlows.keys.toList() }
                for (k in keys) reload(k)
            }
        }
    }

    fun onIncomingText(seq: Int, rssi: Int, groupId: Long, nodeId: Long, hops: Int,
                       textRaw: String, nodeName: String) {
        val chatKey: String
        if (isDmGroupId(groupId)) {
            // ЛС: наше, только если id совпадает с ожидаемым для (я, отправитель)
            val me = myId()
            if (me != 0L && groupId != dmGroupId(me, nodeId)) return  // чужое ЛС
            chatKey = dmChatKey(nodeId)
        } else if (groupId != 0L) {
            chatKey = groupChatKey(groupId)
        } else {
            chatKey = CHAT_GENERAL
        }

        // Расшифровка (если у чата задан ключ). Локация — только из открытого текста.
        val text: String
        if (crypto.isEncrypted(textRaw)) {
            text = crypto.decryptForChat(chatKey, textRaw) ?: "🔒 (зашифровано)"
        } else {
            // [LOC:lat,lon] — не чат, а карта; [CHK]/[CHKOK] — check-in (только открытые)
            if (locRepo.tryParseLoc(nodeId, nodeName, textRaw)) return
            if (onServiceText?.invoke(nodeId, nodeName, textRaw) == true) return
            text = textRaw
        }

        val msg = ChatMessage(
            chatKey = chatKey, dir = MsgDir.IN,
            kind = if (text.startsWith("⚠️")) MsgKind.SOS else MsgKind.TEXT,
            text = text, seq = seq, status = MsgStatus.DELIVERED,
            nodeId = nodeId, nodeName = nodeName, rssi = rssi, hops = hops)
        scope.launch(Dispatchers.IO) {
            if (isDmGroupId(groupId)) {
                db.upsertDm(nodeId, nodeName)
            } else if (groupId != 0L) {
                if (db.groups().none { it.id == groupId }) {
                    db.upsertGroup(GroupInfo(groupId, "0x%08x".format(groupId)))
                }
                db.addGroupMember(groupId, nodeId)
            }
            db.insertMessage(msg)
            if (currentChatKey != chatKey) {
                unread[chatKey] = (unread[chatKey] ?: 0) + 1
            }
            reload(chatKey)
            incoming.tryEmit(msg)
        }
    }

    // ── Приём голосовых чанков ────────────────────────────────
    private class VoiceRx(val total: Int) {
        val chunks = arrayOfNulls<ByteArray>(total)
        var got = 0
        val startedMs = System.currentTimeMillis()
    }
    private val voiceRx = HashMap<String, VoiceRx>()

    fun onVoiceChunk(nodeId: Long, seq: Int, idx: Int, total: Int, dataB64: String,
                     nodeName: String) {
        if (total <= 0 || idx < 0 || idx >= total) return
        val key = "$nodeId:$seq"
        synchronized(voiceRx) {
            voiceRx.entries.removeAll {
                System.currentTimeMillis() - it.value.startedMs > 60_000
            }
            val rx = voiceRx.getOrPut(key) { VoiceRx(total) }
            if (rx.total != total) { voiceRx.remove(key); return }
            if (rx.chunks[idx] == null) {
                rx.chunks[idx] = try {
                    Base64.decode(dataB64, Base64.NO_WRAP)
                } catch (_: Exception) { return }
                rx.got++
            }
            if (rx.got < rx.total) return
            voiceRx.remove(key)
            val out = java.io.ByteArrayOutputStream()
            for (c in rx.chunks) out.write(c!!)
            val bytes = out.toByteArray()
            scope.launch(Dispatchers.IO) {
                val f = recorder.saveIncoming(nodeId, seq, bytes)
                val msg = ChatMessage(
                    chatKey = CHAT_GENERAL, dir = MsgDir.IN, kind = MsgKind.VOICE,
                    voicePath = f.absolutePath, seq = seq, status = MsgStatus.DELIVERED,
                    nodeId = nodeId, nodeName = nodeName)
                db.insertMessage(msg)
                if (currentChatKey != CHAT_GENERAL) {
                    unread[CHAT_GENERAL] = (unread[CHAT_GENERAL] ?: 0) + 1
                }
                reload(CHAT_GENERAL)
                incoming.tryEmit(msg)
            }
        }
    }

    fun insertSos(nodeId: Long, nodeName: String, text: String, rssi: Int) {
        val msg = ChatMessage(
            chatKey = CHAT_GENERAL, dir = MsgDir.IN, kind = MsgKind.SOS,
            text = text, status = MsgStatus.DELIVERED,
            nodeId = nodeId, nodeName = nodeName, rssi = rssi)
        scope.launch(Dispatchers.IO) {
            db.insertMessage(msg)
            reload(CHAT_GENERAL)
        }
    }

    /** Картинка (FSK): исходящая (dir=OUT) или принятая (dir=IN). */
    fun insertImage(chatKey: String, path: String, dir: MsgDir,
                    nodeId: Long = 0, nodeName: String = "") {
        val msg = ChatMessage(
            chatKey = chatKey, dir = dir, kind = MsgKind.IMAGE,
            voicePath = path,   // переиспользуем колонку media-пути
            status = MsgStatus.DELIVERED, nodeId = nodeId, nodeName = nodeName)
        scope.launch(Dispatchers.IO) {
            db.insertMessage(msg)
            if (dir == MsgDir.IN && currentChatKey != chatKey) {
                unread[chatKey] = (unread[chatKey] ?: 0) + 1
            }
            reload(chatKey)
            if (dir == MsgDir.IN) incoming.tryEmit(msg)
        }
    }

    fun insertSystem(chatKey: String, text: String) {
        scope.launch(Dispatchers.IO) {
            db.insertMessage(ChatMessage(
                chatKey = chatKey, dir = MsgDir.IN, kind = MsgKind.SYSTEM,
                text = text, status = MsgStatus.DELIVERED))
            reload(chatKey)
        }
    }

    /**
     * Сообщение, пришедшее из веба (V2.9).
     *
     * Помечаем источник прямо в тексте: у веба и эфира разные ожидания
     * по времени и длине, и человеку полезно видеть, откуда пришло.
     */
    fun insertWebMessage(chatKey: String, from: String, text: String, kind: String) {
        val prefix = if (kind == "sos") "🆘 " else "🌐 "
        scope.launch(Dispatchers.IO) {
            val msg = ChatMessage(
                chatKey = chatKey, dir = MsgDir.IN,
                kind = if (kind == "sos") MsgKind.SYSTEM else MsgKind.TEXT,
                text = prefix + (if (from.isBlank()) text else "$from: $text"),
                status = MsgStatus.DELIVERED, nodeName = from)
            db.insertMessage(msg)
            if (currentChatKey != chatKey) {
                unread[chatKey] = (unread[chatKey] ?: 0) + 1
            }
            reload(chatKey)
            incoming.tryEmit(msg)
        }
    }

    /** Запись о звонке: в ЛС собеседника, либо в общий канал (звонок всем). */
    fun insertCallLog(peerId: Long, peerName: String, text: String) {
        if (peerId != 0L) {
            scope.launch(Dispatchers.IO) {
                db.upsertDm(peerId, peerName)
                db.insertMessage(ChatMessage(
                    chatKey = dmChatKey(peerId), dir = MsgDir.IN, kind = MsgKind.SYSTEM,
                    text = text, status = MsgStatus.DELIVERED, nodeId = peerId,
                    nodeName = peerName))
                reload(dmChatKey(peerId))
            }
        } else {
            insertSystem(CHAT_GENERAL, text)
        }
    }

    // ── ЛС ────────────────────────────────────────────────────
    fun createDm(peerId: Long, name: String): String {
        scope.launch(Dispatchers.IO) {
            db.upsertDm(peerId, name)
            rebuildChats()
        }
        return dmChatKey(peerId)
    }

    fun deleteDm(peerId: Long) {
        scope.launch(Dispatchers.IO) {
            db.deleteDm(peerId)
            db.clearChat(dmChatKey(peerId))
            rebuildChats()
        }
    }

    // ── Группы ────────────────────────────────────────────────
    fun createGroup(name: String): GroupInfo {
        var id = 0L
        // старший бит зарезервирован под ЛС
        while (id == 0L) id = Random.nextLong(1, 0x7FFFFFFFL)
        val g = GroupInfo(id, name)
        scope.launch(Dispatchers.IO) {
            db.upsertGroup(g)
            rebuildChats()
        }
        return g
    }

    fun inviteToGroup(g: GroupInfo, nodeId: Long) {
        ble.sendJson(Proto.groupJoin(nodeId, g.id, g.name))
        scope.launch(Dispatchers.IO) { db.addGroupMember(g.id, nodeId) }
    }

    fun leaveGroup(g: GroupInfo) {
        ble.sendJson(Proto.groupLeave(g.id))
        scope.launch(Dispatchers.IO) {
            db.deleteGroup(g.id)
            db.clearChat(g.chatKey)
            rebuildChats()
        }
    }

    fun onGroupJoined(groupId: Long, name: String, fromId: Long) {
        scope.launch(Dispatchers.IO) {
            db.upsertGroup(GroupInfo(groupId, name.ifEmpty { "0x%08x".format(groupId) }))
            if (fromId != 0L) db.addGroupMember(groupId, fromId)
            rebuildChats()
        }
    }

    fun groupMembers(groupId: Long): List<Long> = db.groupMembers(groupId)

    fun groupOf(chatKey: String): GroupInfo? {
        val c = _chats.value.firstOrNull { it.key == chatKey } ?: return null
        return if (c.isGroup) GroupInfo(c.groupId, c.title) else null
    }

    fun clearChatHistory(chatKey: String) {
        scope.launch(Dispatchers.IO) {
            db.clearChat(chatKey)
            reload(chatKey)
        }
    }
}
