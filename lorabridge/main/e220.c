#include "e220.h"
#include "hwcfg.h"
#include "config.h"
#include "config.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

// E220 задаёт частоту номером канала с шагом 1 МГц от начала
// сетки модуля: 410.125 МГц у 400-й серии, 850.125 у 900-й.
// Держим это здесь, чтобы hwcfg говорил в герцах, как все.
static uint8_t e220_channel_for(uint32_t freq_hz)
{
    uint32_t base = (freq_hz >= 800000000u) ? 850125000u : 410125000u;
    if (freq_hz < base) return 0;
    uint32_t ch = (freq_hz - base) / 1000000u;
    return (uint8_t)(ch > 83 ? 83 : ch);
}

static const char *TAG = "E220";

// Пины, порт и частота — из hwcfg() (V2.9). Имена макросов
// оставлены прежними: тело драйвера от этого не изменилось,
// поменялся только источник значений — с #define на рантайм.
#define RP             (hwcfg()->radio)
#define UART_NUM       ((uart_port_t)RP.uart_num)
#define PIN_E220_TX    (RP.pins.tx)
#define PIN_E220_RX    (RP.pins.rx)
#define PIN_E220_AUX   (RP.pins.aux)
#define PIN_E220_M0    (RP.pins.m0)
#define PIN_E220_M1    (RP.pins.m1)
#define UART_BUF_SZ    1024

#define CMD_WRITE_SAVE   0xC0
#define CMD_READ         0xC1
#define CMD_GET_VERSION  0xC3

static SemaphoreHandle_t e220_mutex = NULL;
static volatile bool in_config = false;

static void set_mode(int mode)
{
    gpio_set_level(PIN_E220_M0, mode & 0x01);
    gpio_set_level(PIN_E220_M1, (mode >> 1) & 0x01);
    vTaskDelay(pdMS_TO_TICKS(50));
}

static void wait_aux_high(int timeout_ms)
{
    int elapsed = 0;
    while (gpio_get_level(PIN_E220_AUX) == 0 && elapsed < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(10));
        elapsed += 10;
    }
}

