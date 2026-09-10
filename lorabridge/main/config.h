#pragma once

#include <stdint.h>

// ============================================================
// Плата: пины, тип радио, частота, батарея — main/boards/
// Выбор платы: menuconfig → EasyBridge Board (CONFIG_EB_BOARD_*)
// ============================================================
#include "boards/board.h"
#include "hwcfg.h"

// ============================================================
// Идентичность — в NVS (identity.c). Здесь только дефолты.
// Имя юзера задаётся из приложения EasyLink командой setname.
// ============================================================
#define BLE_NAME_PREFIX "EasyBridge"

// Legacy-протокол (обратная совместимость со старой прошивкой)
#define MY_DEVICE_ID    0x01
#define PEER_DEVICE_ID  0x02

#define NETWORK_ID  0xDA7A

// ============================================================
// LoRa параметры
// ============================================================
// Частота, канал и мощность живут в hwcfg() (V2.9) — они
// настраиваются из приложения, а не выбираются сборкой.
#define E220_TX_POWER       0           // 0=30dBm, 1=27dBm, 2=24dBm, 3=21dBm
#define E220_MODULE_ADDR    NETWORK_ID  // Адрес модуля = сеть

#define LORA_SF_SLOW        0           // 2.4 kbps (макс дальность)
#define LORA_SF_FAST        5           // 19.2 kbps (быстро)

// SPI-радио (SX127x/SX126x): общие параметры PHY.
// Sync 0x12 = private LoRa — то же, что у EBYTE-модулей.
#define LORA_PREAMBLE_LEN   8
#define LORA_SYNC_WORD      0x12

// ============================================================
// Тайминги
// ============================================================
#define HEARTBEAT_INTERVAL_MS   45000
#define HEARTBEAT_TIMEOUT_MS    120000
// Синхронная смена скорости всей сетью (B14): сколько секунд до перехода
// после анонса CHAN_SWITCH. Должно перекрывать airtime + relay-джиттер.
#define CHAN_SWITCH_COUNTDOWN_S 6
#define MSG_ACK_TIMEOUT_MS      10000
#define LORA_HEALTH_CHECK_MS    60000
#define FRAGMENT_TIMEOUT_MS     60000
#define TX_MUTEX_TIMEOUT_MS     15000

// ============================================================
// Протокол
// ============================================================
#define PACKET_HEADER_SIZE      10
#define MAX_PAYLOAD_PER_PACKET  189
#define MAX_FRAGMENTS           15

#define PKT_DISCOVERY_REQ   0x01
#define PKT_DISCOVERY_RESP  0x02
#define PKT_HEARTBEAT_PING  0x03
#define PKT_HEARTBEAT_PONG  0x04
#define PKT_TEXT_MSG        0x05
#define PKT_TEXT_MSG_ACK    0x06
#define PKT_TEXT_MSG_READ   0x07

#define BROADCAST_ID        0xFF

// ============================================================
// BLE
// ============================================================
#define BLE_MTU_SIZE  185
