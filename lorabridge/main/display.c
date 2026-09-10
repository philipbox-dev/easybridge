#include "display.h"
#include "panel.h"
#include "font8x16.h"
#include "font8x16_cyr.h"
#include "logo.h"
#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "DISPLAY";

#define SPLASH_TIMEOUT_MS    2500
#define RX_MSG_TIMEOUT_MS    10000
#define TX_OK_TIMEOUT_MS     3000
#define TX_FAIL_TIMEOUT_MS   5000
#define ERROR_TIMEOUT_MS     5000

static display_screen_t current_screen = SCREEN_SPLASH;
static uint32_t screen_start_ms = 0;
static SemaphoreHandle_t disp_mutex = NULL;
static bool display_ready = false;
static int  anim_frame = 0;

// Кеш idle
static char     idle_my_name[24]   = {0};
static char     idle_peer_name[24] = {0};
static bool     idle_peer_online   = false;
static int      idle_rssi          = 0;
static int      idle_sf            = 0;
static int      idle_msg_sent      = 0;
static int      idle_msg_recv      = 0;

// Кеш RX
static char     rx_from[24]   = {0};
static int      rx_rssi       = 0;
static char     rx_text[128]  = {0};

// Кеш ошибки
static char     err_title[24] = {0};
static char     err_desc[64]  = {0};

// Метки скоростей E220
static const char *speed_labels[] = {
    "2.4k", "2.4k", "2.4k", "4.8k", "9.6k", "19.2k", "38.4k", "62.5k"
};

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// ============================================================
// Рисование глифов
// ============================================================
static void draw_glyph(int x, int y, const uint8_t *glyph)
{
    for (int col = 0; col < 8; col++) {
        uint8_t byte = glyph[col];
        for (int bit = 0; bit < 8; bit++) {
            if (byte & (1 << bit)) {
                panel_set_pixel(x + col, y + bit, true);
            }
        }
    }
    for (int col = 0; col < 8; col++) {
        uint8_t byte = glyph[8 + col];
        for (int bit = 0; bit < 8; bit++) {
            if (byte & (1 << bit)) {
                panel_set_pixel(x + col, y + 8 + bit, true);
            }
        }
    }
}

static int get_glyph(const char *str, const uint8_t **glyph_out)
{
    uint8_t c = (uint8_t)str[0];

    if (c < 0x80) {
        if (c >= 32 && c <= 126) {
            *glyph_out = font8x16[c - 32];
        } else {
            *glyph_out = font8x16[0];
        }
        return 1;
    }

    if (c == 0xD0 && str[1]) {
        uint8_t c2 = (uint8_t)str[1];
        if (c2 == 0x81) { *glyph_out = font8x16_cyr[64]; return 2; }
        if (c2 >= 0x90 && c2 <= 0x9F) { *glyph_out = font8x16_cyr[c2 - 0x90]; return 2; }
        if (c2 >= 0xA0 && c2 <= 0xAF) { *glyph_out = font8x16_cyr[c2 - 0xA0 + 16]; return 2; }
        if (c2 >= 0xB0 && c2 <= 0xBF) { *glyph_out = font8x16_cyr[c2 - 0xB0 + 32]; return 2; }
    }

    if (c == 0xD1 && str[1]) {
        uint8_t c2 = (uint8_t)str[1];
        if (c2 >= 0x80 && c2 <= 0x8F) { *glyph_out = font8x16_cyr[c2 - 0x80 + 48]; return 2; }
        if (c2 == 0x91) { *glyph_out = font8x16_cyr[65]; return 2; }
    }

    *glyph_out = font8x16[0];
    return (c >= 0xC0) ? 2 : 1;
}

static void draw_text(int x, int y, const char *text)
{
    int px = x;
    const char *p = text;
    while (*p) {
        if (px >= PANEL_W - 7) break;
        const uint8_t *glyph;
        int consumed = get_glyph(p, &glyph);
        draw_glyph(px, y, glyph);
        px += 8;
        p += consumed;
    }
}

