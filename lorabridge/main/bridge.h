#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

void bridge_on_lora_rx(const uint8_t *data, int len, int rssi, float snr);
void bridge_on_ble_rx(const char *json, size_t len);
void bridge_on_peer_status(bool online, int rssi);
void bridge_init(void);
void bridge_announce(void);  // Send proto25 NODE_HELLO broadcast
