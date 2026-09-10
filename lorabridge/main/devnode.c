#include "devnode.h"
#include "config.h"
#include "proto25.h"
#include "identity.h"
#include "lora_manager.h"
#include "ble_server.h"
#include "battery.h"
#include "hwcfg.h"
#include "jsonlite.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_random.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "DEVNODE";

#define DEV_NVS_NS   "devcfg"
#define DEV_NVS_KEY  "blob"

static devnode_cfg_t s_cfg;
static adc_oneshot_unit_handle_t s_adc;
static bool s_adc_ready;
static volatile uint32_t s_counters[DEVNODE_MAX_FIELDS];
static uint32_t s_reports;
// Последний услышанный RSSI: датчик показывает его в своих же
// показаниях — при установке сразу видно, добивает ли до базы.
static volatile int s_last_rssi;

// ── Справочник единиц ───────────────────────────────────────
static const char *k_units[] = {
    "", "°C", "%", "гПа", "мм", "В", "А", "лк", "м/с", "ppm", "м", "шт", "с",
};

const char *devnode_unit_name(uint8_t u)
{
    return u < sizeof(k_units) / sizeof(k_units[0]) ? k_units[u] : "";
}

bool devnode_enabled(void) { return s_cfg.enabled; }
const devnode_cfg_t *devnode_cfg(void) { return &s_cfg; }

// ── Счётчик импульсов ───────────────────────────────────────
// Дождемер и анемометр — это язычок с герконом: считаем фронты.
// Обработчик обязан быть коротким, поэтому только инкремент.
static void IRAM_ATTR pulse_isr(void *arg)
{
    uint32_t idx = (uint32_t)(uintptr_t)arg;
    if (idx < DEVNODE_MAX_FIELDS) s_counters[idx]++;
}

// ── Чтение источников ───────────────────────────────────────
static bool adc_setup(void)
{
    if (s_adc_ready) return true;
    adc_oneshot_unit_init_cfg_t cfg = { .unit_id = ADC_UNIT_1 };
    if (adc_oneshot_new_unit(&cfg, &s_adc) != ESP_OK) {
        ESP_LOGE(TAG, "ADC недоступен");
        return false;
    }
    s_adc_ready = true;
    return true;
}

static int32_t read_source(const devnode_field_t *f, int idx)
{
    int32_t raw = 0;
    switch (f->src) {
    case DEVSRC_ADC: {
        if (f->pin < 0 || !adc_setup()) return 0;
        adc_oneshot_chan_cfg_t ch = { .atten = ADC_ATTEN_DB_12,
                                      .bitwidth = ADC_BITWIDTH_DEFAULT };
        adc_oneshot_config_channel(s_adc, (adc_channel_t)f->pin, &ch);
        int v = 0;
        // Четыре отсчёта: ADC на ESP32 шумит, а показания датчика
        // прыгающие на 5% выглядят как сломанная железка.
        int acc = 0, got = 0;
        for (int i = 0; i < 4; i++) {
            if (adc_oneshot_read(s_adc, (adc_channel_t)f->pin, &v) == ESP_OK) {
                acc += v; got++;
            }
        }
        raw = got ? acc / got : 0;
        break;
    }
    case DEVSRC_GPIO_IN:
        if (f->pin < 0) return 0;
        raw = gpio_get_level(f->pin);
        break;
    case DEVSRC_COUNTER:
        raw = (int32_t)s_counters[idx];
        break;
    case DEVSRC_UPTIME:
        raw = (int32_t)(esp_timer_get_time() / 1000000);
        break;
    case DEVSRC_BATTERY: {
        uint8_t p = battery_get_percent();
        raw = (p == BATT_NONE) ? -1 : p;
        break;
    }
    case DEVSRC_CHIP_TEMP:
        // Встроенный датчик меряет кристалл, а не воздух. Как «есть ли
        // перегрев» годится, как метеостанция — нет; так и подписано.
        raw = 0;
        break;
    case DEVSRC_RSSI:
        raw = s_last_rssi;
        break;
    default:
        return 0;
    }
    int32_t mul = f->mul ? f->mul : 1;
    int32_t dv  = f->div_ ? f->div_ : 1;
    return (int32_t)((int64_t)raw * mul / dv);
}

