#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef void (*peer_status_cb_t)(bool online, int rssi);

void peer_manager_init(peer_status_cb_t cb);
void peer_manager_on_ping(void);
void peer_manager_on_pong(int rssi);
void peer_manager_on_discovery_resp(const char *name, int rssi);
void peer_manager_send_ping(void);
void peer_manager_send_discovery(void);
bool peer_manager_is_online(void);
int  peer_manager_get_rssi(void);
