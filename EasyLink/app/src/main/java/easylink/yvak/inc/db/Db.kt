package easylink.yvak.inc.db

import android.content.ContentValues
import android.content.Context
import android.database.Cursor
import android.database.sqlite.SQLiteDatabase
import android.database.sqlite.SQLiteOpenHelper
import easylink.yvak.inc.proto.ChatMessage
import easylink.yvak.inc.proto.GroupInfo
import easylink.yvak.inc.proto.MsgDir
import easylink.yvak.inc.proto.MsgKind
import easylink.yvak.inc.proto.MsgStatus
import easylink.yvak.inc.proto.PeerLocation

/**
 * Локальная история: сообщения, группы, последние локации узлов.
 * Обычный SQLite без ORM — объёмы у LoRa-сети маленькие.
 */
class Db(context: Context) : SQLiteOpenHelper(context, "easylink.db", null, 4) {

    override fun onCreate(db: SQLiteDatabase) {
        createV2Tables(db)
        createV3Tables(db)
        createV4Tables(db)
        db.execSQL(
            """CREATE TABLE messages(
                _id INTEGER PRIMARY KEY AUTOINCREMENT,
                chat_key TEXT NOT NULL,
                dir INTEGER NOT NULL,
                kind INTEGER NOT NULL,
                text TEXT NOT NULL DEFAULT '',
                voice_path TEXT NOT NULL DEFAULT '',
                seq INTEGER NOT NULL DEFAULT -1,
                status INTEGER NOT NULL DEFAULT 0,
                node_id INTEGER NOT NULL DEFAULT 0,
                node_name TEXT NOT NULL DEFAULT '',
                rssi INTEGER NOT NULL DEFAULT 0,
                hops INTEGER NOT NULL DEFAULT 0,
                ts INTEGER NOT NULL
            )"""
        )
        db.execSQL("CREATE INDEX idx_messages_chat ON messages(chat_key, ts)")
        db.execSQL("CREATE TABLE groups(id INTEGER PRIMARY KEY, name TEXT NOT NULL)")
        db.execSQL(
            """CREATE TABLE locations(
                node_id INTEGER PRIMARY KEY,
                name TEXT NOT NULL DEFAULT '',
                lat REAL NOT NULL, lon REAL NOT NULL, ts INTEGER NOT NULL
            )"""
        )
    }

    override fun onUpgrade(db: SQLiteDatabase, oldV: Int, newV: Int) {
        if (oldV < 2) createV2Tables(db)
        if (oldV < 3) createV3Tables(db)
        if (oldV < 4) createV4Tables(db)
    }

    private fun createV4Tables(db: SQLiteDatabase) {
        db.execSQL(
            """CREATE TABLE IF NOT EXISTS loc_hist(
                _id INTEGER PRIMARY KEY AUTOINCREMENT,
                node_id INTEGER NOT NULL,
                lat REAL NOT NULL, lon REAL NOT NULL, ts INTEGER NOT NULL
            )"""
        )
        db.execSQL("CREATE INDEX IF NOT EXISTS idx_lh ON loc_hist(node_id, ts)")
    }

    // ── История локаций узлов (хвосты на карте) ───────────────
    fun addLocHist(nodeId: Long, lat: Double, lon: Double, ts: Long) {
        writableDatabase.insert("loc_hist", null, ContentValues().apply {
            put("node_id", nodeId); put("lat", lat); put("lon", lon); put("ts", ts)
        })
        // держим сутки
        writableDatabase.delete("loc_hist", "ts < ?",
            arrayOf((System.currentTimeMillis() - 24 * 3_600_000L).toString()))
    }

    fun locHist(sinceMs: Long): Map<Long, List<TrackPoint>> {
        val out = HashMap<Long, MutableList<TrackPoint>>()
        readableDatabase.rawQuery(
            "SELECT node_id, ts, lat, lon FROM loc_hist WHERE ts>=? ORDER BY ts ASC",
            arrayOf(sinceMs.toString())
        ).use { c ->
            while (c.moveToNext()) {
                out.getOrPut(c.getLong(0)) { ArrayList() }
                    .add(TrackPoint(c.getLong(1), c.getDouble(2), c.getDouble(3), 0.0))
            }
        }
        return out
    }

