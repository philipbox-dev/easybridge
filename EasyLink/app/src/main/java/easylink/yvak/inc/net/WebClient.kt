package easylink.yvak.inc.net

import android.util.Log
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import okhttp3.Response
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import okio.ByteString
import okio.ByteString.Companion.toByteString
import org.json.JSONObject
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Транспорт до сервера LoRa-чата: вход по паролю и WebSocket.
 *
 * Обычный WebSocket, а не Socket.IO — так решено на стороне сервера,
 * чтобы здесь хватило OkHttp без лишней библиотеки и её особенностей.
 *
 * Класс занимается только связью: что означают приходящие кадры,
 * знает WebRepo. Разделение важно — переподключение и разбор смысла
 * это разные заботы, и мешать их в одном месте больно.
 */
class WebClient {

    companion object {
        private const val TAG = "WebClient"
        /** Кадр аудио: бинарный, с 8-байтным заголовком (см. WEB_BRIDGE.md). */
        const val AUDIO_MAGIC = 0xA1
        const val AUDIO_HEADER = 8
        const val CODEC_PCM16 = 0
        const val CODEC_OPUS = 1

        private const val PING_SEC = 20L
        private const val RECONNECT_MIN_MS = 1_000L
        private const val RECONNECT_MAX_MS = 30_000L
    }

    data class Session(
        val token: String,
        val memberId: Int,
        val memberName: String,
        val netName: String,
        val netSlug: String,
    )

    /** Состояние связи для интерфейса. */
    sealed class State {
        data object Offline : State()
        data object Connecting : State()
        data object Online : State()
        data class Failed(val reason: String) : State()
    }

    private val http = OkHttpClient.Builder()
        .connectTimeout(15, TimeUnit.SECONDS)
        .readTimeout(0, TimeUnit.MILLISECONDS)   // сокет живёт долго
        .pingInterval(PING_SEC, TimeUnit.SECONDS)
        .retryOnConnectionFailure(true)
        .build()

    private var socket: WebSocket? = null
    private val wantOpen = AtomicBoolean(false)
    private var attempt = 0

    var onState: (State) -> Unit = {}
    var onJson: (JSONObject) -> Unit = {}
    var onAudio: (ByteArray) -> Unit = {}

    @Volatile var lastError: String = ""
        private set

    val connected: Boolean get() = socket != null

    // ── Вход ────────────────────────────────────────────────
    /**
     * Логин телефона. Отдаёт токен устройства — он и есть пропуск в
     * сокет, отдельной сессии приложению не нужно.
     *
     * hasFsk и hwProfile берутся у платы командой get_radio и врать
     * здесь нельзя: сервер по hasFsk решает, можно ли выводить звонок
     * в эфир, и при false честно скажет людям, что голос уйдёт только
     * в веб.
     */
    fun login(
        baseUrl: String,
        uid: String,
        password: String,
        nodeId: String,
        deviceName: String,
        hwProfile: String,
        hasFsk: Boolean,
        canGateway: Boolean,
    ): Result<Session> {
        val body = JSONObject()
            .put("uid", uid)
            .put("password", password)
            .put("node_id", nodeId)
            .put("device_name", deviceName)
            .put("hw_profile", hwProfile)
            .put("has_fsk", hasFsk)
            .put("can_gateway", canGateway)
            .toString()

        val req = Request.Builder()
            .url(normalize(baseUrl) + "/lora-chat/api/login")
            .post(body.toRequestBody("application/json".toMediaType()))
            .build()

        return try {
            http.newCall(req).execute().use { resp -> parseLogin(resp) }
        } catch (e: Exception) {
            lastError = e.message ?: "сеть недоступна"
            Result.failure(e)
        }
    }

    private fun parseLogin(resp: Response): Result<Session> {
        val text = resp.body?.string().orEmpty()
        if (!resp.isSuccessful) {
            // Сервер объясняет отказ по-человечески — покажем это, а не
            // «HTTP 401», иначе человек не поймёт, что не так с паролем.
            val desc = try { JSONObject(text).optString("desc") } catch (_: Exception) { "" }
            lastError = desc.ifBlank { "сервер ответил ${resp.code}" }
            return Result.failure(IllegalStateException(lastError))
        }
        return try {
            val o = JSONObject(text)
            val m = o.getJSONObject("member")
            val n = o.getJSONObject("network")
            Result.success(Session(
                token = o.getString("token"),
                memberId = m.optInt("id"),
                memberName = m.optString("name"),
                netName = n.optString("name"),
                netSlug = n.optString("slug"),
            ))
        } catch (e: Exception) {
            lastError = "непонятный ответ сервера"
            Result.failure(e)
        }
    }

    // ── Сокет ───────────────────────────────────────────────
    fun connect(baseUrl: String, token: String) {
        wantOpen.set(true)
        openSocket(baseUrl, token)
    }