// ── Сборка пакетов ──────────────────────────────────────────
static uint8_t put_str(uint8_t *dst, const char *src, uint8_t max)
{
    uint8_t n = (uint8_t)strlen(src);
    if (n > max) n = max;
    dst[0] = n;
    memcpy(dst + 1, src, n);
    return (uint8_t)(1 + n);
}

void devnode_send_manifest(void)
{
    if (!s_cfg.enabled) return;
    uint8_t p[PKT25_MAX_PAYLOAD];
    uint8_t n = 0;

    p[n++] = DEVNODE_MANIFEST_VER;
    p[n++] = s_cfg.dev_class;
    p[n++] = s_cfg.flags;
    p[n++] = (uint8_t)(s_cfg.interval_s & 0xFF);
    p[n++] = (uint8_t)(s_cfg.interval_s >> 8);
    p[n++] = s_cfg.n_fields;
    p[n++] = s_cfg.n_cmds;
    n += put_str(p + n, s_cfg.name, DEVNODE_NAME_LEN);

    for (int i = 0; i < s_cfg.n_fields; i++) {
        const devnode_field_t *f = &s_cfg.fields[i];
        // Заранее проверяем, влезет ли поле целиком: обрезанный
        // манифест хуже, чем манифест без последнего поля.
        if (n + 4 + 1 + strlen(f->name) > PKT25_MAX_PAYLOAD) break;
        p[n++] = f->type;
        p[n++] = f->unit;
        p[n++] = (uint8_t)f->scale;
        n += put_str(p + n, f->name, DEVNODE_NAME_LEN);
    }
    for (int i = 0; i < s_cfg.n_cmds; i++) {
        const devnode_cmd_t *c = &s_cfg.cmds[i];
        if (n + 2 + 1 + strlen(c->name) > PKT25_MAX_PAYLOAD) break;
        p[n++] = c->id;
        p[n++] = c->action;
        n += put_str(p + n, c->name, DEVNODE_NAME_LEN);
    }

    uint8_t pkt[PKT25_HDR_SIZE + PKT25_MAX_PAYLOAD];
    int len = proto25_build(pkt, sizeof(pkt), NODE_BROADCAST, GROUP_NONE,
                            PKT25_DEV_HELLO, PRIO25_HELLO, 0,
                            PKT25_HOP_MAX, 1, 0, p, n);
    if (len > 0) lora_manager_send(pkt, len);
    ESP_LOGI(TAG, "манифест отправлен (%u байт, полей %u, команд %u)",
             n, s_cfg.n_fields, s_cfg.n_cmds);
}

void devnode_send_data(void)
{
    if (!s_cfg.enabled) return;
    uint8_t p[PKT25_MAX_PAYLOAD];
    uint8_t n = 0;
    p[n++] = DEVNODE_MANIFEST_VER;
    p[n++] = s_cfg.n_fields;

    for (int i = 0; i < s_cfg.n_fields; i++) {
        const devnode_field_t *f = &s_cfg.fields[i];
        int32_t v = read_source(f, i);
        // Каждое значение несёт свой индекс и тип: тот, кто пропустил
        // манифест, всё равно покажет числа, а не молча промолчит.
        if (n + 7 > PKT25_MAX_PAYLOAD) break;
        p[n++] = (uint8_t)i;
        p[n++] = f->type;
        switch (f->type) {
        case DEVF_BOOL:
        case DEVF_PERCENT:
            p[n++] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
            break;
        case DEVF_INT32:
            p[n++] = (uint8_t)(v & 0xFF);
            p[n++] = (uint8_t)((v >> 8) & 0xFF);
            p[n++] = (uint8_t)((v >> 16) & 0xFF);
            p[n++] = (uint8_t)((v >> 24) & 0xFF);
            break;
        default: {   // INT16 / UINT16
            int16_t s16 = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
            p[n++] = (uint8_t)(s16 & 0xFF);
            p[n++] = (uint8_t)((s16 >> 8) & 0xFF);
            break;
        }
        }
    }

    uint8_t pkt[PKT25_HDR_SIZE + PKT25_MAX_PAYLOAD];
    int len = proto25_build(pkt, sizeof(pkt), NODE_BROADCAST, GROUP_NONE,
                            PKT25_DEV_DATA, PRIO25_GPS, 0,
                            PKT25_HOP_MAX, 1, 0, p, n);
    if (len > 0) lora_manager_send(pkt, len);
}

