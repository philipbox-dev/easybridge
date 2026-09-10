#include "ptt.h"
#include "config.h"
#include "esp_log.h"

#include "radio_hal.h"
#include "lora_manager.h"
#include "ble_server.h"
#include "proto25.h"
#include "identity.h"
#include "display.h"
#include "led.h"
#include "mbedtls/base64.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "PTT";

// Имена приходят из веба и могут содержать кавычки: без экранирования
// одно такое имя порвало бы поток событий в телефоне (см. B16).
static void json_escape_ptt(const char *src, char *dst, size_t dst_size)
{
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 2 < dst_size; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') {
            dst[j++] = '\\';
            dst[j++] = (char)c;
        } else if (c >= 0x20) {
            dst[j++] = (char)c;
        }
    }
    dst[j] = '\0';
}

// ── FSK-кадры ────────────────────────────────────────────────
#define FSK_TYPE_AUDIO     0xA1
#define FSK_TYPE_END       0xA2
#define FSK_TYPE_KEEPALIVE 0xA3
#define FSK_TYPE_ACCEPT    0xA4   // V2.9.2: «взял трубку» → звонящему
#define FSK_TYPE_WEBINFO   0xA5   // V2.9: кто в звонке со стороны веба

#define PTT_WATCHDOG_MS    30000   // нет активности → принудительный выход
#define PTT_KEEPALIVE_MS   2000

typedef struct { uint8_t len; uint8_t data[60]; } fsk_pkt_t;
static QueueHandle_t tx_q = NULL;

static volatile bool s_active = false;
static uint32_t s_last_rx_ms = 0;
static uint8_t  s_tx_seq = 0;

// V2.9.5: диагностика звонка — телефон показывает это в дебаг-панели
static volatile uint32_t s_stat_rx = 0;       // принято FSK-аудиопакетов
static volatile uint32_t s_stat_tx = 0;       // отправлено FSK-аудиопакетов
static volatile uint32_t s_stat_txq_drop = 0; // дропнуто: tx_q полна

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// Звонки по эфиру возможны, только если у радио есть FSK.
// У E220 в прозрачном режиме его нет — там остаётся веб (V2.9).
bool ptt_supported(void) { return fsk_available(); }

// Короткое имя активного FSK-бэкенда. Все вызовы ниже идут через
// него, а не через конкретный чип.
#define FSK fsk_ops()
bool ptt_active(void)    { return s_active; }

static void notify_state(const char *state)
{
    char ntf[96];
    snprintf(ntf, sizeof(ntf), "{\"evt\":\"ptt_state\",\"state\":\"%s\"}", state);
    ble_server_notify(ntf);
}

// ── Вход/выход из сессии ─────────────────────────────────────
void ptt_begin(bool initiator, uint32_t peer_node_id, int mode)
{
    if (s_active) return;
    if (!ptt_supported()) {
        // Радио без FSK — звонок по эфиру невозможен. Говорим об
        // этом явно: приложение по этому событию предложит позвонить
        // через веб (V2.9).
        ESP_LOGW(TAG, "звонок невозможен: у %s нет FSK", radio->name);
        ble_server_notify("{\"evt\":\"ptt_state\",\"state\":\"unsupported\","
                          "\"reason\":\"no_fsk\"}");
        return;
    }
    if (mode < 0 || mode > 2) mode = FSK_PROFILE_STANDARD;
    ESP_LOGI(TAG, "PTT begin (initiator=%d, mode=%d)", (int)initiator, mode);

    lora_manager_suspend();

    if (initiator) {
        // Захват канала: PTT_START по LoRa, hop_limit=0 (не ретранслируется —
        // реалтайм-аудио через relay всё равно невозможен)
        const char *name = identity_name()[0] ? identity_name() : identity_ble_name();
        uint8_t payload[1 + 31];
        payload[0] = (uint8_t)mode;   // V2.8: профиль FSK задаёт звонящий
        size_t nlen = strlen(name);
        if (nlen > 31) nlen = 31;
        memcpy(payload + 1, name, nlen);
        uint8_t pkt[PKT25_HDR_SIZE + 32];
        for (int i = 0; i < 2; i++) {
            // V2.9.1: dst = peer_node_id (адресный звонок) либо NODE_BROADCAST;
            // не-адресаты дропают пакет в for_us-фильтре bridge_on_lora_rx
            int len = proto25_build(pkt, sizeof(pkt),
                                    peer_node_id, GROUP_NONE,
                                    PKT25_PTT_START, PRIO25_SOS,
                                    0, /*hop_limit*/0, 1, 0,
                                    payload, (uint8_t)(1 + nlen));
            if (len > 0) radio->send(pkt, len);   // напрямую: lora_manager спит
            vTaskDelay(pdMS_TO_TICKS(250));
        }
    }

    xQueueReset(tx_q);
    FSK->enter(mode);
    s_last_rx_ms = now_ms();
    s_tx_seq = 0;
    s_stat_rx = 0; s_stat_tx = 0; s_stat_txq_drop = 0;
    s_active = true;
    notify_state("active");

    static const char *mode_names[] = { "ДАЛЬН", "СТАНД", "HD" };
    display_ptt_show(mode_names[mode]);
    led_set(true);   // зелёный горит весь звонок
}