    // ── Статистика за день ────────────────────────────────────
    /** (отправлено сегодня, из них доставлено). */
    fun outStatsSince(sinceMs: Long): Pair<Int, Int> {
        var total = 0; var delivered = 0
        readableDatabase.rawQuery(
            "SELECT status, COUNT(*) FROM messages WHERE dir=${MsgDir.OUT.ordinal} " +
                "AND kind=${MsgKind.TEXT.ordinal} AND ts>=? GROUP BY status",
            arrayOf(sinceMs.toString())
        ).use { c ->
            while (c.moveToNext()) {
                total += c.getInt(1)
                if (c.getInt(0) == MsgStatus.DELIVERED.ordinal) delivered += c.getInt(1)
            }
        }
        return total to delivered
    }

    private fun createV3Tables(db: SQLiteDatabase) {
        db.execSQL(
            """CREATE TABLE IF NOT EXISTS track(
                _id INTEGER PRIMARY KEY AUTOINCREMENT,
                ts INTEGER NOT NULL, lat REAL NOT NULL, lon REAL NOT NULL,
                alt REAL NOT NULL DEFAULT 0
            )"""
        )
    }

    // ── Трек ──────────────────────────────────────────────────
    fun addTrackPoint(ts: Long, lat: Double, lon: Double, alt: Double) {
        writableDatabase.insert("track", null, ContentValues().apply {
            put("ts", ts); put("lat", lat); put("lon", lon); put("alt", alt)
        })
    }

    data class TrackPoint(val ts: Long, val lat: Double, val lon: Double, val alt: Double)

    fun trackPoints(): List<TrackPoint> {
        val out = ArrayList<TrackPoint>()
        readableDatabase.rawQuery("SELECT ts, lat, lon, alt FROM track ORDER BY ts ASC", null)
            .use { c ->
                while (c.moveToNext())
                    out.add(TrackPoint(c.getLong(0), c.getDouble(1), c.getDouble(2), c.getDouble(3)))
            }
        return out
    }

    fun trackCount(): Int {
        readableDatabase.rawQuery("SELECT COUNT(*) FROM track", null).use { c ->
            return if (c.moveToNext()) c.getInt(0) else 0
        }
    }

    fun clearTrack() { writableDatabase.delete("track", null, null) }

    private fun createV2Tables(db: SQLiteDatabase) {
        db.execSQL(
            """CREATE TABLE IF NOT EXISTS dms(
                peer_id INTEGER PRIMARY KEY, name TEXT NOT NULL DEFAULT ''
            )"""
        )
        db.execSQL(
            """CREATE TABLE IF NOT EXISTS group_members(
                group_id INTEGER NOT NULL, node_id INTEGER NOT NULL,
                PRIMARY KEY(group_id, node_id)
            )"""
        )
    }

    // ── Личные чаты ───────────────────────────────────────────
    fun upsertDm(peerId: Long, name: String) {
        writableDatabase.insertWithOnConflict("dms", null,
            ContentValues().apply { put("peer_id", peerId); put("name", name) },
            SQLiteDatabase.CONFLICT_REPLACE)
    }

    fun dms(): List<Pair<Long, String>> {
        val out = ArrayList<Pair<Long, String>>()
        readableDatabase.rawQuery("SELECT peer_id, name FROM dms", null).use { c ->
            while (c.moveToNext()) out.add(c.getLong(0) to c.getString(1))
        }
        return out
    }

    fun deleteDm(peerId: Long) {
        writableDatabase.delete("dms", "peer_id=?", arrayOf(peerId.toString()))
    }

    // ── Участники групп ───────────────────────────────────────
    fun addGroupMember(groupId: Long, nodeId: Long) {
        writableDatabase.insertWithOnConflict("group_members", null,
            ContentValues().apply { put("group_id", groupId); put("node_id", nodeId) },
            SQLiteDatabase.CONFLICT_IGNORE)
    }

    fun groupMembers(groupId: Long): List<Long> {
        val out = ArrayList<Long>()
        readableDatabase.rawQuery("SELECT node_id FROM group_members WHERE group_id=?",
            arrayOf(groupId.toString())).use { c ->
            while (c.moveToNext()) out.add(c.getLong(0))
        }
        return out
    }

    // ── Сообщения ─────────────────────────────────────────────
    fun insertMessage(m: ChatMessage): Long = writableDatabase.insert("messages", null,
        ContentValues().apply {
            put("chat_key", m.chatKey); put("dir", m.dir.ordinal); put("kind", m.kind.ordinal)
            put("text", m.text); put("voice_path", m.voicePath); put("seq", m.seq)
            put("status", m.status.ordinal); put("node_id", m.nodeId)
            put("node_name", m.nodeName); put("rssi", m.rssi); put("hops", m.hops)
            put("ts", m.ts)
        })