// ── Выполнение команд ───────────────────────────────────────
static void send_ack(uint32_t dst, uint8_t cmd_id, uint8_t status,
                     const char *detail)
{
    uint8_t p[64];
    uint8_t n = 0;
    p[n++] = cmd_id;
    p[n++] = status;              // 0 = ок, иначе код ошибки
    n += put_str(p + n, detail ? detail : "", 40);

    uint8_t pkt[PKT25_HDR_SIZE + 64];
    int len = proto25_build(pkt, sizeof(pkt), dst, GROUP_NONE,
                            PKT25_DEV_ACK, PRIO25_ACK, 0,
                            PKT25_HOP_MAX, 1, 0, p, n);
    if (len > 0) lora_manager_send(pkt, len);
}

static void pulse_task(void *arg)
{
    // Пин и длительность упакованы в аргумент, чтобы не тащить
    // отдельную структуру ради одного импульса.
    uint32_t packed = (uint32_t)(uintptr_t)arg;
    int pin = (int)(packed & 0xFF);
    int lvl_off = (int)((packed >> 8) & 0x1);
    uint32_t ms = packed >> 16;
    vTaskDelay(pdMS_TO_TICKS(ms));
    gpio_set_level(pin, lvl_off);
    vTaskDelete(NULL);
}

bool devnode_on_cmd(uint32_t src, const uint8_t *payload, uint8_t len)
{
    if (!s_cfg.enabled || len < 1) return false;
    uint8_t cmd_id = payload[0];
    uint16_t arg = s_cfg.cmds[0].default_arg;
    if (len >= 3) arg = (uint16_t)(payload[1] | (payload[2] << 8));

    const devnode_cmd_t *c = NULL;
    for (int i = 0; i < s_cfg.n_cmds; i++) {
        if (s_cfg.cmds[i].id == cmd_id) { c = &s_cfg.cmds[i]; break; }
    }
    if (!c) {
        send_ack(src, cmd_id, 1, "нет такой команды");
        return true;
    }

    ESP_LOGW(TAG, "команда %u '%s' от 0x%08lX (arg=%u)",
             cmd_id, c->name, (unsigned long)src, arg);

    switch (c->action) {
    case DEVACT_GPIO_SET: {
        if (c->pin < 0) { send_ack(src, cmd_id, 2, "пин не задан"); break; }
        int lvl = arg ? 1 : 0;
        gpio_set_level(c->pin, c->active_low ? !lvl : lvl);
        send_ack(src, cmd_id, 0, arg ? "включено" : "выключено");
        break;
    }
    case DEVACT_GPIO_PULSE: {
        if (c->pin < 0) { send_ack(src, cmd_id, 2, "пин не задан"); break; }
        uint32_t ms = arg ? arg : (c->default_arg ? c->default_arg : 500);
        if (ms > 60000) ms = 60000;
        gpio_set_level(c->pin, c->active_low ? 0 : 1);
        uint32_t packed = (uint32_t)(c->pin & 0xFF) |
                          ((uint32_t)(c->active_low ? 1 : 0) << 8) | (ms << 16);
        // Отдельной задачей, а не vTaskDelay здесь: иначе приём встал
        // бы на всю длительность импульса и мы пропустили бы пакеты.
        xTaskCreate(pulse_task, "devpulse", 2048,
                    (void *)(uintptr_t)packed, 5, NULL);
        send_ack(src, cmd_id, 0, "импульс");
        break;
    }
    case DEVACT_REPORT:
        devnode_send_data();
        send_ack(src, cmd_id, 0, "отправлено");
        break;
    case DEVACT_REBOOT:
        send_ack(src, cmd_id, 0, "перезагрузка");
        vTaskDelay(pdMS_TO_TICKS(400));   // дать ACK уйти в эфир
        esp_restart();
        break;
    default:
        send_ack(src, cmd_id, 3, "действие не поддержано");
        break;
    }
    return true;
}

void devnode_note_rssi(int rssi) { s_last_rssi = rssi; }

// ── Разбор чужих пакетов для телефона ───────────────────────
static uint8_t take_str(const uint8_t *p, uint8_t len, uint8_t *pos,
                        char *out, size_t out_sz)
{
    out[0] = '\0';
    if (*pos >= len) return 0;
    uint8_t n = p[(*pos)++];
    if (*pos + n > len) n = (uint8_t)(len - *pos);
    size_t cp = n < out_sz - 1 ? n : out_sz - 1;
    memcpy(out, p + *pos, cp);
    out[cp] = '\0';
    *pos = (uint8_t)(*pos + n);
    return n;
}

