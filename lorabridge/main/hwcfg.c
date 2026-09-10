#include "hwcfg.h"
#include "jsonlite.h"
#include "boards/board.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_chip_info.h"
#include "soc/soc_caps.h"
#include "nvs.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "HWCFG";

#define HW_NVS_NS   "hwcfg"
#define HW_NVS_KEY  "blob"

static hwcfg_t s_cfg;
static bool    s_from_nvs = false;

// ============================================================
// Справочники
// ============================================================
static const char *k_radio_names[RADIO_KIND_MAX] = {
    "none", "E220", "SX127x", "SX126x", "SX128x", "LR11xx", "HaLow",
};
static const char *k_band_names[BAND_MAX] = {
    "433", "470", "868", "915", "2G4", "HaLow-sub1",
};
static const char *k_panel_names[PANEL_KIND_MAX] = {
    "none", "SSD1306-I2C", "SH1106-I2C", "SSD1306-SPI",
    "ST7789-SPI", "ILI9341-SPI", "ST7735-SPI",
};
static const char *k_touch_names[TOUCH_KIND_MAX] = {
    "none", "XPT2046", "FT6236", "CST816", "GT911",
};

const char *hwcfg_radio_name(uint8_t k) { return k < RADIO_KIND_MAX ? k_radio_names[k] : "?"; }
const char *hwcfg_band_name(uint8_t b)  { return b < BAND_MAX       ? k_band_names[b]  : "?"; }
const char *hwcfg_panel_name(uint8_t k) { return k < PANEL_KIND_MAX ? k_panel_names[k] : "?"; }
const char *hwcfg_touch_name(uint8_t k) { return k < TOUCH_KIND_MAX ? k_touch_names[k] : "?"; }

// Границы диапазонов. Верхняя/нижняя — по железу, не по локальному
// законодательству: ограничения по мощности и duty cycle живут в
// bands.c, здесь только «влезает ли частота в этот диапазон вообще».
static const struct { uint32_t lo, hi; } k_band_hz[BAND_MAX] = {
    [BAND_433]        = { 410000000u,  525000000u },
    [BAND_470]        = { 470000000u,  510000000u },
    [BAND_868]        = { 863000000u,  870000000u },
    [BAND_915]        = { 902000000u,  928000000u },
    [BAND_2G4]        = { 2400000000u, 2500000000u },
    [BAND_HALOW_SUB1] = { 850000000u,  950000000u },
};

void hwcfg_band_range(uint8_t band, uint32_t *lo, uint32_t *hi)
{
    if (band >= BAND_MAX) { if (lo) *lo = 0; if (hi) *hi = 0; return; }
    if (lo) *lo = k_band_hz[band].lo;
    if (hi) *hi = k_band_hz[band].hi;
}

bool hwcfg_band_contains(uint8_t band, uint32_t hz)
{
    if (band >= BAND_MAX) return false;
    return hz >= k_band_hz[band].lo && hz <= k_band_hz[band].hi;
}

// Что какое радио физически умеет. Битовая маска по band_t.
#define BM(b) (1u << (b))
static const uint32_t k_radio_bands[RADIO_KIND_MAX] = {
    [RADIO_NONE]   = 0,
    // E220 бывает 400T (433/470) и 900T (868/915) — модуль знает сам,
    // мы лишь не даём выставить заведомо чужую частоту.
    [RADIO_E220]   = BM(BAND_433) | BM(BAND_470) | BM(BAND_868) | BM(BAND_915),
    [RADIO_SX127X] = BM(BAND_433) | BM(BAND_470) | BM(BAND_868) | BM(BAND_915),
    [RADIO_SX126X] = BM(BAND_433) | BM(BAND_470) | BM(BAND_868) | BM(BAND_915),
    [RADIO_SX128X] = BM(BAND_2G4),
    [RADIO_LR11XX] = BM(BAND_433) | BM(BAND_470) | BM(BAND_868) | BM(BAND_915) | BM(BAND_2G4),
    [RADIO_HALOW]  = BM(BAND_HALOW_SUB1),
};

bool hwcfg_radio_supports_band(uint8_t kind, uint8_t band)
{
    if (kind >= RADIO_KIND_MAX || band >= BAND_MAX) return false;
    return (k_radio_bands[kind] & BM(band)) != 0;
}

// ============================================================
// Профили
// ============================================================
#define RP_UNSET  .sck=-1,.miso=-1,.mosi=-1,.cs=-1,.rst=-1,.busy=-1, \
                  .dio0=-1,.dio1=-1,.dio2=-1,.txen=-1,.rxen=-1,      \
                  .tx=-1,.rx=-1,.aux=-1,.m0=-1,.m1=-1
#define DP_UNSET  .sda=-1,.scl=-1,.sck=-1,.mosi=-1,.miso=-1,         \
                  .cs=-1,.dc=-1,.rst=-1,.bl=-1
#define TP_UNSET  .sck=-1,.mosi=-1,.miso=-1,.cs=-1,.sda=-1,.scl=-1,  \
                  .irq=-1,.rst=-1
#define NO_BTN    .buttons = { -1, -1, -1, -1 }

// SSD1306 128x64 на I2C — повторяется в половине профилей.
#define OLED_I2C(_sda, _scl) .disp = {                               \
        .kind = PANEL_SSD1306_I2C, .width = 128, .height = 64,       \
        .i2c_addr = 0x3C, .bus_hz = 400000, DP_UNSET,                \
        .sda = (_sda), .scl = (_scl), .scale = 1 }

// Приём «сначала всё -1, потом точечно переопределить» даёт
// -Woverride-init на каждый пин. Именно так таблица и задумана:
// перечислять шестнадцать полей в каждом профиле, чтобы не
// оставить случайный 0 (а GPIO0 существует и strapping!) —
// гораздо хуже читается и легче ошибиться.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverride-init"