    private fun openSocket(baseUrl: String, token: String) {
        if (!wantOpen.get()) return
        socket?.cancel()
        onState(State.Connecting)

        val url = normalize(baseUrl)
            .replaceFirst("http://", "ws://")
            .replaceFirst("https://", "wss://") + "/lora-chat/api/ws?token=$token"

        val req = Request.Builder().url(url).build()
        socket = http.newWebSocket(req, object : WebSocketListener() {
            override fun onOpen(ws: WebSocket, response: Response) {
                attempt = 0
                lastError = ""
                onState(State.Online)
            }

            override fun onMessage(ws: WebSocket, text: String) {
                val o = try { JSONObject(text) } catch (_: Exception) { return }
                onJson(o)
            }

            override fun onMessage(ws: WebSocket, bytes: ByteString) {
                onAudio(bytes.toByteArray())
            }

            override fun onFailure(ws: WebSocket, t: Throwable, response: Response?) {
                lastError = t.message ?: "разрыв связи"
                Log.w(TAG, "сокет упал: $lastError")
                socket = null
                onState(State.Failed(lastError))
                scheduleReconnect(baseUrl, token)
            }

            override fun onClosed(ws: WebSocket, code: Int, reason: String) {
                socket = null
                onState(State.Offline)
                scheduleReconnect(baseUrl, token)
            }
        })
    }

    private fun scheduleReconnect(baseUrl: String, token: String) {
        if (!wantOpen.get()) return
        // Пауза растёт до 30 с: сервер могли перезапустить, а долбиться
        // раз в секунду на мобильном интернете — это только батарея.
        attempt = (attempt + 1).coerceAtMost(10)
        val delay = (RECONNECT_MIN_MS * attempt).coerceAtMost(RECONNECT_MAX_MS)
        Thread {
            try { Thread.sleep(delay) } catch (_: InterruptedException) { return@Thread }
            openSocket(baseUrl, token)
        }.apply { isDaemon = true }.start()
    }

    fun disconnect() {
        wantOpen.set(false)
        socket?.close(1000, "пока")
        socket = null
        onState(State.Offline)
    }

    // ── Отправка ────────────────────────────────────────────
    fun send(o: JSONObject): Boolean = socket?.send(o.toString()) ?: false

    fun send(build: JSONObject.() -> Unit): Boolean =
        send(JSONObject().apply(build))

    /** Аудиокадр. Отправителя проставит сервер — здесь он не нужен. */
    fun sendAudio(payload: ByteArray, seq: Int, codec: Int, fromAir: Boolean): Boolean {
        val ws = socket ?: return false
        val out = ByteArray(AUDIO_HEADER + payload.size)
        out[0] = AUDIO_MAGIC.toByte()
        out[1] = if (fromAir) 1 else 0
        out[4] = ((seq shr 8) and 0xFF).toByte()
        out[5] = (seq and 0xFF).toByte()
        out[6] = codec.toByte()
        payload.copyInto(out, AUDIO_HEADER)
        return ws.send(out.toByteString())
    }

    /** Разбор входящего аудиокадра. null — это не аудио. */
    fun parseAudio(data: ByteArray): AudioFrame? {
        if (data.size < AUDIO_HEADER) return null
        if ((data[0].toInt() and 0xFF) != AUDIO_MAGIC) return null
        return AudioFrame(
            fromAir = (data[1].toInt() and 0x01) != 0,
            member = ((data[2].toInt() and 0xFF) shl 8) or (data[3].toInt() and 0xFF),
            seq = ((data[4].toInt() and 0xFF) shl 8) or (data[5].toInt() and 0xFF),
            codec = data[6].toInt() and 0xFF,
            payload = data.copyOfRange(AUDIO_HEADER, data.size),
        )
    }

    data class AudioFrame(
        val fromAir: Boolean,
        val member: Int,
        val seq: Int,
        val codec: Int,
        val payload: ByteArray,
    ) {
        // data class с ByteArray без своих equals/hashCode сравнивался бы
        // по ссылке — Kotlin об этом предупреждает, и не зря.
        override fun equals(other: Any?): Boolean {
            if (this === other) return true
            if (other !is AudioFrame) return false
            return seq == other.seq && member == other.member &&
                codec == other.codec && fromAir == other.fromAir &&
                payload.contentEquals(other.payload)
        }

        override fun hashCode(): Int =
            (((seq * 31 + member) * 31 + codec) * 31 + fromAir.hashCode()) * 31 +
                payload.contentHashCode()
    }

    private fun normalize(url: String): String {
        var u = url.trim().trimEnd('/')
        if (u.isEmpty()) return u
        if (!u.startsWith("http://") && !u.startsWith("https://")) u = "https://$u"
        return u
    }
}
