#include "imgfsk.h"
#include "config.h"
#include "esp_log.h"

#include "radio_hal.h"
#include "lora_manager.h"
#include "ble_server.h"
#include "proto25.h"
#include "ptt.h"
#include "identity.h"
#include "display.h"
#include "led.h"
#include "mbedtls/base64.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "IMGFSK";

// ── Кадры FSK ────────────────────────────────────────────────
#define IMG_F_DATA  0xB1   // [0xB1][idx][total][≤57 payload]
#define IMG_F_DONE  0xB2   // [0xB2][total]  (несколько раз в конце)
#define IMG_PASSES  3      // проходов по всей картинке (борьба с потерями)
#define IMG_RX_TIMEOUT_MS 25000

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static volatile bool s_busy = false;     // идёт передача или приём
// Картинки по FSK — та же зависимость, что и у звонков.
bool imgfsk_supported(void) { return fsk_available(); }

#define FSK fsk_ops()
bool imgfsk_active(void)    { return s_busy; }

// ── TX ───────────────────────────────────────────────────────
// Буферы картинок держим в куче и только на время передачи.
// Статически это 11.4 КБ × 2, и вместе с img_asm в bridge.c они
// съедали 34 КБ DRAM круглосуточно — ровно столько, что
// rx_task LoRa (8 КБ) переставал создаваться и радио глохло.
// Картинка живёт секунды, LoRa — всегда.
static uint8_t *s_tx_buf = NULL;
static uint16_t s_tx_len     = 0;
static uint8_t  s_tx_profile = FSK_PROFILE_STANDARD;
static uint32_t s_tx_dst     = 0;
static volatile bool s_tx_go = false;
static SemaphoreHandle_t s_tx_sem = NULL;

// ── RX ───────────────────────────────────────────────────────
static uint8_t *s_rx_buf = NULL;
static uint8_t  s_rx_mask[(IMG_MAX_FRAMES + 7) / 8];
static uint8_t  s_rx_total    = 0;
static uint16_t s_rx_len      = 0;
static uint32_t s_rx_src      = 0;
static int      s_rx_profile  = FSK_PROFILE_STANDARD;
static volatile bool s_rx_go  = false;
static SemaphoreHandle_t s_rx_sem = NULL;

static inline bool mask_get(const uint8_t *m, int i){ return (m[i>>3] >> (i&7)) & 1; }
static inline void mask_set(uint8_t *m, int i){ m[i>>3] |= (uint8_t)(1u << (i&7)); }

static int mask_count(const uint8_t *m, int total)
{
    int c = 0;
    for (int i = 0; i < total; i++) if (mask_get(m, i)) c++;
    return c;
}

// Отдать собранную картинку телефону b64-чанками (JSON < 512).
static void img_forward_to_app(uint32_t src, const uint8_t *data, uint16_t len)
{
    const uint16_t RAW_CHUNK = 225;   // 225 сырых → 300 симв b64
    uint8_t total = (uint8_t)((len + RAW_CHUNK - 1) / RAW_CHUNK);
    static char b64[404];
    static char ntf[512];
    for (uint8_t i = 0; i < total; i++) {
        uint16_t off = (uint16_t)i * RAW_CHUNK;
        uint16_t chunk = len - off;
        if (chunk > RAW_CHUNK) chunk = RAW_CHUNK;
        size_t olen = 0;
        if (mbedtls_base64_encode((unsigned char *)b64, sizeof(b64), &olen,
                                  data + off, chunk) != 0) return;
        b64[olen] = '\0';
        snprintf(ntf, sizeof(ntf),
                 "{\"evt\":\"image\",\"node_id\":\"0x%08"PRIx32"\","
                 "\"idx\":%u,\"total\":%u,\"data\":\"%s\"}",
                 src, i, total, b64);
        ble_server_notify(ntf);
        vTaskDelay(pdMS_TO_TICKS(60));
    }
}

