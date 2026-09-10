#pragma once
// ============================================================
// EasyBridge board dispatcher — selects a board header based
// on Kconfig (CONFIG_EB_BOARD_*). Every board header defines:
//   BOARD_NAME            human-readable name
//   BOARD_HW_ID           1-byte hardware id (goes into NODE_HELLO)
//   BOARD_PROFILE_ID      id профиля в таблице hwcfg.c
//
// V2.9: пины больше НЕ берутся отсюда напрямую — драйверы читают
// hwcfg(). Board-заголовок задаёт лишь профиль по умолчанию для
// пустого NVS (см. hwcfg_profile_builtin()).
// ============================================================
#include "sdkconfig.h"

// hw_id registry (never reuse values — the app maps them to names/icons)
#define HW_ID_UNKNOWN      0
#define HW_ID_S3_E220      1
#define HW_ID_C6_E220      2
#define HW_ID_T3_V161      3
#define HW_ID_S3_E22M33S   4
// Самосбор «свой набор пинов»: конкретное железо описывает hwcfg,
// приложению достаточно знать, что это не заводская плата.
#define HW_ID_GENERIC_SPI  5
#define HW_ID_GENERIC_UART 6

#if   CONFIG_EB_BOARD_S3_E220
#include "boards/board_s3_e220.h"
#elif CONFIG_EB_BOARD_C6_E220
#include "boards/board_c6_e220.h"
#elif CONFIG_EB_BOARD_T3_V161
#include "boards/board_t3_v161.h"
#elif CONFIG_EB_BOARD_S3_E22M33S
#include "boards/board_s3_e22m33s.h"
#else
#error "No EasyBridge board selected — run menuconfig (EasyBridge Board)"
#endif
