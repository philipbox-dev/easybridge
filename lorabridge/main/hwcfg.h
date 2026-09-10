#pragma once
// ============================================================
// EasyBridge hwcfg — рантайм-описание железа (V2.9)
//
// До V2.9 пины, радио и дисплей выбирались на этапе сборки
// (boards/*.h + Kconfig): под каждую комбинацию — свой бинарник.
// Теперь железо описывает один struct, который лежит в NVS и
// правится из приложения / веб-флешера. Компилированные
// board-заголовки остались, но только как СИДЫ профилей по
// умолчанию: пустой NVS → берём профиль сборки.
//
// Смена конфигурации требует перезагрузки: драйверы читают
// hwcfg() один раз при init, менять пины на лету незачем и
// небезопасно.
// ============================================================
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define HWCFG_VERSION      1
#define HWCFG_PIN_NONE     ((int8_t)-1)
#define HWCFG_ID_LEN       24
#define HWCFG_NAME_LEN     24

// ── Радио ───────────────────────────────────────────────────
// Значения не переиспользовать: они уезжают в эфир (NODE_HELLO)
// и в приложение.
typedef enum {
    RADIO_NONE      = 0,
    RADIO_E220      = 1,   // EBYTE E220-xxxTxxD, UART (LLCC68 внутри)
    RADIO_SX127X    = 2,   // Ra-02 / RFM95 / SX1276 / SX1278, SPI
    RADIO_SX126X    = 3,   // SX1262 / SX1268 / E22-xxxM, SPI
    RADIO_SX128X    = 4,   // 2.4 ГГц LoRa (E28), SPI      — плейсхолдер
    RADIO_LR11XX    = 5,   // LR1110/1120, SPI             — плейсхолдер
    RADIO_HALOW     = 6,   // 802.11ah (MM6108), SPI/SDIO  — плейсхолдер
    RADIO_KIND_MAX
} radio_kind_t;

// Диапазон. Радио объявляет, какие умеет; hwcfg_validate() бьёт
// по рукам за 868 МГц на 433-й железке.
typedef enum {
    BAND_433 = 0,   // 433.05–434.79 МГц, ISM
    BAND_470,       // 470–510 МГц (CN)
    BAND_868,       // 863–870 МГц (EU SRD)
    BAND_915,       // 902–928 МГц (US/AU)
    BAND_2G4,       // 2400–2500 МГц (LoRa 2.4G)
    BAND_HALOW_SUB1,// 863–868 / 902–928 для 802.11ah
    BAND_MAX
} band_t;

// Пины радио. Один плоский набор на все семейства: SPI-радио
// заполняют верхнюю часть, UART-радио — нижнюю. -1 = не подключено.
typedef struct {
    int8_t sck, miso, mosi, cs;     // SPI
    int8_t rst, busy;               // сброс / BUSY (SX126x, LR11xx)
    int8_t dio0, dio1, dio2;        // прерывания
    int8_t txen, rxen;              // управление RF-свитчём (E22/M33S)
    int8_t tx, rx, aux, m0, m1;     // UART (E220/E22-T)
} radio_pins_t;

typedef struct {
    uint8_t  kind;              // radio_kind_t
    uint8_t  band;              // band_t
    uint32_t freq_hz;           // центральная частота
    int8_t   max_dbm;           // потолок PA этой платы
    uint8_t  spi_host;          // 1=SPI2, 2=SPI3
    uint32_t spi_hz;
    uint8_t  uart_num;
    uint32_t uart_baud;
    uint16_t tcxo_mv;           // 0 = XTAL, иначе напряжение TCXO (SX126x)
    uint8_t  dio2_as_rf_switch; // SX126x: DIO2 рулит антенным свитчём
    uint8_t  supports_fsk;      // умеет ли GFSK — от этого зависят звонки
    radio_pins_t pins;
} radio_cfg_t;

// ── Дисплей ─────────────────────────────────────────────────
// display.c рисует в монохромный кадровый буфер; панель — это
// то, куда его вылить. Цветные панели получают тот же буфер,
// раскрашенный в fg/bg и растянутый в scale раз.
typedef enum {
    PANEL_NONE          = 0,
    PANEL_SSD1306_I2C   = 1,   // классика 128x64
    PANEL_SH1106_I2C    = 2,   // тот же кадр, смещён на 2 px
    PANEL_SSD1306_SPI   = 3,
    PANEL_ST7789_SPI    = 4,   // 240x240 / 240x320 IPS
    PANEL_ILI9341_SPI   = 5,   // 320x240, часто с XPT2046
    PANEL_ST7735_SPI    = 6,   // 160x128
    PANEL_KIND_MAX
} panel_kind_t;

typedef struct {
    uint8_t  kind;              // panel_kind_t
    uint16_t width, height;     // физическое разрешение панели
    uint8_t  rotation;          // 0..3, шаг 90°
    uint8_t  i2c_addr;          // 0x3C / 0x3D
    uint8_t  invert;            // инверсия цвета контроллера
    uint8_t  bgr;               // порядок субпикселей
    uint16_t x_offset, y_offset;// сдвиг окна (ST7789 240x240 и SH1106)
    uint32_t bus_hz;
    uint8_t  spi_host;
    int8_t   sda, scl;                    // I2C
    int8_t   sck, mosi, miso, cs, dc, rst, bl;  // SPI (+ подсветка)
    uint8_t  bl_active_low;
    uint16_t fg, bg;            // RGB565 для цветных панелей
    uint8_t  scale;             // 0 = подобрать под размер панели
} display_cfg_t;