// ── TX-задача ────────────────────────────────────────────────
static void img_tx_task(void *arg)
{
    while (1) {
        xSemaphoreTake(s_tx_sem, portMAX_DELAY);
        if (!s_tx_go) continue;
        if (ptt_active()) { s_tx_go = false; continue; }

        s_busy = true;
        uint16_t len = s_tx_len;
        uint8_t  total = (uint8_t)((len + IMG_CHUNK - 1) / IMG_CHUNK);
        int      prof = s_tx_profile;
        ESP_LOGI(TAG, "IMG TX: %u bytes, %u frames, profile=%d", len, total, prof);

        lora_manager_suspend();

        // Анонс по LoRa: [profile][frame_total][имя…] — дважды
        const char *name = identity_name()[0] ? identity_name() : identity_ble_name();
        uint8_t apl[2 + 31];
        apl[0] = (uint8_t)prof;
        apl[1] = total;
        size_t nlen = strlen(name); if (nlen > 31) nlen = 31;
        memcpy(apl + 2, name, nlen);
        uint8_t pkt[PKT25_HDR_SIZE + 33];
        for (int i = 0; i < 2; i++) {
            int n = proto25_build(pkt, sizeof(pkt), s_tx_dst, GROUP_NONE,
                                  PKT25_IMG_START, PRIO25_TEXT, 0, 0, 1, 0,
                                  apl, (uint8_t)(2 + nlen));
            if (n > 0) radio->send(pkt, n);   // напрямую: lora_manager спит
            vTaskDelay(pdMS_TO_TICKS(250));
        }

        FSK->enter(prof);
        display_ptt_show("КАРТИНКА");
        led_set(true);

        // Несколько проходов по всем кадрам — приёмник дедупит по idx.
        uint8_t frame[3 + IMG_CHUNK];
        for (int pass = 0; pass < IMG_PASSES; pass++) {
            for (uint8_t idx = 0; idx < total; idx++) {
                uint16_t off = (uint16_t)idx * IMG_CHUNK;
                uint16_t chunk = len - off;
                if (chunk > IMG_CHUNK) chunk = IMG_CHUNK;
                frame[0] = IMG_F_DATA;
                frame[1] = idx;
                frame[2] = total;
                memcpy(frame + 3, s_tx_buf + off, chunk);
                FSK->send(frame, (uint8_t)(3 + chunk));
                vTaskDelay(pdMS_TO_TICKS(6));   // дать приёмнику разгрестись
            }
            // Прогресс в приложение
            char p[64];
            snprintf(p, sizeof(p), "{\"evt\":\"image_tx\",\"pass\":%d,\"of\":%d}",
                     pass + 1, IMG_PASSES);
            ble_server_notify(p);
        }
        // Done-маркеры
        uint8_t done[2] = { IMG_F_DONE, total };
        for (int i = 0; i < 4; i++) { FSK->send(done, 2); vTaskDelay(pdMS_TO_TICKS(40)); }

        FSK->exit();
        radio->configure(lora_manager_get_speed());
        lora_manager_resume();
        display_ptt_end();
        led_set(false);

        ble_server_notify("{\"evt\":\"image_sent\"}");
        ESP_LOGI(TAG, "IMG TX done");
        free(s_tx_buf); s_tx_buf = NULL;
        s_tx_go = false;
        s_busy = false;
    }
}