static size_t esc_json(const char *src, char *dst, size_t dst_sz)
{
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 2 < dst_sz; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') { dst[j++] = '\\'; dst[j++] = (char)c; }
        else if (c >= 0x20) dst[j++] = (char)c;
    }
    dst[j] = '\0';
    return j;
}

int devnode_manifest_to_json(uint32_t src, const uint8_t *p, uint8_t len,
                             char *out, size_t out_sz)
{
    if (len < 8) return -1;
    uint8_t pos = 0;
    uint8_t ver = p[pos++];
    if (ver != DEVNODE_MANIFEST_VER) return -1;
    uint8_t cls = p[pos++], flags = p[pos++];
    uint16_t interval = (uint16_t)(p[pos] | (p[pos + 1] << 8)); pos += 2;
    uint8_t nf = p[pos++], nc = p[pos++];

    char name[DEVNODE_NAME_LEN + 1], esc[48];
    take_str(p, len, &pos, name, sizeof(name));
    esc_json(name, esc, sizeof(esc));

    int n = snprintf(out, out_sz,
                     "{\"evt\":\"dev_hello\",\"node_id\":\"0x%08lX\","
                     "\"name\":\"%s\",\"class\":%u,\"flags\":%u,"
                     "\"interval\":%u,\"fields\":[",
                     (unsigned long)src, esc, cls, flags, interval);

    for (uint8_t i = 0; i < nf && pos + 3 < len; i++) {
        uint8_t type = p[pos++], unit = p[pos++];
        int8_t scale = (int8_t)p[pos++];
        take_str(p, len, &pos, name, sizeof(name));
        esc_json(name, esc, sizeof(esc));
        n += snprintf(out + n, n < (int)out_sz ? out_sz - n : 0,
                      "%s{\"i\":%u,\"name\":\"%s\",\"type\":%u,"
                      "\"unit\":\"%s\",\"scale\":%d}",
                      i ? "," : "", i, esc, type, devnode_unit_name(unit), scale);
    }
    n += snprintf(out + n, n < (int)out_sz ? out_sz - n : 0, "],\"cmds\":[");
    for (uint8_t i = 0; i < nc && pos + 2 < len; i++) {
        uint8_t id = p[pos++], action = p[pos++];
        take_str(p, len, &pos, name, sizeof(name));
        esc_json(name, esc, sizeof(esc));
        n += snprintf(out + n, n < (int)out_sz ? out_sz - n : 0,
                      "%s{\"id\":%u,\"name\":\"%s\",\"action\":%u}",
                      i ? "," : "", id, esc, action);
    }
    n += snprintf(out + n, n < (int)out_sz ? out_sz - n : 0, "]}");
    return n;
}

int devnode_data_to_json(uint32_t src, int rssi, const uint8_t *p, uint8_t len,
                         char *out, size_t out_sz)
{
    if (len < 2) return -1;
    uint8_t pos = 0;
    if (p[pos++] != DEVNODE_MANIFEST_VER) return -1;
    uint8_t nv = p[pos++];

    int n = snprintf(out, out_sz,
                     "{\"evt\":\"dev_data\",\"node_id\":\"0x%08lX\","
                     "\"rssi\":%d,\"v\":[", (unsigned long)src, rssi);
    for (uint8_t k = 0; k < nv && pos + 2 <= len; k++) {
        uint8_t idx = p[pos++], type = p[pos++];
        long v = 0;
        if (type == DEVF_BOOL || type == DEVF_PERCENT) {
            if (pos + 1 > len) break;
            v = p[pos++];
        } else if (type == DEVF_INT32) {
            if (pos + 4 > len) break;
            v = (int32_t)(p[pos] | (p[pos + 1] << 8) |
                          (p[pos + 2] << 16) | ((uint32_t)p[pos + 3] << 24));
            pos += 4;
        } else {
            if (pos + 2 > len) break;
            v = (int16_t)(p[pos] | (p[pos + 1] << 8));
            pos += 2;
        }
        n += snprintf(out + n, n < (int)out_sz ? out_sz - n : 0,
                      "%s{\"i\":%u,\"t\":%u,\"v\":%ld}", k ? "," : "", idx, type, v);
    }
    n += snprintf(out + n, n < (int)out_sz ? out_sz - n : 0, "]}");
    return n;
}

