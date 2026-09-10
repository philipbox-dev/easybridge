#pragma once
// ============================================================
// Identity — persistent node identity in NVS.
// One binary per board; the user name is set from the EasyLink
// app ({"cmd":"setname"}) and survives reboots.
// node_id stays derived from the BT MAC (stable per chip).
// ============================================================
#include <stdint.h>

void        identity_init(void);            // load from NVS (call before BLE)
uint32_t    identity_node_id(void);         // 32-bit id from BT MAC
const char *identity_name(void);            // user name, "" if unset
const char *identity_ble_name(void);        // "EasyBridge-<name|XXXX>"
void        identity_set_name(const char *name);  // persist to NVS
uint8_t     identity_hw_id(void);           // BOARD_HW_ID