// ── Анонс веб-плеча (V2.9) ───────────────────────────────────
void ptt_announce_web(const char *names, uint8_t count)
{
    if (!s_active) return;          // анонсировать нечего
    // Шлём на текущей скорости LoRa ДО ухода в FSK нельзя — мы уже
    // там. Поэтому анонс уходит первым же кадром FSK-сессии: его
    // услышат ровно те, кто в звонке, а это и есть адресаты.
    uint8_t pkt[64];
    pkt[0] = FSK_TYPE_WEBINFO;
    pkt[1] = count;
    size_t nlen = names ? strlen(names) : 0;
    if (nlen > sizeof(pkt) - 3) nlen = sizeof(pkt) - 3;
    if (nlen) memcpy(pkt + 2, names, nlen);
    pkt[2 + nlen] = '\0';
    FSK->send(pkt, (uint8_t)(3 + nlen));
    ESP_LOGI(TAG, "анонс веб-участников: %u (%s)", count, names ? names : "");
}

void ptt_end(bool local)
{
    if (!s_active) return;
    ESP_LOGI(TAG, "PTT end (local=%d)", (int)local);

    if (local) {
        // V2.9.2: сначала ДОСЫЛАЕМ хвост — недопереданные аудио-кадры из
        // очереди, иначе END обрывал последнюю фразу, хотя человек её уже
        // договорил. Дренаж ограничен по времени, чтобы не зависнуть.
        fsk_pkt_t p;
        int drained = 0;
        while (drained < 32 && xQueueReceive(tx_q, &p, 0) == pdTRUE) {
            FSK->send(p.data, p.len);
            drained++;
        }
        if (drained) ESP_LOGI(TAG, "Flushed %d tail frames before END", drained);
    }

    s_active = false;

    if (local) {
        // END-маркеры в FSK, чтобы вторая сторона вышла сразу
        uint8_t end_pkt[1] = { FSK_TYPE_END };
        for (int i = 0; i < 3; i++) {
            FSK->send(end_pkt, 1);
            vTaskDelay(pdMS_TO_TICKS(60));
        }
    }

    FSK->exit();
    radio->configure(lora_manager_get_speed());  // полный LoRa-реконфиг
    lora_manager_resume();
    display_ptt_end();
    led_set(false);

    if (local) {
        // Дублируем PTT_END по LoRa — вдруг FSK-маркер не дошёл
        uint8_t pkt[PKT25_HDR_SIZE];
        int len = proto25_build(pkt, sizeof(pkt),
                                NODE_BROADCAST, GROUP_NONE,
                                PKT25_PTT_END, PRIO25_ACK,
                                0, 0, 1, 0, NULL, 0);
        if (len > 0) lora_manager_send_async(pkt, len);
    }
    notify_state("idle");
}

