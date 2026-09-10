#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

typedef void (*ble_rx_cb_t)(const char *json, size_t len);

esp_err_t ble_server_init(ble_rx_cb_t rx_callback);
void      ble_server_notify(const char *json);
void      ble_server_start_adv(void);
bool      ble_server_is_connected(void);

// V2.9.5: диагностика notify-потока (для evt:ptt_stats)
#include <stdint.h>
uint32_t  ble_server_notify_dropped(void);   // выкинуто: очередь полна
uint32_t  ble_server_notify_aborted(void);   // оборвано на середине (mbuf/GATT)
