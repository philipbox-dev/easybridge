package easylink.yvak.inc.audio

import android.media.MediaCodec
import android.media.MediaFormat
import android.util.Log
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Перекодирование звука между вебом и эфиром.
 *
 * Зачем это на телефоне, а не на сервере: сервер сознательно не трогает
 * содержимое звука. У Android уже есть AMR-NB в MediaCodec, а поставить
 * кодеки на веб-сервер значит держать там ffmpeg и кормить его на
 * каждом звонке.
 *
 *   веб  →  PCM16 16 кГц  →  [сюда]  →  AMR-NB 4.75к  →  плата → эфир
 *   эфир →  AMR-NB        →  [сюда]  →  PCM16 16 кГц  →  веб
 *
 * Веб-сторона говорит PCM16, а не Opus, именно ради этого моста:
 * сырой PCM пережимается без плясок с codec-specific data, которых
 * требует Opus в MediaCodec. Когда эфирного плеча нет, браузеры между
 * собой прекрасно общаются Opus'ом — этот класс тогда не работает.
 *
 * Обе стороны крутятся в своих потоках: перекодирование не должно
 * тормозить ни сокет, ни BLE.
 */
class WebAudioBridge(
    /** Готовые AMR-кадры → на плату (ptt_audio). */
    private val onAmrToAir: (ByteArray) -> Unit,
    /** Готовый PCM 16 кГц → на сервер. */
    private val onPcmToWeb: (ByteArray) -> Unit,
) {
    companion object {
        private const val TAG = "WebAudioBridge"
        private const val AIR_RATE = 8000       // AMR-NB всегда 8 кГц
        private const val WEB_RATE = 16000
        /** Кадр AMR 20 мс = 160 отсчётов на 8 кГц. */
        private const val AIR_FRAME_SAMPLES = 160
        /** Сколько кадров копим перед отправкой на плату.
         *  4×13 Б = 52 Б ≤ 57 Б FSK-чанка прошивки — как в PttAudioEngine. */
        private const val FRAMES_PER_PACKET = 4
        private const val QUEUE_DEPTH = 24
        private const val CODEC_TIMEOUT_US = 10_000L
    }

    private val running = AtomicBoolean(false)
    private val toAir = ArrayBlockingQueue<ByteArray>(QUEUE_DEPTH)
    private val toWeb = ArrayBlockingQueue<ByteArray>(QUEUE_DEPTH)

    private var encoder: MediaCodec? = null
    private var decoder: MediaCodec? = null
    private var encThread: Thread? = null
    private var decThread: Thread? = null

    /** Сколько кадров выкинуто из-за переполнения — видно в дебаге. */
    @Volatile var dropped = 0
        private set

    val active: Boolean get() = running.get()

    fun start(): Boolean {
        if (running.get()) return true
        try {
            val enc = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_AUDIO_AMR_NB)
            val encFmt = MediaFormat.createAudioFormat(
                MediaFormat.MIMETYPE_AUDIO_AMR_NB, AIR_RATE, 1)
            encFmt.setInteger(MediaFormat.KEY_BIT_RATE, 4750)
            enc.configure(encFmt, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            enc.start()
            encoder = enc

            val dec = MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_AUDIO_AMR_NB)
            val decFmt = MediaFormat.createAudioFormat(
                MediaFormat.MIMETYPE_AUDIO_AMR_NB, AIR_RATE, 1)
            decFmt.setInteger(MediaFormat.KEY_BIT_RATE, 4750)
            dec.configure(decFmt, null, null, 0)
            dec.start()
            decoder = dec
        } catch (e: Exception) {
            Log.e(TAG, "не поднялись кодеки: ${e.message}")
            stop()
            return false
        }

        running.set(true)
        encThread = Thread(::encodeLoop, "web-air-enc").apply { isDaemon = true; start() }
        decThread = Thread(::decodeLoop, "air-web-dec").apply { isDaemon = true; start() }
        return true
    }

    fun stop() {
        running.set(false)
        encThread?.interrupt(); decThread?.interrupt()
        encThread = null; decThread = null
        toAir.clear(); toWeb.clear()
        runCatching { encoder?.stop() }; runCatching { encoder?.release() }
        runCatching { decoder?.stop() }; runCatching { decoder?.release() }
        encoder = null; decoder = null
    }

    // ── Вход ────────────────────────────────────────────────
    /** PCM16 16 кГц из веба. Лишнее выкидываем: в звонке лучше
     *  потерять кадр, чем отстать на секунду. */
    fun feedFromWeb(pcm16k: ByteArray) {
        if (!running.get()) return
        if (!toAir.offer(pcm16k)) { toAir.poll(); toAir.offer(pcm16k); dropped++ }
    }

    /** Сырые AMR-кадры из эфира. */
    fun feedFromAir(amr: ByteArray) {
        if (!running.get()) return
        if (!toWeb.offer(amr)) { toWeb.poll(); toWeb.offer(amr); dropped++ }
    }

    // ── Пересчёт частоты ────────────────────────────────────
    // 16 кГц ↔ 8 кГц ровно в два раза. При понижении усредняем пару
    // отсчётов: это грубый ФНЧ, но он честнее простого прореживания,
    // которое заворачивает верхи в слышимый диапазон.
    private fun down16to8(src: ShortArray): ShortArray {
        val out = ShortArray(src.size / 2)
        for (i in out.indices) {
            out[i] = ((src[i * 2].toInt() + src[i * 2 + 1].toInt()) / 2).toShort()
        }
        return out
    }

    private fun up8to16(src: ShortArray): ShortArray {
        val out = ShortArray(src.size * 2)
        for (i in src.indices) {
            val cur = src[i].toInt()
            val next = if (i + 1 < src.size) src[i + 1].toInt() else cur
            out[i * 2] = cur.toShort()
            out[i * 2 + 1] = ((cur + next) / 2).toShort()   // линейная интерполяция
        }
        return out
    }

    private fun bytesToShorts(b: ByteArray): ShortArray {
        val out = ShortArray(b.size / 2)
        for (i in out.indices) {
            out[i] = (((b[i * 2 + 1].toInt() and 0xFF) shl 8) or
                (b[i * 2].toInt() and 0xFF)).toShort()
        }
        return out
    }

    private fun shortsToBytes(s: ShortArray): ByteArray {
        val out = ByteArray(s.size * 2)
        for (i in s.indices) {
            val v = s[i].toInt()
            out[i * 2] = (v and 0xFF).toByte()
            out[i * 2 + 1] = ((v shr 8) and 0xFF).toByte()
        }
        return out
    }

    // ── Веб → эфир ──────────────────────────────────────────
    private fun encodeLoop() {
        val enc = encoder ?: return
        val info = MediaCodec.BufferInfo()
        val pending = ArrayList<ByteArray>(FRAMES_PER_PACKET)
        var carry = ShortArray(0)
        var ptsUs = 0L

        while (running.get()) {
            try {
                val chunk = toAir.poll(200, java.util.concurrent.TimeUnit.MILLISECONDS)
                if (chunk != null) {
                    val at8k = down16to8(bytesToShorts(chunk))
                    carry = if (carry.isEmpty()) at8k else carry + at8k
                }

                // Кодек принимает ровно по кадру: остаток держим до
                // следующей порции, иначе на стыках будут щелчки.
                while (carry.size >= AIR_FRAME_SAMPLES && running.get()) {
                    val frame = carry.copyOfRange(0, AIR_FRAME_SAMPLES)
                    carry = carry.copyOfRange(AIR_FRAME_SAMPLES, carry.size)

                    val inIdx = enc.dequeueInputBuffer(CODEC_TIMEOUT_US)
                    if (inIdx < 0) break
                    val pcm = shortsToBytes(frame)
                    enc.getInputBuffer(inIdx)?.apply { clear(); put(pcm) }
                    enc.queueInputBuffer(inIdx, 0, pcm.size, ptsUs, 0)
                    ptsUs += 20_000L

                    var outIdx = enc.dequeueOutputBuffer(info, 0)
                    while (outIdx >= 0) {
                        val buf = enc.getOutputBuffer(outIdx)
                        if (buf != null && info.size > 0) {
                            val amr = ByteArray(info.size)
                            buf.position(info.offset)
                            buf.get(amr)
                            pending.add(amr)
                        }
                        enc.releaseOutputBuffer(outIdx, false)
                        outIdx = enc.dequeueOutputBuffer(info, 0)
                    }

                    if (pending.size >= FRAMES_PER_PACKET) {
                        onAmrToAir(pending.reduce { a, b -> a + b })
                        pending.clear()
                    }
                }
            } catch (_: InterruptedException) {
                return
            } catch (e: Exception) {
                Log.w(TAG, "кодирование: ${e.message}")
            }
        }
    }

    // ── Эфир → веб ──────────────────────────────────────────
    private fun decodeLoop() {
        val dec = decoder ?: return
        val info = MediaCodec.BufferInfo()
        var ptsUs = 0L

        while (running.get()) {
            try {
                val packet = toWeb.poll(200, java.util.concurrent.TimeUnit.MILLISECONDS)
                    ?: continue
                // Пачка может содержать несколько кадров: режем по TOC,
                // иначе декодер получит мусор с середины и умрёт.
                for (frame in Amr.splitFrames(packet)) {
                    val inIdx = dec.dequeueInputBuffer(CODEC_TIMEOUT_US)
                    if (inIdx < 0) continue
                    dec.getInputBuffer(inIdx)?.apply { clear(); put(frame) }
                    dec.queueInputBuffer(inIdx, 0, frame.size, ptsUs, 0)
                    ptsUs += 20_000L
                }

                var outIdx = dec.dequeueOutputBuffer(info, 0)
                while (outIdx >= 0) {
                    val buf = dec.getOutputBuffer(outIdx)
                    if (buf != null && info.size > 0) {
                        val pcm8 = ByteArray(info.size)
                        buf.position(info.offset)
                        buf.get(pcm8)
                        onPcmToWeb(shortsToBytes(up8to16(bytesToShorts(pcm8))))
                    }
                    dec.releaseOutputBuffer(outIdx, false)
                    outIdx = dec.dequeueOutputBuffer(info, 0)
                }
            } catch (_: InterruptedException) {
                return
            } catch (e: Exception) {
                Log.w(TAG, "декодирование: ${e.message}")
            }
        }
    }
}
