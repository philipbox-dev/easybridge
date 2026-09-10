// SX126x (SX1268) driver for EBYTE E22-400M33S — SPI command interface.
// ⚠ UNTESTED ON HARDWARE. Follows SX1268 datasheet init sequence;
// verify BUSY timing and PA settings during bring-up.
#include "config.h"

#include "sx126x.h"
#include "hwcfg.h"
#include "spi_bus.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

// Пины и параметры шины — из hwcfg() (V2.9).
#define RP            (hwcfg()->radio)
#define PIN_LORA_SCK  (RP.pins.sck)
#define PIN_LORA_MISO (RP.pins.miso)
#define PIN_LORA_MOSI (RP.pins.mosi)
#define PIN_LORA_CS   (RP.pins.cs)
#define PIN_LORA_RST  (RP.pins.rst)
#define PIN_LORA_BUSY (RP.pins.busy)
#define PIN_LORA_DIO1 (RP.pins.dio1)
#define PIN_LORA_TXEN (RP.pins.txen)
#define PIN_LORA_RXEN (RP.pins.rxen)

static const char *TAG = "SX126X";
static spi_device_handle_t spi;

// Потокобезопасность: rx_task + tx_task делят SPI (см. sx1278.c V2.6.1)
#include "freertos/semphr.h"
static SemaphoreHandle_t sx_mutex = NULL;
#define SX_LOCK()   do { if (sx_mutex) xSemaphoreTakeRecursive(sx_mutex, portMAX_DELAY); } while (0)
#define SX_UNLOCK() do { if (sx_mutex) xSemaphoreGiveRecursive(sx_mutex); } while (0)

