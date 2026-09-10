package easylink.yvak.inc.audio

import android.content.Context
import android.media.MediaPlayer
import android.media.MediaRecorder
import android.os.Build
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import java.io.File

/**
 * Запись голосовых: AMR-NB 8 кГц ≤8 с (лимит эфира PKT25_VOICE_MAX = 6144 Б).
 */
class VoiceRecorder(private val context: Context) {
    companion object {
        const val MAX_DURATION_MS = 8000
        const val MAX_BYTES = 6144
    }

    private var recorder: MediaRecorder? = null
    private var outFile: File? = null

    private val _recording = MutableStateFlow(false)
    val recording: StateFlow<Boolean> get() = _recording

    private fun voiceDir(): File =
        File(context.filesDir, "voice").apply { mkdirs() }

    fun start(): Boolean {
        stopInternal()
        val f = File(voiceDir(), "out_${System.currentTimeMillis()}.amr")
        outFile = f
        return try {
            @Suppress("DEPRECATION")
            val r = if (Build.VERSION.SDK_INT >= 31) MediaRecorder(context) else MediaRecorder()
            r.setAudioSource(MediaRecorder.AudioSource.MIC)
            r.setOutputFormat(MediaRecorder.OutputFormat.AMR_NB)
            r.setAudioEncoder(MediaRecorder.AudioEncoder.AMR_NB)
            r.setAudioSamplingRate(8000)
            r.setAudioEncodingBitRate(4750)
            r.setMaxDuration(MAX_DURATION_MS)
            r.setOutputFile(f.absolutePath)
            r.setOnInfoListener { _, what, _ ->
                if (what == MediaRecorder.MEDIA_RECORDER_INFO_MAX_DURATION_REACHED) {
                    // Дозаписалось до лимита — фиксируем, отправку решает UI
                    _recording.value = false
                }
            }
            r.prepare()
            r.start()
            recorder = r
            _recording.value = true
            true
        } catch (e: Exception) {
            outFile = null
            false
        }
    }

    /** Останавливает запись; возвращает байты голосового (обрезанные под эфир) или null. */
    fun stop(): Pair<File, ByteArray>? {
        stopInternal()
        val f = outFile ?: return null
        outFile = null
        if (!f.exists() || f.length() < 32) { f.delete(); return null }
        var bytes = f.readBytes()
        if (bytes.size > MAX_BYTES) {
            bytes = Amr.truncateToFit(bytes, MAX_BYTES)
            f.writeBytes(bytes)
        }
        return f to bytes
    }

    fun cancel() {
        stopInternal()
        outFile?.delete()
        outFile = null
    }

    private fun stopInternal() {
        _recording.value = false
        try { recorder?.stop() } catch (_: Exception) {}
        try { recorder?.release() } catch (_: Exception) {}
        recorder = null
    }

    /** Сохранить входящее голосовое в файл (с AMR-заголовком). */
    fun saveIncoming(nodeId: Long, seq: Int, data: ByteArray): File {
        val f = File(voiceDir(), "in_%08x_%d_%d.amr".format(nodeId, seq, System.currentTimeMillis()))
        if (Amr.hasHeader(data)) f.writeBytes(data)
        else f.writeBytes(Amr.FILE_HEADER + data)
        return f
    }
}

/** Простой плеер голосовых с индикацией, что сейчас играет. */
class VoicePlayer {
    private var player: MediaPlayer? = null
    private val _playingPath = MutableStateFlow<String?>(null)
    val playingPath: StateFlow<String?> get() = _playingPath

    fun toggle(path: String) {
        if (_playingPath.value == path) { stop(); return }
        stop()
        try {
            val p = MediaPlayer()
            p.setDataSource(path)
            p.setOnCompletionListener { stop() }
            p.prepare()
            p.start()
            player = p
            _playingPath.value = path
        } catch (_: Exception) {
            stop()
        }
    }

    fun stop() {
        try { player?.stop(); player?.release() } catch (_: Exception) {}
        player = null
        _playingPath.value = null
    }
}
