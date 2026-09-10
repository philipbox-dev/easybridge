// Compiled only for SX127x boards (LilyGO T3 etc.)
#include "sx1278.h"
#include "hwcfg.h"
#include "spi_bus.h"
#include "config.h"
#include "config.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

extern uint32_t esp_random(void);

// Пины и параметры шины — из hwcfg() (V2.9).
#define RP            (hwcfg()->radio)
#define PIN_LORA_SCK  (RP.pins.sck)
#define PIN_LORA_MISO (RP.pins.miso)
#define PIN_LORA_MOSI (RP.pins.mosi)
#define PIN_LORA_CS   (RP.pins.cs)
#define PIN_LORA_RST  (RP.pins.rst)
#define PIN_LORA_DIO0 (RP.pins.dio0)

static const char *TAG = "SX1278";
static spi_device_handle_t spi_handle;

// V2.6.1: SPI-транзакции идут из rx_task, tx_task и voice_tx_task —
// без мьютекса ловим assert в spi_device_transmit (гонка на шине).
// Лочим на уровне операций (не отдельных регистров), чтобы TX-заполнение
// FIFO не перемешивалось с RX-чтением указателей.
#include "freertos/semphr.h"
static SemaphoreHandle_t sx_mutex = NULL;
#define SX_LOCK()   do { if (sx_mutex) xSemaphoreTakeRecursive(sx_mutex, portMAX_DELAY); } while (0)
#define SX_UNLOCK() do { if (sx_mutex) xSemaphoreGiveRecursive(sx_mutex); } while (0)