// ── Тачскрин ────────────────────────────────────────────────
typedef enum {
    TOUCH_NONE        = 0,
    TOUCH_XPT2046_SPI = 1,   // резистивный, обычно с ILI9341
    TOUCH_FT6236_I2C  = 2,
    TOUCH_CST816_I2C  = 3,
    TOUCH_GT911_I2C   = 4,
    TOUCH_KIND_MAX
} touch_kind_t;

typedef struct {
    uint8_t  kind;              // touch_kind_t
    int8_t   sck, mosi, miso, cs;  // SPI (можно делить шину с дисплеем)
    int8_t   sda, scl;             // I2C
    int8_t   irq, rst;
    uint8_t  spi_host;
    uint8_t  i2c_addr;
    uint8_t  swap_xy, invert_x, invert_y;
    uint16_t cal_x0, cal_y0, cal_x1, cal_y1;  // сырые значения по углам
} touch_cfg_t;

// ── Батарея / индикация ─────────────────────────────────────
typedef struct {
    uint8_t present;
    uint8_t adc_unit;       // 1 или 2
    uint8_t adc_channel;
    uint16_t divider_x100;  // коэффициент делителя ×100 (2.0 → 200)
} batt_cfg_t;

#define HWCFG_MAX_BUTTONS 4

typedef struct {
    uint16_t version;
    char     profile[HWCFG_ID_LEN];  // из какого профиля засеяно
    char     name[HWCFG_NAME_LEN];   // как показывать в приложении
    uint8_t  hw_id;                  // уезжает в NODE_HELLO
    uint8_t  chip;                   // esp_chip_model_t на момент записи

    radio_cfg_t   radio;
    // Второе радио: HaLow-компаньон рядом с LoRa, либо 2.4G рядом с 868.
    // kind = RADIO_NONE, если его нет.
    radio_cfg_t   radio2;

    display_cfg_t disp;
    touch_cfg_t   touch;
    batt_cfg_t    batt;

    int8_t   led_pin;
    uint8_t  led_active_low;
    int8_t   buttons[HWCFG_MAX_BUTTONS];
    uint8_t  buttons_active_low;

    uint32_t crc;   // CRC32 всего выше — ловит битый NVS
} hwcfg_t;

// ── Жизненный цикл ──────────────────────────────────────────
// Вызывать сразу после nvs_flash_init(), ДО любых драйверов.
void hwcfg_init(void);

// Активная конфигурация. Никогда не NULL после hwcfg_init().
const hwcfg_t *hwcfg(void);

// Проверить и записать. Применится после перезагрузки.
// err — человекочитаемая причина отказа (можно NULL).
esp_err_t hwcfg_save(const hwcfg_t *cfg, char *err, size_t err_sz);

// Стереть NVS-запись → на следующем старте профиль сборки.
esp_err_t hwcfg_reset(void);

// Проверка без записи: конфликты пинов, запрещённые GPIO текущего
// чипа, частота вне диапазона, дырки в обязательных пинах.
esp_err_t hwcfg_validate(const hwcfg_t *cfg, char *err, size_t err_sz);

// ── Профили ─────────────────────────────────────────────────
// Готовые комбинации: то, что раньше было boards/*.h, плюс
// популярные самосборы. Веб-флешер показывает таблицу пинов
// именно отсюда — один источник правды.
int             hwcfg_profile_count(void);
const hwcfg_t  *hwcfg_profile_at(int idx);
const hwcfg_t  *hwcfg_profile_find(const char *id);
// Профиль, соответствующий board-заголовку этой сборки.
const hwcfg_t  *hwcfg_profile_builtin(void);

// ── JSON (BLE-команды hwcfg / веб-флешер) ───────────────────
// Патч: применяются только присутствующие в JSON поля, остальное
// берётся из base. Поддерживается "profile":"<id>" — сначала
// грузится профиль, потом накатываются точечные правки.
esp_err_t hwcfg_from_json(const char *json, const hwcfg_t *base,
                          hwcfg_t *out, char *err, size_t err_sz);
int hwcfg_to_json(const hwcfg_t *cfg, char *out, size_t out_sz);

// ── Имена для UI и логов ────────────────────────────────────
const char *hwcfg_radio_name(uint8_t kind);
const char *hwcfg_band_name(uint8_t band);
const char *hwcfg_panel_name(uint8_t kind);
const char *hwcfg_touch_name(uint8_t kind);
bool        hwcfg_radio_supports_band(uint8_t kind, uint8_t band);
bool        hwcfg_band_contains(uint8_t band, uint32_t freq_hz);
// Границы диапазона в Гц (для валидатора и UI-слайдера).
void        hwcfg_band_range(uint8_t band, uint32_t *lo, uint32_t *hi);