// ── RX-задача ────────────────────────────────────────────────
static void img_rx_task(void *arg)
{
    uint8_t rx[64];
    while (1) {
        xSemaphoreTake(s_rx_sem, portMAX_DELAY);
        if (!s_rx_go) continue;
        if (ptt_active() || s_busy) { s_rx_go = false; continue; }

        s_busy = true;
        s_rx_buf = malloc(IMG_MAX_BYTES);
        if (!s_rx_buf) {
            ESP_LOGE(TAG, "нет памяти под приём картинки (%d Б)", IMG_MAX_BYTES);
            ble_server_notify("{\"evt\":\"error\",\"code\":13,"
                              "\"desc\":\"Out of memory for image\"}");
            s_rx_go = false; s_busy = false;
            continue;
        }
        memset(s_rx_mask, 0, sizeof(s_rx_mask));
        s_rx_len = 0;
        uint8_t total = s_rx_total;
        ESP_LOGI(TAG, "IMG RX start from 0x%08"PRIx32" (%u frames, prof=%d)",
                 s_rx_src, total, s_rx_profile);

        lora_manager_suspend();
        FSK->enter(s_rx_profile);
        display_ptt_show("КАРТИНКА<-");
        led_set(true);

        uint32_t start = now_ms();
        uint32_t last_rx = start;
        bool done = false;
        while (!done) {
            int rlen = FSK->read(rx, sizeof(rx));
            if (rlen >= 3 && rx[0] == IMG_F_DATA) {
                uint8_t idx = rx[1];
                uint8_t t   = rx[2];
                if (t > 0 && t <= IMG_MAX_FRAMES && idx < t) {
                    if (total == 0) total = t;
                    uint16_t off = (uint16_t)idx * IMG_CHUNK;
                    uint16_t chunk = (uint16_t)(rlen - 3);
                    if (chunk > IMG_CHUNK) chunk = IMG_CHUNK;
                    if (off + chunk <= IMG_MAX_BYTES && !mask_get(s_rx_mask, idx)) {
                        memcpy(s_rx_buf + off, rx + 3, chunk);
                        mask_set(s_rx_mask, idx);
                        // Последний кадр короче — от него зависит длина
                        uint16_t end = off + chunk;
                        if (idx == t - 1 || end > s_rx_len) s_rx_len = end;
                    }
                    last_rx = now_ms();
                    if (total && mask_count(s_rx_mask, total) == total) done = true;
                }
            } else if (rlen >= 1 && rx[0] == IMG_F_DONE) {
                if (rlen >= 2 && total == 0) total = rx[1];
                last_rx = now_ms();
                // Если done и уже всё собрали — выходим; иначе ждём добор
                if (total && mask_count(s_rx_mask, total) == total) done = true;
                else if (now_ms() - start > 3000) done = true;  // отправитель закончил
            } else {
                vTaskDelay(pdMS_TO_TICKS(3));
            }
            if (now_ms() - last_rx > IMG_RX_TIMEOUT_MS) break;
            if (now_ms() - start > 60000) break;   // жёсткий предел
        }

        FSK->exit();
        radio->configure(lora_manager_get_speed());
        lora_manager_resume();
        display_ptt_end();
        led_set(false);

        int got = total ? mask_count(s_rx_mask, total) : 0;
        ESP_LOGI(TAG, "IMG RX end: %d/%u frames, %u bytes", got, total, s_rx_len);
        if (total && got == total && s_rx_len > 0) {
            led_pulse(400);
            img_forward_to_app(s_rx_src, s_rx_buf, s_rx_len);
        } else {
            char e[96];
            snprintf(e, sizeof(e),
                     "{\"evt\":\"image_fail\",\"got\":%d,\"total\":%u}", got, total);
            ble_server_notify(e);
        }
        free(s_rx_buf); s_rx_buf = NULL;
        s_rx_go = false;
        s_busy = false;
    }
}

// ── Public API ───────────────────────────────────────────────
void imgfsk_send(const uint8_t *data, uint16_t len, uint32_t dst, int profile)
{
    if (!imgfsk_supported()) {
        ble_server_notify("{\"evt\":\"error\",\"code\":12,"
                          "\"desc\":\"No FSK on this radio\"}");
        return;
    }
    if (s_busy || ptt_active()) {
        ble_server_notify("{\"evt\":\"error\",\"code\":10,\"desc\":\"Radio busy\"}");
        return;
    }
    if (len == 0 || len > IMG_MAX_BYTES) {
        ble_server_notify("{\"evt\":\"error\",\"code\":11,\"desc\":\"Image too big\"}");
        return;
    }
    if (profile < 0 || profile > 2) profile = FSK_PROFILE_STANDARD;
    if (!s_tx_buf) s_tx_buf = malloc(IMG_MAX_BYTES);
    if (!s_tx_buf) {
        ble_server_notify("{\"evt\":\"error\",\"code\":13,"
                          "\"desc\":\"Out of memory for image\"}");
        return;
    }
    memcpy(s_tx_buf, data, len);
    s_tx_len = len;
    s_tx_dst = dst;
    s_tx_profile = profile;
    s_tx_go = true;
    xSemaphoreGive(s_tx_sem);
}

void imgfsk_on_start(uint32_t src, int profile, uint8_t frame_total)
{
    if (!imgfsk_supported()) return;   // не услышим — и анонс нам не нужен
    if (s_busy || ptt_active()) return;   // заняты — пропустим (отправитель повторит анонс)
    if (frame_total == 0 || frame_total > IMG_MAX_FRAMES) return;
    if (profile < 0 || profile > 2) profile = FSK_PROFILE_STANDARD;
    s_rx_src = src;
    s_rx_profile = profile;
    s_rx_total = frame_total;
    s_rx_go = true;
    xSemaphoreGive(s_rx_sem);
}

void imgfsk_init(void)
{
    if (!imgfsk_supported()) {
        ESP_LOGI(TAG, "FSK у %s нет — картинки по эфиру недоступны", radio->name);
        return;
    }
    s_tx_sem = xSemaphoreCreateBinary();
    s_rx_sem = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(img_tx_task, "img_tx", 6144, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(img_rx_task, "img_rx", 6144, NULL, 5, NULL, 1);
    ESP_LOGI(TAG, "IMG-FSK ready (%s)", fsk_ops()->name);
}


