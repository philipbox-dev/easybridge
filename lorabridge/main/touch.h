#pragma once
// ============================================================
// Touch HAL (V2.9)
//
// Координаты наружу отдаются в логических пикселях кадра
// (0…127 × 0…63) — в той же системе, в которой рисует display.c.
// Пересчёт из сырых значений контроллера, поворот панели и
// калибровка живут здесь, чтобы интерфейс не знал, какой у него
// экран: резистивный XPT2046 320×240 или ёмкостный CST816 на
// круглом 240×240.
// ============================================================
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "hwcfg.h"

typedef enum {
    TOUCH_EV_DOWN = 0,
    TOUCH_EV_UP,
    TOUCH_EV_MOVE,
} touch_event_t;

typedef struct {
    uint8_t event;      // touch_event_t
    int16_t x, y;       // логические пиксели кадра
    uint16_t raw_x, raw_y;
    uint32_t t_ms;
} touch_point_t;

typedef void (*touch_cb_t)(const touch_point_t *p);

// TOUCH_NONE — успех: просто ничего не будет приходить.
esp_err_t touch_init(void);
bool      touch_present(void);
const char *touch_driver_name(void);

// Подписка на события. Колбэк зовётся из задачи опроса, не из ISR.
void touch_set_callback(touch_cb_t cb);

// Текущее состояние без ожидания события.
bool touch_read(touch_point_t *out);

// Калибровка: сохранить сырые значения углов в hwcfg.
// Мастер калибровки живёт в приложении, устройство только
// принимает результат.
esp_err_t touch_set_calibration(uint16_t x0, uint16_t y0,
                                uint16_t x1, uint16_t y1);