static void draw_special(int x, int y, int index)
{
    if (index < 0 || index > 6) return;
    draw_glyph(x, y, font8x16_special[index]);
}

static void draw_hline(int x, int y, int w)
{
    for (int i = 0; i < w; i++) {
        panel_set_pixel(x + i, y, true);
    }
}

static int text_width(const char *text)
{
    int w = 0;
    const char *p = text;
    while (*p) {
        const uint8_t *glyph;
        int consumed = get_glyph(p, &glyph);
        (void)glyph;
        w += 8;
        p += consumed;
    }
    return w;
}

// ============================================================
// Графические примитивы для анимаций
// ============================================================
static void draw_line(int x0, int y0, int x1, int y1)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (1) {
        panel_set_pixel(x0, y0, true);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// Дуга (полукруг) — радиоволна
static void draw_wave_arc(int cx, int cy, int r, bool right)
{
    for (int dy = -r * 7 / 10; dy <= r * 7 / 10; dy++) {
        int target = r * r - dy * dy;
        int dx = 0;
        while ((dx + 1) * (dx + 1) <= target) dx++;
        if (right) {
            panel_set_pixel(cx + dx, cy + dy, true);
        } else {
            panel_set_pixel(cx - dx, cy + dy, true);
        }
    }
}

// ============================================================
// Рендер экранов
// ============================================================
static void render_splash(void)
{
    panel_clear();
    panel_draw_bitmap(0, 0, logo_bitmap, LOGO_WIDTH, LOGO_HEIGHT);
    panel_update();
}

static void render_idle(void)
{
    panel_clear();
    char buf[32];

    // Строка 0: Имя + RSSI
    draw_text(0, 0, idle_my_name);
    if (idle_peer_online && idle_rssi != 0) {
        snprintf(buf, sizeof(buf), "%ddB", idle_rssi);
        int w = text_width(buf);
        draw_text(PANEL_W - w, 0, buf);
    }

    draw_hline(0, 17, PANEL_W);

    // Строка 1: Пир
    if (idle_peer_online) {
        draw_special(0, 20, 5);  // ●
    } else {
        draw_special(0, 20, 6);  // ○
    }
    snprintf(buf, sizeof(buf), "%s %s",
             idle_peer_name[0] ? idle_peer_name : "???",
             idle_peer_online ? "online" : "offline");
    draw_text(10, 20, buf);

    // Строка 3: Счётчики + скорость
    draw_special(0, 48, 0);  // ↑
    snprintf(buf, sizeof(buf), "%d", idle_msg_sent);
    draw_text(10, 48, buf);

    draw_special(40, 48, 1);  // ↓
    snprintf(buf, sizeof(buf), "%d", idle_msg_recv);
    draw_text(50, 48, buf);

    int idx = (idle_sf >= 0 && idle_sf <= 7) ? idle_sf : 0;
    int w = text_width(speed_labels[idx]);
    draw_text(PANEL_W - w, 48, speed_labels[idx]);

    panel_update();
}

static void render_rx_message(void)
{
    panel_clear();
    char buf[32];

    draw_special(0, 0, 2);  // ←
    draw_text(10, 0, rx_from);
    if (rx_rssi != 0) {
        snprintf(buf, sizeof(buf), "%ddB", rx_rssi);
        int w = text_width(buf);
        draw_text(PANEL_W - w, 0, buf);
    }

    draw_hline(0, 17, PANEL_W);

    // Текст — до 3 строк
    int y = 20;
    const char *p = rx_text;
    int line = 0;
    int max_chars = PANEL_W / 8;

    while (*p && line < 3) {
        int chars = 0;
        const char *line_start = p;

        while (*p && chars < max_chars) {
            const uint8_t *glyph;
            int consumed = get_glyph(p, &glyph);
            (void)glyph;
            p += consumed;
            chars++;
        }

        int px = 0;
        const char *q = line_start;
        while (q < p) {
            const uint8_t *glyph;
            int consumed = get_glyph(q, &glyph);
            draw_glyph(px, y, glyph);
            px += 8;
            q += consumed;
        }

        y += 16;
        line++;
    }

    panel_update();
}

// ★ Анимированная отправка — радиоволны
static void render_tx_sending_anim(int frame)
{
    panel_clear();

    int cx = 64, cy = 22;

    // Точка-антенна в центре
    for (int dx = -2; dx <= 2; dx++)
        for (int dy = -2; dy <= 2; dy++)
            if (dx * dx + dy * dy <= 4)
                panel_set_pixel(cx + dx, cy + dy, true);

    // Радиоволны — расходящиеся дуги
    int waves = frame % 4;  // 0..3
    for (int w = 0; w < waves; w++) {
        int r = 8 + w * 8;
        draw_wave_arc(cx, cy, r, true);
        draw_wave_arc(cx, cy, r, false);
    }

    // Текст "Sending" + анимированные точки
    char txt[16] = "Sending";
    int dots = frame % 4;
    for (int i = 0; i < dots; i++) txt[7 + i] = '.';
    txt[7 + dots] = '\0';

    int tw = text_width(txt);
    draw_text((PANEL_W - tw) / 2, 46, txt);

    panel_update();
}

// ★ Отправлено — большая галочка
static void render_tx_ok(void)
{
    panel_clear();

    // Жирная галочка (2 линии для толщины)
    draw_line(44, 26, 58, 40);
    draw_line(45, 26, 59, 40);
    draw_line(58, 40, 84, 14);
    draw_line(59, 40, 85, 14);

    const char *msg = "Delivered!";
    int tw = text_width(msg);
    draw_text((PANEL_W - tw) / 2, 48, msg);

    panel_update();
}

// ★ Ошибка отправки — большой крест
static void render_tx_fail(void)
{
    panel_clear();

    // Жирный крест
    draw_line(48, 12, 80, 44);
    draw_line(49, 12, 81, 44);
    draw_line(80, 12, 48, 44);
    draw_line(81, 12, 49, 44);

    const char *msg = "TX Error";
    int tw = text_width(msg);
    draw_text((PANEL_W - tw) / 2, 48, msg);

    panel_update();
}

static void render_error(void)
{
    panel_clear();
    draw_special(0, 0, 4);  // ✗
    draw_text(10, 0, err_title);
    draw_hline(0, 17, PANEL_W);
    draw_text(0, 24, err_desc);
    panel_update();
}

// ============================================================
// Таймаут-задача
// ============================================================
static void set_screen(display_screen_t scr)
{
    current_screen = scr;
    screen_start_ms = now_ms();
}

static void display_task(void *arg)
{
    render_splash();
    set_screen(SCREEN_SPLASH);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(200));

        uint32_t elapsed = now_ms() - screen_start_ms;

        if (xSemaphoreTake(disp_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
            continue;
        }

        bool need_idle = false;
        switch (current_screen) {
            case SCREEN_SPLASH:
                if (elapsed > SPLASH_TIMEOUT_MS) need_idle = true;
                break;
            case SCREEN_RX_MSG:
                if (elapsed > RX_MSG_TIMEOUT_MS) need_idle = true;
                break;
            case SCREEN_TX_OK:
                if (elapsed > TX_OK_TIMEOUT_MS) need_idle = true;
                break;
            case SCREEN_TX_FAIL:
                if (elapsed > TX_FAIL_TIMEOUT_MS) need_idle = true;
                break;
            case SCREEN_ERROR:
                if (elapsed > ERROR_TIMEOUT_MS) need_idle = true;
                break;
            case SCREEN_TX_SENDING:
                // ★ Анимация отправки
                anim_frame++;
                render_tx_sending_anim(anim_frame);
                break;
            default:
                break;
        }

        if (need_idle) {
            set_screen(SCREEN_IDLE);
            render_idle();
        }

        // Обновлять idle каждые 5 сек
        if (current_screen == SCREEN_IDLE && elapsed > 5000) {
            screen_start_ms = now_ms();
            render_idle();
        }

        xSemaphoreGive(disp_mutex);
    }
}

