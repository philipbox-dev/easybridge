package easylink.yvak.inc.repo

import easylink.yvak.inc.proto.LogDir
import easylink.yvak.inc.proto.LogEntry
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

/** Кольцевой лог всего BLE-обмена + статистика + пинг + файл сессии. */
class DebugRepo(context: android.content.Context? = null) {
    companion object {
        private const val MAX = 2000
        private const val MAX_FILE_BYTES = 2_000_000L
        private const val KEEP_FILES = 3
    }

    // ── Файл сессии: пишем ВСЁ всегда (кольцо в памяти — только для UI).
    // После похода файл можно скинуть и разобрать, что было в поле.
    private val logDir: java.io.File? = context?.let {
        java.io.File(it.filesDir, "logs").apply { mkdirs() }
    }
    private val logFile: java.io.File? = logDir?.let { dir ->
        // ротация: сносим старые, оставляем KEEP_FILES-1 + новый
        dir.listFiles()?.sortedByDescending { it.name }?.drop(KEEP_FILES - 1)
            ?.forEach { it.delete() }
        java.io.File(dir, "session_" + android.text.format.DateFormat
            .format("yyyyMMdd_HHmmss", System.currentTimeMillis()) + ".log")
    }
    private val fileExec = java.util.concurrent.Executors.newSingleThreadExecutor()

    fun currentLogFile(): java.io.File? = logFile

    private fun writeToFile(dir: LogDir, text: String) {
        val f = logFile ?: return
        fileExec.execute {
            try {
                if (f.length() < MAX_FILE_BYTES) {
                    val t = android.text.format.DateFormat.format(
                        "HH:mm:ss", System.currentTimeMillis())
                    f.appendText("$t $dir $text\n")
                }
            } catch (_: Exception) {}
        }
    }

    private val _log = MutableStateFlow<List<LogEntry>>(emptyList())
    val log: StateFlow<List<LogEntry>> get() = _log

    val paused = MutableStateFlow(false)

    private val _txCount = MutableStateFlow(0)
    val txCount: StateFlow<Int> get() = _txCount
    private val _rxCount = MutableStateFlow(0)
    val rxCount: StateFlow<Int> get() = _rxCount
    private val _errCount = MutableStateFlow(0)
    val errCount: StateFlow<Int> get() = _errCount

    /** Время отправки ble_test для замера задержки. */
    @Volatile var pingSentAt: Long = 0
    private val _lastPingMs = MutableStateFlow(-1L)
    val lastPingMs: StateFlow<Long> get() = _lastPingMs

    fun add(dir: LogDir, text: String) {
        when (dir) {
            LogDir.TX -> _txCount.value++
            LogDir.RX -> _rxCount.value++
            LogDir.ERR -> _errCount.value++
            else -> {}
        }
        writeToFile(dir, text)   // файл пишем всегда, пауза — только про UI
        if (paused.value && dir != LogDir.ERR) return
        val e = LogEntry(System.currentTimeMillis(), dir, text)
        val cur = _log.value
        _log.value = if (cur.size >= MAX) cur.drop(cur.size - MAX + 1) + e else cur + e
    }

    fun onPong() {
        if (pingSentAt > 0) {
            _lastPingMs.value = System.currentTimeMillis() - pingSentAt
            pingSentAt = 0
        }
    }

    fun clear() { _log.value = emptyList() }

    fun exportText(): String = _log.value.joinToString("\n") { e ->
        val t = android.text.format.DateFormat.format("HH:mm:ss", e.ts)
        "$t ${e.dir} ${e.text}"
    }
}
