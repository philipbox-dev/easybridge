#pragma once
// LilyGO T3 v1.6.1 (TTGO LoRa32) — ESP32-classic + SX1276 on SPI,
// onboard OLED, Li-ion charger + battery voltage divider.
// NOTE: these units are the 868/915 MHz variant → they form their own
// sub-net and cannot talk to the 433 MHz E220 devices (by design).

#define BOARD_NAME          "T3-v1.6.1"
#define BOARD_PROFILE_ID    "t3_v161"
#define BOARD_HW_ID         HW_ID_T3_V161

// V2.9: пины, радио, дисплей и батарея переехали в профиль
// hwcfg.c с этим же id — там их видно все разом, и оттуда же
// их берёт таблица пинов на странице прошивки.
