package easylink.yvak.inc.repo

import easylink.yvak.inc.ble.BleClient
import easylink.yvak.inc.proto.DiagInfo
import easylink.yvak.inc.proto.Proto
import easylink.yvak.inc.proto.RadioInfo
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow

/** Типизированные всплывашки для UI (локализуются на месте показа). */
sealed class UiNote {
    data class SosSent(val count: Int) : UiNote()
    data class SpeedSwitch(val fast: Boolean, val inSec: Int) : UiNote()
    data class SpeedApplied(val fast: Boolean) : UiNote()
    data class Psk(val enabled: Boolean) : UiNote()
    data object NameSaved : UiNote()
    data class DeviceError(val desc: String) : UiNote()
    data class Ping(val ms: Long) : UiNote()
    data object LocationSent : UiNote()
    data class GroupInvited(val nodeName: String) : UiNote()
    data class GroupJoined(val name: String) : UiNote()
}

/** Состояние устройства: диагностика, радио, скорость. */
class DeviceRepo(private val ble: BleClient) {

    private val _diag = MutableStateFlow(DiagInfo())
    val diag: StateFlow<DiagInfo> get() = _diag

    private val _radio = MutableStateFlow(RadioInfo())
    val radio: StateFlow<RadioInfo> get() = _radio

    private val _notes = MutableSharedFlow<UiNote>(extraBufferCapacity = 16)
    val notes: SharedFlow<UiNote> get() = _notes

    fun note(n: UiNote) { _notes.tryEmit(n) }

    fun onDiag(d: DiagInfo) { _diag.value = d }
    fun onRadio(r: RadioInfo) { _radio.value = r }
    fun onHw(loraOk: Boolean, rssi: Int, sf: Int, nodes: Int) {
        _diag.value = _diag.value.copy(loraOk = loraOk, rssi = rssi, sf = sf,
            nodesOnline = nodes)
    }
    fun onSpeed(sf: Int, applied: Boolean) {
        if (applied) _diag.value = _diag.value.copy(sf = sf)
    }

    fun refresh() {
        ble.sendJson(Proto.diag())
        ble.sendJson(Proto.getRadio())
    }

    fun setTxPower(idx: Int) = ble.sendJson(Proto.setTxPower(idx))
    fun setAntenna(type: String) = ble.sendJson(Proto.setAntenna(type))
    fun setSpeed(fast: Boolean) = ble.sendJson(Proto.speed(fast))
    fun setPsk(pass: String) = ble.sendJson(Proto.setPsk(pass))
    fun sos() = ble.sendJson(Proto.sos())
    fun setName(name: String) = ble.sendJson(Proto.setName(name))
}