static const hwcfg_t k_profiles[] = {
// ── Оригинальные платы (бывшие boards/*.h) ──────────────────
{
    .version = HWCFG_VERSION, .profile = "s3_e220", .name = "S3+E220",
    .hw_id = HW_ID_S3_E220,
    .radio = { .kind = RADIO_E220, .band = BAND_433, .freq_hz = 433125000,
               .max_dbm = 30, .uart_num = 1, .uart_baud = 9600,
               .supports_fsk = 0,
               .pins = { RP_UNSET, .tx = 17, .rx = 18, .aux = 38, .m0 = 39, .m1 = 40 } },
    OLED_I2C(21, 47),
    .touch = { .kind = TOUCH_NONE, TP_UNSET },
    .led_pin = -1, NO_BTN,
},
{
    .version = HWCFG_VERSION, .profile = "c6_e220", .name = "C6+E220",
    .hw_id = HW_ID_C6_E220,
    .radio = { .kind = RADIO_E220, .band = BAND_433, .freq_hz = 433125000,
               .max_dbm = 30, .uart_num = 1, .uart_baud = 9600,
               .pins = { RP_UNSET, .tx = 4, .rx = 5, .aux = 6, .m0 = 18, .m1 = 19 } },
    OLED_I2C(22, 23),
    .touch = { .kind = TOUCH_NONE, TP_UNSET },
    .led_pin = -1, NO_BTN,
},
{
    // LilyGO T3 v1.6.1. 868-я ревизия — своя подсеть, с 433-ми не говорит.
    .version = HWCFG_VERSION, .profile = "t3_v161", .name = "T3-v1.6.1",
    .hw_id = HW_ID_T3_V161,
    .radio = { .kind = RADIO_SX127X, .band = BAND_868, .freq_hz = 869525000,
               .max_dbm = 20, .spi_host = 1, .spi_hz = 9000000,
               .supports_fsk = 1,
               .pins = { RP_UNSET, .sck = 5, .miso = 19, .mosi = 27, .cs = 18,
                         .rst = 23, .dio0 = 26, .dio1 = 33 } },
    OLED_I2C(21, 22),
    .touch = { .kind = TOUCH_NONE, TP_UNSET },
    .batt = { .present = 1, .adc_unit = 1, .adc_channel = 7, .divider_x100 = 200 },
    .led_pin = 25, NO_BTN,
},
{
    // Та же плата в 915-й ревизии: пины те же, диапазон другой.
    .version = HWCFG_VERSION, .profile = "t3_v161_915", .name = "T3-v1.6.1 915",
    .hw_id = HW_ID_T3_V161,
    .radio = { .kind = RADIO_SX127X, .band = BAND_915, .freq_hz = 906875000,
               .max_dbm = 20, .spi_host = 1, .spi_hz = 9000000,
               .supports_fsk = 1,
               .pins = { RP_UNSET, .sck = 5, .miso = 19, .mosi = 27, .cs = 18,
                         .rst = 23, .dio0 = 26, .dio1 = 33 } },
    OLED_I2C(21, 22),
    .touch = { .kind = TOUCH_NONE, TP_UNSET },
    .batt = { .present = 1, .adc_unit = 1, .adc_channel = 7, .divider_x100 = 200 },
    .led_pin = 25, NO_BTN,
},
{
    .version = HWCFG_VERSION, .profile = "s3_e22m33s", .name = "S3+E22-M33S",
    .hw_id = HW_ID_S3_E22M33S,
    .radio = { .kind = RADIO_SX126X, .band = BAND_433, .freq_hz = 433125000,
               .max_dbm = 33, .spi_host = 1, .spi_hz = 8000000,
               .tcxo_mv = 1800, .supports_fsk = 1,
               // Распайка — как в boards/board_s3_e22m33s.h рабочей версии:
               // RST/BUSY здесь НЕ такие, как в generic-профилях ниже, а
               // TXEN/RXEN сидят на GPIO1/2. Перепутать их — тихая смерть:
               // SPI отвечает, чип «жив», но ВЧ-ключ не открывается и в
               // эфир не уходит и из эфира не приходит ничего.
               .pins = { RP_UNSET, .sck = 12, .miso = 13, .mosi = 11, .cs = 10,
                         .rst = 14, .busy = 9, .dio1 = 3, .txen = 1, .rxen = 2 } },
    OLED_I2C(21, 47),
    .touch = { .kind = TOUCH_NONE, TP_UNSET },
    .led_pin = -1, NO_BTN,
},

// ── Самосборы «взял модуль и припаял» ───────────────────────
{
    // То, что просили в первую очередь: C6 + Ra-02.
    // GPIO8/9 — strapping, 12/13 — USB-JTAG, 16/17 — UART0: обходим.
    .version = HWCFG_VERSION, .profile = "c6_ra02", .name = "C6+Ra-02",
    .hw_id = HW_ID_GENERIC_SPI,
    .radio = { .kind = RADIO_SX127X, .band = BAND_433, .freq_hz = 433125000,
               .max_dbm = 20, .spi_host = 1, .spi_hz = 9000000,
               .supports_fsk = 1,
               .pins = { RP_UNSET, .sck = 6, .miso = 2, .mosi = 7, .cs = 18,
                         .rst = 19, .dio0 = 20, .dio1 = 21 } },
    OLED_I2C(22, 23),
    .touch = { .kind = TOUCH_NONE, TP_UNSET },
    .led_pin = -1, NO_BTN,
},
{
    .version = HWCFG_VERSION, .profile = "s3_ra02", .name = "S3+Ra-02",
    .hw_id = HW_ID_GENERIC_SPI,
    .radio = { .kind = RADIO_SX127X, .band = BAND_433, .freq_hz = 433125000,
               .max_dbm = 20, .spi_host = 1, .spi_hz = 9000000,
               .supports_fsk = 1,
               .pins = { RP_UNSET, .sck = 36, .miso = 37, .mosi = 35, .cs = 38,
                         .rst = 39, .dio0 = 40, .dio1 = 41 } },
    OLED_I2C(21, 47),
    .touch = { .kind = TOUCH_NONE, TP_UNSET },
    .led_pin = -1, NO_BTN,
},
{
    // Цветной IPS без тача: SX1262 + ST7789 240x240.
    .version = HWCFG_VERSION, .profile = "s3_sx1262_st7789", .name = "S3+SX1262+IPS",
    .hw_id = HW_ID_GENERIC_SPI,
    .radio = { .kind = RADIO_SX126X, .band = BAND_868, .freq_hz = 869525000,
               .max_dbm = 22, .spi_host = 1, .spi_hz = 8000000,
               .tcxo_mv = 1800, .supports_fsk = 1,
               .pins = { RP_UNSET, .sck = 12, .miso = 13, .mosi = 11, .cs = 10,
                         .rst = 9, .busy = 14, .dio1 = 8 } },
    // Отдельная шина SPI3 — не делим с радио, чтобы отрисовка не
    // тормозила приём (см. B15).
    .disp = { .kind = PANEL_ST7789_SPI, .width = 240, .height = 240,
              .bus_hz = 40000000, .spi_host = 2, DP_UNSET,
              .sck = 39, .mosi = 40, .cs = 41, .dc = 42, .rst = 45, .bl = 46,
              .fg = 0xFFFF, .bg = 0x0000, .scale = 0 },
    .touch = { .kind = TOUCH_NONE, TP_UNSET },
    .led_pin = -1, NO_BTN,
},
{
    // Тачскрин: классический ILI9341 320x240 + XPT2046 на общей SPI-шине.
    .version = HWCFG_VERSION, .profile = "s3_ili9341_touch", .name = "S3+ILI9341 touch",
    .hw_id = HW_ID_GENERIC_SPI,
    .radio = { .kind = RADIO_SX127X, .band = BAND_433, .freq_hz = 433125000,
               .max_dbm = 20, .spi_host = 1, .spi_hz = 9000000,
               .supports_fsk = 1,
               .pins = { RP_UNSET, .sck = 36, .miso = 37, .mosi = 35, .cs = 38,
                         .rst = 39, .dio0 = 40, .dio1 = 41 } },
    .disp = { .kind = PANEL_ILI9341_SPI, .width = 320, .height = 240,
              .rotation = 1, .bus_hz = 40000000, .spi_host = 2, DP_UNSET,
              .sck = 12, .mosi = 11, .miso = 13, .cs = 10, .dc = 9, .rst = 8, .bl = 7,
              .fg = 0xFFFF, .bg = 0x0000, .scale = 0 },
    .touch = { .kind = TOUCH_XPT2046_SPI, TP_UNSET,
               .sck = 12, .mosi = 11, .miso = 13, .cs = 6, .irq = 5,
               .spi_host = 2,
               .cal_x0 = 300, .cal_y0 = 300, .cal_x1 = 3800, .cal_y1 = 3800 },
    .led_pin = -1, NO_BTN,
},

// ── Плейсхолдеры: описание готово, драйвер — заглушка ────────
{
    // LoRa 2.4 ГГц (E28 / SX1280). Мировой безлицензионный диапазон,
    // до 2 Мбит/с, дальность заметно меньше 433-й.
    .version = HWCFG_VERSION, .profile = "s3_e28_2g4", .name = "S3+E28 2.4G",
    .hw_id = HW_ID_GENERIC_SPI,
    .radio = { .kind = RADIO_SX128X, .band = BAND_2G4, .freq_hz = 2450000000u,
               .max_dbm = 12, .spi_host = 1, .spi_hz = 8000000,
               .supports_fsk = 1,
               .pins = { RP_UNSET, .sck = 12, .miso = 13, .mosi = 11, .cs = 10,
                         .rst = 9, .busy = 14, .dio1 = 8 } },
    OLED_I2C(21, 47),
    .touch = { .kind = TOUCH_NONE, TP_UNSET },
    .led_pin = -1, NO_BTN,
},
{
    // 802.11ah HaLow (MM6108) как ВТОРОЕ радио рядом с LoRa: LoRa
    // остаётся для сообщений и SOS, HaLow — широкий канал под медиа
    // и проброс интернета (см. docs/HALOW_BRIDGE.md).
    .version = HWCFG_VERSION, .profile = "s3_lora_halow", .name = "S3+LoRa+HaLow",
    .hw_id = HW_ID_GENERIC_SPI,
    .radio = { .kind = RADIO_SX126X, .band = BAND_868, .freq_hz = 869525000,
               .max_dbm = 22, .spi_host = 1, .spi_hz = 8000000,
               .tcxo_mv = 1800, .supports_fsk = 1,
               .pins = { RP_UNSET, .sck = 12, .miso = 13, .mosi = 11, .cs = 10,
                         .rst = 9, .busy = 14, .dio1 = 8 } },
    .radio2 = { .kind = RADIO_HALOW, .band = BAND_HALOW_SUB1, .freq_hz = 868000000,
                .max_dbm = 21, .spi_host = 2, .spi_hz = 20000000,
                .pins = { RP_UNSET, .sck = 39, .miso = 38, .mosi = 40, .cs = 41,
                          .rst = 42, .dio0 = 45 } },
    OLED_I2C(21, 47),
    .touch = { .kind = TOUCH_NONE, TP_UNSET },
    .led_pin = -1, NO_BTN,
},
};