// ============================================================
// Низкий уровень SPI
// ============================================================
uint8_t sx1278_read_reg(uint8_t addr)
{
    uint8_t tx[2] = { addr & 0x7F, 0x00 };
    uint8_t rx[2] = { 0x00, 0x00 };
    spi_transaction_t t = {
        .length    = 16,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    spi_device_transmit(spi_handle, &t);
    return rx[1];
}

static void write_reg(uint8_t addr, uint8_t value)
{
    uint8_t tx[2] = { addr | 0x80, value };
    spi_transaction_t t = {
        .length    = 16,
        .tx_buffer = tx,
        .rx_buffer = NULL,
    };
    spi_device_transmit(spi_handle, &t);
}

static void set_mode(uint8_t mode)
{
    write_reg(REG_OP_MODE, MODE_LONG_RANGE | mode);
}

// ============================================================
// Публичный API
// ============================================================
void sx1278_reset(void)
{
    gpio_set_level(PIN_LORA_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_LORA_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
}

bool sx1278_is_alive(void)
{
    uint8_t v = sx1278_read_reg(REG_VERSION);
    ESP_LOGI(TAG, "SX1278 version: 0x%02X (expect 0x12)", v);
    return (v == 0x12);
}

esp_err_t sx1278_init(void)
{
    if (!sx_mutex) sx_mutex = xSemaphoreCreateRecursiveMutex();

    // RST пин
    gpio_config_t rst = {
        .pin_bit_mask = (1ULL << PIN_LORA_RST),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&rst);

    // DIO0 пин — прерывание
    gpio_config_t dio0 = {
        .pin_bit_mask = (1ULL << PIN_LORA_DIO0),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_POSEDGE,
    };
    gpio_config(&dio0);

    // SPI шина. Через spi_bus_acquire(), а не напрямую: на том же
    // хосте может уже сидеть дисплей или тач (V2.9).
    spi_host_device_t host;
    esp_err_t ret = spi_bus_acquire(RP.spi_host, PIN_LORA_SCK, PIN_LORA_MOSI,
                                    PIN_LORA_MISO, 256, &host);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // SPI устройство
    spi_device_interface_config_t dev = {
        .clock_speed_hz = RP.spi_hz ? (int)RP.spi_hz : 1000000,
        .mode           = 0,
        .spics_io_num   = PIN_LORA_CS,
        .queue_size     = 1,
    };
    ret = spi_bus_add_device(host, &dev, &spi_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI add device failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Сброс
    sx1278_reset();

    // Проверка
    if (!sx1278_is_alive()) {
        ESP_LOGE(TAG, "SX1278 не найден!");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "SX1278 OK");
    return ESP_OK;
}

static esp_err_t sx1278_configure_unlocked(uint32_t freq, int sf, int bw, int cr, int tx_power)
{
    set_mode(MODE_SLEEP);
    vTaskDelay(pdMS_TO_TICKS(10));

    // Частота
    uint64_t frf = ((uint64_t)freq << 19) / 32000000;
    write_reg(REG_FRF_MSB, (uint8_t)(frf >> 16));
    write_reg(REG_FRF_MID, (uint8_t)(frf >> 8));
    write_reg(REG_FRF_LSB, (uint8_t)(frf));

    // TX Power
    if (tx_power > 17) {
        write_reg(REG_PA_DAC,    0x87);
        write_reg(REG_PA_CONFIG, 0x80 | (tx_power - 5));
    } else {
        write_reg(REG_PA_DAC,    0x84);
        write_reg(REG_PA_CONFIG, 0x80 | (tx_power - 2));
    }

    // LNA макс усиление
    write_reg(REG_LNA, 0x23);

    // FIFO base
    write_reg(REG_FIFO_TX_BASE_ADDR, 0x00);
    write_reg(REG_FIFO_RX_BASE_ADDR, 0x00);

    // Modem Config 1: BW + CR + explicit header
    write_reg(REG_MODEM_CONFIG_1, (bw << 4) | ((cr - 4) << 1) | 0x00);

    // Modem Config 2: SF + CRC on
    write_reg(REG_MODEM_CONFIG_2, (sf << 4) | 0x04);

    // Modem Config 3: LowDataRateOptimize для SF11/SF12 + BW125
    if (sf >= 11 && bw == 7) {
        write_reg(REG_MODEM_CONFIG_3, 0x08);
    } else {
        write_reg(REG_MODEM_CONFIG_3, 0x00);
    }

    // Detection
    if (sf == 12) {
        write_reg(REG_DETECTION_OPTIMIZE,  0xC3);
        write_reg(REG_DETECTION_THRESHOLD, 0x0A);
    } else {
        write_reg(REG_DETECTION_OPTIMIZE,  0xC5);
        write_reg(REG_DETECTION_THRESHOLD, 0x0C);
    }

    // Преамбула и sync word
    write_reg(REG_PREAMBLE_MSB, 0x00);
    write_reg(REG_PREAMBLE_LSB, LORA_PREAMBLE_LEN);
    write_reg(REG_SYNC_WORD,    LORA_SYNC_WORD);

    // DIO0 = RxDone
    write_reg(REG_DIO_MAPPING_1, 0x00);

    set_mode(MODE_STDBY);
    ESP_LOGI(TAG, "Configured: %luHz SF%d BW%d CR%d PWR%d",
             freq, sf, bw, cr, tx_power);
    return ESP_OK;
}

// ============================================================
// ★ LBT: Listen Before Talk
// ============================================================

// Получить текущий RSSI (не пакета, а прямо сейчас в эфире)
int sx1278_get_current_rssi(void)
{
    // Нужно быть в режиме RX для измерения
    uint8_t rssi_raw = sx1278_read_reg(REG_RSSI_VALUE);
    return -157 + rssi_raw;
}

// Проверка: канал свободен?
static bool sx1278_is_channel_free_unlocked(int threshold_dbm)
{
    // Переключаемся в RX для измерения RSSI
    uint8_t prev_mode = sx1278_read_reg(REG_OP_MODE);

    set_mode(MODE_RX_CONT);
    vTaskDelay(pdMS_TO_TICKS(5));  // Дать время на измерение

    int rssi = sx1278_get_current_rssi();

    // Возвращаем предыдущий режим
    write_reg(REG_OP_MODE, prev_mode);

    ESP_LOGD(TAG, "Channel RSSI: %d dBm (threshold: %d)", rssi, threshold_dbm);

    return (rssi < threshold_dbm);
}

// CAD — Channel Activity Detection (точнее чем RSSI)
static bool sx1278_cad_detect_unlocked(void)
{
    // CAD — специальный режим для детекции LoRa преамбулы
    set_mode(MODE_STDBY);
    vTaskDelay(pdMS_TO_TICKS(1));

    // Очищаем флаги
    write_reg(REG_IRQ_FLAGS, 0xFF);

    // DIO0 = CAD Done
    write_reg(REG_DIO_MAPPING_1, 0x80);

    // Запускаем CAD
    set_mode(MODE_CAD);

    // Ждём завершения CAD (обычно ~1-2 символа)
    int timeout = 50;  // 50 * 2ms = 100ms max
    while (timeout-- > 0) {
        uint8_t flags = sx1278_read_reg(REG_IRQ_FLAGS);
        if (flags & IRQ_CAD_DONE) {
            bool detected = (flags & IRQ_CAD_DETECTED) != 0;
            write_reg(REG_IRQ_FLAGS, 0xFF);  // Очистить флаги

            if (detected) {
                ESP_LOGD(TAG, "CAD: LoRa signal detected!");
            }
            return detected;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    ESP_LOGW(TAG, "CAD timeout");
    write_reg(REG_IRQ_FLAGS, 0xFF);
    return false;  // Таймаут = считаем канал свободным
}

// ============================================================
// Отправка с LBT
// ============================================================
static esp_err_t sx1278_send_unlocked(const uint8_t *data, size_t len)
{
    if (len > 255) return ESP_ERR_INVALID_SIZE;

    // ★ Убираем LBT — он создаёт проблемы, просто отправляем
    set_mode(MODE_STDBY);
    vTaskDelay(pdMS_TO_TICKS(5));

    // DIO0 = TxDone
    write_reg(REG_DIO_MAPPING_1, 0x40);
    write_reg(REG_FIFO_ADDR_PTR, 0x00);
    write_reg(REG_PAYLOAD_LENGTH, len);

    for (size_t i = 0; i < len; i++) {
        write_reg(REG_FIFO, data[i]);
    }

    write_reg(REG_IRQ_FLAGS, 0xFF);  // Очистить флаги перед TX
    set_mode(MODE_TX);

    // Таймаут зависит от длины и SF
    int timeout_ms = 5000 + (len * 100);  // SF12: ~80-100ms на байт
    int timeout_ticks = timeout_ms / 10;

    ESP_LOGI(TAG, "TX start: %d bytes, timeout %d ms", len, timeout_ms);

    while (timeout_ticks-- > 0) {
        uint8_t flags = sx1278_read_reg(REG_IRQ_FLAGS);
        if (flags & IRQ_TX_DONE) {
            write_reg(REG_IRQ_FLAGS, IRQ_TX_DONE);
            ESP_LOGI(TAG, "TX done: %d bytes", len);
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGE(TAG, "TX timeout! (%d bytes, %d ms)", len, timeout_ms);
    set_mode(MODE_STDBY);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t sx1278_start_receive_unlocked(void)
{
    write_reg(REG_DIO_MAPPING_1, 0x00);  // DIO0 = RxDone
    write_reg(REG_IRQ_FLAGS,     0xFF);  // Сброс флагов
    write_reg(REG_FIFO_ADDR_PTR, 0x00);
    set_mode(MODE_RX_CONT);
    return ESP_OK;
}

static int sx1278_read_packet_unlocked(uint8_t *buf, size_t buf_size)
{
    uint8_t flags = sx1278_read_reg(REG_IRQ_FLAGS);

    if (flags & IRQ_PAYLOAD_CRC_ERROR) {
        ESP_LOGW(TAG, "CRC error");
        write_reg(REG_IRQ_FLAGS, 0xFF);
        return -1;
    }
    if (!(flags & IRQ_RX_DONE)) {
        return 0;
    }

    uint8_t len  = sx1278_read_reg(REG_RX_NB_BYTES);
    uint8_t addr = sx1278_read_reg(REG_FIFO_RX_CURRENT_ADDR);

    if (len > buf_size) len = buf_size;

    write_reg(REG_FIFO_ADDR_PTR, addr);
    for (int i = 0; i < len; i++) {
        buf[i] = sx1278_read_reg(REG_FIFO);
    }

    write_reg(REG_IRQ_FLAGS, IRQ_RX_DONE);
    ESP_LOGI(TAG, "RX: %d bytes", len);
    return len;
}

static int sx1278_get_rssi_unlocked(void)
{
    return (int)sx1278_read_reg(REG_PKT_RSSI_VALUE) - 157;
}

static float sx1278_get_snr_unlocked(void)
{
    return (int8_t)sx1278_read_reg(REG_PKT_SNR_VALUE) * 0.25f;
}


// ============================================================
// ★ FSK-режим для PTT-звонков (V2.7)
// GFSK 19.2 кбит/с, fdev 25 кГц, RxBw 83.3 кГц, sync EB 25 F5,
// variable length + CRC + whitening. FIFO 64 байта → пакет ≤60.
// ============================================================
#define REG_FSK_BITRATE_MSB   0x02
#define REG_FSK_BITRATE_LSB   0x03
#define REG_FSK_FDEV_MSB      0x04
#define REG_FSK_FDEV_LSB      0x05
#define REG_FSK_RX_BW         0x12
#define REG_FSK_AFC_BW        0x13
#define REG_FSK_RSSI_VALUE    0x11
#define REG_FSK_PREAMBLE_MSB  0x25
#define REG_FSK_PREAMBLE_LSB  0x26
#define REG_FSK_SYNC_CONFIG   0x27
#define REG_FSK_SYNC_VALUE1   0x28
#define REG_FSK_PKT_CONFIG1   0x30
#define REG_FSK_PKT_CONFIG2   0x31
#define REG_FSK_PAYLOAD_LEN   0x32
#define REG_FSK_FIFO_THRESH   0x35
#define REG_FSK_IRQ_FLAGS1    0x3E
#define REG_FSK_IRQ_FLAGS2    0x3F

#define FSK_IRQ2_PACKET_SENT   0x08
#define FSK_IRQ2_PAYLOAD_READY 0x04
#define FSK_IRQ2_CRC_OK        0x02

#define FSK_MODE_TX  0x03
#define FSK_MODE_RX  0x05

static bool fsk_mode_active = false;

// Смена LongRangeMode допустима только в Sleep
static void opmode_raw(uint8_t lrm, uint8_t mode)
{
    // сохраняем бит LowFrequencyModeOn как есть (бит 3)
    uint8_t cur = sx1278_read_reg(REG_OP_MODE) & 0x08;
    write_reg(REG_OP_MODE, lrm | cur | mode);
}

static esp_err_t sx1278_fsk_enter_unlocked(int profile)
{
    if (fsk_mode_active) return ESP_OK;

    // LoRa sleep → FSK sleep → standby
    set_mode(MODE_SLEEP);
    vTaskDelay(pdMS_TO_TICKS(2));
    opmode_raw(0x00, MODE_SLEEP);
    vTaskDelay(pdMS_TO_TICKS(2));
    opmode_raw(0x00, MODE_STDBY);
    vTaskDelay(pdMS_TO_TICKS(2));

    // Профиль: битрейт / девиация / полоса приёмника
    switch (profile) {
        case FSK_PROFILE_LONGRANGE:
            // 2.4 кбит/с, fdev 10 кГц, RxBw 41.7 кГц — максимум чутья
            write_reg(REG_FSK_BITRATE_MSB, 0x34);   // 32e6/2400 = 13333
            write_reg(REG_FSK_BITRATE_LSB, 0x15);
            write_reg(REG_FSK_FDEV_MSB, 0x00);      // 10000/61.035 = 164
            write_reg(REG_FSK_FDEV_LSB, 0xA4);
            write_reg(REG_FSK_RX_BW,  0x13);        // 41.7 кГц
            write_reg(REG_FSK_AFC_BW, 0x12);        // 83.3 кГц
            break;
        case FSK_PROFILE_HD:
            // 50 кбит/с, fdev 25 кГц, RxBw 125 кГц — для AMR-WB
            write_reg(REG_FSK_BITRATE_MSB, 0x02);   // 32e6/50000 = 640
            write_reg(REG_FSK_BITRATE_LSB, 0x80);
            write_reg(REG_FSK_FDEV_MSB, 0x01);
            write_reg(REG_FSK_FDEV_LSB, 0x9A);
            write_reg(REG_FSK_RX_BW,  0x02);        // 125 кГц
            write_reg(REG_FSK_AFC_BW, 0x09);        // 200 кГц
            break;
        case FSK_PROFILE_STANDARD:
        default:
            // 19.2 кбит/с, fdev 25 кГц, RxBw 83.3 кГц (боевой, 2 км)
            write_reg(REG_FSK_BITRATE_MSB, 0x06);   // 32e6/19200 = 1667
            write_reg(REG_FSK_BITRATE_LSB, 0x83);
            write_reg(REG_FSK_FDEV_MSB, 0x01);
            write_reg(REG_FSK_FDEV_LSB, 0x9A);
            write_reg(REG_FSK_RX_BW,  0x12);
            write_reg(REG_FSK_AFC_BW, 0x0A);
            break;
    }
    // Преамбула 5 байт
    write_reg(REG_FSK_PREAMBLE_MSB, 0x00);
    write_reg(REG_FSK_PREAMBLE_LSB, 0x05);
    // Sync: on, 3 байта, autorestart RX
    write_reg(REG_FSK_SYNC_CONFIG, 0x52);
    write_reg(REG_FSK_SYNC_VALUE1,     0xEB);
    write_reg(REG_FSK_SYNC_VALUE1 + 1, 0x25);
    write_reg(REG_FSK_SYNC_VALUE1 + 2, 0xF5);
    // Variable len + whitening + CRC
    write_reg(REG_FSK_PKT_CONFIG1, 0xD0);
    // Packet mode
    write_reg(REG_FSK_PKT_CONFIG2, 0x40);
    // Max payload
    write_reg(REG_FSK_PAYLOAD_LEN, 64);
    // TX start: FIFO not empty
    write_reg(REG_FSK_FIFO_THRESH, 0x8F);

    // Частота/мощность те же, что были в LoRa (регистры общие,
    // конфигурировались в sx1278_configure)

    opmode_raw(0x00, FSK_MODE_RX);
    fsk_mode_active = true;
    ESP_LOGI(TAG, "FSK mode ON (profile=%d)", profile);
    return ESP_OK;
}

static esp_err_t sx1278_fsk_exit_unlocked(void)
{
    if (!fsk_mode_active) return ESP_OK;
    opmode_raw(0x00, MODE_SLEEP);
    vTaskDelay(pdMS_TO_TICKS(2));
    opmode_raw(MODE_LONG_RANGE, MODE_SLEEP);   // назад в LoRa
    vTaskDelay(pdMS_TO_TICKS(2));
    set_mode(MODE_STDBY);
    fsk_mode_active = false;
    ESP_LOGI(TAG, "FSK mode OFF — back to LoRa (нужен reconfigure)");
    return ESP_OK;
}

static esp_err_t sx1278_fsk_send_unlocked(const uint8_t *data, uint8_t len)
{
    if (!fsk_mode_active || len > 60) return ESP_ERR_INVALID_STATE;

    opmode_raw(0x00, MODE_STDBY);
    // FIFO: [len][payload]
    write_reg(REG_FIFO, len);
    for (uint8_t i = 0; i < len; i++) write_reg(REG_FIFO, data[i]);
    opmode_raw(0x00, FSK_MODE_TX);

    // V2.8.2: ждём до 800мс — на профиле 2.4к (Дальнобой) пакет 40Б
    // летит ~170мс, старый лимит 100мс обрывал КАЖДЫЙ пакет.
    // Быстрые профили выходят по IRQ раньше, для них это не задержка.
    for (int t = 0; t < 400; t++) {
        if (sx1278_read_reg(REG_FSK_IRQ_FLAGS2) & FSK_IRQ2_PACKET_SENT) {
            opmode_raw(0x00, FSK_MODE_RX);
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    opmode_raw(0x00, FSK_MODE_RX);
    return ESP_ERR_TIMEOUT;
}

static int sx1278_fsk_read_unlocked(uint8_t *buf, uint8_t buf_size)
{
    if (!fsk_mode_active) return 0;
    uint8_t irq2 = sx1278_read_reg(REG_FSK_IRQ_FLAGS2);
    if (!(irq2 & FSK_IRQ2_PAYLOAD_READY)) return 0;

    uint8_t len = sx1278_read_reg(REG_FIFO);   // variable mode: 1-й байт длина
    if (len > 60) {                             // мусор — чистим FIFO рестартом RX
        opmode_raw(0x00, MODE_STDBY);
        opmode_raw(0x00, FSK_MODE_RX);
        return 0;
    }
    for (uint8_t i = 0; i < len && i < buf_size; i++) buf[i] = sx1278_read_reg(REG_FIFO);
    return (int)len;
}

// ── Потокобезопасные обёртки ─────────────────────────────────
esp_err_t sx1278_send(const uint8_t *data, size_t len)
{
    SX_LOCK();
    esp_err_t r = sx1278_send_unlocked(data, len);
    SX_UNLOCK();
    return r;
}

int sx1278_read_packet(uint8_t *buf, size_t buf_size)
{
    SX_LOCK();
    int r = sx1278_read_packet_unlocked(buf, buf_size);
    SX_UNLOCK();
    return r;
}

esp_err_t sx1278_start_receive(void)
{
    SX_LOCK();
    esp_err_t r = sx1278_start_receive_unlocked();
    SX_UNLOCK();
    return r;
}

// Что чип на самом деле делает прямо сейчас. Все значения читаются
// обратно из регистров: hwcfg может говорить одно, а в чипе лежать
// другое — ровно это и надо поймать.
void sx1278_log_state(void)
{
    SX_LOCK();
    uint8_t op   = sx1278_read_reg(REG_OP_MODE);
    uint8_t m1   = sx1278_read_reg(REG_MODEM_CONFIG_1);
    uint8_t m2   = sx1278_read_reg(REG_MODEM_CONFIG_2);
    uint8_t irq  = sx1278_read_reg(REG_IRQ_FLAGS);
    uint8_t rssi = sx1278_read_reg(REG_RSSI_VALUE);
    uint32_t frf = ((uint32_t)sx1278_read_reg(REG_FRF_MSB) << 16) |
                   ((uint32_t)sx1278_read_reg(REG_FRF_MID) << 8)  |
                    (uint32_t)sx1278_read_reg(REG_FRF_LSB);
    SX_UNLOCK();

    // FRF → Гц: шаг PLL = 32 МГц / 2^19
    uint32_t hz = (uint32_t)(((uint64_t)frf * 32000000ULL) >> 19);
    static const int bw_khz[10] = { 8, 10, 16, 21, 31, 42, 62, 125, 250, 500 };
    int bwi = (m1 >> 4) & 0x0F;

    ESP_LOGI(TAG, "состояние: mode=0x%02X %s, %lu Гц, SF%d BW%dk CR4/%d, irq=0x%02X, rssi=%d",
             op, (op & 0x07) == MODE_RX_CONT ? "RX_CONT"
               : (op & 0x07) == MODE_STDBY   ? "STANDBY ← НЕ СЛУШАЕТ"
               : (op & 0x07) == MODE_SLEEP   ? "SLEEP ← НЕ СЛУШАЕТ"
               : (op & 0x07) == MODE_TX      ? "TX"
               : (op & 0x07) == MODE_CAD     ? "CAD"
                                             : "?",
             (unsigned long)hz, (m2 >> 4) & 0x0F,
             bwi < 10 ? bw_khz[bwi] : 0, ((m1 >> 1) & 0x07) + 4,
             irq, -157 + (int)rssi);
}

bool sx1278_cad_detect(void)
{
    SX_LOCK();
    bool r = sx1278_cad_detect_unlocked();
    SX_UNLOCK();
    return r;
}

bool sx1278_is_channel_free(int threshold_dbm)
{
    SX_LOCK();
    bool r = sx1278_is_channel_free_unlocked(threshold_dbm);
    SX_UNLOCK();
    return r;
}

int sx1278_get_rssi(void)
{
    SX_LOCK();
    int r = sx1278_get_rssi_unlocked();
    SX_UNLOCK();
    return r;
}

float sx1278_get_snr(void)
{
    SX_LOCK();
    float r = sx1278_get_snr_unlocked();
    SX_UNLOCK();
    return r;
}

esp_err_t sx1278_configure(uint32_t freq, int sf, int bw, int cr, int tx_power)
{
    SX_LOCK();
    esp_err_t r = sx1278_configure_unlocked(freq, sf, bw, cr, tx_power);
    SX_UNLOCK();
    return r;
}


esp_err_t sx1278_fsk_enter(int profile)
{
    SX_LOCK();
    esp_err_t r = sx1278_fsk_enter_unlocked(profile);
    SX_UNLOCK();
    return r;
}

esp_err_t sx1278_fsk_exit(void)
{
    SX_LOCK();
    esp_err_t r = sx1278_fsk_exit_unlocked();
    SX_UNLOCK();
    return r;
}

esp_err_t sx1278_fsk_send(const uint8_t *data, uint8_t len)
{
    SX_LOCK();
    esp_err_t r = sx1278_fsk_send_unlocked(data, len);
    SX_UNLOCK();
    return r;
}

int sx1278_fsk_read(uint8_t *buf, uint8_t buf_size)
{
    SX_LOCK();
    int r = sx1278_fsk_read_unlocked(buf, buf_size);
    SX_UNLOCK();
    return r;
}


