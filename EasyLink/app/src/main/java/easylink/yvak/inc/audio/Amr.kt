package easylink.yvak.inc.audio

/** Утилиты формата AMR-NB. */
object Amr {
    val FILE_HEADER = "#!AMR\n".toByteArray(Charsets.US_ASCII)

    /** Размер кадра (включая TOC-байт) по типу кадра из TOC. */
    private val FRAME_SIZES = intArrayOf(13, 14, 16, 18, 20, 21, 27, 32, 6, 7, 6, 6, 1, 1, 1, 1)

    fun frameSize(tocByte: Byte): Int {
        val type = (tocByte.toInt() shr 3) and 0x0F
        return FRAME_SIZES[type]
    }

    fun hasHeader(data: ByteArray): Boolean =
        data.size >= FILE_HEADER.size &&
            FILE_HEADER.indices.all { data[it] == FILE_HEADER[it] }

    /**
     * Обрезает AMR-запись (с заголовком) до maxBytes по границе кадра.
     * Голосовое в эфире ограничено PKT25_VOICE_MAX = 6144 байта.
     */
    fun truncateToFit(data: ByteArray, maxBytes: Int): ByteArray {
        if (data.size <= maxBytes) return data
        var pos = if (hasHeader(data)) FILE_HEADER.size else 0
        var lastGood = pos
        while (pos < data.size) {
            val sz = frameSize(data[pos])
            if (pos + sz > maxBytes) break
            pos += sz
            lastGood = pos
        }
        return data.copyOfRange(0, lastGood)
    }

    /** Разбивает поток сырых AMR-кадров на отдельные кадры (для декодера). */
    fun splitFrames(data: ByteArray): List<ByteArray> {
        val out = ArrayList<ByteArray>()
        var pos = if (hasHeader(data)) FILE_HEADER.size else 0
        while (pos < data.size) {
            val sz = frameSize(data[pos])
            if (pos + sz > data.size) break
            out.add(data.copyOfRange(pos, pos + sz))
            pos += sz
        }
        return out
    }
}
