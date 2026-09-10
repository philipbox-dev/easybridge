// Radio HAL backend: EBYTE E220-400T30D over UART (transparent mode).
// Thin adapter over the existing e220.c driver.
#include "config.h"

#include "radio_hal.h"
#include "e220.h"

static int e220_hal_receive(uint8_t *buf, size_t buf_size,
                            int *rssi_out, float *snr_out, int timeout_ms)
{
    if (snr_out) *snr_out = 0.0f;  // E220 doesn't report SNR
    return e220_receive(buf, buf_size, rssi_out, timeout_ms);
}

// e220 API takes uint8_t air-rate; HAL speaks int speed presets
static esp_err_t e220_hal_configure(int speed) { return e220_configure((uint8_t)speed); }
static esp_err_t e220_hal_set_speed(int speed) { return e220_set_air_rate((uint8_t)speed); }

const radio_ops_t radio_ops_e220 = {
    .name         = "E220-400T30D",
    .init         = e220_init,
    .is_alive     = e220_is_alive,
    .configure    = e220_hal_configure,
    .send         = e220_send,
    .receive      = e220_hal_receive,
    .set_speed    = e220_hal_set_speed,
    .set_tx_power = e220_set_tx_power,
    .channel_busy = NULL,  // transparent mode can't sense the channel
};