esp_err_t e220_init(void)
{
    e220_mutex = xSemaphoreCreateMutex();

    // M0, M1 — выходы
    gpio_config_t io_out = {
        .pin_bit_mask = (1ULL << PIN_E220_M0) | (1ULL << PIN_E220_M1),
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io_out);

    // AUX — вход с подтяжкой
    gpio_config_t io_aux = {
        .pin_bit_mask = (1ULL << PIN_E220_AUX),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io_aux);

    // UART
    uart_config_t uart_cfg = {
        .baud_rate  = (int)RP.uart_baud,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t ret = uart_driver_install(UART_NUM, UART_BUF_SZ, UART_BUF_SZ, 0, NULL, 0);
    if (ret != ESP_OK) return ret;

    uart_param_config(UART_NUM, &uart_cfg);
    uart_set_pin(UART_NUM, PIN_E220_TX, PIN_E220_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    vTaskDelay(pdMS_TO_TICKS(500));
    wait_aux_high(2000);

    ESP_LOGI(TAG, "UART init OK");
    return ESP_OK;
}

bool e220_is_alive(void)
{
    // Clean mode transition: NORMAL → CONFIG
    set_mode(0);
    vTaskDelay(pdMS_TO_TICKS(100));
    set_mode(3); // CONFIG
    wait_aux_high(1500);
    uart_flush_input(UART_NUM);

    // ── Method 1: GET VERSION (0xC3 0xC3 0xC3) ────────────────
    {
        uint8_t cmd[3] = { CMD_GET_VERSION, CMD_GET_VERSION, CMD_GET_VERSION };
        uart_write_bytes(UART_NUM, cmd, 3);
        uint8_t resp[8] = {0};
        int len = uart_read_bytes(UART_NUM, resp, sizeof(resp), pdMS_TO_TICKS(500));
        if (len >= 1 && resp[0] == CMD_GET_VERSION) {
            ESP_LOGI(TAG, "E220 OK (ver cmd): model=0x%02X ver=0x%02X feat=0x%02X",
                     len > 1 ? resp[1] : 0, len > 2 ? resp[2] : 0, len > 3 ? resp[3] : 0);
            return true;
        }
        ESP_LOGW(TAG, "GET_VERSION returned %d bytes, trying READ_REG fallback...", len);
        uart_flush_input(UART_NUM);
    }

    // ── Method 2: READ CONFIG (0xC1 0x00 0x08) ────────────────
    // Some E220 firmware revisions respond to READ but not GET_VERSION.
    {
        uint8_t cmd[3] = { CMD_READ, 0x00, 0x08 };
        uart_write_bytes(UART_NUM, cmd, 3);
        uint8_t resp[16] = {0};
        int len = uart_read_bytes(UART_NUM, resp, sizeof(resp), pdMS_TO_TICKS(600));
        if (len >= 3 && resp[0] == CMD_READ) {
            ESP_LOGI(TAG, "E220 OK (read reg): %d bytes, REG0=0x%02X",
                     len, len > 3 ? resp[3] : 0);
            return true;
        }
        ESP_LOGE(TAG, "E220 not responding (%d bytes on READ_REG)", len);
    }

    return false;
}

esp_err_t e220_configure(uint8_t air_rate)
{
    set_mode(3); // CONFIG
    wait_aux_high(1000);
    uart_flush_input(UART_NUM);

    uint8_t cmd[11] = {
        CMD_WRITE_SAVE,
        0x00,                                   // start reg
        0x08,                                   // 8 bytes (0x00-0x07)
        (E220_MODULE_ADDR >> 8) & 0xFF,         // ADDH
        E220_MODULE_ADDR & 0xFF,                // ADDL
        (0x03 << 5) | (air_rate & 0x07),        // REG0: UART 9600 | air rate
        (0x01 << 5) | (E220_TX_POWER & 0x03),   // REG1: 200B sub-pkt | RSSI noise ON | tx power
        e220_channel_for(RP.freq_hz),           // REG2: канал из частоты
        0x80,                                   // REG3: RSSI byte ON, transparent
        0x00, 0x00,                             // CRYPT
    };

    uart_write_bytes(UART_NUM, cmd, sizeof(cmd));
    vTaskDelay(pdMS_TO_TICKS(100));

    uint8_t resp[11];
    int len = uart_read_bytes(UART_NUM, resp, sizeof(resp), pdMS_TO_TICKS(500));

    if (len >= 3 && (resp[0] == CMD_WRITE_SAVE || resp[0] == CMD_READ)) {
        ESP_LOGI(TAG, "Config OK: rate=%d ch=%d pwr=%d", air_rate,
                 e220_channel_for(RP.freq_hz), E220_TX_POWER);
    } else {
        ESP_LOGW(TAG, "Config resp: %d bytes (resp[0]=0x%02X), retrying...", len, len > 0 ? resp[0] : 0);
        // Retry once
        uart_flush_input(UART_NUM);
        vTaskDelay(pdMS_TO_TICKS(100));
        uart_write_bytes(UART_NUM, cmd, sizeof(cmd));
        vTaskDelay(pdMS_TO_TICKS(150));
        len = uart_read_bytes(UART_NUM, resp, sizeof(resp), pdMS_TO_TICKS(500));
        if (len >= 3 && (resp[0] == CMD_WRITE_SAVE || resp[0] == CMD_READ)) {
            ESP_LOGI(TAG, "Config OK on retry: rate=%d ch=%d pwr=%d", air_rate,
                 e220_channel_for(RP.freq_hz), E220_TX_POWER);
        } else {
            ESP_LOGE(TAG, "Config FAILED after retry: %d bytes", len);
        }
    }

    set_mode(0); // NORMAL
    wait_aux_high(1000);
    uart_flush_input(UART_NUM);  // clear any transition bytes

    return ESP_OK;
}

esp_err_t e220_send(const uint8_t *data, size_t len)
{
    if (len > 200) return ESP_ERR_INVALID_SIZE;

    xSemaphoreTake(e220_mutex, portMAX_DELAY);

    wait_aux_high(5000);
    int written = uart_write_bytes(UART_NUM, data, len);
    vTaskDelay(pdMS_TO_TICKS(10));
    wait_aux_high(5000);

    xSemaphoreGive(e220_mutex);

    if (written != (int)len) {
        ESP_LOGE(TAG, "TX write failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "TX: %d bytes", (int)len);
    return ESP_OK;
}

int e220_receive(uint8_t *buf, size_t buf_size, int *rssi_out, int timeout_ms)
{
    if (in_config) {
        vTaskDelay(pdMS_TO_TICKS(100));
        return 0;
    }

    // Step 1: wait for first byte with long timeout
    int got = uart_read_bytes(UART_NUM, buf, 1, pdMS_TO_TICKS(timeout_ms));
    if (got <= 0) return 0;

    // Step 2: drain all remaining bytes with short inter-byte timeout.
    // E220 delivers a full LoRa packet as a burst, so 20 ms between
    // bytes is plenty — a complete packet arrives within a few ms.
    while (got < (int)buf_size) {
        int n = uart_read_bytes(UART_NUM, buf + got, buf_size - got,
                                pdMS_TO_TICKS(20));
        if (n <= 0) break;
        got += n;
    }

    // Step 3: strip the RSSI byte appended by E220 (REG3 RSSI=1)
    if (got < 2) return 0;   // need at least 1 data byte + RSSI
    uint8_t raw_rssi = buf[got - 1];
    if (rssi_out) *rssi_out = -(int)(256 - raw_rssi);
    got--;

    ESP_LOGI(TAG, "RX: %d bytes RSSI=%d (raw=0x%02X)",
             got, rssi_out ? *rssi_out : 0, raw_rssi);
    return got;
}

esp_err_t e220_set_air_rate(uint8_t rate)
{
    xSemaphoreTake(e220_mutex, portMAX_DELAY);
    in_config = true;
    vTaskDelay(pdMS_TO_TICKS(100));

    uart_flush_input(UART_NUM);
    set_mode(3); // CONFIG
    wait_aux_high(1000);
    uart_flush_input(UART_NUM);

    uint8_t cmd[4] = {
        CMD_WRITE_SAVE,
        0x02, 0x01,
        (0x03 << 5) | (rate & 0x07),
    };
    uart_write_bytes(UART_NUM, cmd, sizeof(cmd));
    vTaskDelay(pdMS_TO_TICKS(100));

    uint8_t resp[4];
    uart_read_bytes(UART_NUM, resp, sizeof(resp), pdMS_TO_TICKS(200));

    set_mode(0); // NORMAL
    wait_aux_high(1000);
    uart_flush_input(UART_NUM);

    in_config = false;
    xSemaphoreGive(e220_mutex);

    ESP_LOGI(TAG, "Air rate → %d", rate);
    return ESP_OK;
}

esp_err_t e220_set_tx_power(uint8_t pwr_idx)
{
    if (pwr_idx > 3) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(e220_mutex, portMAX_DELAY);
    in_config = true;
    vTaskDelay(pdMS_TO_TICKS(100));

    uart_flush_input(UART_NUM);
    set_mode(3); // CONFIG
    wait_aux_high(1000);
    uart_flush_input(UART_NUM);

    // Write REG1 (offset 0x03): sub-pkt 200B | RSSI noise ON | tx_power
    uint8_t cmd[4] = {
        CMD_WRITE_SAVE,
        0x03, 0x01,
        (0x01 << 5) | (pwr_idx & 0x03),
    };
    uart_write_bytes(UART_NUM, cmd, sizeof(cmd));
    vTaskDelay(pdMS_TO_TICKS(100));

    uint8_t resp[4];
    uart_read_bytes(UART_NUM, resp, sizeof(resp), pdMS_TO_TICKS(200));

    set_mode(0); // NORMAL
    wait_aux_high(1000);
    uart_flush_input(UART_NUM);

    in_config = false;
    xSemaphoreGive(e220_mutex);

    static const int dbm_table[4] = { 30, 27, 24, 21 };
    ESP_LOGI(TAG, "TX power → idx=%u (%d dBm)", pwr_idx, dbm_table[pwr_idx]);
    return ESP_OK;
}

