package easylink.yvak.inc.audio

import android.content.Context
import android.media.AudioAttributes
import android.media.MediaPlayer
import android.media.RingtoneManager
import android.net.Uri

/** Мелодия входящего звонка: выбранный файл или системный рингтон, в цикле. */
class RingtonePlayer(private val context: Context) {
    private var player: MediaPlayer? = null

    fun start(uriStr: String) {
        stop()
        val uri: Uri = if (uriStr.isNotEmpty()) Uri.parse(uriStr)
        else RingtoneManager.getDefaultUri(RingtoneManager.TYPE_RINGTONE) ?: return
        try {
            val p = MediaPlayer()
            p.setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_NOTIFICATION_RINGTONE)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                    .build())
            p.setDataSource(context, uri)
            p.isLooping = true
            p.prepare()
            p.start()
            player = p
        } catch (_: Exception) {
            stop()
        }
    }

    fun stop() {
        try { player?.stop(); player?.release() } catch (_: Exception) {}
        player = null
    }
}
