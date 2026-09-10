#pragma once
// ============================================================
// Panel HAL — куда выливается кадр (V2.9)
//
// display.c рисует интерфейс в монохромный кадровый буфер
// 128×64 — исторический размер SSD1306, и менять его незачем:
// вся вёрстка экранов на него рассчитана. Панель — это только
// «как показать этот кадр». SSD1306 получает его один в один,
// цветной ST7789 — раскрашенным в fg/bg и растянутым в scale
// раз по центру.
//
// Что за панель, на каких пинах и какого размера — берётся из
// hwcfg() при panel_init(); ничего не выбирается на компиляции.
// ============================================================
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "hwcfg.h"

// Логический кадр. Совпадает со старым SSD1306_WIDTH/HEIGHT.
#define PANEL_W  128
#define PANEL_H  64
#define PANEL_PAGES (PANEL_H / 8)

// Бэкенд панели. flush() получает страничный монобуфер
// (PANEL_W × PANEL_PAGES, бит = пиксель, младший бит — верхний).
typedef struct {
    const char *name;
    esp_err_t (*init)(const display_cfg_t *cfg);
    void      (*flush)(const display_cfg_t *cfg, const uint8_t *fb);
    void      (*backlight)(const display_cfg_t *cfg, bool on);
} panel_drv_t;

// Инициализация по hwcfg(). PANEL_NONE — тоже успех: рисование
// просто уходит в никуда, чтобы display.c не оброс проверками.
esp_err_t panel_init(void);
bool      panel_ready(void);
const char *panel_driver_name(void);

void panel_clear(void);
void panel_set_pixel(int x, int y, bool on);
void panel_fill_rect(int x, int y, int w, int h, bool on);
void panel_draw_bitmap(int x, int y, const uint8_t *bitmap, int w, int h);
void panel_update(void);            // вылить кадр на железо
void panel_backlight(bool on);
uint8_t *panel_buffer(void);

// Бэкенды (каждый компилируется всегда, выбирается в рантайме —
// они мелкие, а один бинарник на все платы того стоит).
extern const panel_drv_t panel_drv_ssd1306_i2c;
extern const panel_drv_t panel_drv_sh1106_i2c;
extern const panel_drv_t panel_drv_ssd1306_spi;
extern const panel_drv_t panel_drv_st7789;
extern const panel_drv_t panel_drv_ili9341;
extern const panel_drv_t panel_drv_st7735;
