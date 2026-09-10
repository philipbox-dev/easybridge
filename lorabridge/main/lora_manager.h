#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

typedef void (*lora_rx_cb_t)(const uint8_t *data, int len, int rssi, float snr);

esp_err_t lora_manager_init(void);
void      lora_manager_set_rx_callback(lora_rx_cb_t cb);
esp_err_t lora_manager_send(const uint8_t *data, size_t len);
void      lora_manager_send_async(const uint8_t *data, size_t len);  // ★ NEW
bool      lora_manager_is_healthy(void);
esp_err_t lora_manager_set_speed(int sf);
int       lora_manager_get_speed(void);
// pwr_idx: 0=30dBm,1=27dBm,2=24dBm,3=21dBm (E220-22S)
esp_err_t lora_manager_set_tx_power(uint8_t pwr_idx);
int       lora_manager_get_tx_power(void);

// V2.7: PTT-сессия монополизирует радио (FSK) — на время
// приостанавливаем LoRa-задачи, очередь TX копится
void      lora_manager_suspend(void);
void      lora_manager_resume(void);
