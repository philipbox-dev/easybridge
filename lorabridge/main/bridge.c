// bridge.c — Easy Bridge v2.5
// BLE ↔ LoRa bridge using proto25 air protocol
// BLE interface (JSON) stays backward-compatible with the Android app.
#include "bridge.h"
#include "radio_hal.h"
#include "touch.h"
#include "hwcfg.h"
#include "devnode.h"
#include "proto25.h"
#include "protocol.h"      // kept for old-style heartbeat fallback
#include "lora_manager.h"
#include "peer_manager.h"
#include "ble_server.h"
#include "display.h"
#include "nodedb.h"
#include "blacklist.h"
#include "identity.h"
#include "battery.h"
#include "ptt.h"
#include "imgfsk.h"
#include "led.h"
#include "config.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "BRIDGE";

// ── Profile ───────────────────────────────────────────────────
static char  user_display_name[32] = {0};
static char  known_peer_name[32]   = {0};
static int   bridge_msg_sent       = 0;
static int   bridge_msg_recv       = 0;
static int   bridge_current_sf     = LORA_SF_SLOW;
static int   bridge_last_rssi      = 0;
static float bridge_last_snr       = 0.0f;
// Когда мы в последний раз хоть что-то услышали. Нужно откату скорости
// (B14): «эфир замолчал» и «мы одни в лесу» — разные вещи.
static volatile uint32_t bridge_last_rx_ms = 0;
// V2: antenna type for RSSI compensation (stored in NVS)
static char  bridge_antenna[16]    = "STOCK";
#define BRIDGE_NVS_NS  "bridge"
#define NVS_KEY_ANTENNA "antenna"

