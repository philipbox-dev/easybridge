package easylink.yvak.inc.ble

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.Handler
import android.os.Looper
import android.util.Log
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import easylink.yvak.inc.proto.ConnState
import easylink.yvak.inc.proto.NotifyAssembler
import java.util.UUID
import java.util.concurrent.ConcurrentLinkedQueue

/**
 * BLE-клиент EasyBridge: Nordic UART Service.
 * Пишем JSON-команды в RX-характеристику, получаем события нотификациями
 * из TX-характеристики (чанки склеивает NotifyAssembler).
 */
@SuppressLint("MissingPermission")
class BleClient(private val context: Context) {

    companion object {
        private const val TAG = "BleClient"
        val SVC_UUID: UUID = UUID.fromString("6e400001-b5a3-f393-e0a9-e50e24dcca9e")
        val RX_UUID: UUID = UUID.fromString("6e400002-b5a3-f393-e0a9-e50e24dcca9e")  // write
        val TX_UUID: UUID = UUID.fromString("6e400003-b5a3-f393-e0a9-e50e24dcca9e")  // notify
        val CCCD_UUID: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
        const val DEVICE_NAME_PREFIX = "EasyBridge"
    }

    /** V1.7: быстрый реконнект с бэкоффом 0.8с → 5с вместо фикс. 3с. */
    @Volatile private var reconnectAttempt = 0
    private fun reconnectDelayMs(): Long =
        (800L * (reconnectAttempt + 1)).coerceAtMost(5000L)

    private val handler = Handler(Looper.getMainLooper())
    private val adapter: BluetoothAdapter? =
        (context.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager)?.adapter

    private var gatt: BluetoothGatt? = null
    private var rxChar: BluetoothGattCharacteristic? = null
    private var txChar: BluetoothGattCharacteristic? = null

    /** Хотим ли держать соединение (для автореконнекта). */
    @Volatile private var desiredAddr: String? = null
    @Volatile private var mtu = 23

    private val _connState = MutableStateFlow(ConnState.DISCONNECTED)
    val connState: StateFlow<ConnState> get() = _connState

    private val _deviceName = MutableStateFlow("")
    val deviceName: StateFlow<String> get() = _deviceName

    /** Целые JSON-события от устройства. */
    private val _events = MutableSharedFlow<String>(extraBufferCapacity = 256)
    val events: SharedFlow<String> get() = _events

    /** Отправленные строки (для дебаг-лога). */
    private val _sentLog = MutableSharedFlow<String>(extraBufferCapacity = 256)
    val sentLog: SharedFlow<String> get() = _sentLog

    private val assembler = NotifyAssembler { json ->
        _events.tryEmit(json)
    }

    /** Рассинхроны сборщика JSON-чанков (для дебаг-панели звонка). */
    val assemblerResyncs: Int get() = assembler.resyncCount

    /** Максимум полезных байт в одной GATT-записи. */
    val maxWriteBytes: Int get() = (mtu - 3).coerceAtLeast(20)

    // ── Очередь записи (по одной GATT-операции за раз) ────────
    private val writeQueue = ConcurrentLinkedQueue<ByteArray>()
    @Volatile private var writing = false

    fun sendJson(json: String): Boolean {
        val g = gatt ?: return false
        val bytes = json.toByteArray(Charsets.UTF_8)
        if (bytes.size > maxWriteBytes) {
            Log.w(TAG, "Command too big for MTU: ${bytes.size} > $maxWriteBytes")
        }
        writeQueue.add(bytes)
        _sentLog.tryEmit(json)
        drainQueue(g)
        return true
    }

    @Synchronized
    private fun drainQueue(g: BluetoothGatt) {
        if (writing) return
        val next = writeQueue.poll() ?: return
        val ch = rxChar ?: return
        writing = true
        @Suppress("DEPRECATION")
        ch.value = next
        @Suppress("DEPRECATION")
        ch.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
        @Suppress("DEPRECATION")
        if (!g.writeCharacteristic(ch)) {
            writing = false
            handler.postDelayed({ gatt?.let { drainQueue(it) } }, 50)
        }
    }

    // ── Скан ──────────────────────────────────────────────────
    data class Found(val name: String, val addr: String, val rssi: Int, val isBridge: Boolean)

    private val _scanResults = MutableStateFlow<List<Found>>(emptyList())
    val scanResults: StateFlow<List<Found>> get() = _scanResults
    private val _scanning = MutableStateFlow(false)
    val scanning: StateFlow<Boolean> get() = _scanning

