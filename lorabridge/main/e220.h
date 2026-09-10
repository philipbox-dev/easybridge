#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

esp_err_t e220_init(void);
bool      e220_is_alive(void);
esp_err_t e220_configure(uint8_t air_rate);
esp_err_t e220_send(const uint8_t *data, size_t len);
int       e220_receive(uint8_t *buf, size_t buf_size, int *rssi_out, int timeout_ms);
esp_err_t e220_set_air_rate(uint8_t rate);
// pwr_idx: 0=30dBm, 1=27dBm, 2=24dBm, 3=21dBm
esp_err_t e220_set_tx_power(uint8_t pwr_idx);
