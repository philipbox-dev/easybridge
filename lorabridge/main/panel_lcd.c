// ============================================================
// Цветные SPI-панели: ST7789, ILI9341, ST7735.
//
// Интерфейс EasyBridge нарисован под 128×64 монохрома, и
// переписывать всю вёрстку под 320×240 ради красоты — отдельная
// большая работа. Пока цветная панель показывает тот же кадр,
// раскрашенный в fg/bg и увеличенный целым коэффициентом по
// центру экрана: на 240×240 это ×3 (384×192 — влезает по ширине
// с полями), на 320×240 — ×2.
//
// Целый коэффициент выбран намеренно: дробное масштабирование
// монохромного шрифта 8×16 превращает его в кашу.
// ============================================================
#include "panel.h"
#include "spi_bus.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "PANEL-LCD";

#define MAX_SCALE 4
static uint16_t s_row[PANEL_W * MAX_SCALE];

static spi_device_handle_t s_dev;
static int s_scale, s_ox, s_oy;      // масштаб и отступы кадра на панели

// ── Транспорт ───────────────────────────────────────────────
static void lcd_cmd(const display_cfg_t *c, uint8_t cmd)
{
    gpio_set_level(c->dc, 0);
    spi_transaction_t t = { .length = 8, .tx_buffer = &cmd };
    spi_device_polling_transmit(s_dev, &t);
}

static void lcd_data(const display_cfg_t *c, const void *data, size_t len)
{
    if (!len) return;
    gpio_set_level(c->dc, 1);
    spi_transaction_t t = { .length = len * 8, .tx_buffer = data };
    spi_device_polling_transmit(s_dev, &t);
}

static void lcd_cmd_d(const display_cfg_t *c, uint8_t cmd,
                      const uint8_t *args, size_t n)
{
    lcd_cmd(c, cmd);
    if (n) lcd_data(c, args, n);
}

// ── Геометрия ───────────────────────────────────────────────
static void eff_size(const display_cfg_t *c, int *w, int *h)
{
    if (c->rotation & 1) { *w = c->height; *h = c->width; }
    else                 { *w = c->width;  *h = c->height; }
}

// MADCTL: MY MX MV ML BGR MH - -
static uint8_t madctl_for(const display_cfg_t *c)
{
    static const uint8_t rot[4] = { 0x00, 0x60, 0xC0, 0xA0 };
    uint8_t v = rot[c->rotation & 3];
    if (c->bgr) v |= 0x08;
    return v;
}

static void set_window(const display_cfg_t *c, int x, int y, int w, int h)
{
    // Смещение окна: у 240×240 ST7789 видимая область не с нуля,
    // и без x_offset/y_offset кадр уезжает за край.
    int xo = c->x_offset, yo = c->y_offset;
    uint8_t buf[4];
    buf[0] = (x + xo) >> 8; buf[1] = (x + xo) & 0xFF;
    buf[2] = (x + xo + w - 1) >> 8; buf[3] = (x + xo + w - 1) & 0xFF;
    lcd_cmd_d(c, 0x2A, buf, 4);                 // CASET
    buf[0] = (y + yo) >> 8; buf[1] = (y + yo) & 0xFF;
    buf[2] = (y + yo + h - 1) >> 8; buf[3] = (y + yo + h - 1) & 0xFF;
    lcd_cmd_d(c, 0x2B, buf, 4);                 // RASET
    lcd_cmd(c, 0x2C);                           // RAMWR
}

static void fill_screen(const display_cfg_t *c, uint16_t color)
{
    int w, h; eff_size(c, &w, &h);
    uint16_t be = (color >> 8) | (color << 8);
    for (int i = 0; i < PANEL_W * MAX_SCALE; i++) s_row[i] = be;
    set_window(c, 0, 0, w, h);
    int per = PANEL_W * MAX_SCALE;
    for (int y = 0; y < h; y++) {
        int left = w;
        while (left > 0) {
            int n = left > per ? per : left;
            lcd_data(c, s_row, n * 2);
            left -= n;
        }
    }
}