int devnode_ack_to_json(uint32_t src, const uint8_t *p, uint8_t len,
                        char *out, size_t out_sz)
{
    if (len < 2) return -1;
    uint8_t pos = 0;
    uint8_t cmd_id = p[pos++], status = p[pos++];
    char detail[48], esc[64];
    take_str(p, len, &pos, detail, sizeof(detail));
    esc_json(detail, esc, sizeof(esc));
    return snprintf(out, out_sz,
                    "{\"evt\":\"dev_ack\",\"node_id\":\"0x%08lX\",\"cmd\":%u,"
                    "\"status\":%u,\"desc\":\"%s\"}",
                    (unsigned long)src, cmd_id, status, esc);
}

// ── Конфигурация: JSON ↔ NVS ────────────────────────────────
static const char *k_src_names[] = {
    "none", "adc", "gpio", "counter", "uptime", "battery", "chip_temp", "rssi",
};
static const char *k_type_names[] = {
    "int16", "uint16", "int32", "bool", "percent",
};
static const char *k_act_names[] = {
    "none", "gpio_set", "gpio_pulse", "reboot", "report",
};
static const char *k_unit_ids[] = {
    "none", "c", "percent", "hpa", "mm", "v", "a", "lx", "mps", "ppm", "m",
    "count", "s",
};

esp_err_t devnode_apply_json(const char *json, char *err, size_t err_sz)
{
    if (!json) return ESP_ERR_INVALID_ARG;
    if (err && err_sz) err[0] = '\0';
    const char *e = json + strlen(json);

    devnode_cfg_t c;
    memset(&c, 0, sizeof(c));
    c.interval_s = 60;
    c.manifest_every = 10;

    long v;
    c.enabled = jl_int(json, e, "enabled", &v) ? (v != 0) : false;
    jl_u8(json, e, "class", &c.dev_class);
    jl_u16(json, e, "interval", &c.interval_s);
    jl_u16(json, e, "manifest_every", &c.manifest_every);
    jl_str(json, e, "name", c.name, sizeof(c.name));
    if (c.interval_s < 5) c.interval_s = 5;         // не душим эфир
    if (c.manifest_every < 1) c.manifest_every = 1;

    const char *arr = jl_member(json, e, "fields");
    if (arr && *arr == '[') {
        const char *p = arr + 1;
        while (c.n_fields < DEVNODE_MAX_FIELDS) {
            p = jl_skip_ws(p, e);
            if (p >= e || *p != '{') break;
            const char *item_end = jl_skip_value(p, e);
            if (!item_end) break;
            devnode_field_t *f = &c.fields[c.n_fields];
            f->pin = -1;
            f->mul = 1; f->div_ = 1;
            jl_kind(p, item_end, "src", k_src_names,
                    sizeof(k_src_names) / sizeof(k_src_names[0]), &f->src);
            jl_kind(p, item_end, "type", k_type_names,
                    sizeof(k_type_names) / sizeof(k_type_names[0]), &f->type);
            jl_kind(p, item_end, "unit", k_unit_ids,
                    sizeof(k_unit_ids) / sizeof(k_unit_ids[0]), &f->unit);
            jl_pin(p, item_end, "pin", &f->pin);
            if (jl_int(p, item_end, "scale", &v)) f->scale = (int8_t)v;
            if (jl_int(p, item_end, "mul", &v)) f->mul = (int32_t)v;
            if (jl_int(p, item_end, "div", &v)) f->div_ = (int32_t)v;
            if (f->div_ == 0) f->div_ = 1;
            jl_str(p, item_end, "name", f->name, sizeof(f->name));
            if (f->src != DEVSRC_NONE) c.n_fields++;
            p = jl_skip_ws(item_end, e);
            if (p < e && *p == ',') p++; else break;
        }
    }

    arr = jl_member(json, e, "cmds");
    if (arr && *arr == '[') {
        const char *p = arr + 1;
        while (c.n_cmds < DEVNODE_MAX_CMDS) {
            p = jl_skip_ws(p, e);
            if (p >= e || *p != '{') break;
            const char *item_end = jl_skip_value(p, e);
            if (!item_end) break;
            devnode_cmd_t *cm = &c.cmds[c.n_cmds];
            cm->pin = -1;
            cm->id = (uint8_t)(c.n_cmds + 1);
            jl_u8(p, item_end, "id", &cm->id);
            jl_kind(p, item_end, "action", k_act_names,
                    sizeof(k_act_names) / sizeof(k_act_names[0]), &cm->action);
            jl_pin(p, item_end, "pin", &cm->pin);
            jl_u8(p, item_end, "active_low", &cm->active_low);
            jl_u16(p, item_end, "arg", &cm->default_arg);
            jl_str(p, item_end, "name", cm->name, sizeof(cm->name));
            if (cm->action != DEVACT_NONE) c.n_cmds++;
            p = jl_skip_ws(item_end, e);
            if (p < e && *p == ',') p++; else break;
        }
    }

    if (c.enabled && c.n_fields == 0 && c.n_cmds == 0) {
        if (err && err_sz)
            snprintf(err, err_sz, "устройство включено, но у него нет "
                                  "ни одного датчика и ни одной команды");
        return ESP_ERR_INVALID_ARG;
    }
    if (c.n_cmds) c.flags |= DEV_FLAG_CMDS;

    // Пины команд не должны совпадать с пинами радио и дисплея:
    // «реле не щёлкает, зато пропала связь» — худший способ это узнать.
    for (int i = 0; i < c.n_cmds; i++) {
        int8_t pin = c.cmds[i].pin;
        if (pin < 0) continue;
        const radio_pins_t *rp = &hwcfg()->radio.pins;
        const int8_t busy[] = { rp->sck, rp->miso, rp->mosi, rp->cs, rp->rst,
                                rp->busy, rp->dio0, rp->dio1, rp->tx, rp->rx,
                                rp->aux, rp->m0, rp->m1,
                                hwcfg()->disp.sda, hwcfg()->disp.scl,
                                hwcfg()->disp.sck, hwcfg()->disp.mosi,
                                hwcfg()->disp.cs, hwcfg()->disp.dc };
        for (size_t k = 0; k < sizeof(busy) / sizeof(busy[0]); k++) {
            if (busy[k] >= 0 && busy[k] == pin) {
                if (err && err_sz)
                    snprintf(err, err_sz,
                             "GPIO%d команды '%s' уже занят радио или дисплеем",
                             pin, c.cmds[i].name);
                return ESP_ERR_INVALID_ARG;
            }
        }
    }

    nvs_handle_t h;
    esp_err_t r = nvs_open(DEV_NVS_NS, NVS_READWRITE, &h);
    if (r != ESP_OK) {
        if (err && err_sz) snprintf(err, err_sz, "NVS недоступен");
        return r;
    }
    r = nvs_set_blob(h, DEV_NVS_KEY, &c, sizeof(c));
    if (r == ESP_OK) r = nvs_commit(h);
    nvs_close(h);
    if (r != ESP_OK && err && err_sz) snprintf(err, err_sz, "не записалось в NVS");
    if (r == ESP_OK)
        ESP_LOGI(TAG, "конфиг сохранён: '%s', полей %u, команд %u — "
                      "применится после перезагрузки",
                 c.name, c.n_fields, c.n_cmds);
    return r;
}