// ── Вызываемый принял звонок ─────────────────────────────────
void ptt_accept_call(void)
{
    if (!s_active) return;
    // Несколько ACCEPT-маркеров подряд — в горах пакеты теряются,
    // а звонящему важно надёжно получить «соединено».
    uint8_t acc[1] = { FSK_TYPE_ACCEPT };
    for (int i = 0; i < 3; i++) {
        FSK->send(acc, 1);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGI(TAG, "Sent ACCEPT to caller");
}

// ── Аудио с телефона → FSK-очередь ───────────────────────────
void ptt_queue_audio(const uint8_t *data, int len)
{
    if (!s_active || len <= 0) return;
    // V2.9.6: чанк 57 → 52 = 4 кадра AMR 4.75 (по 13Б). 57 резал пачку
    // ПОПЕРЁК кадра — приёмник кормил декодер обрывками с середины,
    // MediaCodec на телефоне дох навсегда (тишина после пары секунд).
    // 52 держит границы кадров при любом размере пачки, кратном 13.
    int off = 0;
    while (off < len) {
        fsk_pkt_t p;
        int chunk = len - off;
        if (chunk > 52) chunk = 52;
        p.data[0] = FSK_TYPE_AUDIO;
        p.data[1] = s_tx_seq++;
        memcpy(p.data + 2, data + off, chunk);
        p.len = (uint8_t)(chunk + 2);
        if (xQueueSend(tx_q, &p, 0) != pdTRUE) {
            s_stat_txq_drop++;
            ESP_LOGW(TAG, "PTT TX queue full — кадры дропаются");
            break;
        }
        off += chunk;
    }
}

// ── Движок сессии ────────────────────────────────────────────
static void ptt_task(void *arg)
{
    uint8_t rx_buf[64];
    static char b64[128];
    static char ntf[192];
    uint32_t last_tx_ms = 0;
    uint32_t last_stats_ms = 0;

    while (1) {
        if (!s_active) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }

        // V2.9.5: раз в 2с шлём телефону статистику звонка (дебаг-панель).
        // Компактный JSON, один чанк — очередь notify не напрягает.
        if (now_ms() - last_stats_ms > 2000) {
            last_stats_ms = now_ms();
            snprintf(ntf, sizeof(ntf),
                     "{\"evt\":\"ptt_stats\",\"rx\":%u,\"tx\":%u,\"txq\":%u,"
                     "\"qd\":%u,\"nd\":%u,\"na\":%u,\"age\":%u}",
                     (unsigned)s_stat_rx, (unsigned)s_stat_tx,
                     (unsigned)uxQueueMessagesWaiting(tx_q),
                     (unsigned)s_stat_txq_drop,
                     (unsigned)ble_server_notify_dropped(),
                     (unsigned)ble_server_notify_aborted(),
                     (unsigned)(now_ms() - s_last_rx_ms));
            ble_server_notify(ntf);
        }

        bool did_something = false;

        // 1. Приём FSK
        int rlen = FSK->read(rx_buf, sizeof(rx_buf));
        if (rlen > 0) {
            s_last_rx_ms = now_ms();
            did_something = true;
            switch (rx_buf[0]) {
                // V2.9: у звонка есть веб-плечо. Кадр приходит внутри
                // FSK-сессии, а не по LoRa, намеренно: к моменту, когда
                // анонс дошёл бы по LoRa, участники уже ушли в FSK и
                // его бы никто не услышал.
                case FSK_TYPE_WEBINFO: {
                    if (rlen < 3) break;
                    char who[48];
                    uint8_t n = rx_buf[1];
                    uint8_t cp = (uint8_t)(rlen - 2);
                    if (cp > sizeof(who) - 1) cp = sizeof(who) - 1;
                    memcpy(who, rx_buf + 2, cp);
                    who[cp] = '\0';
                    static char esc_w[96];
                    json_escape_ptt(who, esc_w, sizeof(esc_w));
                    snprintf(ntf, sizeof(ntf),
                             "{\"evt\":\"ptt_web\",\"count\":%u,\"names\":\"%s\"}",
                             n, esc_w);
                    ble_server_notify(ntf);
                    display_ptt_show("WEB+FSK");
                    ESP_LOGI(TAG, "в звонке есть веб: %u (%s)", n, who);
                    break;
                }

                case FSK_TYPE_AUDIO: {
                    if (rlen <= 2) break;
                    s_stat_rx++;
                    display_ptt_activity(2);   // ПРИЁМ
                    size_t olen = 0;
                    if (mbedtls_base64_encode((unsigned char *)b64, sizeof(b64),
                                              &olen, rx_buf + 2, rlen - 2) == 0) {
                        b64[olen] = '\0';
                        snprintf(ntf, sizeof(ntf),
                                 "{\"evt\":\"ptt_audio\",\"d\":\"%s\"}", b64);
                        ble_server_notify(ntf);
                    }
                    break;
                }
                case FSK_TYPE_END:
                    ESP_LOGI(TAG, "Peer ended PTT");
                    ptt_end(false);
                    continue;
                case FSK_TYPE_ACCEPT:
                    // V2.9.2: вызываемый принял звонок — сообщаем телефону
                    ESP_LOGI(TAG, "Peer accepted call");
                    ble_server_notify("{\"evt\":\"ptt_answered\"}");
                    break;
                case FSK_TYPE_KEEPALIVE:
                default:
                    break;
            }
        }

        // 2. Передача из очереди
        fsk_pkt_t p;
        if (xQueueReceive(tx_q, &p, 0) == pdTRUE) {
            display_ptt_activity(1);   // ПЕРЕДАЧА
            FSK->send(p.data, p.len);
            s_stat_tx++;
            last_tx_ms = now_ms();
            did_something = true;
        }

        // Тишина дольше секунды → экран «В ЭФИРЕ»
        if (now_ms() - last_tx_ms > 1000 && now_ms() - s_last_rx_ms > 1000) {
            display_ptt_activity(0);
        }

        // 3. Keepalive в тишине (держит watchdog обеих сторон)
        if (now_ms() - last_tx_ms > PTT_KEEPALIVE_MS) {
            uint8_t ka = FSK_TYPE_KEEPALIVE;
            FSK->send(&ka, 1);
            last_tx_ms = now_ms();
        }

        // 4. Watchdog: вторая сторона пропала → не висим в FSK
        if (now_ms() - s_last_rx_ms > PTT_WATCHDOG_MS) {
            ESP_LOGW(TAG, "PTT watchdog — выходим в LoRa");
            ptt_end(true);
            continue;
        }

        if (!did_something) vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void ptt_init(void)
{
    if (!ptt_supported()) {
        // Задачу и очередь не поднимаем совсем: на плате без FSK они
        // просто съели бы 6 КБ стека и никогда бы не пригодились.
        ESP_LOGI(TAG, "FSK у %s нет — движок звонков не запускаем", radio->name);
        return;
    }
    tx_q = xQueueCreate(16, sizeof(fsk_pkt_t));
    xTaskCreatePinnedToCore(ptt_task, "ptt", 6144, NULL, 6, NULL, 1);
    ESP_LOGI(TAG, "PTT engine ready (%s)", fsk_ops()->name);
}