// ── Инициализация ───────────────────────────────────────────
static esp_err_t lcd_common_init(const display_cfg_t *c, uint8_t ctrl)
{
    spi_host_device_t host;
    esp_err_t e = spi_bus_acquire(c->spi_host, c->sck, c->mosi, c->miso,
                                  PANEL_W * MAX_SCALE * 2 + 16, &host);
    if (e != ESP_OK) return e;

    uint64_t mask = (1ULL << c->dc);
    if (c->rst >= 0) mask |= (1ULL << c->rst);
    if (c->bl  >= 0) mask |= (1ULL << c->bl);
    gpio_config_t io = { .pin_bit_mask = mask, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&io);

    spi_device_interface_config_t dev = {
        .clock_speed_hz = c->bus_hz ? c->bus_hz : 40000000,
        .mode = 0,
        .spics_io_num = c->cs,
        .queue_size = 4,
    };
    e = spi_bus_add_device(host, &dev, &s_dev);
    if (e != ESP_OK) return e;

    if (c->rst >= 0) {
        gpio_set_level(c->rst, 0); vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(c->rst, 1); vTaskDelay(pdMS_TO_TICKS(120));
    }

    lcd_cmd(c, 0x01);                       // SWRESET
    vTaskDelay(pdMS_TO_TICKS(150));
    lcd_cmd(c, 0x11);                       // SLPOUT
    vTaskDelay(pdMS_TO_TICKS(120));

    uint8_t colmod = 0x55;                  // 16 бит на пиксель
    lcd_cmd_d(c, 0x3A, &colmod, 1);
    uint8_t mad = madctl_for(c);
    lcd_cmd_d(c, 0x36, &mad, 1);

    if (ctrl == PANEL_ILI9341_SPI) {
        // Питание и тайминги ILI9341: значения из даташита,
        // с ними панель не мерцает на 40 МГц.
        const uint8_t pwr1[] = { 0x23 };            lcd_cmd_d(c, 0xC0, pwr1, 1);
        const uint8_t pwr2[] = { 0x10 };            lcd_cmd_d(c, 0xC1, pwr2, 1);
        const uint8_t vcom1[] = { 0x3E, 0x28 };     lcd_cmd_d(c, 0xC5, vcom1, 2);
        const uint8_t vcom2[] = { 0x86 };           lcd_cmd_d(c, 0xC7, vcom2, 1);
        const uint8_t frm[]  = { 0x00, 0x18 };      lcd_cmd_d(c, 0xB1, frm, 2);
        const uint8_t dfc[]  = { 0x08, 0x82, 0x27 };lcd_cmd_d(c, 0xB6, dfc, 3);
    }

    lcd_cmd(c, c->invert ? 0x21 : 0x20);    // INVON / INVOFF
    lcd_cmd(c, 0x13);                       // NORON
    lcd_cmd(c, 0x29);                       // DISPON
    vTaskDelay(pdMS_TO_TICKS(20));

    // Масштаб и центровка. scale=0 — подобрать самим.
    int w, h; eff_size(c, &w, &h);
    s_scale = c->scale;
    if (s_scale == 0) {
        int sx = w / PANEL_W, sy = h / PANEL_H;
        s_scale = sx < sy ? sx : sy;
    }
    if (s_scale < 1) s_scale = 1;
    if (s_scale > MAX_SCALE) s_scale = MAX_SCALE;
    s_ox = (w - PANEL_W * s_scale) / 2;
    s_oy = (h - PANEL_H * s_scale) / 2;
    if (s_ox < 0) s_ox = 0;
    if (s_oy < 0) s_oy = 0;

    fill_screen(c, c->bg);
    ESP_LOGI(TAG, "панель %dx%d, кадр 128x64 ×%d, поля %d/%d",
             w, h, s_scale, s_ox, s_oy);
    return ESP_OK;
}

static esp_err_t st7789_init(const display_cfg_t *c)  { return lcd_common_init(c, PANEL_ST7789_SPI); }
static esp_err_t ili9341_init(const display_cfg_t *c) { return lcd_common_init(c, PANEL_ILI9341_SPI); }
static esp_err_t st7735_init(const display_cfg_t *c)  { return lcd_common_init(c, PANEL_ST7735_SPI); }

// ── Вывод кадра ─────────────────────────────────────────────
static void lcd_flush(const display_cfg_t *c, const uint8_t *fb)
{
    // Панель ждёт big-endian RGB565, а SPI шлёт байты как есть.
    uint16_t fg = (c->fg >> 8) | (c->fg << 8);
    uint16_t bg = (c->bg >> 8) | (c->bg << 8);
    int rw = PANEL_W * s_scale;

    set_window(c, s_ox, s_oy, rw, PANEL_H * s_scale);
    for (int y = 0; y < PANEL_H; y++) {
        const uint8_t *page = fb + (y / 8) * PANEL_W;
        uint8_t bit = 1 << (y % 8);
        for (int x = 0; x < PANEL_W; x++) {
            uint16_t px = (page[x] & bit) ? fg : bg;
            for (int s = 0; s < s_scale; s++) s_row[x * s_scale + s] = px;
        }
        // Строку повторяем scale раз — окно уже выставлено, RAMWR
        // сам двигает курсор.
        for (int s = 0; s < s_scale; s++) lcd_data(c, s_row, rw * 2);
    }
}

static void lcd_backlight(const display_cfg_t *c, bool on)
{
    if (c->bl < 0) return;
    gpio_set_level(c->bl, c->bl_active_low ? !on : on);
}

const panel_drv_t panel_drv_st7789 = {
    .name = "ST7789", .init = st7789_init, .flush = lcd_flush, .backlight = lcd_backlight,
};
const panel_drv_t panel_drv_ili9341 = {
    .name = "ILI9341", .init = ili9341_init, .flush = lcd_flush, .backlight = lcd_backlight,
};
const panel_drv_t panel_drv_st7735 = {
    .name = "ST7735", .init = st7735_init, .flush = lcd_flush, .backlight = lcd_backlight,
};