int devnode_to_json(char *out, size_t out_sz)
{
    int n = snprintf(out, out_sz,
                     "{\"enabled\":%s,\"name\":\"%s\",\"class\":%u,"
                     "\"interval\":%u,\"manifest_every\":%u,\"fields\":[",
                     s_cfg.enabled ? "true" : "false", s_cfg.name,
                     s_cfg.dev_class, s_cfg.interval_s, s_cfg.manifest_every);
    for (int i = 0; i < s_cfg.n_fields; i++) {
        const devnode_field_t *f = &s_cfg.fields[i];
        n += snprintf(out + n, n < (int)out_sz ? out_sz - n : 0,
                      "%s{\"name\":\"%s\",\"src\":\"%s\",\"type\":\"%s\","
                      "\"unit\":\"%s\",\"pin\":%d,\"scale\":%d,"
                      "\"mul\":%ld,\"div\":%ld}",
                      i ? "," : "", f->name, k_src_names[f->src],
                      k_type_names[f->type], k_unit_ids[f->unit],
                      f->pin, f->scale, (long)f->mul, (long)f->div_);
    }
    n += snprintf(out + n, n < (int)out_sz ? out_sz - n : 0, "],\"cmds\":[");
    for (int i = 0; i < s_cfg.n_cmds; i++) {
        const devnode_cmd_t *c = &s_cfg.cmds[i];
        n += snprintf(out + n, n < (int)out_sz ? out_sz - n : 0,
                      "%s{\"id\":%u,\"name\":\"%s\",\"action\":\"%s\","
                      "\"pin\":%d,\"active_low\":%u,\"arg\":%u}",
                      i ? "," : "", c->id, c->name, k_act_names[c->action],
                      c->pin, c->active_low, c->default_arg);
    }
    n += snprintf(out + n, n < (int)out_sz ? out_sz - n : 0, "]}");
    return n;
}

