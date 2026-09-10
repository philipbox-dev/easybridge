#pragma once
// ESP32-C6 (WROOM-1 devkit) + EBYTE E220-400T30D.
// Free-pin wiring suggestion — adjust here if you solder differently.
// Avoid: GPIO8/9 (strapping), GPIO12/13 (USB-JTAG), GPIO16/17 (UART0).

#define BOARD_NAME         "C6+E220"
#define BOARD_PROFILE_ID    "c6_e220"
#define BOARD_HW_ID        HW_ID_C6_E220

// V2.9: пины, радио, дисплей и батарея переехали в профиль
// hwcfg.c с этим же id — там их видно все разом, и оттуда же
// их берёт таблица пинов на странице прошивки.
