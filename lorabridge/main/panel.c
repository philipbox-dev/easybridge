#include "panel.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "PANEL";

static uint8_t s_fb[PANEL_W * PANEL_PAGES];
static const panel_drv_t *s_drv = NULL;
static const display_cfg_t *s_cfg = NULL;

static const panel_drv_t *pick_driver(uint8_t kind)
{
    switch (kind) {
    case PANEL_SSD1306_I2C: return &panel_drv_ssd1306_i2c;
    case PANEL_SH1106_I2C:  return &panel_drv_sh1106_i2c;
    case PANEL_SSD1306_SPI: return &panel_drv_ssd1306_spi;
    case PANEL_ST7789_SPI:  return &panel_drv_st7789;
    case PANEL_ILI9341_SPI: return &panel_drv_ili9341;
    case PANEL_ST7735_SPI:  return &panel_drv_st7735;
    default:                return NULL;
    }
}

esp_err_t panel_init(void)
{
    s_cfg = &hwcfg()->disp;
    memset(s_fb, 0, sizeof(s_fb));

    if (s_cfg->kind == PANEL_NONE) {
        ESP_LOGI(TAG, "дисплея нет — рисование в никуда");
        return ESP_OK;
    }

    const panel_drv_t *drv = pick_driver(s_cfg->kind);
    if (!drv) {
        ESP_LOGE(TAG, "нет драйвера для панели %u", s_cfg->kind);
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_err_t e = drv->init(s_cfg);
    if (e != ESP_OK) {
        // Дисплей — не критичный узел: без него устройство работает,
        // поэтому не роняем систему, а честно логируем и живём дальше.
        ESP_LOGE(TAG, "%s не поднялся (%s) — работаем без экрана",
                 drv->name, esp_err_to_name(e));
        return e;
    }

    s_drv = drv;
    ESP_LOGI(TAG, "%s готов: %ux%u, поворот %u", drv->name,
             s_cfg->width, s_cfg->height, s_cfg->rotation);
    if (drv->backlight) drv->backlight(s_cfg, true);
    return ESP_OK;
}

bool panel_ready(void) { return s_drv != NULL; }
const char *panel_driver_name(void) { return s_drv ? s_drv->name : "none"; }
uint8_t *panel_buffer(void) { return s_fb; }

void panel_clear(void) { memset(s_fb, 0, sizeof(s_fb)); }

void panel_set_pixel(int x, int y, bool on)
{
    if (x < 0 || x >= PANEL_W || y < 0 || y >= PANEL_H) return;
    int idx = (y / 8) * PANEL_W + x;
    uint8_t mask = 1 << (y % 8);
    if (on) s_fb[idx] |= mask;
    else    s_fb[idx] &= ~mask;
}

void panel_fill_rect(int x, int y, int w, int h, bool on)
{
    for (int py = y; py < y + h; py++)
        for (int px = x; px < x + w; px++)
            panel_set_pixel(px, py, on);
}

void panel_draw_bitmap(int x, int y, const uint8_t *bitmap, int w, int h)
{
    int pages = (h + 7) / 8;
    for (int page = 0; page < pages; page++) {
        for (int col = 0; col < w; col++) {
            uint8_t byte = bitmap[page * w + col];
            for (int bit = 0; bit < 8; bit++) {
                if (byte & (1 << bit))
                    panel_set_pixel(x + col, y + page * 8 + bit, true);
            }
        }
    }
}

void panel_update(void)
{
    if (s_drv) s_drv->flush(s_cfg, s_fb);
}

void panel_backlight(bool on)
{
    if (s_drv && s_drv->backlight) s_drv->backlight(s_cfg, on);
}