#pragma GCC diagnostic pop

#define PROFILE_COUNT (sizeof(k_profiles) / sizeof(k_profiles[0]))

int hwcfg_profile_count(void) { return (int)PROFILE_COUNT; }

const hwcfg_t *hwcfg_profile_at(int idx)
{
    if (idx < 0 || idx >= (int)PROFILE_COUNT) return NULL;
    return &k_profiles[idx];
}

const hwcfg_t *hwcfg_profile_find(const char *id)
{
    if (!id) return NULL;
    for (size_t i = 0; i < PROFILE_COUNT; i++)
        if (strcmp(k_profiles[i].profile, id) == 0) return &k_profiles[i];
    return NULL;
}

const hwcfg_t *hwcfg_profile_builtin(void)
{
    const hwcfg_t *p = hwcfg_profile_find(BOARD_PROFILE_ID);
    // Профиль сборки обязан существовать: board.h и таблица выше
    // должны сходиться. Если разошлись — падаем на первый, но громко.
    if (!p) {
        ESP_LOGE(TAG, "build profile '%s' missing from table!", BOARD_PROFILE_ID);
        p = &k_profiles[0];
    }
    return p;
}

// ============================================================
// Валидатор
//
// Дешевле поймать конфликт пинов здесь, чем ловить его потом
// как «радио иногда не отвечает»: SPI-драйвер на занятом пине
// не падает, он просто портит соседа.
// ============================================================
#define ERR(...) do { if (err && err_sz) snprintf(err, err_sz, __VA_ARGS__); } while (0)

