package easylink.yvak.inc.repo

import android.content.Context
import android.util.Log
import com.huawei.wearengine.HiWear
import com.huawei.wearengine.auth.AuthCallback
import com.huawei.wearengine.auth.Permission
import com.huawei.wearengine.device.Device
import com.huawei.wearengine.p2p.Message
import com.huawei.wearengine.p2p.Receiver
import com.huawei.wearengine.p2p.SendCallback
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import org.json.JSONObject

/**
 * Связка с часами Huawei (Wear Engine).
 *
 * Протокол (JSON поверх P2P):
 *  телефон → часы: {"t":"msg","from","text"} | {"t":"sos","from"} | {"t":"st","conn","nodes"}
 *  часы → телефон: {"t":"send","text"} | {"t":"sos"} | {"t":"get"}
 *
 * Работает только после одобрения заявки Wear Engine для отпечатка подписи
 * этого APK и при установленном Huawei Health.
 */
class WearRepo(private val context: Context) {

    companion object {
        private const val TAG = "WearRepo"
        const val WATCH_PKG = "inc.yvak.easylink.watch"
        // Для Lite Wearable в качестве peer fingerprint используется значение
        // из сертификата watch-приложения; до выпуска сертификата — bundleName.
        // После выпуска подписи часов проверь значение в AppGallery Connect.
        const val WATCH_FINGERPRINT = "inc.yvak.easylink.watch"
    }

    sealed class State {
        data object Off : State()
        data object Connecting : State()
        data class Ready(val deviceName: String) : State()
        data class Error(val reason: String) : State()
    }

    private val _state = MutableStateFlow<State>(State.Off)
    val state: StateFlow<State> get() = _state

    /** Команда с часов: t=send/sos/get. */
    var onCommand: ((JSONObject) -> Unit)? = null

    private var device: Device? = null
    private val p2p by lazy {
        HiWear.getP2pClient(context).apply {
            setPeerPkgName(WATCH_PKG)
            setPeerFingerPrint(WATCH_FINGERPRINT)
        }
    }

    private val receiver = Receiver { message ->
        val text = try { String(message.data, Charsets.UTF_8) } catch (_: Exception) { return@Receiver }
        Log.i(TAG, "From watch: $text")
        val obj = try { JSONObject(text) } catch (_: Exception) { return@Receiver }
        onCommand?.invoke(obj)
    }

    fun start() {
        if (_state.value is State.Ready || _state.value is State.Connecting) return
        _state.value = State.Connecting
        try {
            val authClient = HiWear.getAuthClient(context)
            authClient.requestPermission(object : AuthCallback {
                override fun onOk(permissions: Array<out Permission>?) {
                    findDevice()
                }

                override fun onCancel() {
                    _state.value = State.Error("auth cancelled")
                }
            }, Permission.DEVICE_MANAGER)
                .addOnFailureListener { e ->
                    _state.value = State.Error(e.message ?: "auth failed")
                }
        } catch (e: Exception) {
            // Huawei Health не установлен / сервис недоступен
            _state.value = State.Error(e.message ?: "Wear Engine unavailable")
        }
    }

    private fun findDevice() {
        try {
            HiWear.getDeviceClient(context).bondedDevices
                .addOnSuccessListener { devices ->
                    val d = devices?.firstOrNull { it.isConnected }
                        ?: devices?.firstOrNull()
                    if (d == null) {
                        _state.value = State.Error("no bonded watch")
                        return@addOnSuccessListener
                    }
                    device = d
                    registerReceiver(d)
                }
                .addOnFailureListener { e ->
                    _state.value = State.Error(e.message ?: "device query failed")
                }
        } catch (e: Exception) {
            _state.value = State.Error(e.message ?: "device client failed")
        }
    }

    private fun registerReceiver(d: Device) {
        try {
            p2p.registerReceiver(d, receiver)
                .addOnSuccessListener {
                    _state.value = State.Ready(d.name ?: "watch")
                    Log.i(TAG, "Watch link ready: ${d.name}")
                }
                .addOnFailureListener { e ->
                    _state.value = State.Error(e.message ?: "receiver failed")
                }
        } catch (e: Exception) {
            _state.value = State.Error(e.message ?: "register failed")
        }
    }

    fun stop() {
        try { p2p.unregisterReceiver(receiver) } catch (_: Exception) {}
        device = null
        _state.value = State.Off
    }

    // ── Отправка на часы ──────────────────────────────────────
    private fun send(obj: JSONObject) {
        val d = device ?: return
        if (_state.value !is State.Ready) return
        try {
            val message = Message.Builder()
                .setPayload(obj.toString().toByteArray(Charsets.UTF_8))
                .build()
            p2p.send(d, message, object : SendCallback {
                override fun onSendResult(code: Int) {
                    if (code != 207) Log.w(TAG, "send result=$code")
                }
                override fun onSendProgress(progress: Long) {}
            })
        } catch (e: Exception) {
            Log.w(TAG, "send failed", e)
        }
    }

    fun pushMessage(from: String, text: String) {
        send(JSONObject().put("t", "msg")
            .put("from", from.take(24))
            .put("text", text.take(200)))
    }

    fun pushSos(from: String) {
        send(JSONObject().put("t", "sos").put("from", from.take(24)))
    }

    fun pushStatus(connected: Boolean, nodesOnline: Int) {
        send(JSONObject().put("t", "st")
            .put("conn", if (connected) 1 else 0)
            .put("nodes", nodesOnline))
    }
}
