#pragma once
// ESP32-S3 (YD-ESP32-23) + EBYTE E220-400T30D — the original EasyBridge rig.

#define BOARD_NAME         "S3+E220"
#define BOARD_PROFILE_ID    "s3_e220"
#define BOARD_HW_ID        HW_ID_S3_E220

// V2.9: пины, радио, дисплей и батарея переехали в профиль
// hwcfg.c с этим же id — там их видно все разом, и оттуда же
// их берёт таблица пинов на странице прошивки.
