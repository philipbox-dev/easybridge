#include "touch.h"
#include "panel.h"
#include "spi_bus.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "TOUCH";

#define I2C_NUM        I2C_NUM_0
#define POLL_MS        20
#define DEBOUNCE_MS    30

static const touch_cfg_t *s_cfg;
static const char *s_name = "none";
static touch_cb_t   s_cb;
static spi_device_handle_t s_spi;
static bool s_down;
static touch_point_t s_last;

// ── XPT2046 ─────────────────────────────────────────────────
// Резистивная плёнка шумит: одно измерение может дать что угодно.
// Берём медиану трёх — дёшево и убирает выбросы, в отличие от
// среднего, которое выброс как раз размазывает.
static uint16_t med3(uint16_t a, uint16_t b, uint16_t c)
{
    if (a > b) { uint16_t t = a; a = b; b = t; }
    if (b > c) { uint16_t t = b; b = c; c = t; }
    if (a > b) { uint16_t t = a; a = b; b = t; }
    return b;
}

static uint16_t xpt_read(uint8_t cmd)
{
    uint8_t tx[3] = { cmd, 0x00, 0x00 };
    uint8_t rx[3] = { 0 };
    spi_transaction_t t = {
        .length = 24, .tx_buffer = tx, .rx_buffer = rx,
    };
    if (spi_device_polling_transmit(s_spi, &t) != ESP_OK) return 0;
    return (uint16_t)(((rx[1] << 8) | rx[2]) >> 3);   // 12 бит
}

static bool xpt_sample(uint16_t *rx_, uint16_t *ry)
{
    if (s_cfg->irq >= 0 && gpio_get_level(s_cfg->irq)) return false;

    uint16_t xs[3], ys[3];
    for (int i = 0; i < 3; i++) {
        ys[i] = xpt_read(0x90);   // Y
        xs[i] = xpt_read(0xD0);   // X
    }
    uint16_t x = med3(xs[0], xs[1], xs[2]);
    uint16_t y = med3(ys[0], ys[1], ys[2]);
    // На отпускании АЦП выдаёт края шкалы — это не касание.
    if (x < 100 || x > 4000 || y < 100 || y > 4000) return false;
    *rx_ = x; *ry = y;
    return true;
}

// ── Ёмкостные по I2C ────────────────────────────────────────
static bool i2c_rd(uint8_t addr, uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_write_read_device(I2C_NUM, addr, &reg, 1, buf, n,
                                        pdMS_TO_TICKS(50)) == ESP_OK;
}

static bool cap_sample(uint16_t *rx_, uint16_t *ry)
{
    uint8_t addr = s_cfg->i2c_addr;
    uint8_t b[6];
    switch (s_cfg->kind) {
    case TOUCH_FT6236_I2C:
        if (!i2c_rd(addr ? addr : 0x38, 0x02, b, 5)) return false;
        if ((b[0] & 0x0F) == 0) return false;            // нет касаний
        *rx_ = ((b[1] & 0x0F) << 8) | b[2];
        *ry  = ((b[3] & 0x0F) << 8) | b[4];
        return true;
    case TOUCH_CST816_I2C:
        if (!i2c_rd(addr ? addr : 0x15, 0x02, b, 5)) return false;
        if (b[0] == 0) return false;
        *rx_ = ((b[1] & 0x0F) << 8) | b[2];
        *ry  = ((b[3] & 0x0F) << 8) | b[4];
        return true;
    case TOUCH_GT911_I2C: {
        uint8_t st[1];
        // GT911 адресуется 16-битным регистром — общий i2c_rd не подходит.
        uint8_t reg[2] = { 0x81, 0x4E };
        if (i2c_master_write_read_device(I2C_NUM, addr ? addr : 0x5D, reg, 2,
                                         st, 1, pdMS_TO_TICKS(50)) != ESP_OK)
            return false;
        if (!(st[0] & 0x80) || (st[0] & 0x0F) == 0) return false;
        uint8_t preg[2] = { 0x81, 0x50 };
        if (i2c_master_write_read_device(I2C_NUM, addr ? addr : 0x5D, preg, 2,
                                         b, 4, pdMS_TO_TICKS(50)) != ESP_OK)
            return false;
        *rx_ = b[0] | (b[1] << 8);
        *ry  = b[2] | (b[3] << 8);
        // Статус надо сбросить, иначе контроллер не отдаст следующее касание.
        uint8_t clr[3] = { 0x81, 0x4E, 0x00 };
        i2c_master_write_to_device(I2C_NUM, addr ? addr : 0x5D, clr, 3,
                                   pdMS_TO_TICKS(50));
        return true;
    }
    default:
        return false;
    }
}

