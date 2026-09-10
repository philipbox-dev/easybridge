package easylink.yvak.inc.repo

import android.content.Context
import android.util.Base64
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import org.json.JSONObject
import java.security.MessageDigest
import javax.crypto.Cipher
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

/**
 * Шифрование сообщений по чату/группе на прикладном уровне (поверх send).
 * У каждого чата свой пароль → AES-256-GCM. Это НЕЗАВИСИМО от эфирного PSK
 * прошивки: PSK прячет весь эфир одним ключом на всю сеть, а тут — разные
 * ключи для разных групп, и работает даже без PSK.
 *
 * Формат в эфире: "E1:" + base64(iv[12] + ciphertext+tag).
 */
class CryptoRepo(context: Context) {
    companion object {
        const val MARK = "E1:"
        private const val PREFS = "easylink_keys"
    }

    private val sp = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
    // chatKey -> passphrase
    private val _keys = MutableStateFlow(load())
    val keys: StateFlow<Map<String, String>> get() = _keys

    private fun load(): Map<String, String> {
        val out = HashMap<String, String>()
        val raw = sp.getString("map", "{}") ?: "{}"
        try {
            val o = JSONObject(raw)
            o.keys().forEach { out[it] = o.getString(it) }
        } catch (_: Exception) {}
        return out
    }

    private fun persist(map: Map<String, String>) {
        val o = JSONObject()
        map.forEach { (k, v) -> o.put(k, v) }
        sp.edit().putString("map", o.toString()).apply()
        _keys.value = map
    }

    fun hasKey(chatKey: String): Boolean = _keys.value.containsKey(chatKey)

    fun setKey(chatKey: String, passphrase: String) {
        val m = _keys.value.toMutableMap()
        if (passphrase.isBlank()) m.remove(chatKey) else m[chatKey] = passphrase
        persist(m)
    }

    private fun aesKey(passphrase: String): SecretKeySpec {
        val d = MessageDigest.getInstance("SHA-256").digest(passphrase.toByteArray())
        return SecretKeySpec(d, "AES")
    }

    /** Зашифровать, если у чата есть ключ; иначе вернуть текст как есть. */
    fun encryptForChat(chatKey: String, text: String): String {
        val pass = _keys.value[chatKey] ?: return text
        return try {
            val cipher = Cipher.getInstance("AES/GCM/NoPadding")
            val iv = ByteArray(12).also { java.security.SecureRandom().nextBytes(it) }
            cipher.init(Cipher.ENCRYPT_MODE, aesKey(pass), GCMParameterSpec(128, iv))
            val ct = cipher.doFinal(text.toByteArray(Charsets.UTF_8))
            MARK + Base64.encodeToString(iv + ct, Base64.NO_WRAP)
        } catch (_: Exception) { text }
    }

    /** true если текст выглядит зашифрованным. */
    fun isEncrypted(text: String): Boolean = text.startsWith(MARK)

    /**
     * Расшифровать входящее. Возвращает открытый текст, либо null если это
     * шифртекст, но ключа для чата нет / не подошёл.
     */
    fun decryptForChat(chatKey: String, text: String): String? {
        if (!isEncrypted(text)) return text
        val pass = _keys.value[chatKey] ?: return null
        return try {
            val raw = Base64.decode(text.removePrefix(MARK), Base64.NO_WRAP)
            val iv = raw.copyOfRange(0, 12)
            val ct = raw.copyOfRange(12, raw.size)
            val cipher = Cipher.getInstance("AES/GCM/NoPadding")
            cipher.init(Cipher.DECRYPT_MODE, aesKey(pass), GCMParameterSpec(128, iv))
            String(cipher.doFinal(ct), Charsets.UTF_8)
        } catch (_: Exception) { null }
    }
}