    fun updateStatusBySeq(seq: Int, status: MsgStatus, onlyIfBelow: MsgStatus? = null): Int {
        val where = StringBuilder("seq=? AND dir=${MsgDir.OUT.ordinal}")
        if (onlyIfBelow != null) where.append(" AND status<${onlyIfBelow.ordinal}")
        return writableDatabase.update("messages",
            ContentValues().apply { put("status", status.ordinal) },
            where.toString(), arrayOf(seq.toString()))
    }

    fun messages(chatKey: String, limit: Int = 500): List<ChatMessage> {
        val out = ArrayList<ChatMessage>()
        readableDatabase.rawQuery(
            "SELECT * FROM (SELECT * FROM messages WHERE chat_key=? ORDER BY ts DESC, _id DESC LIMIT $limit) ORDER BY ts ASC, _id ASC",
            arrayOf(chatKey)
        ).use { c -> while (c.moveToNext()) out.add(c.toMessage()) }
        return out
    }

    /** Исходящие текстовые, которые так и не ушли в эфир (status=SENDING). */
    fun pendingUnsent(sinceMs: Long): List<ChatMessage> {
        val out = ArrayList<ChatMessage>()
        readableDatabase.rawQuery(
            "SELECT * FROM messages WHERE dir=${MsgDir.OUT.ordinal} AND kind=${MsgKind.TEXT.ordinal} " +
                "AND status=${MsgStatus.SENDING.ordinal} AND ts>=? ORDER BY ts ASC LIMIT 50",
            arrayOf(sinceMs.toString())
        ).use { c -> while (c.moveToNext()) out.add(c.toMessage()) }
        return out
    }

    fun lastMessage(chatKey: String): ChatMessage? {
        readableDatabase.rawQuery(
            "SELECT * FROM messages WHERE chat_key=? ORDER BY ts DESC, _id DESC LIMIT 1",
            arrayOf(chatKey)
        ).use { c -> return if (c.moveToNext()) c.toMessage() else null }
    }

    fun clearChat(chatKey: String) {
        writableDatabase.delete("messages", "chat_key=?", arrayOf(chatKey))
    }

    private fun Cursor.toMessage() = ChatMessage(
        id = getLong(getColumnIndexOrThrow("_id")),
        chatKey = getString(getColumnIndexOrThrow("chat_key")),
        dir = MsgDir.entries[getInt(getColumnIndexOrThrow("dir"))],
        kind = MsgKind.entries[getInt(getColumnIndexOrThrow("kind"))],
        text = getString(getColumnIndexOrThrow("text")),
        voicePath = getString(getColumnIndexOrThrow("voice_path")),
        seq = getInt(getColumnIndexOrThrow("seq")),
        status = MsgStatus.entries[getInt(getColumnIndexOrThrow("status"))],
        nodeId = getLong(getColumnIndexOrThrow("node_id")),
        nodeName = getString(getColumnIndexOrThrow("node_name")),
        rssi = getInt(getColumnIndexOrThrow("rssi")),
        hops = getInt(getColumnIndexOrThrow("hops")),
        ts = getLong(getColumnIndexOrThrow("ts")),
    )

    // ── Группы ────────────────────────────────────────────────
    fun groups(): List<GroupInfo> {
        val out = ArrayList<GroupInfo>()
        readableDatabase.rawQuery("SELECT id, name FROM groups ORDER BY name", null).use { c ->
            while (c.moveToNext()) out.add(GroupInfo(c.getLong(0), c.getString(1)))
        }
        return out
    }

    fun upsertGroup(g: GroupInfo) {
        writableDatabase.insertWithOnConflict("groups", null,
            ContentValues().apply { put("id", g.id); put("name", g.name) },
            SQLiteDatabase.CONFLICT_REPLACE)
    }

    fun deleteGroup(id: Long) {
        writableDatabase.delete("groups", "id=?", arrayOf(id.toString()))
    }

    // ── Локации ───────────────────────────────────────────────
    fun upsertLocation(l: PeerLocation) {
        writableDatabase.insertWithOnConflict("locations", null,
            ContentValues().apply {
                put("node_id", l.nodeId); put("name", l.name)
                put("lat", l.lat); put("lon", l.lon); put("ts", l.ts)
            }, SQLiteDatabase.CONFLICT_REPLACE)
    }

    fun locations(): List<PeerLocation> {
        val out = ArrayList<PeerLocation>()
        readableDatabase.rawQuery("SELECT node_id, name, lat, lon, ts FROM locations", null)
            .use { c ->
                while (c.moveToNext()) out.add(
                    PeerLocation(c.getLong(0), c.getString(1), c.getDouble(2),
                        c.getDouble(3), c.getLong(4)))
            }
        return out
    }
}
