package easylink.yvak.inc.repo

import android.graphics.Bitmap
import android.graphics.Color
import com.google.zxing.BarcodeFormat
import com.google.zxing.qrcode.QRCodeWriter
import org.json.JSONObject

/**
 * #Ф3 QR-обмен: контакт (node_id + имя) и группа (id + имя + ключ).
 * Payload — компактный JSON с маркером "el".
 */
object QrCodec {

    sealed class Payload {
        data class Contact(val nodeId: Long, val name: String) : Payload()
        data class Group(val groupId: Long, val name: String, val key: String) : Payload()
    }

    fun encodeContact(nodeId: Long, name: String): String = JSONObject()
        .put("el", 1).put("t", "c")
        .put("id", "0x%08x".format(nodeId)).put("n", name).toString()

    fun encodeGroup(groupId: Long, name: String, key: String): String = JSONObject()
        .put("el", 1).put("t", "g")
        .put("gid", "0x%08x".format(groupId)).put("n", name)
        .apply { if (key.isNotEmpty()) put("k", key) }.toString()

    fun parse(text: String): Payload? {
        val o = try { JSONObject(text) } catch (_: Exception) { return null }
        if (o.optInt("el") != 1) return null
        fun hex(key: String): Long = o.optString(key).removePrefix("0x")
            .toLongOrNull(16) ?: 0L
        return when (o.optString("t")) {
            "c" -> {
                val id = hex("id")
                if (id == 0L) null else Payload.Contact(id, o.optString("n"))
            }
            "g" -> {
                val gid = hex("gid")
                if (gid == 0L) null
                else Payload.Group(gid, o.optString("n"), o.optString("k"))
            }
            else -> null
        }
    }

    /** QR → Bitmap (чёрное на белом, с полем). */
    fun toBitmap(text: String, size: Int = 640): Bitmap? = try {
        val m = QRCodeWriter().encode(text, BarcodeFormat.QR_CODE, size, size)
        val bmp = Bitmap.createBitmap(size, size, Bitmap.Config.RGB_565)
        for (x in 0 until size) for (y in 0 until size) {
            bmp.setPixel(x, y, if (m[x, y]) Color.BLACK else Color.WHITE)
        }
        bmp
    } catch (_: Exception) { null }
}
