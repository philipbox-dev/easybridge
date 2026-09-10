#pragma once
// ESP32-S3 + EBYTE E22-400M33S — raw SX1268 over SPI with a 2 W PA.
// Free-pin wiring suggestion — adjust here to match your soldering.
// GPIO35-37 avoided (used by octal PSRAM on N16R8 modules).
// The module has TXEN/RXEN pins that MUST toggle around TX/RX
// (they drive the RF switch in front of the PA/LNA).

#define BOARD_NAME          "S3+E22-M33S"
#define BOARD_PROFILE_ID    "s3_e22m33s"
#define BOARD_HW_ID         HW_ID_S3_E22M33S

// V2.9: пины, радио, дисплей и батарея переехали в профиль
// hwcfg.c с этим же id — там их видно все разом, и оттуда же
// их берёт таблица пинов на странице прошивки.
