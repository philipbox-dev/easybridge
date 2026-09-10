// Radio HAL backend: SX1276/78 over SPI (LilyGO T3 v1.6.1 etc.)
#include "config.h"

#include "radio_hal.h"
#include "hwcfg.h"
#include "sx1278.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "RADIO_SX127X";

// Потолок PA берётся из hwcfg: у Ra-02 это 20 dBm, у E22-M33S — 33,
// и жечь модуль настройкой от другой платы мы не хотим.
#define BOARD_LORA_MAX_DBM (hwcfg()->radio.max_dbm)

// Speed preset → SF/BW/CR, matched to EBYTE E220 air-rates so that a
// 433 MHz SX-board can hear an E220 on the same frequency.
//   SLOW (E220 2.4 kbps) : SF9  BW125 CR4/5
//   FAST (E220 19.2 kbps): SF7  BW250 CR4/5
// BW reg values (SX127x ModemConfig1): 7=125k, 8=250k, 9=500k
typedef struct { int sf, bw, cr; } sx_preset_t;

static sx_preset_t preset_for(int speed)
{
    if (speed == LORA_SF_FAST) return (sx_preset_t){ .sf = 7, .bw = 8, .cr = 5 };
    return (sx_preset_t){ .sf = 9, .bw = 7, .cr = 5 };  // SLOW / default
}

// legacy pwr_idx 0..3 → dBm (top = board max)
static int dbm_for(uint8_t pwr_idx)
{
    int dbm = BOARD_LORA_MAX_DBM - 3 * (int)pwr_idx;
    return dbm < 2 ? 2 : dbm;
}

static int     s_speed   = LORA_SF_SLOW;
static uint8_t s_pwr_idx = 0;

static esp_err_t sx127x_hal_configure(int speed)
{
    sx_preset_t p = preset_for(speed);
    esp_err_t ret = sx1278_configure(hwcfg()->radio.freq_hz, p.sf, p.bw, p.cr,
                                     dbm_for(s_pwr_idx));
    if (ret != ESP_OK) return ret;
    s_speed = speed;
    return sx1278_start_receive();
}

static esp_err_t sx127x_hal_init(void)
{
    esp_err_t ret = sx1278_init();
    if (ret != ESP_OK) return ret;
    return ESP_OK;
}

static esp_err_t sx127x_hal_send(const uint8_t *data, size_t len)
{
    esp_err_t ret = sx1278_send(data, len);
    sx1278_start_receive();  // back to RX either way
    return ret;
}

static int sx127x_hal_receive(uint8_t *buf, size_t buf_size,
                              int *rssi_out, float *snr_out, int timeout_ms)
{
    int waited = 0;
    while (waited < timeout_ms) {
        int len = sx1278_read_packet(buf, buf_size);
        if (len > 0) {
            if (rssi_out) *rssi_out = sx1278_get_rssi();
            if (snr_out)  *snr_out  = sx1278_get_snr();
            return len;
        }
        if (len < 0) return -1;  // CRC error
        vTaskDelay(pdMS_TO_TICKS(20));
        waited += 20;
    }
    return 0;
}

static esp_err_t sx127x_hal_set_speed(int speed)
{
    return sx127x_hal_configure(speed);
}

static esp_err_t sx127x_hal_set_tx_power(uint8_t pwr_idx)
{
    if (pwr_idx > 3) return ESP_ERR_INVALID_ARG;
    s_pwr_idx = pwr_idx;
    ESP_LOGI(TAG, "TX power idx %u → %d dBm", pwr_idx, dbm_for(pwr_idx));
    return sx127x_hal_configure(s_speed);  // re-apply full config
}

static bool sx127x_hal_channel_busy(void)
{
    return sx1278_cad_detect();
}

const radio_ops_t radio_ops_sx127x = {
    .name         = "SX1276",
    .init         = sx127x_hal_init,
    .is_alive     = sx1278_is_alive,
    .configure    = sx127x_hal_configure,
    .send         = sx127x_hal_send,
    .receive      = sx127x_hal_receive,
    .set_speed    = sx127x_hal_set_speed,
    .set_tx_power = sx127x_hal_set_tx_power,
    .channel_busy = sx127x_hal_channel_busy,
    .log_state    = sx1278_log_state,
};



// ── FSK-бэкенд ──────────────────────────────────────────────
// Тонкая обёртка над sx1278_fsk_*: ptt.c и imgfsk.c больше не
// зовут чип напрямую, а спрашивают fsk_ops(). Когда появится
// FSK на SX126x/SX128x, добавится второй такой же блок, и код
// звонков не изменится ни строкой.
const fsk_ops_t fsk_ops_sx127x = {
    .name  = "SX127x GFSK",
    .enter = sx1278_fsk_enter,
    .exit  = sx1278_fsk_exit,
    .send  = sx1278_fsk_send,
    .read  = sx1278_fsk_read,
};