// Пины, физически занятые на этом чипе (флеш/PSRAM) или отсутствующие.
static bool pin_reserved(int p)
{
#if CONFIG_IDF_TARGET_ESP32
    if (p >= 6 && p <= 11) return true;              // SPI-флеш
    if (p == 20 || p == 24 || (p >= 28 && p <= 31)) return true;  // нет таких
#elif CONFIG_IDF_TARGET_ESP32S3
    if (p >= 26 && p <= 32) return true;             // SPI-флеш/PSRAM
#elif CONFIG_IDF_TARGET_ESP32C6
    if (p >= 24 && p <= 30) return true;             // SPI-флеш
#elif CONFIG_IDF_TARGET_ESP32C3
    if (p >= 11 && p <= 17) return true;
#endif
    return false;
}

// Только вход: как CS/RST/MOSI такой пин молча не заработает.
static bool pin_input_only(int p)
{
#if CONFIG_IDF_TARGET_ESP32
    return p >= 34 && p <= 39;
#else
    (void)p; return false;
#endif
}

static int pin_max(void)
{
#if CONFIG_IDF_TARGET_ESP32
    return 39;
#elif CONFIG_IDF_TARGET_ESP32S3
    return 48;
#elif CONFIG_IDF_TARGET_ESP32C6
    return 30;
#else
    return 48;
#endif
}

typedef struct {
    int8_t      pin;
    const char *owner;
    bool        shared_bus;  // sck/mosi/miso — их делить можно
    bool        needs_out;
} pinuse_t;

#define MAX_PINUSE 40

static esp_err_t pin_add(pinuse_t *tab, int *n, int8_t pin, const char *owner,
                         bool shared, bool needs_out, char *err, size_t err_sz)
{
    if (pin < 0) return ESP_OK;             // не подключено — не наше дело
    if (pin > pin_max()) {
        ERR("%s: GPIO%d не существует на этом чипе", owner, pin);
        return ESP_ERR_INVALID_ARG;
    }
    if (pin_reserved(pin)) {
        ERR("%s: GPIO%d занят флешем/PSRAM", owner, pin);
        return ESP_ERR_INVALID_ARG;
    }
    if (needs_out && pin_input_only(pin)) {
        ERR("%s: GPIO%d только на вход, выходом быть не может", owner, pin);
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < *n; i++) {
        if (tab[i].pin != pin) continue;
        // Две линии одной шины на одном пине — норма (дисплей и тач
        // на общей SPI). Всё остальное — ошибка монтажа.
        if (tab[i].shared_bus && shared) continue;
        ERR("GPIO%d занят дважды: %s и %s", pin, tab[i].owner, owner);
        return ESP_ERR_INVALID_ARG;
    }
    if (*n >= MAX_PINUSE) return ESP_OK;
    tab[*n].pin = pin; tab[*n].owner = owner;
    tab[*n].shared_bus = shared; tab[*n].needs_out = needs_out;
    (*n)++;
    return ESP_OK;
}

