// ============================================================
// Монохромные панели: SSD1306 (I2C и SPI) и SH1106 (I2C).
//
// SH1106 — почти тот же контроллер, но с 132 колонками RAM при
// 128 видимых: кадр надо сдвигать на 2 пикселя и лить постранично,
// автоинкремент адреса у него работает иначе. Отсюда две ветки
// flush вместо одной.
// ============================================================
#include "panel.h"
#include "spi_bus.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "PANEL-MONO";

#define I2C_NUM  I2C_NUM_0

static spi_device_handle_t s_spi;
static bool s_i2c_ready;

// ── I2C ─────────────────────────────────────────────────────
static esp_err_t i2c_cmd(uint8_t addr, uint8_t cmd)
{
    uint8_t d[2] = { 0x00, cmd };
    return i2c_master_write_to_device(I2C_NUM, addr, d, 2, pdMS_TO_TICKS(100));
}

static esp_err_t i2c_data(uint8_t addr, const uint8_t *data, size_t len)
{
    uint8_t *buf = malloc(len + 1);
    if (!buf) return ESP_ERR_NO_MEM;
    buf[0] = 0x40;
    memcpy(buf + 1, data, len);
    esp_err_t e = i2c_master_write_to_device(I2C_NUM, addr, buf, len + 1,
                                             pdMS_TO_TICKS(100));
    free(buf);
    return e;
}

static esp_err_t i2c_bring_up(const display_cfg_t *c)
{
    if (s_i2c_ready) return ESP_OK;
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = c->sda,
        .scl_io_num = c->scl,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = c->bus_hz ? c->bus_hz : 400000,
    };
    esp_err_t e = i2c_param_config(I2C_NUM, &conf);
    if (e != ESP_OK) return e;
    e = i2c_driver_install(I2C_NUM, I2C_MODE_MASTER, 0, 0, 0);
    // Шину мог поднять уже кто-то (например, ёмкостный тач на тех же пинах).
    if (e == ESP_ERR_INVALID_STATE) e = ESP_OK;
    if (e == ESP_OK) s_i2c_ready = true;
    return e;
}

// Общая для SSD1306/SH1106 стартовая последовательность.
// Различия — в charge pump и множителе, они вынесены параметрами.
static void mono_init_seq(void (*cmd)(void *ctx, uint8_t), void *ctx,
                          const display_cfg_t *c, bool sh1106)
{
    cmd(ctx, 0xAE);                    // off
    cmd(ctx, 0xD5); cmd(ctx, 0x80);    // clock
    cmd(ctx, 0xA8); cmd(ctx, 0x3F);    // mux 1/64
    cmd(ctx, 0xD3); cmd(ctx, 0x00);    // offset
    cmd(ctx, 0x40);                    // start line 0
    if (sh1106) {
        cmd(ctx, 0xAD); cmd(ctx, 0x8B);  // DC-DC on
        cmd(ctx, 0x32);                  // pump 8.0 V
    } else {
        cmd(ctx, 0x8D); cmd(ctx, 0x14);  // charge pump on
        cmd(ctx, 0x20); cmd(ctx, 0x00);  // horizontal addressing
    }
    // Поворот на 180° делается зеркалированием сегментов и строк —
    // у монохромных контроллеров других углов нет.
    bool flip = (c->rotation == 2);
    cmd(ctx, flip ? 0xA0 : 0xA1);      // segment remap
    cmd(ctx, flip ? 0xC0 : 0xC8);      // COM scan direction
    cmd(ctx, 0xDA); cmd(ctx, 0x12);    // COM pins
    cmd(ctx, 0x81); cmd(ctx, 0xCF);    // contrast
    cmd(ctx, 0xD9); cmd(ctx, 0xF1);    // precharge
    cmd(ctx, 0xDB); cmd(ctx, 0x40);    // vcom
    cmd(ctx, 0xA4);                    // follow RAM
    cmd(ctx, c->invert ? 0xA7 : 0xA6);
    cmd(ctx, 0xAF);                    // on
}

static void i2c_cmd_cb(void *ctx, uint8_t v) { i2c_cmd(*(uint8_t *)ctx, v); }

