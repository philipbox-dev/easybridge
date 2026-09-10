package easylink.yvak.inc.repo

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Matrix
import android.net.Uri
import android.util.Base64
import easylink.yvak.inc.ble.BleClient
import easylink.yvak.inc.proto.Proto
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import java.io.ByteArrayOutputStream
import java.io.File

/**
 * FSK-картинки. Фото сжимается в крошечный JPEG (эфир FSK — медленный и
 * лоссовый пакетный канал), уходит на устройство чанками, оно блнёт по FSK.
 * Приём: устройство собирает JPEG, шлёт b64-чанками → показываем в чате.
 */
class ImageRepo(
    private val context: Context,
    private val ble: BleClient,
    private val scope: CoroutineScope,
) {
    companion object {
        // Профиль FSK: 0 дальнобой (медленно), 1 стандарт, 2 HD (быстро)
        // Держим картинку маленькой — 3 прохода избыточности в эфире.
        const val MAX_BYTES = 9000
        const val TARGET_DIM = 320   // длинная сторона, px
    }

    sealed class TxState {
        data object Idle : TxState()
        data object Preparing : TxState()
        data class Sending(val pass: Int, val of: Int) : TxState()
        data object Done : TxState()
        data class Error(val reason: String) : TxState()
    }

    private val _txState = MutableStateFlow<TxState>(TxState.Idle)
    val txState: StateFlow<TxState> get() = _txState

    /** Готовое принятое изображение: путь к файлу + отправитель. */
    data class Received(val path: String, val nodeId: Long, val nodeName: String)
    val received = MutableSharedFlow<Received>(extraBufferCapacity = 8)

    /** Входящее изображение началось (для индикации «Абонент шлёт фото»). */
    val incomingFrom = MutableStateFlow<String?>(null)

    private fun imgDir(): File = File(context.filesDir, "img").apply { mkdirs() }

    /** Сжать выбранное фото до крошечного JPEG под лимит эфира. */
    private fun compress(uri: Uri): ByteArray? {
        return try {
            val src: Bitmap = context.contentResolver.openInputStream(uri).use { input ->
                BitmapFactory.decodeStream(input)
            } ?: return null

            // Масштаб под TARGET_DIM по длинной стороне
            val w = src.width; val h = src.height
            val scale = TARGET_DIM.toFloat() / maxOf(w, h)
            val scaled = if (scale < 1f) {
                Bitmap.createScaledBitmap(src, (w * scale).toInt().coerceAtLeast(1),
                    (h * scale).toInt().coerceAtLeast(1), true)
            } else src

            // Понижаем качество, пока не влезем в MAX_BYTES
            var quality = 60
            var bytes: ByteArray
            do {
                val out = ByteArrayOutputStream()
                scaled.compress(Bitmap.CompressFormat.JPEG, quality, out)
                bytes = out.toByteArray()
                quality -= 10
            } while (bytes.size > MAX_BYTES && quality >= 20)

            if (bytes.size > MAX_BYTES) null else bytes
        } catch (_: Exception) {
            null
        }
    }

    /** Сжать фото и сохранить локальную копию. null = не удалось. */
    fun prepare(uri: Uri): Pair<File, ByteArray>? {
        val bytes = compress(uri) ?: return null
        val f = File(imgDir(), "out_${System.currentTimeMillis()}.jpg")
        f.writeBytes(bytes)
        return f to bytes
    }

    /** Блнуть готовый JPEG на устройство чанками → оно уйдёт по FSK. */
    fun blast(bytes: ByteArray, nodeId: Long, profile: Int = 1) {
        if (_txState.value is TxState.Sending) return
        _txState.value = TxState.Sending(0, 3)
        scope.launch(Dispatchers.IO) {
            val overhead = 90
            val b64Budget = (ble.maxWriteBytes - overhead).coerceAtLeast(60)
            val rawChunk = (b64Budget / 4) * 3
            val total = (bytes.size + rawChunk - 1) / rawChunk
            for (i in 0 until total) {
                val from = i * rawChunk
                val to = minOf(from + rawChunk, bytes.size)
                val b64 = Base64.encodeToString(bytes, from, to - from, Base64.NO_WRAP)
                ble.sendJson(Proto.imgTx(i, total, profile, nodeId, b64))
                delay(35)
            }
            // Дальше устройство блнёт в эфир — прогресс придёт evt image_tx/image_sent
        }
    }

    // ── События от устройства ─────────────────────────────────
    fun onTxProgress(pass: Int, of: Int) { _txState.value = TxState.Sending(pass, of) }
    fun onSent() {
        _txState.value = TxState.Done
        scope.launch { delay(2500); if (_txState.value is TxState.Done) _txState.value = TxState.Idle }
    }
    fun onTxError(reason: String) { _txState.value = TxState.Error(reason) }

    fun onIncomingStart(nodeName: String) { incomingFrom.value = nodeName }

    // ── Приём картинки чанками ────────────────────────────────
    private class ImgRx(val total: Int) {
        val chunks = arrayOfNulls<ByteArray>(total)
        var got = 0
        val startedMs = System.currentTimeMillis()
    }
    private val imgRx = HashMap<Long, ImgRx>()

    fun onImageChunk(nodeId: Long, idx: Int, total: Int, dataB64: String, nodeName: String) {
        if (total <= 0 || idx < 0 || idx >= total) return
        synchronized(imgRx) {
            imgRx.entries.removeAll { System.currentTimeMillis() - it.value.startedMs > 120_000 }
            val rx = imgRx.getOrPut(nodeId) { ImgRx(total) }
            if (rx.total != total) { imgRx.remove(nodeId); return }
            if (rx.chunks[idx] == null) {
                rx.chunks[idx] = try { Base64.decode(dataB64, Base64.NO_WRAP) }
                catch (_: Exception) { return }
                rx.got++
            }
            if (rx.got < rx.total) return
            imgRx.remove(nodeId)
            incomingFrom.value = null
            val out = ByteArrayOutputStream()
            for (c in rx.chunks) out.write(c!!)
            val bytes = out.toByteArray()
            scope.launch(Dispatchers.IO) {
                val f = File(imgDir(), "in_%08x_%d.jpg".format(nodeId, System.currentTimeMillis()))
                f.writeBytes(bytes)
                received.tryEmit(Received(f.absolutePath, nodeId, nodeName))
            }
        }
    }

    fun onImageFail(got: Int, total: Int) {
        incomingFrom.value = null
        _txState.value = TxState.Idle
    }

    /** Повернуть/декодировать принятый JPEG для показа (учёт EXIF не нужен — свой). */
    fun decodeForDisplay(path: String): Bitmap? = try {
        BitmapFactory.decodeFile(path)
    } catch (_: Exception) { null }
}