    private val scanCb = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            val name = result.scanRecord?.deviceName ?: result.device.name ?: return
            val found = Found(name, result.device.address, result.rssi,
                name.startsWith(DEVICE_NAME_PREFIX))
            val cur = _scanResults.value.toMutableList()
            val i = cur.indexOfFirst { it.addr == found.addr }
            if (i >= 0) cur[i] = found else cur.add(found)
            _scanResults.value = cur.sortedByDescending { it.rssi }
        }
    }

    fun startScan() {
        val scanner = adapter?.bluetoothLeScanner ?: return
        _scanResults.value = emptyList()
        _scanning.value = true
        try {
            scanner.startScan(null,
                ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build(),
                scanCb)
        } catch (e: Exception) {
            Log.e(TAG, "startScan failed", e)
            _scanning.value = false
        }
    }

    fun stopScan() {
        _scanning.value = false
        try { adapter?.bluetoothLeScanner?.stopScan(scanCb) } catch (_: Exception) {}
    }

    // ── Подключение ───────────────────────────────────────────
    fun connect(addr: String, name: String) {
        desiredAddr = addr
        _deviceName.value = name
        doConnect(addr)
    }

    private fun doConnect(addr: String) {
        stopScan()
        closeGatt()
        val dev: BluetoothDevice = try {
            adapter?.getRemoteDevice(addr) ?: return
        } catch (_: IllegalArgumentException) { return }
        _connState.value =
            if (_connState.value == ConnState.DISCONNECTED) ConnState.CONNECTING
            else ConnState.RECONNECTING
        assembler.reset()
        gatt = dev.connectGatt(context, false, gattCb, BluetoothDevice.TRANSPORT_LE)
    }

    fun disconnect() {
        desiredAddr = null
        handler.removeCallbacksAndMessages(null)
        closeGatt()
        _connState.value = ConnState.DISCONNECTED
    }

    /**
     * PTT-звонок гонит ~20 evt:ptt_audio/с через notify-очередь устройства
     * (глубина 8) — на дефолтном BLE-интервале соединения (BALANCED, у
     * многих телефонов 45-100мс) нотификации не успевают уходить, очередь
     * забивается за пару секунд и дальше молча дропает всё подряд.
     * HIGH сужает интервал до ~11-15мс. Звать на входе в звонок/выходе.
     */
    fun requestHighPriority(high: Boolean) {
        try {
            gatt?.requestConnectionPriority(
                if (high) BluetoothGatt.CONNECTION_PRIORITY_HIGH
                else BluetoothGatt.CONNECTION_PRIORITY_BALANCED)
        } catch (_: Exception) {}
    }

    private fun closeGatt() {
        writeQueue.clear()
        writing = false
        rxChar = null; txChar = null
        try { gatt?.disconnect(); gatt?.close() } catch (_: Exception) {}
        gatt = null
        mtu = 23
    }

    private fun scheduleReconnect() {
        val addr = desiredAddr ?: return
        _connState.value = ConnState.RECONNECTING
        val delay = reconnectDelayMs()
        reconnectAttempt++
        handler.postDelayed({
            if (desiredAddr == addr && _connState.value == ConnState.RECONNECTING) {
                doConnect(addr)
            }
        }, delay)
    }

    private val gattCb = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            Log.i(TAG, "onConnectionStateChange status=$status state=$newState")
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                handler.post { g.discoverServices() }
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                writeQueue.clear(); writing = false
                if (desiredAddr != null) {
                    try { g.close() } catch (_: Exception) {}
                    if (gatt === g) gatt = null
                    scheduleReconnect()
                } else {
                    _connState.value = ConnState.DISCONNECTED
                }
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            val svc = g.getService(SVC_UUID)
            if (svc == null) {
                Log.e(TAG, "NUS service not found")
                g.disconnect(); return
            }
            rxChar = svc.getCharacteristic(RX_UUID)
            txChar = svc.getCharacteristic(TX_UUID)
            if (rxChar == null || txChar == null) {
                Log.e(TAG, "NUS characteristics missing")
                g.disconnect(); return
            }
            g.requestMtu(256)
        }

        override fun onMtuChanged(g: BluetoothGatt, newMtu: Int, status: Int) {
            mtu = if (status == BluetoothGatt.GATT_SUCCESS) newMtu else 23
            Log.i(TAG, "MTU = $mtu")
            val tx = txChar ?: return
            g.setCharacteristicNotification(tx, true)
            val cccd = tx.getDescriptor(CCCD_UUID) ?: return
            @Suppress("DEPRECATION")
            cccd.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
            @Suppress("DEPRECATION")
            g.writeDescriptor(cccd)
        }

        override fun onDescriptorWrite(g: BluetoothGatt, d: BluetoothGattDescriptor, status: Int) {
            if (d.uuid == CCCD_UUID) {
                Log.i(TAG, "Subscribed, connection ready")
                reconnectAttempt = 0
                _connState.value = ConnState.CONNECTED
            }
        }

        override fun onCharacteristicWrite(g: BluetoothGatt, c: BluetoothGattCharacteristic,
                                           status: Int) {
            writing = false
            drainQueue(g)
        }

        @Deprecated("Deprecated in Java")
        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic) {
            @Suppress("DEPRECATION")
            val value = c.value ?: return
            if (c.uuid == TX_UUID) assembler.feed(value)
        }
    }
}
