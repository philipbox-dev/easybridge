package easylink.yvak.inc.audio

import android.annotation.SuppressLint
import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioRecord
import android.media.AudioTrack
import android.media.MediaCodec
import android.media.MediaFormat
import android.media.MediaRecorder
import android.util.Log
import java.io.ByteArrayOutputStream
import java.util.concurrent.LinkedBlockingQueue

/**
 * Реалтайм-аудио для PTT-рации.
 * Микрофон → AMR-NB 4.75 кбит/с → пачки кадров наружу (ptt_audio);
 * входящие кадры → декодер → динамик.
 * Кадры шлём только пока зажата кнопка (txActive).
 */
class PttAudioEngine(
    private val context: Context,
    private val onFrames: (ByteArray) -> Unit,
) {

    companion object {
        private const val TAG = "PttAudio"
        private const val SAMPLE_RATE = 8000
        // 4×13Б (AMR 4.75) = 52 Б ≤ 57 Б FSK-чанка прошивки. При 5 кадрах
        // (65 Б) прошивка резала пачку на 57+8 ПОПЕРЁК кадра — приёмник
        // кормил декодер мусором с середины кадра, MediaCodec умирал в
        // error-state навсегда = «пару секунд слышно, потом тишина».
        private const val FRAMES_PER_PACKET = 4      // 4×20 мс = 80 мс на команду
        private const val MAX_PACKET_RAW = 57        // = FSK-чанк, без разрезов
        private const val AMR475_FRAME = 13          // кадр AMR-NB 4.75 с TOC
        private const val AMR475_TOC = 0x04          // валидный TOC нашего потока
    }

    @Volatile var txActive = false
    @Volatile private var running = false

    /**
     * V1.7.5: программное усиление. AGC+шумодав VOICE_COMMUNICATION на
     * части телефонов (Huawei) душит микрофон — собеседник еле слышит.
     * Лямбды читаются на каждом пакете: слайдер в настройках действует
     * прямо посреди звонка.
     */
    var micGain: () -> Float = { 1f }
    var rxGain: () -> Float = { 1f }

    /** PCM16LE × gain с защитой от клиппинга. */
    private fun applyGain(pcm: ByteArray, len: Int, gain: Float) {
        if (gain == 1f) return
        var i = 0
        while (i + 1 < len) {
            val s = ((pcm[i + 1].toInt() shl 8) or (pcm[i].toInt() and 0xFF))
            val v = (s * gain).toInt().coerceIn(-32768, 32767)
            pcm[i] = (v and 0xFF).toByte()
            pcm[i + 1] = (v shr 8).toByte()
            i += 2
        }
    }

    // Диагностика (дебаг-панель звонка)
    @Volatile var rxPackets = 0; private set      // feedRx: пачек с эфира
    @Volatile var rxBytes = 0L; private set
    @Volatile var rxQueueDrops = 0; private set   // rxQueue переполнена
    @Volatile var framesDecoded = 0; private set  // AMR-кадров ушло в декодер
    @Volatile var decodeErrors = 0; private set
    @Volatile var garbageFrames = 0; private set  // битые кадры, отсеяны
    @Volatile var decoderRecoveries = 0; private set  // реанимаций MediaCodec
    @Volatile var txPacketsSent = 0; private set  // пачек ушло в BLE
    val trackUnderruns: Int get() = try { track?.underrunCount ?: 0 } catch (_: Exception) { 0 }

    private var record: AudioRecord? = null
    private var track: AudioTrack? = null
    private var encoder: MediaCodec? = null
    private var decoder: MediaCodec? = null
    private val rxQueue = LinkedBlockingQueue<ByteArray>(64)

    // Восстанавливаем при выходе из звонка
    private var prevAudioMode = AudioManager.MODE_NORMAL
    private var prevSpeakerOn = false

    private var encThread: Thread? = null
    private var decThread: Thread? = null

    @SuppressLint("MissingPermission")
    fun start(): Boolean {
        if (running) return true
        try {
            // Без MODE_IN_COMMUNICATION поток STREAM_VOICE_CALL у некоторых
            // прошивок Android остаётся неслышимым (громкость не совпадает с
            // громкостью медиа) — именно поэтому «не слышно звонящего».
            // Громкоговоритель включаем явно: рация используется не у уха.
            val am = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
            prevAudioMode = am.mode
            prevSpeakerOn = am.isSpeakerphoneOn
            am.mode = AudioManager.MODE_IN_COMMUNICATION
            @Suppress("DEPRECATION")
            am.isSpeakerphoneOn = true

            // ── Кодер ──
            val enc = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_AUDIO_AMR_NB)
            val encFmt = MediaFormat.createAudioFormat(
                MediaFormat.MIMETYPE_AUDIO_AMR_NB, SAMPLE_RATE, 1)
            encFmt.setInteger(MediaFormat.KEY_BIT_RATE, 4750)
            enc.configure(encFmt, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            enc.start()
            encoder = enc

            // ── Декодер ──
            val dec = MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_AUDIO_AMR_NB)
            val decFmt = MediaFormat.createAudioFormat(
                MediaFormat.MIMETYPE_AUDIO_AMR_NB, SAMPLE_RATE, 1)
            dec.configure(decFmt, null, null, 0)
            dec.start()
            decoder = dec

            // ── Микрофон ──
            val minRec = AudioRecord.getMinBufferSize(SAMPLE_RATE,
                AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT)
            val rec = AudioRecord(MediaRecorder.AudioSource.VOICE_COMMUNICATION,
                SAMPLE_RATE, AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT, maxOf(minRec, 3200) * 2)
            rec.startRecording()
            record = rec

            // ── Динамик ──
            val minTrk = AudioTrack.getMinBufferSize(SAMPLE_RATE,
                AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_16BIT)
            val trk = AudioTrack(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_VOICE_COMMUNICATION)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                    .build(),
                AudioFormat.Builder()
                    .setSampleRate(SAMPLE_RATE)
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                    .build(),
                maxOf(minTrk, 3200) * 2,
                AudioTrack.MODE_STREAM, AudioManager.AUDIO_SESSION_ID_GENERATE)
            trk.play()
            track = trk

            running = true
            encThread = Thread(::encodeLoop, "ptt-enc").apply { start() }
            decThread = Thread(::decodeLoop, "ptt-dec").apply { start() }
            return true
        } catch (e: Exception) {
            Log.e(TAG, "start failed", e)
            stop()
            return false
        }
    }

    /** Переключение динамик/наушник прямо во время звонка. */
    fun setSpeakerphone(on: Boolean) {
        if (!running) return
        try {
            val am = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
            @Suppress("DEPRECATION")
            am.isSpeakerphoneOn = on
        } catch (_: Exception) {}
    }

    fun stop() {
        running = false
        txActive = false
        encThread?.interrupt(); decThread?.interrupt()
        encThread = null; decThread = null
        try {
            val am = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
            @Suppress("DEPRECATION")
            am.isSpeakerphoneOn = prevSpeakerOn
            am.mode = prevAudioMode
        } catch (_: Exception) {}
        try { record?.stop() } catch (_: Exception) {}
        try { record?.release() } catch (_: Exception) {}
        record = null
        try { track?.stop() } catch (_: Exception) {}
        try { track?.release() } catch (_: Exception) {}
        track = null
        try { encoder?.stop(); encoder?.release() } catch (_: Exception) {}
        encoder = null
        try { decoder?.stop(); decoder?.release() } catch (_: Exception) {}
        decoder = null
        rxQueue.clear()
    }

    /** Сырые AMR-кадры, принятые по эфиру (уже без b64). */
    fun feedRx(data: ByteArray) {
        if (!running) return
        rxPackets++; rxBytes += data.size
        if (!rxQueue.offer(data)) rxQueueDrops++
    }

    // ── Кодирование: PCM → AMR → пачки кадров ────────────────
    private fun encodeLoop() {
        val enc = encoder ?: return
        val rec = record ?: return
        val pcm = ByteArray(320)  // 20 мс @ 8кГц 16бит
        val batch = ByteArrayOutputStream()
        var batchFrames = 0
        val info = MediaCodec.BufferInfo()
        var wasTx = false

        while (running) {
            try {
                val n = rec.read(pcm, 0, pcm.size)
                if (n <= 0) continue
                applyGain(pcm, n, micGain())

                if (!txActive) {
                    // Отпустили кнопку: досылаем последнюю недособранную пачку,
                    // иначе конец фразы обрезался. Только на переходе talk→release.
                    if (wasTx && batch.size() > 0) { onFrames(batch.toByteArray()); txPacketsSent++ }
                    if (batch.size() > 0) { batch.reset(); batchFrames = 0 }
                    wasTx = false
                    continue
                }
                wasTx = true

                val inIdx = enc.dequeueInputBuffer(10000)
                if (inIdx >= 0) {
                    val ib = enc.getInputBuffer(inIdx) ?: continue
                    ib.clear(); ib.put(pcm, 0, n)
                    enc.queueInputBuffer(inIdx, 0, n, System.nanoTime() / 1000, 0)
                }

                var outIdx = enc.dequeueOutputBuffer(info, 0)
                while (outIdx >= 0) {
                    val ob = enc.getOutputBuffer(outIdx)
                    if (ob != null && info.size > 0) {
                        val frame = ByteArray(info.size)
                        ob.position(info.offset); ob.get(frame)
                        if (batch.size() + frame.size > MAX_PACKET_RAW ||
                            batchFrames >= FRAMES_PER_PACKET) {
                            if (batch.size() > 0) { onFrames(batch.toByteArray()); txPacketsSent++ }
                            batch.reset(); batchFrames = 0
                        }
                        batch.write(frame)
                        batchFrames++
                    }
                    enc.releaseOutputBuffer(outIdx, false)
                    outIdx = enc.dequeueOutputBuffer(info, 0)
                }
            } catch (e: InterruptedException) {
                return
            } catch (e: Exception) {
                if (running) Log.w(TAG, "encode loop", e)
            }
        }
    }

    // ── Декодирование: AMR-кадры → PCM → динамик ─────────────
    private fun decodeLoop() {
        var dec = decoder ?: return
        val trk = track ?: return
        val info = MediaCodec.BufferInfo()

        while (running) {
            try {
                val data = rxQueue.poll(100, java.util.concurrent.TimeUnit.MILLISECONDS)
                if (data != null) {
                    for (frame in Amr.splitFrames(data)) {
                        // Битые байты с эфира (или старое приложение,
                        // режущее кадры поперёк) — в декодер не пускаем.
                        if (frame.size != AMR475_FRAME ||
                            frame[0].toInt() != AMR475_TOC) {
                            garbageFrames++
                            continue
                        }
                        val inIdx = dec.dequeueInputBuffer(10000)
                        if (inIdx >= 0) {
                            val ib = dec.getInputBuffer(inIdx) ?: continue
                            ib.clear(); ib.put(frame)
                            dec.queueInputBuffer(inIdx, 0, frame.size,
                                System.nanoTime() / 1000, 0)
                            framesDecoded++
                        }
                        drainDecoder(dec, trk, info)
                    }
                }
                drainDecoder(dec, trk, info)
            } catch (e: InterruptedException) {
                return
            } catch (e: Exception) {
                decodeErrors++
                if (!running) return
                Log.w(TAG, "decode loop", e)
                // MediaCodec после CodecException мёртв навсегда — лечим
                // пересозданием, иначе весь остаток звонка без звука.
                try {
                    dec.reset()
                    val fmt = MediaFormat.createAudioFormat(
                        MediaFormat.MIMETYPE_AUDIO_AMR_NB, SAMPLE_RATE, 1)
                    dec.configure(fmt, null, null, 0)
                    dec.start()
                    decoderRecoveries++
                } catch (e2: Exception) {
                    Log.e(TAG, "decoder recovery failed", e2)
                    try { dec.release() } catch (_: Exception) {}
                    dec = try {
                        MediaCodec.createDecoderByType(
                            MediaFormat.MIMETYPE_AUDIO_AMR_NB).apply {
                            val fmt = MediaFormat.createAudioFormat(
                                MediaFormat.MIMETYPE_AUDIO_AMR_NB, SAMPLE_RATE, 1)
                            configure(fmt, null, null, 0)
                            start()
                        }
                    } catch (e3: Exception) {
                        Log.e(TAG, "decoder recreate failed", e3)
                        return
                    }
                    decoder = dec
                    decoderRecoveries++
                }
            }
        }
    }

    private fun drainDecoder(dec: MediaCodec, trk: AudioTrack, info: MediaCodec.BufferInfo) {
        var outIdx = dec.dequeueOutputBuffer(info, 0)
        while (outIdx >= 0) {
            val ob = dec.getOutputBuffer(outIdx)
            if (ob != null && info.size > 0) {
                val pcm = ByteArray(info.size)
                ob.position(info.offset); ob.get(pcm)
                applyGain(pcm, pcm.size, rxGain())
                trk.write(pcm, 0, pcm.size)
            }
            dec.releaseOutputBuffer(outIdx, false)
            outIdx = dec.dequeueOutputBuffer(info, 0)
        }
    }
}