// ============================================================
// Публичный API
// ============================================================
// ── PTT-звонок ───────────────────────────────────────────────
static char ptt_mode_name[16] = {0};
static int  ptt_dir = 0;            // 0=эфир, 1=TX, 2=RX
static uint32_t ptt_last_draw = 0;

static void render_ptt(void)
{
    panel_clear();

    draw_text(0, 0, "РАЦИЯ");
    int w = text_width(ptt_mode_name);
    draw_text(PANEL_W - w, 0, ptt_mode_name);
    draw_hline(0, 17, PANEL_W);

    const char *status = (ptt_dir == 1) ? "ПЕРЕДАЧА"
                       : (ptt_dir == 2) ? "ПРИЁМ" : "В ЭФИРЕ";
    int sw = text_width(status);
    draw_text((PANEL_W - sw) / 2, 24, status);

    // Анимация: бегущие столбики-эквалайзер при активности,
    // мигающая точка в тишине
    anim_frame++;
    if (ptt_dir != 0) {
        // 16 столбиков псевдо-эквалайзера
        for (int i = 0; i < 16; i++) {
            int h = 3 + ((i * 7 + anim_frame * 5) % 14);
            int x = 4 + i * 8;
            if (ptt_dir == 2) h = 3 + ((i * 5 + anim_frame * 3) % 12);
            panel_fill_rect(x, 60 - h, 5, h, true);
        }
        // Стрелки направления
        if (ptt_dir == 1) {
            for (int a = 0; a < 3; a++) {
                int ax = 100 + a * 9 - (anim_frame % 3) * 3;
                if (ax > 96 && ax < 122) {
                    draw_line(ax, 26, ax + 4, 30);
                    draw_line(ax + 4, 30, ax, 34);
                }
            }
        }
    } else {
        // Тишина: пульсирующая точка по центру
        int r = 2 + (anim_frame % 4);
        panel_fill_rect(PANEL_W / 2 - r / 2, 52 - r / 2, r, r, true);
    }

    panel_update();
}

