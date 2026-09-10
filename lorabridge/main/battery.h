#pragma once
// ============================================================
// Battery monitor — ADC voltage divider (LilyGO T3 etc.)
// On boards without a battery every call is a cheap no-op:
// battery_get_percent() returns BATT_NONE.
// ============================================================
#include <stdint.h>
#include <stdbool.h>

#define BATT_NONE 0xFF   // "no battery on this board" marker

void    battery_init(void);
uint8_t battery_get_percent(void);   // 0-100, or BATT_NONE
int     battery_get_millivolts(void); // -1 if unavailable
