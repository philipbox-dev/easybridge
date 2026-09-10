// Radio HAL backend: SX126x over SPI (EBYTE E22-400M33S)
#include "config.h"

#include "radio_hal.h"
#include "hwcfg.h"
#include "sx126x.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "RADIO_SX126X";

// Потолок PA берётся из hwcfg: у Ra-02 это 20 dBm, у E22-M33S — 33,
// и жечь модуль настройкой от другой платы мы не хотим.
#define BOARD_LORA_MAX_DBM (hwcfg()->radio.max_dbm)

// Same presets as radio_sx127x.c — matched to E220 air-rates
typedef struct { int sf, bw_khz, cr; } sx_preset_t;

static sx_preset_t preset_for(int speed)
{
    if (speed == LORA_SF_FAST) return (sx_preset_t){ .sf = 7, .bw_khz = 250, .cr = 5 };
    return (sx_preset_t){ .sf = 9, .bw_khz = 125, .cr = 5 };
}

static int dbm_for(uint8_t pwr_idx)
{
    int dbm = BOARD_LORA_MAX_DBM - 3 * (int)pwr_idx;
    return dbm < 2 ? 2 : dbm;
}

static int     s_speed   = LORA_SF_SLOW;
static uint8_t s_pwr_idx = 0;

static esp_err_t sx126x_hal_configure(int speed)
{
    sx_preset_t p = preset_for(speed);
    esp_err_t ret = sx126x_configure(hwcfg()->radio.freq_hz, p.sf, p.bw_khz, p.cr,
                                     dbm_for(s_pwr_idx));
    if (ret != ESP_OK) return ret;
    s_speed = speed;
    return sx126x_start_receive();
}

static esp_err_t sx126x_hal_send(const uint8_t *data, size_t len)
{
    esp_err_t ret = sx126x_send(data, len);
    sx126x_start_receive();
    return ret;
}

static int sx126x_hal_receive(uint8_t *buf, size_t buf_size,
                              int *rssi_out, float *snr_out, int timeout_ms)
{
    int waited = 0;
    while (waited < timeout_ms) {
        int len = sx126x_read_packet(buf, buf_size);
        if (len > 0) {
            if (rssi_out) *rssi_out = sx126x_get_rssi();
            if (snr_out)  *snr_out  = sx126x_get_snr();
            return len;
        }
        if (len < 0) return -1;
        vTaskDelay(pdMS_TO_TICKS(20));
        waited += 20;
    }
    return 0;
}

static esp_err_t sx126x_hal_set_speed(int speed)
{
    return sx126x_hal_configure(speed);
}

static esp_err_t sx126x_hal_set_tx_power(uint8_t pwr_idx)
{
    if (pwr_idx > 3) return ESP_ERR_INVALID_ARG;
    s_pwr_idx = pwr_idx;
    ESP_LOGI(TAG, "TX power idx %u → %d dBm", pwr_idx, dbm_for(pwr_idx));
    return sx126x_hal_configure(s_speed);
}

const radio_ops_t radio_ops_sx126x = {
    .name         = "E22-400M33S (SX1268)",
    .init         = sx126x_init,
    .is_alive     = sx126x_is_alive,
    .configure    = sx126x_hal_configure,
    .send         = sx126x_hal_send,
    .receive      = sx126x_hal_receive,
    .set_speed    = sx126x_hal_set_speed,
    .set_tx_power = sx126x_hal_set_tx_power,
    .channel_busy = sx126x_cad_detect,
};