void display_ptt_show(const char *mode_name)
{
    if (!display_ready) return;
    if (xSemaphoreTake(disp_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        strncpy(ptt_mode_name, mode_name, sizeof(ptt_mode_name) - 1);
        ptt_dir = 0;
        set_screen(SCREEN_PTT);
        render_ptt();
        xSemaphoreGive(disp_mutex);
    }
}

void display_ptt_activity(int dir)
{
    if (!display_ready || current_screen != SCREEN_PTT) return;
    uint32_t now = now_ms();
    // не чаще 7 кадров/с — I2C не любит суету, а звук важнее
    if (dir == ptt_dir && now - ptt_last_draw < 150) return;
    if (xSemaphoreTake(disp_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        ptt_dir = dir;
        ptt_last_draw = now;
        render_ptt();
        xSemaphoreGive(disp_mutex);
    }
}

void display_ptt_end(void)
{
    if (!display_ready) return;
    if (xSemaphoreTake(disp_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        set_screen(SCREEN_IDLE);
        render_idle();
        xSemaphoreGive(disp_mutex);
    }
}

void display_init(void)
{
    disp_mutex = xSemaphoreCreateMutex();

    esp_err_t ret = panel_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SSD1306 init failed!");
        return;
    }

    display_ready = true;
    xTaskCreate(display_task, "display", 4096, NULL, 1, NULL);
    ESP_LOGI(TAG, "Display init OK");
}

void display_show_splash(void)
{
    if (!display_ready) return;
    if (xSemaphoreTake(disp_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        set_screen(SCREEN_SPLASH);
        render_splash();
        xSemaphoreGive(disp_mutex);
    }
}

void display_show_idle(const char *my_name, const char *peer_name,
                       bool peer_online, int rssi, int sf,
                       int msg_sent, int msg_recv)
{
    if (!display_ready) return;
    if (xSemaphoreTake(disp_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        strncpy(idle_my_name, my_name, sizeof(idle_my_name) - 1);
        strncpy(idle_peer_name, peer_name, sizeof(idle_peer_name) - 1);
        idle_peer_online = peer_online;
        idle_rssi = rssi;
        idle_sf = sf;
        idle_msg_sent = msg_sent;
        idle_msg_recv = msg_recv;

        if (current_screen == SCREEN_IDLE || current_screen == SCREEN_SPLASH) {
            set_screen(SCREEN_IDLE);
            render_idle();
        }
        xSemaphoreGive(disp_mutex);
    }
}

void display_show_rx_message(const char *from, int rssi, const char *text)
{
    if (!display_ready) return;
    if (xSemaphoreTake(disp_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        strncpy(rx_from, from, sizeof(rx_from) - 1);
        rx_rssi = rssi;
        strncpy(rx_text, text, sizeof(rx_text) - 1);
        set_screen(SCREEN_RX_MSG);
        render_rx_message();
        xSemaphoreGive(disp_mutex);
    }
}

void display_show_tx_sending(void)
{
    if (!display_ready) return;
    if (xSemaphoreTake(disp_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        anim_frame = 0;
        set_screen(SCREEN_TX_SENDING);
        render_tx_sending_anim(0);
        xSemaphoreGive(disp_mutex);
    }
}

void display_show_tx_ok(void)
{
    if (!display_ready) return;
    if (xSemaphoreTake(disp_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        idle_msg_sent++;
        set_screen(SCREEN_TX_OK);
        render_tx_ok();
        xSemaphoreGive(disp_mutex);
    }
}

void display_show_tx_fail(void)
{
    if (!display_ready) return;
    if (xSemaphoreTake(disp_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        set_screen(SCREEN_TX_FAIL);
        render_tx_fail();
        xSemaphoreGive(disp_mutex);
    }
}

void display_show_error(const char *title, const char *desc)
{
    if (!display_ready) return;
    if (xSemaphoreTake(disp_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        strncpy(err_title, title, sizeof(err_title) - 1);
        strncpy(err_desc, desc, sizeof(err_desc) - 1);
        set_screen(SCREEN_ERROR);
        render_error();
        xSemaphoreGive(disp_mutex);
    }
}

void display_update_idle(const char *my_name, const char *peer_name,
                         bool peer_online, int rssi, int sf,
                         int msg_sent, int msg_recv)
{
    strncpy(idle_my_name, my_name, sizeof(idle_my_name) - 1);
    strncpy(idle_peer_name, peer_name, sizeof(idle_peer_name) - 1);
    idle_peer_online = peer_online;
    idle_rssi = rssi;
    idle_sf = sf;
    idle_msg_sent = msg_sent;
    idle_msg_recv = msg_recv;
}

void display_on_peer_status(const char *peer_name, bool online, int rssi)
{
    if (peer_name && peer_name[0]) {
        strncpy(idle_peer_name, peer_name, sizeof(idle_peer_name) - 1);
    }
    idle_peer_online = online;
    idle_rssi = rssi;
    if (current_screen == SCREEN_IDLE) {
        display_show_idle(idle_my_name, idle_peer_name,
                          online, rssi, idle_sf,
                          idle_msg_sent, idle_msg_recv);
    }
}

void display_on_msg_received(const char *from, int rssi, const char *text)
{
    idle_msg_recv++;
    display_show_rx_message(from, rssi, text);
}

void display_on_msg_sending(void)
{
    display_show_tx_sending();
}

void display_on_msg_sent(void)
{
    display_show_tx_ok();
}

void display_on_msg_failed(void)
{
    display_show_tx_fail();
}