// ── BUSY handling ────────────────────────────────────────────
// Every command must wait for BUSY low before and after.
static esp_err_t wait_busy(int timeout_ms)
{
    int elapsed = 0;
    while (gpio_get_level(PIN_LORA_BUSY)) {
        if (elapsed >= timeout_ms) {
            ESP_LOGE(TAG, "BUSY stuck high");
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
        elapsed++;
    }
    return ESP_OK;
}

// ── SPI primitives ───────────────────────────────────────────
static esp_err_t cmd_write(uint8_t opcode, const uint8_t *args, size_t argc)
{
    if (wait_busy(100) != ESP_OK) return ESP_ERR_TIMEOUT;
    uint8_t tx[16] = { opcode };
    if (argc > sizeof(tx) - 1) return ESP_ERR_INVALID_SIZE;
    if (args && argc) memcpy(tx + 1, args, argc);
    spi_transaction_t t = { .length = 8 * (1 + argc), .tx_buffer = tx };
    return spi_device_transmit(spi, &t);
}

static esp_err_t cmd_read(uint8_t opcode, uint8_t *out, size_t outc)
{
    if (wait_busy(100) != ESP_OK) return ESP_ERR_TIMEOUT;
    uint8_t tx[16] = { opcode, 0x00 };  // NOP status byte
    uint8_t rx[16] = { 0 };
    size_t total = 2 + outc;
    if (total > sizeof(tx)) return ESP_ERR_INVALID_SIZE;
    spi_transaction_t t = { .length = 8 * total, .tx_buffer = tx, .rx_buffer = rx };
    esp_err_t ret = spi_device_transmit(spi, &t);
    if (ret == ESP_OK && out) memcpy(out, rx + 2, outc);
    return ret;
}

static void reg_write(uint16_t addr, uint8_t val)
{
    uint8_t args[3] = { (uint8_t)(addr >> 8), (uint8_t)addr, val };
    cmd_write(SX126X_WRITE_REGISTER, args, 3);
}

static uint8_t reg_read(uint16_t addr)
{
    if (wait_busy(100) != ESP_OK) return 0;
    uint8_t tx[5] = { SX126X_READ_REGISTER, (uint8_t)(addr >> 8), (uint8_t)addr, 0, 0 };
    uint8_t rx[5] = { 0 };
    spi_transaction_t t = { .length = 40, .tx_buffer = tx, .rx_buffer = rx };
    spi_device_transmit(spi, &t);
    return rx[4];
}

static uint16_t irq_get(void)
{
    uint8_t st[3] = { 0 };
    cmd_read(SX126X_GET_IRQ_STATUS, st, 2);
    // cmd_read returns bytes after opcode+status: [irq_msb, irq_lsb]
    return ((uint16_t)st[0] << 8) | st[1];
}

static void irq_clear(uint16_t mask)
{
    uint8_t args[2] = { (uint8_t)(mask >> 8), (uint8_t)mask };
    cmd_write(SX126X_CLR_IRQ_STATUS, args, 2);
}

// ── RF switch (E22 TXEN/RXEN) ────────────────────────────────
static void rf_switch(bool tx, bool rx)
{
    if (PIN_LORA_TXEN >= 0) gpio_set_level(PIN_LORA_TXEN, tx ? 1 : 0);
    if (PIN_LORA_RXEN >= 0) gpio_set_level(PIN_LORA_RXEN, rx ? 1 : 0);
}

// ── Public API ───────────────────────────────────────────────
void sx126x_reset(void)
{
    gpio_set_level(PIN_LORA_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(PIN_LORA_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    wait_busy(100);
}

bool sx126x_is_alive(void)
{
    // No version register like SX127x — check sync-word register
    // survives a write/readback round-trip.
    uint8_t prev = reg_read(SX126X_REG_SYNC_WORD_MSB);
    reg_write(SX126X_REG_SYNC_WORD_MSB, 0xA5);
    bool ok = (reg_read(SX126X_REG_SYNC_WORD_MSB) == 0xA5);
    reg_write(SX126X_REG_SYNC_WORD_MSB, prev);
    return ok;
}

esp_err_t sx126x_init(void)
{
    if (!sx_mutex) sx_mutex = xSemaphoreCreateRecursiveMutex();

    // TXEN/RXEN есть не у всех модулей: у голого SX1262 антенный
    // свитч рулится самим чипом через DIO2. Незаданные пины (-1)
    // в маску не попадают.
    uint64_t out_mask = (1ULL << PIN_LORA_RST);
    if (PIN_LORA_TXEN >= 0) out_mask |= (1ULL << PIN_LORA_TXEN);
    if (PIN_LORA_RXEN >= 0) out_mask |= (1ULL << PIN_LORA_RXEN);
    gpio_config_t out = {
        .pin_bit_mask = out_mask,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);
    gpio_config_t in = {
        .pin_bit_mask = (1ULL << PIN_LORA_BUSY) | (1ULL << PIN_LORA_DIO1),
        .mode = GPIO_MODE_INPUT,
    };
    gpio_config(&in);
    rf_switch(false, false);

    spi_host_device_t host;
    esp_err_t ret = spi_bus_acquire(RP.spi_host, PIN_LORA_SCK, PIN_LORA_MOSI,
                                    PIN_LORA_MISO, 300, &host);
    if (ret != ESP_OK) return ret;

    spi_device_interface_config_t dev = {
        .clock_speed_hz = RP.spi_hz ? (int)RP.spi_hz : 2000000,
        .mode = 0,
        .spics_io_num = PIN_LORA_CS,
        .queue_size = 1,
    };
    ret = spi_bus_add_device(host, &dev, &spi);
    if (ret != ESP_OK) return ret;

    sx126x_reset();

    if (!sx126x_is_alive()) {
        ESP_LOGE(TAG, "SX126x not responding");
        return ESP_ERR_NOT_FOUND;
    }

    // STDBY_RC, DC-DC regulator, calibrate all blocks
    uint8_t stdby = 0x00;
    cmd_write(SX126X_SET_STANDBY, &stdby, 1);
    uint8_t reg_mode = 0x01;  // DC-DC
    cmd_write(SX126X_SET_REGULATOR_MODE, &reg_mode, 1);
    uint8_t calib = 0x7F;
    cmd_write(SX126X_CALIBRATE, &calib, 1);
    vTaskDelay(pdMS_TO_TICKS(5));

    ESP_LOGI(TAG, "SX126x OK");
    return ESP_OK;
}

static esp_err_t sx126x_configure_unlocked(uint32_t freq_hz, int sf, int bw_khz, int cr, int tx_dbm)
{
    uint8_t stdby = 0x00;
    cmd_write(SX126X_SET_STANDBY, &stdby, 1);

    // Packet type: LoRa
    uint8_t pkt_type = 0x01;
    cmd_write(SX126X_SET_PACKET_TYPE, &pkt_type, 1);

    // Frequency: freq * 2^25 / 32e6
    uint32_t frf = (uint32_t)(((uint64_t)freq_hz << 25) / 32000000ULL);
    uint8_t fargs[4] = { (uint8_t)(frf >> 24), (uint8_t)(frf >> 16),
                         (uint8_t)(frf >> 8),  (uint8_t)frf };
    cmd_write(SX126X_SET_RF_FREQUENCY, fargs, 4);

    // PA config for SX1268 @ +22 dBm (datasheet table 13-21)
    uint8_t pa[4] = { 0x04, 0x07, 0x00, 0x01 };
    cmd_write(SX126X_SET_PA_CONFIG, pa, 4);
    // OCP 140 mA
    reg_write(SX126X_REG_OCP, 0x38);

    // TX params: power, ramp 200us
    if (tx_dbm > 22) tx_dbm = 22;
    if (tx_dbm < -3) tx_dbm = -3;
    uint8_t txp[2] = { (uint8_t)(int8_t)tx_dbm, 0x04 };
    cmd_write(SX126X_SET_TX_PARAMS, txp, 2);

    // Modulation: SF, BW, CR, LDRO
    uint8_t bw_code;
    switch (bw_khz) {
        case 250: bw_code = 0x05; break;
        case 500: bw_code = 0x06; break;
        default:  bw_code = 0x04; break;  // 125 kHz
    }
    uint8_t ldro = (sf >= 11 && bw_khz == 125) ? 0x01 : 0x00;
    uint8_t mod[4] = { (uint8_t)sf, bw_code, (uint8_t)(cr - 4), ldro };
    cmd_write(SX126X_SET_MODULATION, mod, 4);

    // Packet params: preamble, explicit header, max payload, CRC on, no IQ inv
    uint8_t pp[6] = { 0x00, LORA_PREAMBLE_LEN, 0x00, 0xFF, 0x01, 0x00 };
    cmd_write(SX126X_SET_PACKET_PARAMS, pp, 6);

    // Sync word: private-network LoRa (0x12 → regs 0x1424, EBYTE-compatible)
    reg_write(SX126X_REG_SYNC_WORD_MSB, 0x14);
    reg_write(SX126X_REG_SYNC_WORD_LSB, 0x24);

    // Buffer bases
    uint8_t bases[2] = { 0x00, 0x00 };
    cmd_write(SX126X_SET_BUF_BASE, bases, 2);

    // IRQ: TxDone|RxDone|CrcErr|CadDone|CadDetected|Timeout → DIO1
    uint16_t irqs = SX126X_IRQ_TX_DONE | SX126X_IRQ_RX_DONE | SX126X_IRQ_CRC_ERR |
                    SX126X_IRQ_CAD_DONE | SX126X_IRQ_CAD_DETECTED | SX126X_IRQ_TIMEOUT;
    uint8_t di[8] = { (uint8_t)(irqs >> 8), (uint8_t)irqs,
                      (uint8_t)(irqs >> 8), (uint8_t)irqs, 0, 0, 0, 0 };
    cmd_write(SX126X_SET_DIO_IRQ_PARAMS, di, 8);

    ESP_LOGI(TAG, "Configured: %luHz SF%d BW%d CR%d PWR%d",
             (unsigned long)freq_hz, sf, bw_khz, cr, tx_dbm);
    return ESP_OK;
}

static esp_err_t sx126x_send_unlocked(const uint8_t *data, size_t len)
{
    if (len > 255) return ESP_ERR_INVALID_SIZE;

    uint8_t stdby = 0x00;
    cmd_write(SX126X_SET_STANDBY, &stdby, 1);
    rf_switch(true, false);

    // Write payload at offset 0
    if (wait_busy(100) != ESP_OK) { rf_switch(false, false); return ESP_ERR_TIMEOUT; }
    {
        static uint8_t tx_buf[2 + 255];
        tx_buf[0] = SX126X_WRITE_BUFFER;
        tx_buf[1] = 0x00;
        memcpy(tx_buf + 2, data, len);
        spi_transaction_t t = { .length = 8 * (2 + len), .tx_buffer = tx_buf };
        spi_device_transmit(spi, &t);
    }

    // Update payload length in packet params
    uint8_t pp[6] = { 0x00, LORA_PREAMBLE_LEN, 0x00, (uint8_t)len, 0x01, 0x00 };
    cmd_write(SX126X_SET_PACKET_PARAMS, pp, 6);

    irq_clear(0xFFFF);

    // TX with 5s hw timeout (15.625us units: 5s = 320000 = 0x04E200)
    uint8_t txargs[3] = { 0x04, 0xE2, 0x00 };
    cmd_write(SX126X_SET_TX, txargs, 3);

    int timeout_ms = 6000;
    while (timeout_ms > 0) {
        uint16_t irq = irq_get();
        if (irq & SX126X_IRQ_TX_DONE) {
            irq_clear(0xFFFF);
            rf_switch(false, false);
            ESP_LOGI(TAG, "TX done: %d bytes", (int)len);
            return ESP_OK;
        }
        if (irq & SX126X_IRQ_TIMEOUT) break;
        vTaskDelay(pdMS_TO_TICKS(10));
        timeout_ms -= 10;
    }

    irq_clear(0xFFFF);
    rf_switch(false, false);
    ESP_LOGE(TAG, "TX timeout");
    return ESP_ERR_TIMEOUT;
}

static esp_err_t sx126x_start_receive_unlocked(void)
{
    rf_switch(false, true);
    irq_clear(0xFFFF);
    uint8_t rxargs[3] = { 0xFF, 0xFF, 0xFF };  // continuous RX
    return cmd_write(SX126X_SET_RX, rxargs, 3);
}

static int last_rssi = 0;
static float last_snr = 0.0f;

static int sx126x_read_packet_unlocked(uint8_t *buf, size_t buf_size)
{
    uint16_t irq = irq_get();
    if (irq & SX126X_IRQ_CRC_ERR) {
        irq_clear(0xFFFF);
        sx126x_start_receive_unlocked();
        return -1;
    }
    if (!(irq & SX126X_IRQ_RX_DONE)) return 0;

    uint8_t st[3] = { 0 };
    cmd_read(SX126X_GET_RX_BUF_STATUS, st, 2);
    uint8_t len = st[0], offset = st[1];
    if (len > buf_size) len = buf_size;

    // Packet status → RSSI/SNR
    uint8_t ps[4] = { 0 };
    cmd_read(SX126X_GET_PACKET_STATUS, ps, 3);
    last_rssi = -((int)ps[0]) / 2;
    last_snr  = (float)(int8_t)ps[1] / 4.0f;

    // Read buffer
    if (wait_busy(100) == ESP_OK) {
        static uint8_t rx_raw[3 + 255];
        uint8_t tx_raw[3 + 255] = { SX126X_READ_BUFFER, offset, 0x00 };
        spi_transaction_t t = { .length = 8 * (3 + len),
                                .tx_buffer = tx_raw, .rx_buffer = rx_raw };
        spi_device_transmit(spi, &t);
        memcpy(buf, rx_raw + 3, len);
    }

    irq_clear(0xFFFF);
    ESP_LOGI(TAG, "RX: %d bytes rssi=%d", len, last_rssi);
    return len;
}

int sx126x_get_rssi(void) { return last_rssi; }
float sx126x_get_snr(void) { return last_snr; }

static bool sx126x_cad_detect_unlocked(void)
{
    uint8_t stdby = 0x00;
    cmd_write(SX126X_SET_STANDBY, &stdby, 1);
    rf_switch(false, true);
    irq_clear(0xFFFF);

    // CAD params: 4 symbols, detPeak/detMin defaults, exit to STDBY
    uint8_t cad[7] = { 0x02, 0x16, 0x0A, 0x00, 0x00, 0x00, 0x00 };
    cmd_write(SX126X_SET_CAD_PARAMS, cad, 7);
    cmd_write(SX126X_SET_CAD, NULL, 0);

    int timeout = 100;
    while (timeout-- > 0) {
        uint16_t irq = irq_get();
        if (irq & SX126X_IRQ_CAD_DONE) {
            bool detected = (irq & SX126X_IRQ_CAD_DETECTED) != 0;
            irq_clear(0xFFFF);
            sx126x_start_receive_unlocked();
            return detected;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    irq_clear(0xFFFF);
    sx126x_start_receive_unlocked();
    return false;
}


// ── Потокобезопасные обёртки ─────────────────────────────────
esp_err_t sx126x_send(const uint8_t *data, size_t len)
{
    SX_LOCK();
    esp_err_t r = sx126x_send_unlocked(data, len);
    SX_UNLOCK();
    return r;
}

int sx126x_read_packet(uint8_t *buf, size_t buf_size)
{
    SX_LOCK();
    int r = sx126x_read_packet_unlocked(buf, buf_size);
    SX_UNLOCK();
    return r;
}

esp_err_t sx126x_start_receive(void)
{
    SX_LOCK();
    esp_err_t r = sx126x_start_receive_unlocked();
    SX_UNLOCK();
    return r;
}

bool sx126x_cad_detect(void)
{
    SX_LOCK();
    bool r = sx126x_cad_detect_unlocked();
    SX_UNLOCK();
    return r;
}

esp_err_t sx126x_configure(uint32_t freq_hz, int sf, int bw_khz, int cr, int tx_dbm)
{
    SX_LOCK();
    esp_err_t r = sx126x_configure_unlocked(freq_hz, sf, bw_khz, cr, tx_dbm);
    SX_UNLOCK();
    return r;
}