static esp_err_t ssd1306_i2c_init(const display_cfg_t *c)
{
    esp_err_t e = i2c_bring_up(c);
    if (e != ESP_OK) { ESP_LOGE(TAG, "I2C не поднялся: %s", esp_err_to_name(e)); return e; }
    vTaskDelay(pdMS_TO_TICKS(100));
    uint8_t addr = c->i2c_addr ? c->i2c_addr : 0x3C;
    // Проверяем, что панель вообще откликается: иначе весь дальнейший
    // вывод уйдёт в тишину, а мы будем думать, что экран работает.
    if (i2c_cmd(addr, 0xAE) != ESP_OK) {
        ESP_LOGE(TAG, "нет ответа по адресу 0x%02X (SDA=%d SCL=%d)",
                 addr, c->sda, c->scl);
        return ESP_ERR_NOT_FOUND;
    }
    mono_init_seq(i2c_cmd_cb, &addr, c, c->kind == PANEL_SH1106_I2C);
    return ESP_OK;
}

static void ssd1306_i2c_flush(const display_cfg_t *c, const uint8_t *fb)
{
    uint8_t addr = c->i2c_addr ? c->i2c_addr : 0x3C;
    i2c_cmd(addr, 0x21); i2c_cmd(addr, 0); i2c_cmd(addr, PANEL_W - 1);
    i2c_cmd(addr, 0x22); i2c_cmd(addr, 0); i2c_cmd(addr, PANEL_PAGES - 1);
    i2c_data(addr, fb, PANEL_W * PANEL_PAGES);
}

static void sh1106_i2c_flush(const display_cfg_t *c, const uint8_t *fb)
{
    uint8_t addr = c->i2c_addr ? c->i2c_addr : 0x3C;
    uint8_t col = (uint8_t)(c->x_offset ? c->x_offset : 2);
    for (int page = 0; page < PANEL_PAGES; page++) {
        i2c_cmd(addr, 0xB0 | page);
        i2c_cmd(addr, 0x00 | (col & 0x0F));
        i2c_cmd(addr, 0x10 | (col >> 4));
        i2c_data(addr, fb + page * PANEL_W, PANEL_W);
    }
}

// ── SPI ─────────────────────────────────────────────────────
static void spi_write(const display_cfg_t *c, bool is_cmd,
                      const uint8_t *data, size_t len)
{
    if (!s_spi || len == 0) return;
    gpio_set_level(c->dc, is_cmd ? 0 : 1);
    spi_transaction_t t = { .length = len * 8, .tx_buffer = data };
    spi_device_polling_transmit(s_spi, &t);
}

static void spi_cmd_cb(void *ctx, uint8_t v)
{
    spi_write((const display_cfg_t *)ctx, true, &v, 1);
}

static esp_err_t ssd1306_spi_init(const display_cfg_t *c)
{
    spi_host_device_t host;
    esp_err_t e = spi_bus_acquire(c->spi_host, c->sck, c->mosi, c->miso,
                                  PANEL_W * PANEL_PAGES + 8, &host);
    if (e != ESP_OK) return e;

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << c->dc) | (c->rst >= 0 ? (1ULL << c->rst) : 0),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);

    spi_device_interface_config_t dev = {
        .clock_speed_hz = c->bus_hz ? c->bus_hz : 8000000,
        .mode = 0,
        .spics_io_num = c->cs,
        .queue_size = 3,
    };
    e = spi_bus_add_device(host, &dev, &s_spi);
    if (e != ESP_OK) return e;

    if (c->rst >= 0) {
        gpio_set_level(c->rst, 0); vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(c->rst, 1); vTaskDelay(pdMS_TO_TICKS(50));
    }
    mono_init_seq(spi_cmd_cb, (void *)c, c, false);
    return ESP_OK;
}

static void ssd1306_spi_flush(const display_cfg_t *c, const uint8_t *fb)
{
    uint8_t hdr[6] = { 0x21, 0, PANEL_W - 1, 0x22, 0, PANEL_PAGES - 1 };
    spi_write(c, true, hdr, sizeof(hdr));
    spi_write(c, false, fb, PANEL_W * PANEL_PAGES);
}

const panel_drv_t panel_drv_ssd1306_i2c = {
    .name = "SSD1306/I2C", .init = ssd1306_i2c_init, .flush = ssd1306_i2c_flush,
};
const panel_drv_t panel_drv_sh1106_i2c = {
    .name = "SH1106/I2C",  .init = ssd1306_i2c_init, .flush = sh1106_i2c_flush,
};
const panel_drv_t panel_drv_ssd1306_spi = {
    .name = "SSD1306/SPI", .init = ssd1306_spi_init, .flush = ssd1306_spi_flush,
};