static void bridge_load_antenna(void) {
    nvs_handle_t h;
    if (nvs_open(BRIDGE_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t sz = sizeof(bridge_antenna);
    nvs_get_str(h, NVS_KEY_ANTENNA, bridge_antenna, &sz);
    nvs_close(h);
}
static void bridge_save_antenna(const char *val) {
    nvs_handle_t h;
    if (nvs_open(BRIDGE_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, NVS_KEY_ANTENNA, val);
    nvs_commit(h);
    nvs_close(h);
}

// ── Crypto PSK persistence (V2.9) ─────────────────────────────
// Key = SHA-256(passphrase)[0:16]. Two devices with the same passphrase
// interoperate; empty passphrase disables encryption (plaintext).
#define NVS_KEY_PSK "psk16"
static void crypto_load_key(void) {
    nvs_handle_t h;
    if (nvs_open(BRIDGE_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t key[16]; size_t sz = sizeof(key);
    if (nvs_get_blob(h, NVS_KEY_PSK, key, &sz) == ESP_OK && sz == 16) {
        proto25_set_key(key);
    }
    nvs_close(h);
}
static void crypto_save_key(const uint8_t *key16) {
    nvs_handle_t h;
    if (nvs_open(BRIDGE_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (key16) nvs_set_blob(h, NVS_KEY_PSK, key16, 16);
    else       nvs_erase_key(h, NVS_KEY_PSK);
    nvs_commit(h);
    nvs_close(h);
}

static const char *get_display_name(void) {
    return user_display_name[0] ? user_display_name : identity_ble_name();
}
static const char *get_peer_name(void) {
    return known_peer_name[0] ? known_peer_name : "???";
}
static void refresh_idle_display(void) {
    display_show_idle(get_display_name(), get_peer_name(),
                      peer_manager_is_online(), peer_manager_get_rssi(),
                      bridge_current_sf, bridge_msg_sent, bridge_msg_recv);
}

// ── JSON helpers ──────────────────────────────────────────────
static int get_string(const char *json, const char *key, char *out, size_t out_size)
{
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *start = strstr(json, pattern);
    if (!start) return -1;
    start += strlen(pattern);
    size_t i = 0;
    while (*start && *start != '"' && i < out_size - 1) {
        if (*start == '\\' && *(start + 1)) {
            start++;
            switch (*start) {
                case '"':  out[i++] = '"';  break;
                case '\\': out[i++] = '\\'; break;
                case 'n':  out[i++] = '\n'; break;
                case 'r':  out[i++] = '\r'; break;
                case 't':  out[i++] = '\t'; break;
                default:   out[i++] = *start; break;
            }
        } else {
            out[i++] = *start;
        }
        start++;
    }
    out[i] = '\0';
    return (int)i;
}

static int get_int(const char *json, const char *key)
{
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *pos = strstr(json, pattern);
    if (!pos) return 0;
    return atoi(pos + strlen(pattern));
}

static uint32_t get_uint32_hex(const char *json, const char *key)
{
    char tmp[12] = {0};
    get_string(json, key, tmp, sizeof(tmp));
    return (uint32_t)strtoul(tmp, NULL, 0);
}

static size_t json_escape(const char *src, char *dst, size_t dst_size)
{
    size_t j = 0;
    for (size_t i = 0; src[i] && j < dst_size - 2; i++) {
        char c = src[i];
        if (c == '"' || c == '\\') {
            if (j + 2 >= dst_size) break;
            dst[j++] = '\\'; dst[j++] = c;
        } else if (c == '\n') {
            if (j + 2 >= dst_size) break;
            dst[j++] = '\\'; dst[j++] = 'n';
        } else if (c == '\r') {
            // skip
        } else {
            dst[j++] = c;
        }
    }
    dst[j] = '\0';
    return j;
}

// ── Sequence ID mapping (proto25 packet_id ↔ app seq) ─────────
#define SEQ_MAP_SIZE 16
static struct { uint16_t pkt_id; int app_seq; } seq_map[SEQ_MAP_SIZE];
static int seq_map_head = 0;

static void seq_map_add(uint16_t pkt_id, int app_seq)
{
    seq_map[seq_map_head].pkt_id  = pkt_id;
    seq_map[seq_map_head].app_seq = app_seq;
    seq_map_head = (seq_map_head + 1) % SEQ_MAP_SIZE;
}
static int seq_map_find(uint16_t pkt_id)
{
    for (int i = 0; i < SEQ_MAP_SIZE; i++)
        if (seq_map[i].pkt_id == pkt_id) return seq_map[i].app_seq;
    return -1;
}

// ── Proto25 fragment reassembly ───────────────────────────────
// B3: several concurrent sessions keyed on (src, packet_id). A single
// shared session meant two peers sending long messages at once wiped
// each other's buffer; now each gets its own slot.
#define FRAG25_BUF_SIZE  1240
#define FRAG25_TIMEOUT_MS 8000
#define FRAG25_SESSIONS   4
typedef struct {
    bool     in_use;
    uint32_t src_node_id;
    uint16_t packet_id;
    uint8_t  frag_total;
    uint8_t  received_mask;
    uint16_t total_len;
    uint32_t first_ms;
    uint8_t  buf[FRAG25_BUF_SIZE];
} frag25_sess_t;
static frag25_sess_t frag25_sess[FRAG25_SESSIONS];

static bool frag25_reassemble(const pkt25_hdr_t *hdr,
                               const uint8_t *payload, uint8_t plen,
                               uint8_t *out_buf, uint16_t *out_len)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

    if (hdr->frag_total == 1) {
        memcpy(out_buf, payload, plen);
        *out_len = plen;
        return true;
    }

    // Expire stale sessions
    for (int i = 0; i < FRAG25_SESSIONS; i++) {
        if (frag25_sess[i].in_use &&
            now - frag25_sess[i].first_ms > FRAG25_TIMEOUT_MS) {
            ESP_LOGW(TAG, "Frag25 timeout, dropping pid=%d", frag25_sess[i].packet_id);
            memset(&frag25_sess[i], 0, sizeof(frag25_sess[i]));
        }
    }

    // Find the session for this (src, packet_id)
    frag25_sess_t *s = NULL;
    for (int i = 0; i < FRAG25_SESSIONS; i++) {
        if (frag25_sess[i].in_use &&
            frag25_sess[i].src_node_id == hdr->src_node_id &&
            frag25_sess[i].packet_id   == hdr->packet_id) {
            s = &frag25_sess[i];
            break;
        }
    }
    // New session: take a free slot, else evict the oldest
    if (!s) {
        for (int i = 0; i < FRAG25_SESSIONS; i++) {
            if (!frag25_sess[i].in_use) { s = &frag25_sess[i]; break; }
        }
        if (!s) {
            uint32_t oldest = UINT32_MAX; int oi = 0;
            for (int i = 0; i < FRAG25_SESSIONS; i++) {
                if (frag25_sess[i].first_ms < oldest) {
                    oldest = frag25_sess[i].first_ms; oi = i;
                }
            }
            s = &frag25_sess[oi];
        }
        memset(s, 0, sizeof(*s));
        s->in_use      = true;
        s->src_node_id = hdr->src_node_id;
        s->packet_id   = hdr->packet_id;
        s->frag_total  = hdr->frag_total;
        s->first_ms    = now;
    }

    uint16_t offset = (uint16_t)hdr->frag_index * (uint16_t)PKT25_MAX_PAYLOAD;
    if ((uint16_t)(offset + plen) > (uint16_t)FRAG25_BUF_SIZE) {
        ESP_LOGE(TAG, "Frag25 overflow"); memset(s, 0, sizeof(*s));
        return false;
    }

    memcpy(s->buf + offset, payload, plen);
    s->received_mask |= (uint8_t)(1u << hdr->frag_index);
    uint16_t end = offset + plen;
    if (end > s->total_len) s->total_len = end;

    uint8_t full_mask = (uint8_t)((1u << hdr->frag_total) - 1);
    if ((s->received_mask & full_mask) == full_mask) {
        memcpy(out_buf, s->buf, s->total_len);
        *out_len = s->total_len;
        memset(s, 0, sizeof(*s));
        return true;
    }
    return false;
}

// ── Quick phrase decode ───────────────────────────────────────
static const char *qp_decode(uint8_t code)
{
    switch (code) {
        case QP_YES:      return "Да";
        case QP_NO:       return "Нет";
        case QP_OK:       return "Окей";
        case QP_COMING:   return "Иду к тебе";
        case QP_HELP:     return "Помоги мне!";
        case QP_APPROACH: return "Подойди сюда";
        case QP_STOP:     return "Стой!";
        case QP_RUN:      return "Бежим!";
        case QP_WAIT:     return "Жди меня";
        case QP_HELLO:    return "Привет!";
        default:          return "?";
    }
}

// ── Node-seen BLE notification ────────────────────────────────
static void notify_node_seen(uint32_t node_id, int rssi, int hops)
{
    char notify[160];
    node_entry_t *e = nodedb_find(node_id);
    const char *name = (e && e->name[0]) ? e->name : "";
    snprintf(notify, sizeof(notify),
             "{\"evt\":\"node_seen\",\"node_id\":\"0x%08"PRIx32"\","
             "\"name\":\"%s\",\"rssi\":%d,\"hops\":%d,"
             "\"hw\":%d,\"batt\":%d}",
             node_id, name, rssi, hops,
             e ? e->hw : 0, e ? e->batt : 255);
    ble_server_notify(notify);
}

// ── NODE_HELLO v2 payload ─────────────────────────────────────
// [0x00][hw_id][batt][name…]  (marker 0x00 is impossible as a
// first name byte, so legacy name-only hellos stay parseable)
static int build_hello_payload(uint8_t *buf, size_t buf_size)
{
    const char *name = get_display_name();
    size_t nlen = strlen(name);
    if (nlen > buf_size - 3) nlen = buf_size - 3;
    buf[0] = 0x00;
    buf[1] = identity_hw_id();
    buf[2] = battery_get_percent();  // BATT_NONE (0xFF) if no battery
    memcpy(buf + 3, name, nlen);
    return (int)(3 + nlen);
}

// Ответ на чужой широковещательный hello — не чаще раза в 5 минут на узел.
#define HELLO_REPLY_MIN_MS  300000

static void send_hello(uint32_t dst_node_id)
{
    uint8_t payload[3 + 32];
    int plen = build_hello_payload(payload, sizeof(payload));
    uint8_t hello_buf[PKT25_HDR_SIZE + sizeof(payload)];
    int hlen = proto25_build(hello_buf, sizeof(hello_buf),
                              dst_node_id, GROUP_NONE,
                              PKT25_NODE_HELLO, PRIO25_HELLO,
                              0, (dst_node_id == NODE_BROADCAST) ? PKT25_HOP_MAX : 1,
                              1, 0, payload, (uint8_t)plen);
    if (hlen > 0) lora_manager_send_async(hello_buf, hlen);
}

// ── TX queue ──────────────────────────────────────────────────
typedef struct { uint16_t pkt_id; int app_seq; size_t len; uint32_t group_id; } tx_msg_hdr_t;
static char tx_text_buf[512];
static volatile bool tx_ready = false;
static SemaphoreHandle_t tx_sem = NULL;
static tx_msg_hdr_t pending_msg;

// ── TX task ───────────────────────────────────────────────────
static void bridge_tx_task(void *arg)
{
    uint8_t pkt_buf[PKT25_MAX_SIZE];

    while (1) {
        xSemaphoreTake(tx_sem, portMAX_DELAY);
        if (!tx_ready) continue;

        tx_msg_hdr_t msg   = pending_msg;
        size_t       txt_len = msg.len;
        if (txt_len > sizeof(tx_text_buf) - 1) txt_len = sizeof(tx_text_buf) - 1;

        display_on_msg_sending();

        uint8_t frag_total = (txt_len + PKT25_MAX_PAYLOAD - 1) / PKT25_MAX_PAYLOAD;
        if (frag_total < 1) frag_total = 1;
        if (frag_total > PKT25_MAX_FRAGS) {
            ble_server_notify("{\"evt\":\"error\",\"code\":7,\"desc\":\"Message too long\"}");
            tx_ready = false; display_on_msg_failed(); continue;
        }

        seq_map_add(msg.pkt_id, msg.app_seq);

        uint32_t gid   = msg.group_id;
        uint8_t  gflags = FLAG25_ACK_REQ | (gid ? FLAG25_GROUP : 0);

        bool ok = true;
        for (uint8_t frag = 0; frag < frag_total && ok; frag++) {
            size_t offset = (size_t)frag * PKT25_MAX_PAYLOAD;
            size_t chunk  = txt_len - offset;
            if (chunk > PKT25_MAX_PAYLOAD) chunk = PKT25_MAX_PAYLOAD;

            // B1: every fragment shares msg.pkt_id (the id also stored in
            // seq_map above) so the receiver keys reassembly on it correctly.
            int plen = proto25_build_id(
                pkt_buf, sizeof(pkt_buf),
                NODE_BROADCAST, gid,
                PKT25_TEXT_MSG, PRIO25_TEXT,
                gflags,
                PKT25_HOP_MAX,
                msg.pkt_id,
                frag_total, frag,
                (uint8_t *)(tx_text_buf + offset), (uint8_t)chunk
            );

            if (plen <= 0 || lora_manager_send(pkt_buf, plen) != ESP_OK) {
                ok = false;
            } else {
                ESP_LOGI(TAG, "TX frag %d/%d sent", frag + 1, frag_total);
                if (frag < frag_total - 1)
                    vTaskDelay(pdMS_TO_TICKS(300 + (esp_random() % 200)));
            }
        }

        tx_ready = false;
        char notify[128];
        if (ok) {
            bridge_msg_sent++;
            snprintf(notify, sizeof(notify), "{\"evt\":\"sent\",\"seq\":%d}", msg.app_seq);
            display_on_msg_sent();
        } else {
            snprintf(notify, sizeof(notify),
                     "{\"evt\":\"error\",\"code\":6,\"desc\":\"TX failed\",\"seq\":%d}", msg.app_seq);
            display_on_msg_failed();
        }
        ble_server_notify(notify);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ── Relay scheduler ───────────────────────────────────────────
// Мгновенная ретрансляция = гарантированная коллизия, когда пакет
// слышат 2+ узла: они отвечают одновременно. Поэтому relay уходит
// со случайной задержкой 50–500 мс, и отменяется, если за это время
// мы услышали чужой relay того же пакета (значит, эфир уже занят им).
#define RELAY_SLOTS 6   // V2.9.4: +2 слота — SOS-эхо живёт до 90 с
typedef struct {
    bool     in_use;
    uint32_t fire_at_ms;
    uint32_t src_node_id;
    uint16_t packet_id;
    uint8_t  frag_index;
    int      len;
    uint8_t  buf[PKT25_MAX_SIZE];
} relay_slot_t;
static relay_slot_t relay_slots[RELAY_SLOTS];
static SemaphoreHandle_t relay_mtx = NULL;

static uint32_t now_ms32(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// ── Synchronized speed switch (B14 / CHAN_SWITCH) ─────────────
// Changing speed on one node alone makes it deaf to everyone still on the
// old preset. Instead the initiator broadcasts CHAN_SWITCH with a countdown;
// every node (initiator included) arms a deferred switch and flips together.
// Nodes reached via relay switch a fraction of a second later (relay jitter
// ≪ countdown), so the network converges. relay_task ticks the timer.
static volatile bool     cs_pending      = false;
static volatile uint32_t cs_fire_ms      = 0;
static volatile int      cs_target_speed = 0;

// Откат скорости (B14). Смена пресета — распределённая операция без
// подтверждения: анонс уходит на СТАРОЙ скорости, и если сосед его не
// услышал, он остался на прежнем SF. После этого связи нет вообще —
// включая все последующие анонсы, они уже уйдут на новой скорости.
// Сеть расколота до перезагрузки. Поэтому после переключения держим
// окно: не услышали за него ни одного пакета — возвращаемся назад.
#define SF_FALLBACK_MS   150000   // ~3 интервала hello
static volatile uint32_t sf_fallback_at = 0;   // 0 = не следим
static volatile uint32_t sf_switch_ms   = 0;
static volatile int      sf_fallback_to = 0;

static void chan_switch_arm(int speed, uint32_t delay_ms)
{
    cs_target_speed = speed;
    cs_fire_ms      = now_ms32() + delay_ms;
    cs_pending      = true;
    ESP_LOGI(TAG, "CHAN_SWITCH armed → speed=%d in %ums", speed, (unsigned)delay_ms);
}

static void relay_schedule(const uint8_t *data, int len,
                           uint32_t src, uint16_t pkt_id, uint8_t frag_index)
{
    if (len > (int)PKT25_MAX_SIZE) return;
    xSemaphoreTake(relay_mtx, portMAX_DELAY);
    for (int i = 0; i < RELAY_SLOTS; i++) {
        if (relay_slots[i].in_use) continue;
        relay_slots[i].in_use      = true;
        relay_slots[i].fire_at_ms  = now_ms32() + 50 + (esp_random() % 450);
        relay_slots[i].src_node_id = src;
        relay_slots[i].packet_id   = pkt_id;
        relay_slots[i].frag_index  = frag_index;
        relay_slots[i].len         = len;
        memcpy(relay_slots[i].buf, data, (size_t)len);
        xSemaphoreGive(relay_mtx);
        return;
    }
    xSemaphoreGive(relay_mtx);
    ESP_LOGW(TAG, "Relay slots full, dropping relay");
}

// V2.9.4 (SOS-эхо): запланировать повтор пакета с произвольной задержкой.
// Повторяем ЧУЖОЙ SOS verbatim (тот же packet_id) — узлы, уже получившие
// оригинал, гасят повтор дедупом, а кто был вне связи — получает тревогу.
static void relay_schedule_at(const uint8_t *data, int len, uint32_t delay_ms)
{
    if (len > (int)PKT25_MAX_SIZE) return;
    const pkt25_hdr_t *h = (const pkt25_hdr_t *)data;
    xSemaphoreTake(relay_mtx, portMAX_DELAY);
    for (int i = 0; i < RELAY_SLOTS; i++) {
        if (relay_slots[i].in_use) continue;
        relay_slots[i].in_use      = true;
        relay_slots[i].fire_at_ms  = now_ms32() + delay_ms;
        relay_slots[i].src_node_id = h->src_node_id;
        relay_slots[i].packet_id   = h->packet_id;
        relay_slots[i].frag_index  = h->frag_index;
        relay_slots[i].len         = len;
        memcpy(relay_slots[i].buf, data, (size_t)len);
        xSemaphoreGive(relay_mtx);
        return;
    }
    xSemaphoreGive(relay_mtx);
}

// Отмена: кто-то уже ретранслировал этот фрагмент
static void relay_cancel_if_pending(uint32_t src, uint16_t pkt_id, uint8_t frag_index)
{
    xSemaphoreTake(relay_mtx, portMAX_DELAY);
    for (int i = 0; i < RELAY_SLOTS; i++) {
        if (relay_slots[i].in_use &&
            relay_slots[i].src_node_id == src &&
            relay_slots[i].packet_id   == pkt_id &&
            relay_slots[i].frag_index  == frag_index) {
            relay_slots[i].in_use = false;
            ESP_LOGD(TAG, "Relay cancelled (heard echo) src=0x%08"PRIx32, src);
        }
    }
    xSemaphoreGive(relay_mtx);
}

static void relay_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(50));
        uint32_t now = now_ms32();
        for (int i = 0; i < RELAY_SLOTS; i++) {
            bool fire = false;
            uint8_t buf[PKT25_MAX_SIZE];
            int len = 0;
            xSemaphoreTake(relay_mtx, portMAX_DELAY);
            if (relay_slots[i].in_use &&
                (int32_t)(now - relay_slots[i].fire_at_ms) >= 0) {
                fire = true;
                len  = relay_slots[i].len;
                memcpy(buf, relay_slots[i].buf, (size_t)len);
                relay_slots[i].in_use = false;
            }
            xSemaphoreGive(relay_mtx);
            if (fire) {
                lora_manager_send_async(buf, len);
                pkt25_hdr_t *h = (pkt25_hdr_t *)buf;
                ESP_LOGD(TAG, "Relayed pkt src=0x%08"PRIx32" hops=%d",
                         h->src_node_id, h->hop_count);
            }
        }

        // Deferred speed switch (CHAN_SWITCH) — fire when countdown elapses
        if (cs_pending && (int32_t)(now - cs_fire_ms) >= 0) {
            cs_pending = false;
            int tgt  = cs_target_speed;
            int prev = lora_manager_get_speed();
            ESP_LOGW(TAG, "CHAN_SWITCH firing → speed=%d", tgt);
            if (lora_manager_set_speed(tgt) == ESP_OK) {
                bridge_current_sf = tgt;
                // Следим за откатом только если до переключения связь
                // была: одинокому узлу возвращаться не к кому.
                if (tgt != prev && bridge_last_rx_ms &&
                    now - bridge_last_rx_ms < NODE_TIMEOUT_MS) {
                    sf_fallback_to = prev;
                    sf_switch_ms   = now;
                    sf_fallback_at = now + SF_FALLBACK_MS;
                }
                char n[96];
                snprintf(n, sizeof(n),
                         "{\"evt\":\"speed\",\"sf\":%d,\"applied\":true}", tgt);
                ble_server_notify(n);
                refresh_idle_display();
            }
        }

        // Окно после смены скорости истекло — слышали кого-нибудь?
        if (sf_fallback_at && (int32_t)(now - sf_fallback_at) >= 0) {
            uint32_t at = sf_fallback_at;
            sf_fallback_at = 0;
            if ((int32_t)(bridge_last_rx_ms - sf_switch_ms) < 0) {
                // За SF_FALLBACK_MS после переключения — тишина. Скорее
                // всего анонс не долетел и сосед остался на прежнем
                // пресете. Возвращаемся к нему: услышать друг друга
                // важнее, чем держать выбранную скорость.
                ESP_LOGW(TAG, "после смены скорости тишина %ums — откат на speed=%d",
                         (unsigned)(now - at + SF_FALLBACK_MS), sf_fallback_to);
                if (lora_manager_set_speed(sf_fallback_to) == ESP_OK) {
                    bridge_current_sf = sf_fallback_to;
                    char n[128];
                    snprintf(n, sizeof(n),
                             "{\"evt\":\"speed\",\"sf\":%d,\"applied\":true,"
                             "\"desc\":\"откат: сеть не отозвалась\"}",
                             sf_fallback_to);
                    ble_server_notify(n);
                    refresh_idle_display();
                }
            }
        }
    }
}

// ── Voice (V2.6) ──────────────────────────────────────────────
// Телефон кодирует AMR-NB и шлёт b64-чанками {"cmd":"voice_tx",...}.
// Устройство копит буфер, затем фрагментирует в PKT25_VOICE_MSG.
// Приём: сборка фрагментов (маска uint64) → b64-чанки в приложение.
static uint8_t  voice_tx_buf[PKT25_VOICE_MAX];
static uint16_t voice_tx_len      = 0;
static int      voice_tx_seq      = -1;
static uint8_t  voice_tx_expected = 0;   // сколько BLE-чанков ждём
static uint8_t  voice_tx_got      = 0;
static volatile bool voice_tx_go  = false;
static SemaphoreHandle_t voice_sem = NULL;

static struct {
    bool     in_use;
    uint32_t src_node_id;
    uint16_t packet_id;
    uint8_t  frag_total;
    uint64_t mask;
    uint16_t total_len;
    uint32_t first_ms;
    uint8_t  buf[PKT25_VOICE_MAX];
} voice_rx;

static void voice_tx_task(void *arg)
{
    uint8_t pkt_buf[PKT25_MAX_SIZE];
    while (1) {
        xSemaphoreTake(voice_sem, portMAX_DELAY);
        if (!voice_tx_go) continue;

        uint16_t len   = voice_tx_len;
        uint16_t pkt_id = proto25_next_pkt_id();
        uint8_t  frag_total = (len + PKT25_MAX_PAYLOAD - 1) / PKT25_MAX_PAYLOAD;
        seq_map_add(pkt_id, voice_tx_seq);

        ESP_LOGI(TAG, "Voice TX: %u bytes, %u frags", len, frag_total);
        bool ok = true;
        for (uint8_t f = 0; f < frag_total && ok; f++) {
            size_t off   = (size_t)f * PKT25_MAX_PAYLOAD;
            size_t chunk = len - off;
            if (chunk > PKT25_MAX_PAYLOAD) chunk = PKT25_MAX_PAYLOAD;
            // B1: all voice fragments share pkt_id (also in seq_map)
            int plen = proto25_build_id(pkt_buf, sizeof(pkt_buf),
                                     NODE_BROADCAST, GROUP_NONE,
                                     PKT25_VOICE_MSG, PRIO25_TEXT,
                                     FLAG25_ACK_REQ, PKT25_HOP_MAX,
                                     pkt_id,
                                     frag_total, f,
                                     voice_tx_buf + off, (uint8_t)chunk);
            if (plen <= 0 || lora_manager_send(pkt_buf, plen) != ESP_OK) ok = false;
            else if (f < frag_total - 1)
                vTaskDelay(pdMS_TO_TICKS(150 + (esp_random() % 100)));
        }

        char ntf[96];
        snprintf(ntf, sizeof(ntf),
                 ok ? "{\"evt\":\"voice_sent\",\"seq\":%d}"
                    : "{\"evt\":\"error\",\"code\":8,\"desc\":\"Voice TX failed\",\"seq\":%d}",
                 voice_tx_seq);
        ble_server_notify(ntf);
        voice_tx_go = false;
        voice_tx_len = 0; voice_tx_got = 0; voice_tx_expected = 0;
    }
}

// Полное голосовое пришло по LoRa → отдать в приложение b64-чанками
static void voice_forward_to_app(uint32_t src, uint16_t pkt_id,
                                 const uint8_t *data, uint16_t len)
{
    // b64-чанк: 225 сырых байт → 300 символов (JSON < 512)
    const uint16_t RAW_CHUNK = 225;
    uint8_t total = (uint8_t)((len + RAW_CHUNK - 1) / RAW_CHUNK);
    static char b64[404];
    static char ntf[512];

    for (uint8_t i = 0; i < total; i++) {
        uint16_t off   = (uint16_t)i * RAW_CHUNK;
        uint16_t chunk = len - off;
        if (chunk > RAW_CHUNK) chunk = RAW_CHUNK;
        size_t olen = 0;
        if (mbedtls_base64_encode((unsigned char *)b64, sizeof(b64), &olen,
                                  data + off, chunk) != 0) return;
        b64[olen] = '\0';
        snprintf(ntf, sizeof(ntf),
                 "{\"evt\":\"voice\",\"node_id\":\"0x%08"PRIx32"\","
                 "\"seq\":%u,\"idx\":%u,\"total\":%u,\"data\":\"%s\"}",
                 src, pkt_id, i, total, b64);
        ble_server_notify(ntf);
        vTaskDelay(pdMS_TO_TICKS(60));  // дать notify-очереди дышать
    }
}

// ── LoRa RX → BLE ─────────────────────────────────────────────
void bridge_on_lora_rx(const uint8_t *data, int len, int rssi, float snr)
{
    bridge_last_rssi  = rssi;
    bridge_last_snr   = snr;
    bridge_last_rx_ms = (uint32_t)(esp_timer_get_time() / 1000);

    // ── Try proto25 first ─────────────────────────────────────
    pkt25_hdr_t hdr;
    uint8_t payload[PKT25_MAX_PAYLOAD];
    uint8_t payload_len = 0;

    esp_err_t p25_err = proto25_parse(data, len, &hdr, payload, &payload_len);

    if (p25_err == ESP_OK) {
        // ── From self? drop ───────────────────────────────────
        if (hdr.src_node_id == proto25_my_id()) return;

        // ── Blacklist (skip SOS override) ─────────────────────
        bool is_sos_override = (hdr.flags & FLAG25_SOS_OVERRIDE) != 0;
        if (!is_sos_override && blacklist_contains(hdr.src_node_id)) {
            ESP_LOGD(TAG, "Blocked node 0x%08"PRIx32, hdr.src_node_id);
            return;
        }

        // ── Signal stats (полезны даже для дубликатов) ────────
        nodedb_update_signal(hdr.src_node_id, rssi, snr, hdr.hop_count);

        bool for_us = (hdr.dst_node_id == proto25_my_id() ||
                       hdr.dst_node_id == NODE_BROADCAST   ||
                       hdr.dst_node_id == NODE_SOS_ALL);

        // ── Dedup ПЕРЕД rate-limit (B11) ──────────────────────
        // Ключевой порядок: relay-копия чужого пакета не должна тратить
        // токены оригинального отправителя. Раньше rate-limit стоял выше
        // dedup — в меше из 3+ узлов бёрст выгорал и сообщения молча
        // дропались. Ключ (src, packet_id, frag_index): фрагменты одной
        // посылки делят packet_id (B1), поэтому frag_index обязателен —
        // иначе frag 1..N посчитались бы дубликатами frag 0.
        if (proto25_dedup_seen(hdr.src_node_id, hdr.packet_id, hdr.frag_index)) {
            // Уже обработали этот фрагмент. Если наш relay ещё в очереди —
            // отменяем, эфир донёс его без нас.
            relay_cancel_if_pending(hdr.src_node_id, hdr.packet_id, hdr.frag_index);
            return;
        }
        proto25_dedup_add(hdr.src_node_id, hdr.packet_id, hdr.frag_index);

        // ── Rate limit (только новые спамогенные первые фрагменты) ──
        // Лимитируем только спамогенные типы и только frag 0 — иначе
        // лимитер убивал хвосты голосовых и hello/ACK-обмен.
        bool rate_limitable =
            (hdr.pkt_type == PKT25_TEXT_MSG ||
             hdr.pkt_type == PKT25_GPS_BEACON ||
             hdr.pkt_type == PKT25_QUICK_PHRASE ||
             hdr.pkt_type == PKT25_VOICE_MSG) &&
            hdr.frag_index == 0;
        if (!is_sos_override && rate_limitable &&
            proto25_rate_limited(hdr.src_node_id)) {
            ESP_LOGW(TAG, "Rate-limited node 0x%08"PRIx32, hdr.src_node_id);
            return;
        }

        ESP_LOGI(TAG, "P25 RX type=0x%02X from=0x%08"PRIx32" hops=%d/%d rssi=%d",
                 hdr.pkt_type, hdr.src_node_id, hdr.hop_count, hdr.hop_limit, rssi);

        // ── Relay (mesh forwarding) со случайным джиттером ────
        if (hdr.hop_count < hdr.hop_limit &&
            hdr.dst_node_id != proto25_my_id()) {
            uint8_t relay_buf[PKT25_MAX_SIZE];
            memcpy(relay_buf, data, (size_t)len);
            pkt25_hdr_t *rh = (pkt25_hdr_t *)relay_buf;
            rh->hop_count++;
            rh->flags |= FLAG25_RELAY;
            relay_schedule(relay_buf, len, hdr.src_node_id, hdr.packet_id, hdr.frag_index);
        }

        node_entry_t *peer = nodedb_find(hdr.src_node_id);
        bool is_new_node = (peer && peer->last_seq == 0);
        if (peer) peer->last_seq = hdr.packet_id;

        if (!for_us) return;

        // ── Dispatch by type ──────────────────────────────────
        // Bumped 1900→2048 in V2.5 to fit added node_id/hops fields
        static char notify[2048];

        switch (hdr.pkt_type) {

            // ── Text message ──────────────────────────────────
            case PKT25_TEXT_MSG: {
                static uint8_t assembled[FRAG25_BUF_SIZE + 1];  // B4: room for '\0'
                uint16_t assembled_len = 0;

                bool complete = frag25_reassemble(&hdr, payload, payload_len,
                                                  assembled, &assembled_len);
                if (!complete) {
                    ESP_LOGI(TAG, "Frag %d/%d pid=%d",
                             hdr.frag_index + 1, hdr.frag_total, hdr.packet_id);
                    break;
                }

                // ACK — B2: carry the acked packet_id so the sender can map
                // it back to its app seq (id is little-endian, 2 bytes).
                uint8_t ack_pl[2] = { (uint8_t)(hdr.packet_id & 0xFF),
                                      (uint8_t)(hdr.packet_id >> 8) };
                uint8_t ack_buf[PKT25_HDR_SIZE + 2];
                int alen = proto25_build(ack_buf, sizeof(ack_buf),
                                         hdr.src_node_id, GROUP_NONE,
                                         PKT25_TEXT_ACK, PRIO25_ACK,
                                         0, 1, 1, 0, ack_pl, 2);
                if (alen > 0) lora_manager_send_async(ack_buf, alen);

                assembled[assembled_len] = '\0';
                static char escaped[1801];
                json_escape((char *)assembled, escaped, sizeof(escaped));

                snprintf(notify, sizeof(notify),
                         "{\"evt\":\"msg\",\"seq\":%d,\"rssi\":%d,"
                         "\"group_id\":\"0x%08"PRIx32"\","
                         "\"node_id\":\"0x%08"PRIx32"\","
                         "\"hops\":%u,\"text\":\"%s\"}",
                         hdr.packet_id, rssi, hdr.group_id,
                         hdr.src_node_id, hdr.hop_count, escaped);
                ble_server_notify(notify);

                bridge_msg_recv++;
                led_pulse(150);   // зелёная вспышка: пришло сообщение
                display_on_msg_received(get_peer_name(), rssi, (char *)assembled);
                peer_manager_on_pong(rssi);

                // Update legacy peer name from nodedb
                node_entry_t *src = nodedb_find(hdr.src_node_id);
                if (src && src->name[0]) {
                    strncpy(known_peer_name, src->name, sizeof(known_peer_name) - 1);
                }
                if (is_new_node) notify_node_seen(hdr.src_node_id, rssi, hdr.hop_count);
                break;
            }

            // ── Text ACK ──────────────────────────────────────
            case PKT25_TEXT_ACK: {
                // B2: the acked packet_id is in the payload (LE); fall back
                // to the header id for ACKs from old firmware.
                uint16_t acked_pid = hdr.packet_id;
                if (payload_len >= 2) {
                    acked_pid = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
                }
                int app_seq = seq_map_find(acked_pid);
                snprintf(notify, sizeof(notify), "{\"evt\":\"ack\",\"seq\":%d}",
                         (app_seq >= 0) ? app_seq : (int)acked_pid);
                ble_server_notify(notify);
                break;
            }

            // ── Node hello / announce ─────────────────────────
            case PKT25_NODE_HELLO: {
                // v2 payload: [0x00][hw_id][batt][name…]; legacy: [name…]
                const uint8_t *name_p  = payload;
                uint8_t        name_ln = payload_len;
                if (payload_len >= 3 && payload[0] == 0x00) {
                    nodedb_set_hw(hdr.src_node_id, payload[1]);
                    node_entry_t *he = nodedb_get_or_create(hdr.src_node_id);
                    if (he) he->batt = payload[2];
                    name_p  = payload + 3;
                    name_ln = payload_len - 3;
                }
                if (name_ln > 0) {
                    char nbuf[NODE_NAME_LEN];
                    uint8_t cp = name_ln < NODE_NAME_LEN - 1 ? name_ln : NODE_NAME_LEN - 1;
                    memcpy(nbuf, name_p, cp);
                    nbuf[cp] = '\0';
                    nodedb_set_name(hdr.src_node_id, nbuf);
                    node_entry_t *e = nodedb_find(hdr.src_node_id);
                    if (e && !known_peer_name[0]) {
                        strncpy(known_peer_name, e->name, sizeof(known_peer_name) - 1);
                    }
                }
                notify_node_seen(hdr.src_node_id, rssi, hdr.hop_count);
                peer_manager_on_pong(rssi);

                // V2.6.1: имя пира в приложение прямо из hello — раньше
                // оно приходило только через legacy discovery, который
                // ломался на совпадающих legacy-ID
                {
                    node_entry_t *he = nodedb_find(hdr.src_node_id);
                    if (he && he->name[0]) {
                        static char esc_hn[48];
                        json_escape(he->name, esc_hn, sizeof(esc_hn));
                        snprintf(notify, sizeof(notify),
                                 "{\"evt\":\"peer\",\"name\":\"%s\","
                                 "\"online\":true,\"rssi\":%d}",
                                 esc_hn, rssi);
                        ble_server_notify(notify);
                        display_on_peer_status(he->name, true, rssi);
                    }
                }

                // Reply only to broadcasts — unicast hellos are already replies,
                // replying to them creates an infinite ping-pong storm.
                // И не чаще HELLO_REPLY_MIN_MS на узел: hello теперь ещё и
                // периодический keepalive, а ответ на каждый широковещательный
                // — это N-1 юникастов с каждого узла каждые 45 с (O(N²) в эфире).
                // Для знакомства ответ нужен один раз: дальше стороны слышат
                // периодические анонсы друг друга сами.
                if (hdr.dst_node_id == NODE_BROADCAST) {
                    node_entry_t *re = nodedb_get_or_create(hdr.src_node_id);
                    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
                    if (!re || re->last_hello_ms == 0 ||
                        now - re->last_hello_ms > HELLO_REPLY_MIN_MS) {
                        if (re) re->last_hello_ms = now;
                        send_hello(hdr.src_node_id);
                    }
                }
                break;
            }

            // ── GPS beacon ────────────────────────────────────
            case PKT25_GPS_BEACON: {
                if (payload_len < (int)sizeof(gps25_t)) break;
                gps25_t gps;
                memcpy(&gps, payload, sizeof(gps25_t));
                nodedb_update_gps(hdr.src_node_id,
                                  gps.lat, gps.lon, gps.alt, gps.batt);

                // Forward GPS as location tag (for Android map compat)
                double lat = (double)gps.lat / 1e7;
                double lon = (double)gps.lon / 1e7;
                snprintf(notify, sizeof(notify),
                         "{\"evt\":\"msg\",\"seq\":%d,\"rssi\":%d,"
                         "\"text\":\"[LOC:%.7f,%.7f]\"}",
                         hdr.packet_id, rssi, lat, lon);
                ble_server_notify(notify);
                break;
            }

            // ── SOS ───────────────────────────────────────────
            case PKT25_SOS: {
                if (payload_len > 0 && payload_len < PKT25_MAX_PAYLOAD) {
                    payload[payload_len] = '\0';
                }
                const char *sos_text = payload_len > 0 ? (char *)payload : "SOS!";
                ESP_LOGW(TAG, "★★★ SOS from 0x%08"PRIx32": %s", hdr.src_node_id, sos_text);
                static char esc_sos[256];
                json_escape(sos_text, esc_sos, sizeof(esc_sos));
                // Dedicated SOS-incoming event — phone shows modal + alarm
                snprintf(notify, sizeof(notify),
                         "{\"evt\":\"sos_in\",\"node_id\":\"0x%08"PRIx32"\",\"seq\":%d,\"rssi\":%d,\"text\":\"%s\"}",
                         hdr.src_node_id, hdr.packet_id, rssi, esc_sos);
                ble_server_notify(notify);
                // Also log to chat history as a regular message
                snprintf(notify, sizeof(notify),
                         "{\"evt\":\"msg\",\"seq\":%d,\"rssi\":%d,\"text\":\"⚠️ %s\"}",
                         hdr.packet_id, rssi, esc_sos);
                ble_server_notify(notify);

                // V2.9.4: SOS-эхо — повторяем пакет через 30 и 90 с, чтобы
                // накрыть узлы, которые в момент оригинала были вне связи.
                relay_schedule_at(data, len, 30000);
                relay_schedule_at(data, len, 90000);
                break;
            }

            // ── Panic (forces phone siren) ─────────────────────
            case PKT25_PANIC: {
                ESP_LOGW(TAG, "★★★ PANIC from 0x%08"PRIx32, hdr.src_node_id);
                snprintf(notify, sizeof(notify),
                         "{\"evt\":\"panic\",\"node_id\":\"0x%08"PRIx32"\",\"rssi\":%d}",
                         hdr.src_node_id, rssi);
                ble_server_notify(notify);
                break;
            }

            // ── Quick phrase ──────────────────────────────────
            case PKT25_QUICK_PHRASE: {
                if (payload_len == 0) break;
                const char *phrase = qp_decode(payload[0]);
                static char esc_qp[128];
                json_escape(phrase, esc_qp, sizeof(esc_qp));
                snprintf(notify, sizeof(notify),
                         "{\"evt\":\"msg\",\"seq\":%d,\"rssi\":%d,\"text\":\"%s\"}",
                         hdr.packet_id, rssi, esc_qp);
                ble_server_notify(notify);
                break;
            }

            // ── Voice message (V2.6) ──────────────────────────
            case PKT25_VOICE_MSG: {
                uint32_t vnow = now_ms32();
                if (voice_rx.in_use && (vnow - voice_rx.first_ms > 60000)) {
                    memset(&voice_rx, 0, sizeof(voice_rx));
                }
                if (!voice_rx.in_use ||
                    voice_rx.src_node_id != hdr.src_node_id ||
                    voice_rx.packet_id   != hdr.packet_id) {
                    memset(&voice_rx, 0, sizeof(voice_rx));
                    voice_rx.in_use      = true;
                    voice_rx.src_node_id = hdr.src_node_id;
                    voice_rx.packet_id   = hdr.packet_id;
                    voice_rx.frag_total  = hdr.frag_total;
                    voice_rx.first_ms    = vnow;
                }
                if (hdr.frag_index >= PKT25_VOICE_FRAGS) break;
                uint32_t voff = (uint32_t)hdr.frag_index * PKT25_MAX_PAYLOAD;
                if (voff + payload_len > PKT25_VOICE_MAX) break;
                memcpy(voice_rx.buf + voff, payload, payload_len);
                voice_rx.mask |= (1ULL << hdr.frag_index);
                uint16_t vend = (uint16_t)(voff + payload_len);
                if (vend > voice_rx.total_len) voice_rx.total_len = vend;

                uint64_t vfull = (voice_rx.frag_total >= 64)
                                 ? ~0ULL : ((1ULL << voice_rx.frag_total) - 1);
                if ((voice_rx.mask & vfull) == vfull) {
                    ESP_LOGI(TAG, "Voice RX complete: %u bytes from 0x%08"PRIx32,
                             voice_rx.total_len, hdr.src_node_id);
                    // ACK отправителю (B2: с подтверждаемым packet_id)
                    uint8_t vack_pl[2] = { (uint8_t)(hdr.packet_id & 0xFF),
                                           (uint8_t)(hdr.packet_id >> 8) };
                    uint8_t ack_buf[PKT25_HDR_SIZE + 2];
                    int alen = proto25_build(ack_buf, sizeof(ack_buf),
                                             hdr.src_node_id, GROUP_NONE,
                                             PKT25_TEXT_ACK, PRIO25_ACK,
                                             0, 1, 1, 0, vack_pl, 2);
                    if (alen > 0) lora_manager_send_async(ack_buf, alen);

                    voice_forward_to_app(hdr.src_node_id, hdr.packet_id,
                                         voice_rx.buf, voice_rx.total_len);
                    bridge_msg_recv++;
                    led_pulse(400);   // длинная вспышка: голосовое
                    memset(&voice_rx, 0, sizeof(voice_rx));
                }
                break;
            }

            // ── PTT: входящий звонок (V2.7) ───────────────────
            case PKT25_PTT_START: {
                if (!ptt_supported()) break;   // E220 не умеет FSK
                // V2.8 payload: [mode][имя]; V2.7 (legacy): [имя]
                int ptt_mode = 1;  // FSK_PROFILE_STANDARD
                const uint8_t *nm = payload;
                uint8_t nm_len = payload_len;
                if (payload_len >= 1 && payload[0] <= 2) {
                    ptt_mode = payload[0];
                    nm = payload + 1;
                    nm_len = payload_len - 1;
                }
                char caller[32] = "???";
                if (nm_len > 0) {
                    uint8_t cp = nm_len < 31 ? nm_len : 31;
                    memcpy(caller, nm, cp); caller[cp] = '\0';
                }
                static char esc_c[48];
                json_escape(caller, esc_c, sizeof(esc_c));
                snprintf(notify, sizeof(notify),
                         "{\"evt\":\"ptt_incoming\",\"node_id\":\"0x%08"PRIx32"\","
                         "\"name\":\"%s\",\"mode\":%d}",
                         hdr.src_node_id, esc_c, ptt_mode);
                ble_server_notify(notify);
                ESP_LOGW(TAG, "★ PTT call from 0x%08"PRIx32" '%s' mode=%d",
                         hdr.src_node_id, caller, ptt_mode);
                ptt_begin(false, hdr.src_node_id, ptt_mode);
                break;
            }

            case PKT25_PTT_END: {
                ptt_end(false);
                break;
            }

            // ── Автономные устройства (V2.9) ──────────────────
            // Датчик или реле без телефона. Мы можем быть как их
            // адресатом, так и просто соседом: манифест и показания
            // широковещательные, и полезны всем, кто их слышит.
            case PKT25_DEV_HELLO: {
                static char devbuf[900];
                if (devnode_manifest_to_json(hdr.src_node_id, payload,
                                             payload_len, devbuf,
                                             sizeof(devbuf)) > 0) {
                    ble_server_notify(devbuf);
                }
                break;
            }

            case PKT25_DEV_DATA: {
                devnode_note_rssi(rssi);
                static char devbuf[512];
                if (devnode_data_to_json(hdr.src_node_id, rssi, payload,
                                         payload_len, devbuf,
                                         sizeof(devbuf)) > 0) {
                    ble_server_notify(devbuf);
                }
                break;
            }

            case PKT25_DEV_CMD: {
                // Команду выполняем, только если мы и есть устройство и
                // адресована она именно нам: широковещательное «включи
                // реле» щёлкнуло бы все реле в округе.
                if (hdr.dst_node_id != identity_node_id()) break;
                devnode_on_cmd(hdr.src_node_id, payload, payload_len);
                break;
            }

            case PKT25_DEV_ACK: {
                static char devbuf[256];
                if (devnode_ack_to_json(hdr.src_node_id, payload,
                                        payload_len, devbuf,
                                        sizeof(devbuf)) > 0) {
                    ble_server_notify(devbuf);
                }
                break;
            }

            // ── Приём картинки по FSK (V2.9.3) ────────────────
            case PKT25_IMG_START: {
                if (!imgfsk_supported()) break;   // E220 не умеет FSK
                // payload: [profile][frame_total][имя…]
                if (payload_len < 2) break;
                int img_prof = payload[0];
                uint8_t frame_total = payload[1];
                if (payload_len > 2) {
                    char sender[32] = "???";
                    uint8_t nl = payload_len - 2;
                    if (nl > 31) nl = 31;
                    memcpy(sender, payload + 2, nl); sender[nl] = '\0';
                    static char esc_s[48];
                    json_escape(sender, esc_s, sizeof(esc_s));
                    snprintf(notify, sizeof(notify),
                             "{\"evt\":\"image_incoming\",\"node_id\":\"0x%08"PRIx32"\","
                             "\"name\":\"%s\"}",
                             hdr.src_node_id, esc_s);
                    ble_server_notify(notify);
                }
                ESP_LOGI(TAG, "IMG_START from 0x%08"PRIx32" prof=%d frames=%d",
                         hdr.src_node_id, img_prof, frame_total);
                imgfsk_on_start(hdr.src_node_id, img_prof, frame_total);
                break;
            }

            // ── Channel/speed switch (B14) ────────────────────
            case PKT25_CHAN_SWITCH: {
                if (payload_len < (uint8_t)sizeof(chan_switch25_t)) break;
                chan_switch25_t cs;
                memcpy(&cs, payload, sizeof(cs));
                int tgt = (cs.new_sf == LORA_SF_FAST) ? LORA_SF_FAST : LORA_SF_SLOW;
                // Relay already forwarded this via the generic mesh path;
                // dedup guarantees we arm the switch only once.
                if (tgt != lora_manager_get_speed()) {
                    uint8_t cd = cs.countdown_sec ? cs.countdown_sec : CHAN_SWITCH_COUNTDOWN_S;
                    chan_switch_arm(tgt, (uint32_t)cd * 1000);
                    snprintf(notify, sizeof(notify),
                             "{\"evt\":\"chan_switch\",\"sf\":%d,\"in\":%d}", tgt, cd);
                    ble_server_notify(notify);
                }
                break;
            }

            // ── Ping → Pong ───────────────────────────────────
            case PKT25_PING: {
                uint8_t pong_buf[PKT25_HDR_SIZE];
                int plen = proto25_build(pong_buf, sizeof(pong_buf),
                                          hdr.src_node_id, GROUP_NONE,
                                          PKT25_PONG, PRIO25_ACK,
                                          0, 1, 1, 0, NULL, 0);
                if (plen > 0) lora_manager_send_async(pong_buf, plen);
                break;
            }

            // ── Group JOIN ────────────────────────────────────
            case PKT25_GROUP_JOIN: {
                if (payload_len < (uint8_t)sizeof(group25_info_t)) break;
                group25_info_t info;
                memcpy(&info, payload, sizeof(group25_info_t));
                info.name[sizeof(info.name) - 1] = '\0';
                static char esc_gname[32];
                json_escape(info.name, esc_gname, sizeof(esc_gname));
                snprintf(notify, sizeof(notify),
                         "{\"evt\":\"group_joined\","
                         "\"group_id\":\"0x%08"PRIx32"\","
                         "\"name\":\"%s\","
                         "\"from\":\"0x%08"PRIx32"\"}",
                         info.group_id, esc_gname, hdr.src_node_id);
                ble_server_notify(notify);
                ESP_LOGI(TAG, "GROUP_JOIN group=0x%08"PRIx32" '%s' from=0x%08"PRIx32,
                         info.group_id, info.name, hdr.src_node_id);
                break;
            }

            // ── Group LEAVE ───────────────────────────────────
            case PKT25_GROUP_LEAVE: {
                snprintf(notify, sizeof(notify),
                         "{\"evt\":\"group_left\","
                         "\"group_id\":\"0x%08"PRIx32"\","
                         "\"from\":\"0x%08"PRIx32"\"}",
                         hdr.group_id, hdr.src_node_id);
                ble_server_notify(notify);
                ESP_LOGI(TAG, "GROUP_LEAVE group=0x%08"PRIx32" from=0x%08"PRIx32,
                         hdr.group_id, hdr.src_node_id);
                break;
            }

            default:
                ESP_LOGD(TAG, "Unhandled proto25 type 0x%02X", hdr.pkt_type);
                break;
        }

        return; // Handled by proto25 path
    }

    // ── Fallback: old protocol (heartbeats from legacy devices) ──
    pkt_header_t old_hdr;
    uint8_t old_payload[250];
    uint8_t old_payload_len = 0;
    if (protocol_parse_packet(data, len, &old_hdr, old_payload, &old_payload_len) != ESP_OK) {
        return; // Unknown format
    }

    switch (old_hdr.pkt_type) {
        case PKT_HEARTBEAT_PING: peer_manager_on_ping(); break;
        case PKT_HEARTBEAT_PONG: peer_manager_on_pong(rssi); break;

        case PKT_DISCOVERY_REQ: {
            const char *name = get_display_name();
            uint8_t buf[64];
            int plen = protocol_build_packet(buf, sizeof(buf), old_hdr.from_id,
                                              PKT_DISCOVERY_RESP,
                                              protocol_next_seq(), 1, 0,
                                              (uint8_t *)name, strlen(name));
            if (plen > 0) lora_manager_send_async(buf, plen);
            break;
        }

        case PKT_DISCOVERY_RESP: {
            old_payload[old_payload_len < 31 ? old_payload_len : 31] = '\0';
            peer_manager_on_discovery_resp((char *)old_payload, rssi);
            strncpy(known_peer_name, (char *)old_payload, sizeof(known_peer_name) - 1);
            static char esc_name[64];
            json_escape((char *)old_payload, esc_name, sizeof(esc_name));
            char ntf[128];
            snprintf(ntf, sizeof(ntf),
                     "{\"evt\":\"peer\",\"name\":\"%s\",\"online\":true,\"rssi\":%d}",
                     esc_name, rssi);
            ble_server_notify(ntf);
            display_on_peer_status(known_peer_name, true, rssi);
            break;
        }

        default:
            break;
    }
}

// ── BLE → LoRa ────────────────────────────────────────────────
void bridge_on_ble_rx(const char *json, size_t len)
{
    (void)len;
    ESP_LOGI(TAG, "BLE: %.120s", json);

    char notify[256];
    char cmd[24] = {0};
    get_string(json, "cmd", cmd, sizeof(cmd));

    // ── setname ──────────────────────────────────────────────
    if (strcmp(cmd, "setname") == 0) {
        char name[32] = {0};
        int nlen = get_string(json, "name", name, sizeof(name));
        if (nlen > 0) {
            strncpy(user_display_name, name, sizeof(user_display_name) - 1);
            identity_set_name(user_display_name);  // persist to NVS
            snprintf(notify, sizeof(notify),
                     "{\"evt\":\"name_set\",\"name\":\"%s\"}", user_display_name);
            ble_server_notify(notify);
            refresh_idle_display();

            // Broadcast NODE_HELLO with new name (+hw/batt)
            send_hello(NODE_BROADCAST);
        }
        return;
    }

    // ── status ────────────────────────────────────────────────
    if (strcmp(cmd, "status") == 0) {
        int online_count = nodedb_count_online();
        snprintf(notify, sizeof(notify),
                 "{\"evt\":\"hw\",\"lora\":%s,\"peer_online\":%s,"
                 "\"rssi\":%d,\"sf\":%d,\"nodes\":%d}",
                 lora_manager_is_healthy() ? "true" : "false",
                 peer_manager_is_online()  ? "true" : "false",
                 peer_manager_get_rssi(),
                 lora_manager_get_speed(),
                 online_count);
        ble_server_notify(notify);
        return;
    }

    // ── discover ──────────────────────────────────────────────
    if (strcmp(cmd, "discover") == 0) {
        if (!lora_manager_is_healthy()) return;
        send_hello(NODE_BROADCAST);
        // Also send old-format discovery for legacy devices
        peer_manager_send_discovery();
        return;
    }

    // ── speed ─────────────────────────────────────────────────
    // B14: не переключаемся молча в одиночку (иначе оглохнем для сети),
    // а анонсируем CHAN_SWITCH и переходим все вместе по общему countdown.
    if (strcmp(cmd, "speed") == 0) {
        if (!lora_manager_is_healthy()) {
            ble_server_notify("{\"evt\":\"error\",\"desc\":\"No LoRa\"}"); return;
        }
        char mode[16];
        get_string(json, "mode", mode, sizeof(mode));
        int new_sf = (strcmp(mode, "fast") == 0) ? LORA_SF_FAST : LORA_SF_SLOW;
        if (new_sf == lora_manager_get_speed()) {
            snprintf(notify, sizeof(notify),
                     "{\"evt\":\"speed\",\"mode\":\"%s\",\"sf\":%d,\"applied\":true}",
                     mode, new_sf);
            ble_server_notify(notify);
            return;
        }

        // Анонс на ТЕКУЩЕЙ скорости (её слышит вся сеть), один packet_id
        // на все повторы, hop_limit=max для меш-распространения.
        chan_switch25_t cs = {
            .new_sf        = (uint8_t)new_sf,
            .new_freq_khz  = 0,   // частоту пока не трогаем, только пресет
            .countdown_sec = CHAN_SWITCH_COUNTDOWN_S,
        };
        uint16_t pid = proto25_next_pkt_id();
        uint8_t buf[PKT25_HDR_SIZE + sizeof(cs)];
        for (int i = 0; i < 3; i++) {
            int n = proto25_build_id(buf, sizeof(buf),
                                     NODE_BROADCAST, GROUP_NONE,
                                     PKT25_CHAN_SWITCH, PRIO25_TEXT,
                                     0, PKT25_HOP_MAX, pid, 1, 0,
                                     (uint8_t *)&cs, (uint8_t)sizeof(cs));
            if (n > 0) lora_manager_send(buf, n);
            if (i < 2) vTaskDelay(pdMS_TO_TICKS(400));
        }
        chan_switch_arm(new_sf, (uint32_t)CHAN_SWITCH_COUNTDOWN_S * 1000);

        snprintf(notify, sizeof(notify),
                 "{\"evt\":\"speed\",\"mode\":\"%s\",\"sf\":%d,\"in\":%d}",
                 mode, new_sf, CHAN_SWITCH_COUNTDOWN_S);
        ble_server_notify(notify);
        return;
    }

    // ── send ──────────────────────────────────────────────────
    if (strcmp(cmd, "send") == 0) {
        if (!lora_manager_is_healthy()) {
            ble_server_notify("{\"evt\":\"error\",\"desc\":\"No LoRa\"}"); return;
        }
        // V2.9.2: во время звонка радио в FSK, LoRa-TX подвешен — не копим
        // сообщения (в т.ч. авто-GPS) в очереди, иначе она переполняется и
        // рвёт связь/звонок. Приложение и так не должно слать в звонке.
        if (ptt_active() || imgfsk_active()) {
            ble_server_notify("{\"evt\":\"error\",\"code\":9,\"desc\":\"In call\"}"); return;
        }
        if (tx_ready) {
            ble_server_notify("{\"evt\":\"error\",\"desc\":\"Busy\"}"); return;
        }
        int app_seq = get_int(json, "seq");
        int txt_len = get_string(json, "text", tx_text_buf, sizeof(tx_text_buf));
        if (txt_len <= 0) return;

        pending_msg.pkt_id   = proto25_next_pkt_id();
        pending_msg.app_seq  = app_seq;
        pending_msg.len      = (size_t)txt_len;
        pending_msg.group_id = get_uint32_hex(json, "group_id");
        tx_ready = true;
        xSemaphoreGive(tx_sem);
        return;
    }

    // ── groupjoin ─────────────────────────────────────────────
    if (strcmp(cmd, "groupjoin") == 0) {
        if (!lora_manager_is_healthy()) return;
        uint32_t target_node = get_uint32_hex(json, "node_id");
        uint32_t group_id    = get_uint32_hex(json, "group_id");
        if (target_node == 0 || group_id == 0) return;

        group25_info_t info = {0};
        info.group_id = group_id;
        get_string(json, "name", info.name, sizeof(info.name));

        uint8_t buf[PKT25_HDR_SIZE + sizeof(group25_info_t)];
        int plen = proto25_build(buf, sizeof(buf),
                                  target_node, group_id,
                                  PKT25_GROUP_JOIN, PRIO25_TEXT,
                                  FLAG25_GROUP, PKT25_HOP_MAX, 1, 0,
                                  (uint8_t *)&info, (uint8_t)sizeof(info));
        if (plen > 0) lora_manager_send_async(buf, plen);
        ESP_LOGI(TAG, "Sent GROUP_JOIN to 0x%08"PRIx32" group=0x%08"PRIx32" '%s'",
                 target_node, group_id, info.name);
        return;
    }

    // ── groupleave ────────────────────────────────────────────
    if (strcmp(cmd, "groupleave") == 0) {
        if (!lora_manager_is_healthy()) return;
        uint32_t group_id = get_uint32_hex(json, "group_id");
        if (group_id == 0) return;

        uint8_t buf[PKT25_HDR_SIZE];
        int plen = proto25_build(buf, sizeof(buf),
                                  NODE_BROADCAST, group_id,
                                  PKT25_GROUP_LEAVE, PRIO25_TEXT,
                                  FLAG25_GROUP, PKT25_HOP_MAX, 1, 0,
                                  NULL, 0);
        if (plen > 0) lora_manager_send_async(buf, plen);
        ESP_LOGI(TAG, "Sent GROUP_LEAVE group=0x%08"PRIx32, group_id);
        return;
    }

    // ── PTT-звонки (V2.7) ─────────────────────────────────────
    if (strcmp(cmd, "ptt_start") == 0) {
        if (!ptt_supported()) {
            ble_server_notify("{\"evt\":\"error\",\"desc\":\"PTT: only SX127x boards\"}");
            return;
        }
        if (!lora_manager_is_healthy()) {
            ble_server_notify("{\"evt\":\"error\",\"desc\":\"No LoRa\"}"); return;
        }
        if (imgfsk_active()) {
            ble_server_notify("{\"evt\":\"error\",\"code\":10,\"desc\":\"Radio busy\"}"); return;
        }
        int ptt_mode = get_int(json, "mode");   // 0/1/2, дефолт 1
        if (ptt_mode < 0 || ptt_mode > 2) ptt_mode = 1;
        // V2.9.1: адресный звонок — node_id в команде; 0/нет = всем (как раньше)
        uint32_t ptt_dst = get_uint32_hex(json, "node_id");
        ptt_begin(true, ptt_dst ? ptt_dst : NODE_BROADCAST, ptt_mode);
        return;
    }

    if (strcmp(cmd, "ptt_stop") == 0) {
        ptt_end(true);
        return;
    }

    // V2.9: телефон работает мостом веб↔эфир и сообщает, кто говорит
    // со стороны интернета. Уходит FSK-кадром внутри уже начатой
    // сессии, поэтому звать это надо ПОСЛЕ ptt_start.
    // {"cmd":"ptt_web","names":"Philip, Misha","n":2}
    if (strcmp(cmd, "ptt_web") == 0) {
        if (!ptt_active()) {
            ble_server_notify("{\"evt\":\"error\",\"code\":16,"
                              "\"desc\":\"No active call\"}");
            return;
        }
        char who[48] = {0};
        get_string(json, "names", who, sizeof(who));
        int n = get_int(json, "n");
        if (n < 0) n = 0;
        if (n > 255) n = 255;
        ptt_announce_web(who, (uint8_t)n);
        return;
    }

    // V2.9.2: вызываемый нажал «ответить» → шлём ACCEPT звонящему
    if (strcmp(cmd, "ptt_accept") == 0) {
        ptt_accept_call();
        return;
    }

    // {"cmd":"ptt_audio","d":"<b64 AMR-кадры>"}
    if (strcmp(cmd, "ptt_audio") == 0) {
        if (!ptt_active()) return;
        static char pab64[160];
        int blen = get_string(json, "d", pab64, sizeof(pab64));
        if (blen <= 0) return;
        uint8_t raw[120];
        size_t olen = 0;
        if (mbedtls_base64_decode(raw, sizeof(raw), &olen,
                                  (unsigned char *)pab64, (size_t)blen) != 0) return;
        ptt_queue_audio(raw, (int)olen);
        return;
    }

    // ── voice_tx: чанк голосового от приложения ───────────────
    // {"cmd":"voice_tx","seq":N,"idx":i,"total":t,"data":"<b64>"}
    if (strcmp(cmd, "voice_tx") == 0) {
        if (!lora_manager_is_healthy()) {
            ble_server_notify("{\"evt\":\"error\",\"desc\":\"No LoRa\"}"); return;
        }
        if (ptt_active() || imgfsk_active()) {
            ble_server_notify("{\"evt\":\"error\",\"code\":9,\"desc\":\"In call\"}"); return;
        }
        if (voice_tx_go) {
            ble_server_notify("{\"evt\":\"error\",\"desc\":\"Voice busy\"}"); return;
        }
        int seq   = get_int(json, "seq");
        int idx   = get_int(json, "idx");
        int total = get_int(json, "total");
        static char b64buf[420];
        int blen = get_string(json, "data", b64buf, sizeof(b64buf));
        if (blen <= 0 || total <= 0 || idx < 0 || idx >= total) return;

        // Новая передача — сброс
        if (idx == 0) {
            voice_tx_len = 0; voice_tx_got = 0;
            voice_tx_seq = seq; voice_tx_expected = (uint8_t)total;
        }
        if (seq != voice_tx_seq) return;  // чанк от другой передачи

        size_t olen = 0;
        uint8_t raw[320];
        if (mbedtls_base64_decode(raw, sizeof(raw), &olen,
                                  (unsigned char *)b64buf, (size_t)blen) != 0) return;
        if (voice_tx_len + olen > PKT25_VOICE_MAX) { voice_tx_len = 0; return; }
        memcpy(voice_tx_buf + voice_tx_len, raw, olen);
        voice_tx_len += (uint16_t)olen;
        voice_tx_got++;

        if (voice_tx_got >= voice_tx_expected) {
            voice_tx_go = true;
            xSemaphoreGive(voice_sem);
        }
        return;
    }

    // ── img_tx: чанк картинки от телефона (V2.9.3) ────────────
    // {"cmd":"img_tx","idx":i,"total":t,"prof":P,"node_id":"0x..","data":"b64"}
    // Устройство копит JPEG, на последнем чанке шлёт по FSK (imgfsk).
    if (strcmp(cmd, "img_tx") == 0) {
        if (!imgfsk_supported()) {
            ble_server_notify("{\"evt\":\"error\",\"desc\":\"Images: only SX127x boards\"}");
            return;
        }
        if (imgfsk_active() || ptt_active()) {
            ble_server_notify("{\"evt\":\"error\",\"code\":10,\"desc\":\"Radio busy\"}");
            return;
        }
        // Тот же счёт, что в imgfsk.c: 11.4 КБ статики под сборку
        // картинки из BLE-чанков держать круглосуточно нельзя —
        // это часть тех 34 КБ, из-за которых не поднимался rx_task.
        // Живёт от первого чанка до отправки.
        static uint8_t *img_asm = NULL;
        static uint16_t img_asm_len = 0;
        static int      img_expected = 0;
        static int      img_got = 0;
        static uint32_t img_dst = 0;
        static int      img_prof = 1;

        int idx   = get_int(json, "idx");
        int total = get_int(json, "total");
        static char b64buf[420];
        int blen = get_string(json, "data", b64buf, sizeof(b64buf));
        if (blen <= 0 || total <= 0 || idx < 0 || idx >= total) return;

        if (idx == 0) {
            img_asm_len = 0; img_got = 0; img_expected = total;
            img_dst  = get_uint32_hex(json, "node_id");
            if (img_dst == 0) img_dst = NODE_BROADCAST;
            img_prof = get_int(json, "prof");
            if (!img_asm) img_asm = malloc(IMG_MAX_BYTES);
        }
        if (!img_asm) {
            ble_server_notify("{\"evt\":\"error\",\"code\":13,"
                              "\"desc\":\"Out of memory for image\"}");
            return;
        }

        size_t olen = 0;
        uint8_t raw[320];
        if (mbedtls_base64_decode(raw, sizeof(raw), &olen,
                                  (unsigned char *)b64buf, (size_t)blen) != 0) return;
        if (img_asm_len + olen > IMG_MAX_BYTES) {
            img_asm_len = 0;
            free(img_asm); img_asm = NULL;
            return;
        }
        memcpy(img_asm + img_asm_len, raw, olen);
        img_asm_len += (uint16_t)olen;
        img_got++;

        if (img_got >= img_expected) {
            imgfsk_send(img_asm, img_asm_len, img_dst, img_prof);
            free(img_asm); img_asm = NULL;   // imgfsk_send уже скопировал
        }
        return;
    }

    // ── ble_test ──────────────────────────────────────────────
    if (strcmp(cmd, "ble_test") == 0) {
        ble_server_notify("{\"evt\":\"ble_pong\"}"); return;
    }

    // ── diag ─────────────────────────────────────────────────
    if (strcmp(cmd, "diag") == 0) {
        snprintf(notify, sizeof(notify),
                 "{\"evt\":\"diag\","
                 "\"lora_ok\":%s,\"peer_ok\":%s,"
                 "\"rssi\":%d,\"snr\":%.1f,\"sf\":%d,"
                 "\"tx_count\":%d,\"rx_count\":%d,"
                 "\"node_id\":\"0x%08"PRIx32"\",\"nodes_online\":%d,"
                 "\"hw\":%d,\"batt\":%d,\"board\":\"%s\",\"enc\":%s}",
                 lora_manager_is_healthy() ? "true" : "false",
                 peer_manager_is_online()  ? "true" : "false",
                 bridge_last_rssi, bridge_last_snr,
                 lora_manager_get_speed(),
                 bridge_msg_sent, bridge_msg_recv,
                 proto25_my_id(),
                 nodedb_count_online(),
                 identity_hw_id(), battery_get_percent(), BOARD_NAME,
                 proto25_crypto_enabled() ? "true" : "false");
        ble_server_notify(notify);
        return;
    }

    // ── setpsk (шифрование эфира) ─────────────────────────────
    // {"cmd":"setpsk","pass":"секрет"} — общий пароль на всех устройствах.
    // Пустой pass выключает шифрование. Ключ = SHA-256(pass)[0:16].
    if (strcmp(cmd, "setpsk") == 0) {
        char pass[64] = {0};
        int pl = get_string(json, "pass", pass, sizeof(pass));
        if (pl <= 0) {
            proto25_set_key(NULL);
            crypto_save_key(NULL);
            ble_server_notify("{\"evt\":\"psk\",\"enabled\":false}");
            ESP_LOGW(TAG, "Encryption DISABLED");
            return;
        }
        uint8_t digest[32];
        if (mbedtls_sha256((const unsigned char *)pass, (size_t)pl, digest, 0) != 0) {
            ble_server_notify("{\"evt\":\"error\",\"desc\":\"psk derive failed\"}");
            return;
        }
        proto25_set_key(digest);      // first 16 bytes used inside
        crypto_save_key(digest);
        ble_server_notify("{\"evt\":\"psk\",\"enabled\":true}");
        ESP_LOGW(TAG, "Encryption ENABLED (key set)");
        return;
    }

    // ── sos ───────────────────────────────────────────────────
    if (strcmp(cmd, "sos") == 0) {
        if (!lora_manager_is_healthy()) {
            ble_server_notify("{\"evt\":\"error\",\"desc\":\"No LoRa for SOS\"}"); return;
        }
        ESP_LOGW(TAG, "★★★ SOS ACTIVATED ★★★");
        int saved_sf = lora_manager_get_speed();
        if (saved_sf != LORA_SF_SLOW) lora_manager_set_speed(LORA_SF_SLOW);

        static char sos_text[64];
        snprintf(sos_text, sizeof(sos_text), "SOS! %s", get_display_name());

        int sos_sent = 0;
        uint8_t sos_buf[PKT25_HDR_SIZE + 64];
        for (int i = 0; i < 3; i++) {
            int plen = proto25_build(sos_buf, sizeof(sos_buf),
                                      NODE_SOS_ALL, GROUP_NONE,
                                      PKT25_SOS, PRIO25_SOS,
                                      FLAG25_SOS_OVERRIDE,
                                      PKT25_HOP_MAX, 1, 0,
                                      (uint8_t *)sos_text, (uint8_t)strlen(sos_text));
            if (plen > 0 && lora_manager_send(sos_buf, plen) == ESP_OK) sos_sent++;
            if (i < 2) vTaskDelay(pdMS_TO_TICKS(1000));
        }

        if (saved_sf != LORA_SF_SLOW) lora_manager_set_speed(saved_sf);
        snprintf(notify, sizeof(notify), "{\"evt\":\"sos_sent\",\"count\":%d}", sos_sent);
        ble_server_notify(notify);
        return;
    }

    // ── blocknode ─────────────────────────────────────────────
    if (strcmp(cmd, "blocknode") == 0) {
        uint32_t nid = get_uint32_hex(json, "node_id");
        if (nid == 0) nid = (uint32_t)get_int(json, "node_id");
        if (nid != 0 && nid != proto25_my_id()) {
            blacklist_add(nid);
            blacklist_save();
            snprintf(notify, sizeof(notify),
                     "{\"evt\":\"blocked\",\"node_id\":\"0x%08"PRIx32"\"}", nid);
            ble_server_notify(notify);
            ESP_LOGW(TAG, "Blocked 0x%08"PRIx32, nid);
        }
        return;
    }

    // ── unblocknode ───────────────────────────────────────────
    if (strcmp(cmd, "unblocknode") == 0) {
        uint32_t nid = get_uint32_hex(json, "node_id");
        if (nid == 0) nid = (uint32_t)get_int(json, "node_id");
        if (nid != 0) {
            blacklist_remove(nid);
            blacklist_save();
            snprintf(notify, sizeof(notify),
                     "{\"evt\":\"unblocked\",\"node_id\":\"0x%08"PRIx32"\"}", nid);
            ble_server_notify(notify);
        }
        return;
    }

    // ── set_tx_power ──────────────────────────────────────────
    // Args: { "cmd":"set_tx_power", "idx":0..3 }   0=30dBm,1=27,2=24,3=21
    if (strcmp(cmd, "set_tx_power") == 0) {
        int idx = get_int(json, "idx");
        if (idx < 0 || idx > 3) {
            ble_server_notify("{\"evt\":\"error\",\"desc\":\"tx_power idx must be 0..3\"}");
            return;
        }
        esp_err_t r = lora_manager_set_tx_power((uint8_t)idx);
        static const int dbm_table[4] = { 30, 27, 24, 21 };
        if (r == ESP_OK) {
            snprintf(notify, sizeof(notify),
                     "{\"evt\":\"tx_power\",\"idx\":%d,\"dbm\":%d}",
                     idx, dbm_table[idx]);
        } else {
            snprintf(notify, sizeof(notify),
                     "{\"evt\":\"error\",\"desc\":\"tx_power set failed (%d)\"}", (int)r);
        }
        ble_server_notify(notify);
        return;
    }

    // ── set_antenna ───────────────────────────────────────────
    // Args: { "cmd":"set_antenna", "type":"YAGI" }
    // No radio change — saved to NVS for RSSI compensation in stats.
    if (strcmp(cmd, "set_antenna") == 0) {
        char type[16] = {0};
        if (get_string(json, "type", type, sizeof(type)) <= 0) {
            ble_server_notify("{\"evt\":\"error\",\"desc\":\"antenna type missing\"}");
            return;
        }
        strncpy(bridge_antenna, type, sizeof(bridge_antenna) - 1);
        bridge_antenna[sizeof(bridge_antenna) - 1] = '\0';
        bridge_save_antenna(bridge_antenna);
        ESP_LOGI(TAG, "Antenna → %s", bridge_antenna);
        snprintf(notify, sizeof(notify),
                 "{\"evt\":\"antenna_set\",\"type\":\"%s\"}", bridge_antenna);
        ble_server_notify(notify);
        return;
    }

    // ── devcfg: превратить плату в автономный датчик/реле ─────
    // {"cmd":"devcfg"}                    — прочитать
    // {"cmd":"devcfg","set":{…}}          — записать (нужна перезагрузка)
    // {"cmd":"devcfg","action":"reset"}   — снова обычный узел
    if (strcmp(cmd, "devcfg") == 0) {
        static char devjson[1024];
        char action[16] = {0};
        get_string(json, "action", action, sizeof(action));

        if (strcmp(action, "reset") == 0) {
            esp_err_t e = devnode_reset();
            snprintf(notify, sizeof(notify),
                     "{\"evt\":\"devcfg\",\"applied\":%s,\"reboot\":true}",
                     e == ESP_OK ? "true" : "false");
            ble_server_notify(notify);
            return;
        }

        const char *set = strstr(json, "\"set\"");
        if (set) {
            char err[128] = {0};
            esp_err_t e = devnode_apply_json(set, err, sizeof(err));
            char esc[160];
            json_escape(err, esc, sizeof(esc));
            snprintf(notify, sizeof(notify),
                     "{\"evt\":\"devcfg\",\"applied\":%s,\"reboot\":%s,"
                     "\"desc\":\"%s\"}",
                     e == ESP_OK ? "true" : "false",
                     e == ESP_OK ? "true" : "false",
                     e == ESP_OK ? "сохранено, нужна перезагрузка" : esc);
            ble_server_notify(notify);
            return;
        }

        int n = devnode_to_json(devjson + 22, sizeof(devjson) - 26);
        if (n < 0 || n >= (int)sizeof(devjson) - 26) {
            ble_server_notify("{\"evt\":\"error\",\"code\":17,"
                              "\"desc\":\"devcfg too big\"}");
            return;
        }
        memmove(devjson, "{\"evt\":\"devcfg\",\"cfg\":", 21);
        memmove(devjson + 21, devjson + 22, n + 1);
        snprintf(devjson + 21 + n, sizeof(devjson) - 21 - n, "}");
        ble_server_notify(devjson);
        return;
    }

    // ── devcmd: отправить команду чужому устройству ───────────
    // {"cmd":"devcmd","node_id":"0x1A2B3C4D","id":1,"arg":1}
    if (strcmp(cmd, "devcmd") == 0) {
        uint32_t dst = get_uint32_hex(json, "node_id");
        if (!dst || dst == NODE_BROADCAST) {
            ble_server_notify("{\"evt\":\"error\",\"code\":18,"
                              "\"desc\":\"Need node_id\"}");
            return;
        }
        int id = get_int(json, "id");
        int arg = get_int(json, "arg");
        uint8_t pl[3] = { (uint8_t)id, (uint8_t)(arg & 0xFF),
                          (uint8_t)((arg >> 8) & 0xFF) };
        uint8_t pkt[PKT25_HDR_SIZE + 8];
        int len = proto25_build(pkt, sizeof(pkt), dst, GROUP_NONE,
                                PKT25_DEV_CMD, PRIO25_TEXT, 0,
                                PKT25_HOP_MAX, 1, 0, pl, sizeof(pl));
        if (len > 0) lora_manager_send_async(pkt, len);
        snprintf(notify, sizeof(notify),
                 "{\"evt\":\"devcmd_sent\",\"node_id\":\"0x%08" PRIx32 "\","
                 "\"id\":%d}", dst, id);
        ble_server_notify(notify);
        return;
    }

    // ── hwcfg: чтение, запись, сброс, список профилей ─────────
    //
    // Всё железо настраивается отсюда: {"cmd":"hwcfg"} отдаёт
    // текущую конфигурацию, {"cmd":"hwcfg","set":{...}} применяет
    // патч (см. hwcfg_from_json — можно менять хоть один пин),
    // {"cmd":"hwcfg","action":"profiles"} — что можно выбрать,
    // {"cmd":"hwcfg","action":"reset"} — назад к профилю сборки.
    //
    // Новая конфигурация вступает в силу только после перезагрузки:
    // переучивать живые драйверы на другие пины — верный способ
    // получить полудохлое радио и невоспроизводимый баг.
    if (strcmp(cmd, "hwcfg") == 0) {
        static char hw_buf[2048];   // не на стеке: конфиг под 1.5 КБ JSON
        char action[16] = {0};
        get_string(json, "action", action, sizeof(action));

        if (strcmp(action, "profiles") == 0) {
            int pos = snprintf(hw_buf, sizeof(hw_buf),
                               "{\"evt\":\"hwprofiles\",\"list\":[");
            for (int i = 0; i < hwcfg_profile_count(); i++) {
                const hwcfg_t *pr = hwcfg_profile_at(i);
                pos += snprintf(hw_buf + pos, sizeof(hw_buf) - pos,
                                "%s{\"id\":\"%s\",\"name\":\"%s\",\"radio\":\"%s\","
                                "\"band\":\"%s\",\"freq\":%lu,\"display\":\"%s\","
                                "\"touch\":\"%s\",\"fsk\":%u}",
                                i ? "," : "", pr->profile, pr->name,
                                hwcfg_radio_name(pr->radio.kind),
                                hwcfg_band_name(pr->radio.band),
                                (unsigned long)pr->radio.freq_hz,
                                hwcfg_panel_name(pr->disp.kind),
                                hwcfg_touch_name(pr->touch.kind),
                                pr->radio.supports_fsk);
                if (pos >= (int)sizeof(hw_buf) - 128) break;
            }
            snprintf(hw_buf + pos, sizeof(hw_buf) - pos, "]}");
            ble_server_notify(hw_buf);
            return;
        }

        if (strcmp(action, "reset") == 0) {
            esp_err_t e = hwcfg_reset();
            snprintf(notify, sizeof(notify),
                     "{\"evt\":\"hwcfg\",\"applied\":%s,\"reboot\":true,"
                     "\"desc\":\"%s\"}",
                     e == ESP_OK ? "true" : "false",
                     e == ESP_OK ? "сброшено к профилю сборки" : "NVS недоступен");
            ble_server_notify(notify);
            return;
        }

        const char *set = strstr(json, "\"set\"");
        if (set) {
            hwcfg_t cand;
            char err[128] = {0};
            // Патч накатывается на ТЕКУЩУЮ конфигурацию, поэтому
            // приложению не нужно присылать всё целиком.
            esp_err_t e = hwcfg_from_json(set, hwcfg(), &cand, err, sizeof(err));
            if (e == ESP_OK) e = hwcfg_save(&cand, err, sizeof(err));
            char esc[160];
            json_escape(err, esc, sizeof(esc));
            snprintf(notify, sizeof(notify),
                     "{\"evt\":\"hwcfg\",\"applied\":%s,\"reboot\":%s,\"desc\":\"%s\"}",
                     e == ESP_OK ? "true" : "false",
                     e == ESP_OK ? "true" : "false",
                     e == ESP_OK ? "сохранено, нужна перезагрузка" : esc);
            ble_server_notify(notify);
            return;
        }

        int n = hwcfg_to_json(hwcfg(), hw_buf + 24, sizeof(hw_buf) - 32);
        if (n < 0 || n >= (int)sizeof(hw_buf) - 32) {
            ble_server_notify("{\"evt\":\"error\",\"code\":13,"
                              "\"desc\":\"hwcfg JSON too big\"}");
            return;
        }
        // Оборачиваем готовый JSON конфига в событие, не переклеивая
        // его заново: hw_buf специально с запасом слева.
        memmove(hw_buf, "{\"evt\":\"hwcfg\",\"cfg\":", 21);
        memmove(hw_buf + 21, hw_buf + 24, n + 1);
        snprintf(hw_buf + 21 + n, sizeof(hw_buf) - 21 - n, "}");
        ble_server_notify(hw_buf);
        return;
    }

    // ── touch_cal: результат мастера калибровки из приложения ─
    // {"cmd":"touch_cal","x0":..,"y0":..,"x1":..,"y1":..}
    if (strcmp(cmd, "touch_cal") == 0) {
        if (!touch_present()) {
            ble_server_notify("{\"evt\":\"error\",\"code\":14,\"desc\":\"No touch\"}");
            return;
        }
        // get_int отдаёт 0 на отсутствующий ключ — для сырых
        // значений АЦП это заведомо невалидный угол, так что
        // отдельного «не задано» здесь не нужно.
        int x0 = get_int(json, "x0"), y0 = get_int(json, "y0");
        int x1 = get_int(json, "x1"), y1 = get_int(json, "y1");
        if (x0 <= 0 || y0 <= 0 || x1 <= x0 || y1 <= y0) {
            ble_server_notify("{\"evt\":\"error\",\"code\":15,"
                              "\"desc\":\"Bad calibration\"}");
            return;
        }
        esp_err_t e = touch_set_calibration((uint16_t)x0, (uint16_t)y0,
                                            (uint16_t)x1, (uint16_t)y1);
        snprintf(notify, sizeof(notify),
                 "{\"evt\":\"touch_cal\",\"saved\":%s,\"reboot\":true}",
                 e == ESP_OK ? "true" : "false");
        ble_server_notify(notify);
        return;
    }

    // ── get_radio (current power + antenna) ───────────────────
    if (strcmp(cmd, "get_radio") == 0) {
        static const int dbm_table[4] = { 30, 27, 24, 21 };
        int idx = lora_manager_get_tx_power();
        if (idx < 0 || idx > 3) idx = 0;
        // dbm_table — исторические значения E220 на 30 dBm; для
        // остальных радио считаем от реального потолка платы.
        int max_dbm = hwcfg()->radio.max_dbm;
        int dbm = (hwcfg()->radio.kind == RADIO_E220) ? dbm_table[idx]
                                                      : max_dbm - 3 * idx;
        snprintf(notify, sizeof(notify),
                 "{\"evt\":\"radio\",\"tx_idx\":%d,\"tx_dbm\":%d,"
                 "\"antenna\":\"%s\",\"chip\":\"%s\",\"band\":\"%s\","
                 "\"freq\":%lu,\"max_dbm\":%d,\"fsk\":%s,\"profile\":\"%s\"}",
                 idx, dbm, bridge_antenna, hwcfg_radio_name(hwcfg()->radio.kind),
                 hwcfg_band_name(hwcfg()->radio.band),
                 (unsigned long)hwcfg()->radio.freq_hz, max_dbm,
                 fsk_available() ? "true" : "false", hwcfg()->profile);
        ble_server_notify(notify);
        return;
    }

    // ── nodes (dump node list) ────────────────────────────────
    if (strcmp(cmd, "nodes") == 0) {
        // V2.9.5: во время звонка дамп не отдаём — 2КБ на 11+ чанков
        // конкурируют с 20 evt:ptt_audio/с, душат очередь notify и
        // рассинхронизируют сборщик JSON в приложении (глохнет звук).
        // Свежих данных в FSK-режиме всё равно нет (LoRa спит).
        if (ptt_active() || imgfsk_active()) return;
        node_entry_t *all = nodedb_get_all();
        static char big_buf[2048];   // V2.7.1: не на стеке
        int pos = 0;
        pos += snprintf(big_buf + pos, sizeof(big_buf) - pos, "{\"evt\":\"nodes\",\"list\":[");
        bool first = true;
        for (int i = 0; i < NODEDB_MAX_NODES; i++) {
            if (!all[i].in_use) continue;
            pos += snprintf(big_buf + pos, sizeof(big_buf) - pos,
                            "%s{\"id\":\"0x%08"PRIx32"\",\"name\":\"%s\","
                            "\"rssi\":%d,\"hops\":%d,\"batt\":%d,"
                            "\"hw\":%d,\"status\":%d}",
                            first ? "" : ",",
                            all[i].node_id, all[i].name,
                            all[i].rssi, all[i].hop_count,
                            all[i].batt, all[i].hw, (int)all[i].status);
            first = false;
        }
        pos += snprintf(big_buf + pos, sizeof(big_buf) - pos, "]}");
        ble_server_notify(big_buf);
        return;
    }
}

// ── Peer status callback (from peer_manager) ──────────────────
void bridge_on_peer_status(bool online, int rssi)
{
    char notify[128];
    snprintf(notify, sizeof(notify),
             "{\"evt\":\"peer\",\"online\":%s,\"rssi\":%d}",
             online ? "true" : "false", rssi);
    ble_server_notify(notify);
    display_on_peer_status(get_peer_name(), online, rssi);
}

// ── Init ─────────────────────────────────────────────────────
void bridge_init(void)
{
    // Identity (node_id from BT MAC + user name from NVS)
    uint32_t node_id = identity_node_id();
    if (identity_name()[0]) {
        strncpy(user_display_name, identity_name(), sizeof(user_display_name) - 1);
    }

    proto25_init(node_id);
    protocol_set_my_id((uint8_t)(node_id & 0xFF));  // legacy-ID уникален per-device
    nodedb_init();
    blacklist_init(); // also calls blacklist_load() internally
    bridge_load_antenna();
    crypto_load_key();  // V2.9: encryption on if a PSK was set before

    memset(seq_map,   0, sizeof(seq_map));
    memset(frag25_sess, 0, sizeof(frag25_sess));

    tx_sem = xSemaphoreCreateBinary();
    tx_ready = false;
    bridge_msg_sent = 0;
    bridge_msg_recv = 0;
    bridge_current_sf = LORA_SF_SLOW;

    ptt_init();
    imgfsk_init();

    voice_sem = xSemaphoreCreateBinary();
    memset(&voice_rx, 0, sizeof(voice_rx));
    xTaskCreatePinnedToCore(voice_tx_task, "voice_tx", 6144, NULL, 3, NULL, 0);

    relay_mtx = xSemaphoreCreateMutex();
    memset(relay_slots, 0, sizeof(relay_slots));
    xTaskCreatePinnedToCore(bridge_tx_task, "bridge_tx", 8192, NULL, 3, NULL, 0);
    xTaskCreatePinnedToCore(relay_task, "relay", 4096, NULL, 3, NULL, 0);

    ESP_LOGI(TAG, "Bridge v2.5 init OK — node_id=0x%08"PRIx32, node_id);
}

// ── Startup announcement ──────────────────────────────────────
void bridge_announce(void)
{
    if (!lora_manager_is_healthy()) return;

    send_hello(NODE_BROADCAST);
    ESP_LOGI(TAG, "Startup NODE_HELLO sent (name='%s')", get_display_name());
}