static esp_err_t validate_radio(const radio_cfg_t *r, const char *who,
                                pinuse_t *tab, int *n, char *err, size_t err_sz)
{
    if (r->kind == RADIO_NONE) return ESP_OK;
    if (r->kind >= RADIO_KIND_MAX) { ERR("%s: неизвестный тип радио %u", who, r->kind); return ESP_ERR_INVALID_ARG; }
    if (r->band >= BAND_MAX)       { ERR("%s: неизвестный диапазон %u", who, r->band); return ESP_ERR_INVALID_ARG; }
    if (!hwcfg_radio_supports_band(r->kind, r->band)) {
        ERR("%s: %s не работает в диапазоне %s", who,
            hwcfg_radio_name(r->kind), hwcfg_band_name(r->band));
        return ESP_ERR_INVALID_ARG;
    }
    if (!hwcfg_band_contains(r->band, r->freq_hz)) {
        uint32_t lo, hi; hwcfg_band_range(r->band, &lo, &hi);
        ERR("%s: частота %lu Гц вне диапазона %s (%lu…%lu)", who,
            (unsigned long)r->freq_hz, hwcfg_band_name(r->band),
            (unsigned long)lo, (unsigned long)hi);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t e;
    const radio_pins_t *p = &r->pins;
    if (r->kind == RADIO_E220) {
        if (p->tx < 0 || p->rx < 0 || p->m0 < 0 || p->m1 < 0 || p->aux < 0) {
            ERR("%s: E220 требует TX, RX, M0, M1 и AUX", who);
            return ESP_ERR_INVALID_ARG;
        }
        if ((e = pin_add(tab, n, p->tx,  "radio TX",  false, true,  err, err_sz))) return e;
        if ((e = pin_add(tab, n, p->rx,  "radio RX",  false, false, err, err_sz))) return e;
        if ((e = pin_add(tab, n, p->aux, "radio AUX", false, false, err, err_sz))) return e;
        if ((e = pin_add(tab, n, p->m0,  "radio M0",  false, true,  err, err_sz))) return e;
        if ((e = pin_add(tab, n, p->m1,  "radio M1",  false, true,  err, err_sz))) return e;
        return ESP_OK;
    }

#if SOC_SPI_PERIPH_NUM <= 2
    if (r->spi_host == 2) {
        ERR("%s: у этого чипа только один SPI-хост", who);
        return ESP_ERR_INVALID_ARG;
    }
#endif

    // Все SPI-радио: шина + CS обязательны.
    if (p->sck < 0 || p->mosi < 0 || p->miso < 0 || p->cs < 0) {
        ERR("%s: SPI-радио требует SCK, MOSI, MISO и CS", who);
        return ESP_ERR_INVALID_ARG;
    }
    if ((e = pin_add(tab, n, p->sck,  "radio SCK",  true,  true,  err, err_sz))) return e;
    if ((e = pin_add(tab, n, p->mosi, "radio MOSI", true,  true,  err, err_sz))) return e;
    if ((e = pin_add(tab, n, p->miso, "radio MISO", true,  false, err, err_sz))) return e;
    if ((e = pin_add(tab, n, p->cs,   "radio CS",   false, true,  err, err_sz))) return e;
    if ((e = pin_add(tab, n, p->rst,  "radio RST",  false, true,  err, err_sz))) return e;
    if ((e = pin_add(tab, n, p->busy, "radio BUSY", false, false, err, err_sz))) return e;
    if ((e = pin_add(tab, n, p->dio0, "radio DIO0", false, false, err, err_sz))) return e;
    if ((e = pin_add(tab, n, p->dio1, "radio DIO1", false, false, err, err_sz))) return e;
    if ((e = pin_add(tab, n, p->dio2, "radio DIO2", false, false, err, err_sz))) return e;
    if ((e = pin_add(tab, n, p->txen, "radio TXEN", false, true,  err, err_sz))) return e;
    if ((e = pin_add(tab, n, p->rxen, "radio RXEN", false, true,  err, err_sz))) return e;

    if (r->kind == RADIO_SX127X && p->dio0 < 0) {
        ERR("%s: SX127x без DIO0 не сможет сообщить о приёме", who);
        return ESP_ERR_INVALID_ARG;
    }
    if ((r->kind == RADIO_SX126X || r->kind == RADIO_SX128X) && p->busy < 0) {
        ERR("%s: %s требует BUSY", who, hwcfg_radio_name(r->kind));
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t hwcfg_validate(const hwcfg_t *c, char *err, size_t err_sz)
{
    if (!c) return ESP_ERR_INVALID_ARG;
    if (err && err_sz) err[0] = '\0';

    if (c->version != HWCFG_VERSION) {
        ERR("версия конфига %u, ожидалась %u", c->version, HWCFG_VERSION);
        return ESP_ERR_INVALID_VERSION;
    }

    pinuse_t tab[MAX_PINUSE];
    int n = 0;
    esp_err_t e;

    if ((e = validate_radio(&c->radio,  "радио",  tab, &n, err, err_sz))) return e;
    if ((e = validate_radio(&c->radio2, "радио 2", tab, &n, err, err_sz))) return e;
    if (c->radio.kind == RADIO_NONE) {
        ERR("основное радио не задано");
        return ESP_ERR_INVALID_ARG;
    }

    // Дисплей
    const display_cfg_t *d = &c->disp;
    if (d->kind >= PANEL_KIND_MAX) { ERR("неизвестный дисплей %u", d->kind); return ESP_ERR_INVALID_ARG; }
    if (d->kind == PANEL_SSD1306_I2C || d->kind == PANEL_SH1106_I2C) {
        if (d->sda < 0 || d->scl < 0) { ERR("I2C-дисплей требует SDA и SCL"); return ESP_ERR_INVALID_ARG; }
        if ((e = pin_add(tab, &n, d->sda, "disp SDA", false, true, err, err_sz))) return e;
        if ((e = pin_add(tab, &n, d->scl, "disp SCL", false, true, err, err_sz))) return e;
    } else if (d->kind != PANEL_NONE) {
        if (d->sck < 0 || d->mosi < 0 || d->dc < 0) {
            ERR("SPI-дисплей требует SCK, MOSI и DC"); return ESP_ERR_INVALID_ARG;
        }
        if ((e = pin_add(tab, &n, d->sck,  "disp SCK",  true,  true, err, err_sz))) return e;
        if ((e = pin_add(tab, &n, d->mosi, "disp MOSI", true,  true, err, err_sz))) return e;
        if ((e = pin_add(tab, &n, d->miso, "disp MISO", true,  false, err, err_sz))) return e;
        if ((e = pin_add(tab, &n, d->cs,   "disp CS",   false, true, err, err_sz))) return e;
        if ((e = pin_add(tab, &n, d->dc,   "disp DC",   false, true, err, err_sz))) return e;
        if ((e = pin_add(tab, &n, d->rst,  "disp RST",  false, true, err, err_sz))) return e;
        if ((e = pin_add(tab, &n, d->bl,   "disp BL",   false, true, err, err_sz))) return e;
    }
    if (d->kind != PANEL_NONE && (d->width == 0 || d->height == 0)) {
        ERR("у дисплея нулевой размер"); return ESP_ERR_INVALID_ARG;
    }

    // Тач
    const touch_cfg_t *t = &c->touch;
    if (t->kind >= TOUCH_KIND_MAX) { ERR("неизвестный тач %u", t->kind); return ESP_ERR_INVALID_ARG; }
    if (t->kind == TOUCH_XPT2046_SPI) {
        if (t->sck < 0 || t->mosi < 0 || t->miso < 0 || t->cs < 0) {
            ERR("XPT2046 требует SCK, MOSI, MISO и CS"); return ESP_ERR_INVALID_ARG;
        }
        if ((e = pin_add(tab, &n, t->sck,  "touch SCK",  true,  true,  err, err_sz))) return e;
        if ((e = pin_add(tab, &n, t->mosi, "touch MOSI", true,  true,  err, err_sz))) return e;
        if ((e = pin_add(tab, &n, t->miso, "touch MISO", true,  false, err, err_sz))) return e;
        if ((e = pin_add(tab, &n, t->cs,   "touch CS",   false, true,  err, err_sz))) return e;
        if ((e = pin_add(tab, &n, t->irq,  "touch IRQ",  false, false, err, err_sz))) return e;
    } else if (t->kind != TOUCH_NONE) {
        if (t->sda < 0 || t->scl < 0) { ERR("I2C-тач требует SDA и SCL"); return ESP_ERR_INVALID_ARG; }
        // Тач на той же шине, что и I2C-дисплей — обычное дело.
        bool same_i2c = (d->kind == PANEL_SSD1306_I2C || d->kind == PANEL_SH1106_I2C) &&
                        t->sda == d->sda && t->scl == d->scl;
        if (!same_i2c) {
            if ((e = pin_add(tab, &n, t->sda, "touch SDA", false, true, err, err_sz))) return e;
            if ((e = pin_add(tab, &n, t->scl, "touch SCL", false, true, err, err_sz))) return e;
        }
        if ((e = pin_add(tab, &n, t->irq, "touch IRQ", false, false, err, err_sz))) return e;
    }
    if (t->kind != TOUCH_NONE && d->kind == PANEL_NONE) {
        ERR("тач без дисплея"); return ESP_ERR_INVALID_ARG;
    }

    if ((e = pin_add(tab, &n, c->led_pin, "LED", false, true, err, err_sz))) return e;
    for (int i = 0; i < HWCFG_MAX_BUTTONS; i++) {
        if ((e = pin_add(tab, &n, c->buttons[i], "кнопка", false, false, err, err_sz))) return e;
    }

    if (c->batt.present && c->batt.adc_unit != 1 && c->batt.adc_unit != 2) {
        ERR("ADC-юнит батареи должен быть 1 или 2"); return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

// ============================================================
// NVS
// ============================================================
static uint32_t cfg_crc(const hwcfg_t *c)
{
    // CRC считаем по всему, кроме самого поля crc (оно последнее).
    return esp_rom_crc32_le(0, (const uint8_t *)c, offsetof(hwcfg_t, crc));
}

void hwcfg_init(void)
{
    const hwcfg_t *dflt = hwcfg_profile_builtin();
    s_cfg = *dflt;
    s_from_nvs = false;

    nvs_handle_t h;
    if (nvs_open(HW_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        hwcfg_t tmp;
        size_t sz = sizeof(tmp);
        esp_err_t e = nvs_get_blob(h, HW_NVS_KEY, &tmp, &sz);
        nvs_close(h);
        if (e == ESP_OK && sz == sizeof(tmp)) {
            char why[96];
            if (tmp.version != HWCFG_VERSION) {
                // Формат сменился — молча возвращаемся к профилю сборки,
                // иначе устройство останется без радио после апдейта.
                ESP_LOGW(TAG, "NVS-конфиг версии %u, ожидалась %u — беру профиль сборки",
                         tmp.version, HWCFG_VERSION);
            } else if (tmp.crc != cfg_crc(&tmp)) {
                ESP_LOGE(TAG, "NVS-конфиг с битой CRC — беру профиль сборки");
            } else if (hwcfg_validate(&tmp, why, sizeof(why)) != ESP_OK) {
                ESP_LOGE(TAG, "NVS-конфиг не проходит проверку (%s) — беру профиль сборки", why);
            } else {
                s_cfg = tmp;
                s_from_nvs = true;
            }
        }
    }

    esp_chip_info_t ci;
    esp_chip_info(&ci);
    s_cfg.chip = (uint8_t)ci.model;

    ESP_LOGI(TAG, "профиль '%s' (%s) %s", s_cfg.profile, s_cfg.name,
             s_from_nvs ? "из NVS" : "из сборки");

    // Запись в NVS переживает перепрошивку (образ флешера кладёт только
    // bootloader/таблицу/приложение), поэтому одна плата может годами
    // молча сидеть на чужой частоте, пока вторая работает на профиле
    // сборки. Услышать друг друга они не могут по определению, а
    // симптом — «просто офлайн», без единой ошибки. Кричим в лог.
    if (s_from_nvs && (strcmp(s_cfg.profile, dflt->profile) != 0 ||
                       s_cfg.radio.freq_hz != dflt->radio.freq_hz)) {
        ESP_LOGW(TAG, "★ конфиг из NVS расходится с профилем сборки:");
        ESP_LOGW(TAG, "★   профиль '%s' вместо '%s'", s_cfg.profile, dflt->profile);
        ESP_LOGW(TAG, "★   радио %lu Гц вместо %lu Гц",
                 (unsigned long)s_cfg.radio.freq_hz,
                 (unsigned long)dflt->radio.freq_hz);
        ESP_LOGW(TAG, "★   соседи на частоте сборки вас НЕ услышат.");
        ESP_LOGW(TAG, "★   сброс: {\"cmd\":\"hwcfg\",\"action\":\"reset\"} + перезагрузка");
    }
    ESP_LOGI(TAG, "  радио: %s, %s, %lu Гц, до %d dBm%s",
             hwcfg_radio_name(s_cfg.radio.kind), hwcfg_band_name(s_cfg.radio.band),
             (unsigned long)s_cfg.radio.freq_hz, s_cfg.radio.max_dbm,
             s_cfg.radio.supports_fsk ? ", FSK есть" : ", без FSK");
    if (s_cfg.radio2.kind != RADIO_NONE)
        ESP_LOGI(TAG, "  радио 2: %s, %s", hwcfg_radio_name(s_cfg.radio2.kind),
                 hwcfg_band_name(s_cfg.radio2.band));
    ESP_LOGI(TAG, "  дисплей: %s %ux%u, тач: %s",
             hwcfg_panel_name(s_cfg.disp.kind), s_cfg.disp.width, s_cfg.disp.height,
             hwcfg_touch_name(s_cfg.touch.kind));
}

const hwcfg_t *hwcfg(void) { return &s_cfg; }

esp_err_t hwcfg_save(const hwcfg_t *cfg, char *err, size_t err_sz)
{
    esp_err_t e = hwcfg_validate(cfg, err, err_sz);
    if (e != ESP_OK) return e;

    hwcfg_t out = *cfg;
    out.crc = cfg_crc(&out);

    nvs_handle_t h;
    e = nvs_open(HW_NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) { ERR("NVS недоступен"); return e; }
    e = nvs_set_blob(h, HW_NVS_KEY, &out, sizeof(out));
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) { ERR("не удалось записать NVS"); return e; }

    ESP_LOGI(TAG, "конфиг сохранён (профиль '%s') — применится после перезагрузки",
             out.profile);
    return ESP_OK;
}

esp_err_t hwcfg_reset(void)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(HW_NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_erase_key(h, HW_NVS_KEY);
    if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "конфиг сброшен к профилю сборки — нужна перезагрузка");
    return e;
}

// ============================================================
// JSON
//
// Разбор — общий jsonlite.c (его же использует devnode). Парсер
// толерантный: неизвестные ключи игнорируются, отсутствующие поля
// берутся из base. Это важно — приложение шлёт патч («поменяй
// только DIO0»), а не всю простыню.
// ============================================================
static void parse_radio(const char *o, const char *e, radio_cfg_t *r)
{
    if (!o) return;
    const char *oe = jl_skip_value(o, e);
    if (!oe) oe = e;
    jl_kind(o, oe, "kind", k_radio_names, RADIO_KIND_MAX, &r->kind);
    jl_kind(o, oe, "band", k_band_names,  BAND_MAX,       &r->band);
    jl_u32(o, oe, "freq", &r->freq_hz);
    long v;
    if (jl_int(o, oe, "max_dbm", &v)) r->max_dbm = (int8_t)v;
    jl_u8 (o, oe, "spi_host", &r->spi_host);
    jl_u32(o, oe, "spi_hz",   &r->spi_hz);
    jl_u8 (o, oe, "uart",     &r->uart_num);
    jl_u32(o, oe, "baud",     &r->uart_baud);
    jl_u16(o, oe, "tcxo_mv",  &r->tcxo_mv);
    jl_u8 (o, oe, "dio2_rfsw", &r->dio2_as_rf_switch);
    jl_u8 (o, oe, "fsk",      &r->supports_fsk);

    const char *p = jl_member(o, oe, "pins");
    if (!p) return;
    const char *pe = jl_skip_value(p, oe);
    if (!pe) pe = oe;
    radio_pins_t *k = &r->pins;
    jl_pin(p, pe, "sck", &k->sck);   jl_pin(p, pe, "miso", &k->miso);
    jl_pin(p, pe, "mosi", &k->mosi); jl_pin(p, pe, "cs",   &k->cs);
    jl_pin(p, pe, "rst", &k->rst);   jl_pin(p, pe, "busy", &k->busy);
    jl_pin(p, pe, "dio0", &k->dio0); jl_pin(p, pe, "dio1", &k->dio1);
    jl_pin(p, pe, "dio2", &k->dio2); jl_pin(p, pe, "txen", &k->txen);
    jl_pin(p, pe, "rxen", &k->rxen); jl_pin(p, pe, "tx",   &k->tx);
    jl_pin(p, pe, "rx",  &k->rx);    jl_pin(p, pe, "aux",  &k->aux);
    jl_pin(p, pe, "m0",  &k->m0);    jl_pin(p, pe, "m1",   &k->m1);
}

esp_err_t hwcfg_from_json(const char *json, const hwcfg_t *base,
                          hwcfg_t *out, char *err, size_t err_sz)
{
    if (!json || !out) return ESP_ERR_INVALID_ARG;
    if (err && err_sz) err[0] = '\0';
    const char *e = json + strlen(json);

    // Шаг 1: основа. "profile" в патче означает «начни с этого профиля»,
    // иначе базой служит текущая конфигурация.
    char pid[HWCFG_ID_LEN];
    if (jl_str(json, e, "profile", pid, sizeof(pid))) {
        const hwcfg_t *p = hwcfg_profile_find(pid);
        if (!p) { ERR("нет профиля '%s'", pid); return ESP_ERR_NOT_FOUND; }
        *out = *p;
    } else {
        *out = base ? *base : *hwcfg();
    }
    out->version = HWCFG_VERSION;

    // Шаг 2: точечные правки поверх.
    jl_str(json, e, "name", out->name, sizeof(out->name));
    jl_u8 (json, e, "hw_id", &out->hw_id);

    parse_radio(jl_member(json, e, "radio"),  e, &out->radio);
    parse_radio(jl_member(json, e, "radio2"), e, &out->radio2);

    const char *d = jl_member(json, e, "display");
    if (d) {
        const char *de = jl_skip_value(d, e); if (!de) de = e;
        display_cfg_t *c = &out->disp;
        jl_kind(d, de, "kind", k_panel_names, PANEL_KIND_MAX, &c->kind);
        jl_u16(d, de, "w", &c->width);   jl_u16(d, de, "h", &c->height);
        jl_u8 (d, de, "rot", &c->rotation);
        jl_u8 (d, de, "addr", &c->i2c_addr);
        jl_u8 (d, de, "invert", &c->invert);
        jl_u8 (d, de, "bgr", &c->bgr);
        jl_u16(d, de, "xoff", &c->x_offset);
        jl_u16(d, de, "yoff", &c->y_offset);
        jl_u32(d, de, "bus_hz", &c->bus_hz);
        jl_u8 (d, de, "spi_host", &c->spi_host);
        jl_u16(d, de, "fg", &c->fg);  jl_u16(d, de, "bg", &c->bg);
        jl_u8 (d, de, "scale", &c->scale);
        jl_u8 (d, de, "bl_low", &c->bl_active_low);
        const char *p = jl_member(d, de, "pins");
        if (p) {
            const char *pe = jl_skip_value(p, de); if (!pe) pe = de;
            jl_pin(p, pe, "sda", &c->sda);   jl_pin(p, pe, "scl",  &c->scl);
            jl_pin(p, pe, "sck", &c->sck);   jl_pin(p, pe, "mosi", &c->mosi);
            jl_pin(p, pe, "miso", &c->miso); jl_pin(p, pe, "cs",   &c->cs);
            jl_pin(p, pe, "dc",  &c->dc);    jl_pin(p, pe, "rst",  &c->rst);
            jl_pin(p, pe, "bl",  &c->bl);
        }
    }

    const char *t = jl_member(json, e, "touch");
    if (t) {
        const char *te = jl_skip_value(t, e); if (!te) te = e;
        touch_cfg_t *c = &out->touch;
        jl_kind(t, te, "kind", k_touch_names, TOUCH_KIND_MAX, &c->kind);
        jl_u8 (t, te, "spi_host", &c->spi_host);
        jl_u8 (t, te, "addr", &c->i2c_addr);
        jl_u8 (t, te, "swap_xy", &c->swap_xy);
        jl_u8 (t, te, "invert_x", &c->invert_x);
        jl_u8 (t, te, "invert_y", &c->invert_y);
        jl_u16(t, te, "cal_x0", &c->cal_x0); jl_u16(t, te, "cal_y0", &c->cal_y0);
        jl_u16(t, te, "cal_x1", &c->cal_x1); jl_u16(t, te, "cal_y1", &c->cal_y1);
        const char *p = jl_member(t, te, "pins");
        if (p) {
            const char *pe = jl_skip_value(p, te); if (!pe) pe = te;
            jl_pin(p, pe, "sck", &c->sck);   jl_pin(p, pe, "mosi", &c->mosi);
            jl_pin(p, pe, "miso", &c->miso); jl_pin(p, pe, "cs",   &c->cs);
            jl_pin(p, pe, "sda", &c->sda);   jl_pin(p, pe, "scl",  &c->scl);
            jl_pin(p, pe, "irq", &c->irq);   jl_pin(p, pe, "rst",  &c->rst);
        }
    }

    const char *b = jl_member(json, e, "batt");
    if (b) {
        const char *be = jl_skip_value(b, e); if (!be) be = e;
        jl_u8 (b, be, "present", &out->batt.present);
        jl_u8 (b, be, "unit", &out->batt.adc_unit);
        jl_u8 (b, be, "ch", &out->batt.adc_channel);
        jl_u16(b, be, "div", &out->batt.divider_x100);
    }

    jl_pin(json, e, "led", &out->led_pin);
    jl_u8 (json, e, "led_low", &out->led_active_low);

    const char *btn = jl_member(json, e, "buttons");
    if (btn && *btn == '[') {
        const char *p = btn + 1;
        for (int i = 0; i < HWCFG_MAX_BUTTONS; i++) {
            p = jl_skip_ws(p, e);
            if (p >= e || *p == ']') break;
            out->buttons[i] = (int8_t)strtol(p, NULL, 10);
            p = jl_skip_value(p, e);
            if (!p) break;
            p = jl_skip_ws(p, e);
            if (p < e && *p == ',') p++;
        }
    }

    return hwcfg_validate(out, err, err_sz);
}

// ── Сериализация ────────────────────────────────────────────
#define APP(...) do {                                       \
        int _w = snprintf(o + n, (n < sz) ? sz - n : 0, __VA_ARGS__); \
        if (_w < 0) return -1;                              \
        n += _w;                                            \
    } while (0)

static int radio_to_json(const radio_cfg_t *r, char *o, size_t sz, int n)
{
    APP("{\"kind\":\"%s\",\"band\":\"%s\",\"freq\":%lu,\"max_dbm\":%d,"
        "\"fsk\":%u,\"spi_host\":%u,\"spi_hz\":%lu,\"uart\":%u,\"baud\":%lu,"
        "\"tcxo_mv\":%u,\"dio2_rfsw\":%u,\"pins\":{",
        hwcfg_radio_name(r->kind), hwcfg_band_name(r->band),
        (unsigned long)r->freq_hz, r->max_dbm, r->supports_fsk,
        r->spi_host, (unsigned long)r->spi_hz, r->uart_num,
        (unsigned long)r->uart_baud, r->tcxo_mv, r->dio2_as_rf_switch);
    const radio_pins_t *p = &r->pins;
    APP("\"sck\":%d,\"miso\":%d,\"mosi\":%d,\"cs\":%d,\"rst\":%d,\"busy\":%d,"
        "\"dio0\":%d,\"dio1\":%d,\"dio2\":%d,\"txen\":%d,\"rxen\":%d,"
        "\"tx\":%d,\"rx\":%d,\"aux\":%d,\"m0\":%d,\"m1\":%d}}",
        p->sck, p->miso, p->mosi, p->cs, p->rst, p->busy,
        p->dio0, p->dio1, p->dio2, p->txen, p->rxen,
        p->tx, p->rx, p->aux, p->m0, p->m1);
    return n;
}

int hwcfg_to_json(const hwcfg_t *c, char *o, size_t sz)
{
    if (!c) c = hwcfg();
    int n = 0;
    APP("{\"profile\":\"%s\",\"name\":\"%s\",\"hw_id\":%u,\"ver\":%u,\"radio\":",
        c->profile, c->name, c->hw_id, c->version);
    n = radio_to_json(&c->radio, o, sz, n);
    if (n < 0) return -1;
    if (c->radio2.kind != RADIO_NONE) {
        APP(",\"radio2\":");
        n = radio_to_json(&c->radio2, o, sz, n);
        if (n < 0) return -1;
    }
    const display_cfg_t *d = &c->disp;
    APP(",\"display\":{\"kind\":\"%s\",\"w\":%u,\"h\":%u,\"rot\":%u,\"addr\":%u,"
        "\"invert\":%u,\"bgr\":%u,\"xoff\":%u,\"yoff\":%u,\"bus_hz\":%lu,"
        "\"spi_host\":%u,\"fg\":%u,\"bg\":%u,\"scale\":%u,\"bl_low\":%u,"
        "\"pins\":{\"sda\":%d,\"scl\":%d,\"sck\":%d,\"mosi\":%d,\"miso\":%d,"
        "\"cs\":%d,\"dc\":%d,\"rst\":%d,\"bl\":%d}}",
        hwcfg_panel_name(d->kind), d->width, d->height, d->rotation, d->i2c_addr,
        d->invert, d->bgr, d->x_offset, d->y_offset, (unsigned long)d->bus_hz,
        d->spi_host, d->fg, d->bg, d->scale, d->bl_active_low,
        d->sda, d->scl, d->sck, d->mosi, d->miso, d->cs, d->dc, d->rst, d->bl);

    const touch_cfg_t *t = &c->touch;
    APP(",\"touch\":{\"kind\":\"%s\",\"spi_host\":%u,\"addr\":%u,\"swap_xy\":%u,"
        "\"invert_x\":%u,\"invert_y\":%u,\"cal_x0\":%u,\"cal_y0\":%u,"
        "\"cal_x1\":%u,\"cal_y1\":%u,\"pins\":{\"sck\":%d,\"mosi\":%d,\"miso\":%d,"
        "\"cs\":%d,\"sda\":%d,\"scl\":%d,\"irq\":%d,\"rst\":%d}}",
        hwcfg_touch_name(t->kind), t->spi_host, t->i2c_addr, t->swap_xy,
        t->invert_x, t->invert_y, t->cal_x0, t->cal_y0, t->cal_x1, t->cal_y1,
        t->sck, t->mosi, t->miso, t->cs, t->sda, t->scl, t->irq, t->rst);

    APP(",\"batt\":{\"present\":%u,\"unit\":%u,\"ch\":%u,\"div\":%u}",
        c->batt.present, c->batt.adc_unit, c->batt.adc_channel, c->batt.divider_x100);
    APP(",\"led\":%d,\"led_low\":%u,\"buttons\":[%d,%d,%d,%d]}",
        c->led_pin, c->led_active_low,
        c->buttons[0], c->buttons[1], c->buttons[2], c->buttons[3]);
    return n;   // > sz означает «не влезло», как у snprintf
}