// ── Сырое → логическое ──────────────────────────────────────
static void map_point(uint16_t rx_, uint16_t ry, int16_t *lx, int16_t *ly)
{
    uint16_t x0 = s_cfg->cal_x0, x1 = s_cfg->cal_x1;
    uint16_t y0 = s_cfg->cal_y0, y1 = s_cfg->cal_y1;
    if (x1 <= x0) { x0 = 0; x1 = 4095; }
    if (y1 <= y0) { y0 = 0; y1 = 4095; }

    int32_t xv = ((int32_t)rx_ - x0) * PANEL_W / (int32_t)(x1 - x0);
    int32_t yv = ((int32_t)ry - y0) * PANEL_H / (int32_t)(y1 - y0);

    if (s_cfg->swap_xy) {
        int32_t t = xv; xv = yv; yv = t;
    }
    if (s_cfg->invert_x) xv = PANEL_W - 1 - xv;
    if (s_cfg->invert_y) yv = PANEL_H - 1 - yv;

    if (xv < 0) xv = 0;
    if (xv >= PANEL_W) xv = PANEL_W - 1;
    if (yv < 0) yv = 0;
    if (yv >= PANEL_H) yv = PANEL_H - 1;
    *lx = (int16_t)xv; *ly = (int16_t)yv;
}

static bool sample(touch_point_t *p)
{
    uint16_t rx_ = 0, ry = 0;
    bool got = (s_cfg->kind == TOUCH_XPT2046_SPI) ? xpt_sample(&rx_, &ry)
                                                  : cap_sample(&rx_, &ry);
    if (!got) return false;
    p->raw_x = rx_; p->raw_y = ry;
    map_point(rx_, ry, &p->x, &p->y);
    p->t_ms = (uint32_t)(esp_timer_get_time() / 1000);
    return true;
}

static void touch_task(void *arg)
{
    (void)arg;
    uint32_t last_change = 0;
    for (;;) {
        touch_point_t p = { 0 };
        bool now = sample(&p);
        uint32_t t = (uint32_t)(esp_timer_get_time() / 1000);

        if (now != s_down) {
            // Дребезг на резистивной плёнке даёт ложные up/down парами;
            // фиксируем смену состояния только если она устоялась.
            if (t - last_change >= DEBOUNCE_MS) {
                s_down = now;
                last_change = t;
                p.event = now ? TOUCH_EV_DOWN : TOUCH_EV_UP;
                if (!now) { p.x = s_last.x; p.y = s_last.y; p.t_ms = t; }
                s_last = p;
                if (s_cb) s_cb(&p);
            }
        } else if (now && (p.x != s_last.x || p.y != s_last.y)) {
            p.event = TOUCH_EV_MOVE;
            s_last = p;
            if (s_cb) s_cb(&p);
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

esp_err_t touch_init(void)
{
    s_cfg = &hwcfg()->touch;
    if (s_cfg->kind == TOUCH_NONE) return ESP_OK;

    if (s_cfg->kind == TOUCH_XPT2046_SPI) {
        spi_host_device_t host;
        esp_err_t e = spi_bus_acquire(s_cfg->spi_host, s_cfg->sck, s_cfg->mosi,
                                      s_cfg->miso, 64, &host);
        if (e != ESP_OK) return e;
        spi_device_interface_config_t dev = {
            // XPT2046 медленный: выше 2 МГц он начинает врать.
            .clock_speed_hz = 2000000,
            .mode = 0,
            .spics_io_num = s_cfg->cs,
            .queue_size = 2,
        };
        e = spi_bus_add_device(host, &dev, &s_spi);
        if (e != ESP_OK) return e;
        if (s_cfg->irq >= 0) {
            gpio_config_t io = {
                .pin_bit_mask = 1ULL << s_cfg->irq,
                .mode = GPIO_MODE_INPUT,
                .pull_up_en = GPIO_PULLUP_ENABLE,
            };
            gpio_config(&io);
        }
        s_name = "XPT2046";
    } else {
        // I2C-шину мог уже поднять дисплей — install вернёт INVALID_STATE,
        // и это нормально.
        i2c_config_t conf = {
            .mode = I2C_MODE_MASTER,
            .sda_io_num = s_cfg->sda,
            .scl_io_num = s_cfg->scl,
            .sda_pullup_en = GPIO_PULLUP_ENABLE,
            .scl_pullup_en = GPIO_PULLUP_ENABLE,
            .master.clk_speed = 400000,
        };
        i2c_param_config(I2C_NUM, &conf);
        esp_err_t e = i2c_driver_install(I2C_NUM, I2C_MODE_MASTER, 0, 0, 0);
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;
        s_name = hwcfg_touch_name(s_cfg->kind);
    }

    if (xTaskCreate(touch_task, "touch", 3072, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "не удалось создать задачу опроса");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "%s готов", s_name);
    return ESP_OK;
}

bool touch_present(void) { return s_cfg && s_cfg->kind != TOUCH_NONE; }
const char *touch_driver_name(void) { return s_name; }
void touch_set_callback(touch_cb_t cb) { s_cb = cb; }

bool touch_read(touch_point_t *out)
{
    if (!touch_present() || !s_down) return false;
    if (out) *out = s_last;
    return true;
}

esp_err_t touch_set_calibration(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    if (x1 <= x0 || y1 <= y0) return ESP_ERR_INVALID_ARG;
    hwcfg_t c = *hwcfg();
    c.touch.cal_x0 = x0; c.touch.cal_y0 = y0;
    c.touch.cal_x1 = x1; c.touch.cal_y1 = y1;
    char err[96];
    esp_err_t e = hwcfg_save(&c, err, sizeof(err));
    if (e != ESP_OK) ESP_LOGE(TAG, "калибровка не сохранена: %s", err);
    return e;
}
