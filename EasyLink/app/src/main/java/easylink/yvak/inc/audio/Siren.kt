package easylink.yvak.inc.audio

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioTrack
import android.os.VibrationEffect
import android.os.Vibrator
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlin.math.PI
import kotlin.math.sin

/**
 * Сирена SOS: синтезированный вой 600↔1200 Гц (USAGE_ALARM, пробивает
 * беззвучный режим медиа) + вибрация.
 */
class Siren(private val context: Context) {
    private var track: AudioTrack? = null
    private var thread: Thread? = null
    @Volatile private var running = false

    private val _active = MutableStateFlow(false)
    val active: StateFlow<Boolean> get() = _active

    fun start(volume: Float, vibrate: Boolean) {
        if (running) return
        running = true
        _active.value = true

        if (vibrate) {
            val vib = context.getSystemService(Context.VIBRATOR_SERVICE) as? Vibrator
            vib?.vibrate(
                VibrationEffect.createWaveform(longArrayOf(0, 600, 250), 0))
        }

        thread = Thread {
            val sr = 22050
            val min = AudioTrack.getMinBufferSize(sr, AudioFormat.CHANNEL_OUT_MONO,
                AudioFormat.ENCODING_PCM_16BIT)
            val t = AudioTrack(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_ALARM)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                    .build(),
                AudioFormat.Builder().setSampleRate(sr)
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .setChannelMask(AudioFormat.CHANNEL_OUT_MONO).build(),
                maxOf(min, 8192), AudioTrack.MODE_STREAM,
                AudioManager.AUDIO_SESSION_ID_GENERATE)
            t.setVolume(volume.coerceIn(0.05f, 1f))
            t.play()
            track = t

            val buf = ShortArray(1024)
            var phase = 0.0
            var time = 0.0
            while (running) {
                for (i in buf.indices) {
                    // Частота качается 600..1200 Гц с периодом ~1 с
                    val f = 900.0 + 300.0 * sin(2.0 * PI * time)
                    phase += 2.0 * PI * f / sr
                    time += 1.0 / sr
                    buf[i] = (sin(phase) * 32000).toInt().toShort()
                }
                try { t.write(buf, 0, buf.size) } catch (_: Exception) { break }
            }
            try { t.stop(); t.release() } catch (_: Exception) {}
        }.apply { start() }
    }

    fun stop() {
        running = false
        _active.value = false
        (context.getSystemService(Context.VIBRATOR_SERVICE) as? Vibrator)?.cancel()
        thread = null
        track = null
    }
}