esp_err_t devnode_reset(void)
{
    nvs_handle_t h;
    esp_err_t r = nvs_open(DEV_NVS_NS, NVS_READWRITE, &h);
    if (r != ESP_OK) return r;
    r = nvs_erase_key(h, DEV_NVS_KEY);
    if (r == ESP_ERR_NVS_NOT_FOUND) r = ESP_OK;
    if (r == ESP_OK) r = nvs_commit(h);
    nvs_close(h);
    return r;
}

// ── Задача автономного узла ─────────────────────────────────
static void devnode_task(void *arg)
{
    (void)arg;
    // Разъезжаем старты во времени: десять датчиков, включённых от
    // одного щелчка рубильника, иначе будут вечно говорить хором.
    vTaskDelay(pdMS_TO_TICKS(2000 + (esp_random() % 4000)));
    devnode_send_manifest();

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(s_cfg.interval_s * 1000));
        devnode_send_data();
        if (++s_reports % s_cfg.manifest_every == 0) {
            // Манифест повторяем: тот, кто включился позже, иначе будет
            // видеть безымянные числа до перезагрузки датчика.
            vTaskDelay(pdMS_TO_TICKS(300));
            devnode_send_manifest();
        }
    }
}

static void setup_pins(void)
{
    for (int i = 0; i < s_cfg.n_fields; i++) {
        devnode_field_t *f = &s_cfg.fields[i];
        if (f->pin < 0) continue;
        if (f->src == DEVSRC_GPIO_IN || f->src == DEVSRC_COUNTER) {
            gpio_config_t io = {
                .pin_bit_mask = 1ULL << f->pin,
                .mode = GPIO_MODE_INPUT,
                .pull_up_en = GPIO_PULLUP_ENABLE,
                .intr_type = (f->src == DEVSRC_COUNTER)
                             ? GPIO_INTR_NEGEDGE : GPIO_INTR_DISABLE,
            };
            gpio_config(&io);
            if (f->src == DEVSRC_COUNTER) {
                static bool isr_installed;
                if (!isr_installed) {
                    gpio_install_isr_service(0);
                    isr_installed = true;
                }
                gpio_isr_handler_add(f->pin, pulse_isr,
                                     (void *)(uintptr_t)i);
            }
        }
    }
    for (int i = 0; i < s_cfg.n_cmds; i++) {
        devnode_cmd_t *c = &s_cfg.cmds[i];
        if (c->pin < 0) continue;
        if (c->action == DEVACT_GPIO_SET || c->action == DEVACT_GPIO_PULSE) {
            gpio_config_t io = { .pin_bit_mask = 1ULL << c->pin,
                                 .mode = GPIO_MODE_OUTPUT };
            gpio_config(&io);
            // Стартовое состояние — выключено. Реле, которое включается
            // само при перезагрузке, однажды включится не вовремя.
            gpio_set_level(c->pin, c->active_low ? 1 : 0);
        }
    }
}

void devnode_init(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));

    nvs_handle_t h;
    if (nvs_open(DEV_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t sz = sizeof(s_cfg);
        if (nvs_get_blob(h, DEV_NVS_KEY, &s_cfg, &sz) != ESP_OK ||
            sz != sizeof(s_cfg)) {
            memset(&s_cfg, 0, sizeof(s_cfg));
        }
        nvs_close(h);
    }

    if (!s_cfg.enabled) {
        ESP_LOGI(TAG, "роль автономного устройства выключена "
                      "(чужие датчики всё равно принимаем)");
        return;
    }
    if (s_cfg.interval_s < 5) s_cfg.interval_s = 60;
    if (s_cfg.manifest_every < 1) s_cfg.manifest_every = 10;

    setup_pins();
    xTaskCreate(devnode_task, "devnode", 4096, NULL, 3, NULL);
    ESP_LOGI(TAG, "устройство '%s': полей %u, команд %u, интервал %u с",
             s_cfg.name, s_cfg.n_fields, s_cfg.n_cmds, s_cfg.interval_s);
}
