// proto25.c — Easy Bridge Protocol v2.5 codec
#include "proto25.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "mbedtls/aes.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "P25";

// ── Payload encryption (V2.9) ─────────────────────────────────
// AES-128-CTR over the payload only — the 25-byte header stays in the
// clear so relay/dedup work without the key. CTR is length-preserving,
// so fragmentation/reassembly math is untouched (unlike GCM, which would
// add a 16-byte tag per fragment). Nonce = src||packet_id||frag_index,
// unique per fragment for a given key. Trade-off: CTR gives confidentiality
// but not authentication — bit-flips aren't detected (PHY CRC catches
// random corruption). GCM/auth is a documented follow-up.
static bool    g_crypto_on = false;
static uint8_t g_crypto_key[16];

void proto25_set_key(const uint8_t *key16)
{
    if (!key16) { g_crypto_on = false; memset(g_crypto_key, 0, sizeof(g_crypto_key)); return; }
    memcpy(g_crypto_key, key16, 16);
    g_crypto_on = true;
}

bool proto25_crypto_enabled(void) { return g_crypto_on; }

// CTR is symmetric: same call encrypts and decrypts.
static void crypto_xform(uint32_t src, uint16_t pid, uint8_t frag,
                         uint8_t *buf, uint8_t len)
{
    if (!g_crypto_on || len == 0) return;
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    if (mbedtls_aes_setkey_enc(&ctx, g_crypto_key, 128) != 0) {
        mbedtls_aes_free(&ctx);
        return;
    }
    uint8_t nonce[16] = {0};
    uint8_t stream[16] = {0};
    size_t  nc_off = 0;
    nonce[0] = (uint8_t)(src >> 24); nonce[1] = (uint8_t)(src >> 16);
    nonce[2] = (uint8_t)(src >> 8);  nonce[3] = (uint8_t)src;
    nonce[4] = (uint8_t)(pid >> 8);  nonce[5] = (uint8_t)pid;
    nonce[6] = frag;
    // payload ≤155 B = ≤10 blocks, counter stays in the low byte → no
    // carry into the nonce region.
    mbedtls_aes_crypt_ctr(&ctx, len, &nc_off, nonce, stream, buf, buf);
    mbedtls_aes_free(&ctx);
}

// ── Node identity ─────────────────────────────────────────────
static uint32_t  g_my_node_id  = 0;
static uint16_t  g_pkt_counter = 1;
static SemaphoreHandle_t g_ctr_mutex = NULL;

// ── Dedup table (relay storm guard) ──────────────────────────
typedef struct {
    uint32_t src_node_id;
    uint16_t packet_id;
    uint8_t  frag_index;
    uint32_t seen_ms;
} dedup_entry_t;

static dedup_entry_t g_dedup[PKT25_DEDUP_SIZE];
static int           g_dedup_head = 0;

// ── Rate limiter ──────────────────────────────────────────────
#define RATE_TABLE_SIZE 16
typedef struct {
    uint32_t src_node_id;
    uint32_t last_rx_ms;      // база пополнения токенов
    uint8_t  tokens;          // V2.7.2: token bucket вместо жёсткого 1/1.5с
} rate_entry_t;

#define RATE_BURST     6      // допускаем очередь из 6 подряд
#define RATE_REFILL_MS 1000   // +1 токен в секунду
static rate_entry_t g_rate[RATE_TABLE_SIZE];

// ── Helpers ───────────────────────────────────────────────────
static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// ── Init ──────────────────────────────────────────────────────
void proto25_init(uint32_t my_node_id)
{
    g_my_node_id = my_node_id;
    // B5: start the counter at a random value, not 1. A reboot within the
    // 60 s dedup window otherwise re-emits packet_ids that neighbours still
    // hold as "seen" → our first post-boot packets get silently dropped.
    g_pkt_counter = (uint16_t)(esp_random() | 1u);   // never 0
    g_ctr_mutex = xSemaphoreCreateMutex();
    memset(g_dedup, 0, sizeof(g_dedup));
    memset(g_rate,  0, sizeof(g_rate));
    ESP_LOGI(TAG, "Init — my node_id=0x%08"PRIx32, my_node_id);
}

uint32_t proto25_my_id(void) { return g_my_node_id; }

uint16_t proto25_next_pkt_id(void)
{
    uint16_t id = 0;
    if (g_ctr_mutex) xSemaphoreTake(g_ctr_mutex, portMAX_DELAY);
    id = g_pkt_counter++;
    if (g_pkt_counter == 0) g_pkt_counter = 1; // skip 0
    if (g_ctr_mutex) xSemaphoreGive(g_ctr_mutex);
    return id;
}

// ── Build ─────────────────────────────────────────────────────
int proto25_build_id(
    uint8_t        *out_buf,
    size_t          out_buf_size,
    uint32_t        dst_node_id,
    uint32_t        group_id,
    uint8_t         pkt_type,
    uint8_t         prio,
    uint8_t         flags,
    uint8_t         hop_limit,
    uint16_t        packet_id,
    uint8_t         frag_total,
    uint8_t         frag_index,
    const uint8_t  *payload,
    uint8_t         payload_len)
{
    if (payload_len > PKT25_MAX_PAYLOAD) {
        ESP_LOGE(TAG, "build: payload_len %d > %d", payload_len, PKT25_MAX_PAYLOAD);
        return -1;
    }
    size_t total = PKT25_HDR_SIZE + payload_len;
    if (total > out_buf_size || total > PKT25_MAX_SIZE) {
        ESP_LOGE(TAG, "build: buf too small (%d + %d > %d)",
                 (int)PKT25_HDR_SIZE, payload_len, (int)out_buf_size);
        return -1;
    }

    pkt25_hdr_t *hdr = (pkt25_hdr_t *)out_buf;
    hdr->magic      = PKT25_MAGIC;
    hdr->version    = PKT25_VERSION;
    hdr->flags      = flags;
    hdr->src_node_id = g_my_node_id;
    hdr->dst_node_id = dst_node_id;
    hdr->group_id   = group_id;
    hdr->packet_id  = packet_id;
    hdr->hop_limit  = hop_limit;
    hdr->hop_count  = 0;
    hdr->pkt_type   = pkt_type;
    hdr->prio       = prio;
    hdr->frag_total = frag_total;
    hdr->frag_index = frag_index;
    hdr->payload_len = payload_len;

    if (payload && payload_len > 0) {
        memcpy(out_buf + PKT25_HDR_SIZE, payload, payload_len);
        // Encrypt the payload in place and mark the packet. Header stays
        // clear. No-op when no key is configured (plaintext, backward-compat).
        if (g_crypto_on) {
            crypto_xform(g_my_node_id, packet_id, frag_index,
                         out_buf + PKT25_HDR_SIZE, payload_len);
            hdr->flags |= FLAG25_ENCRYPTED;
        }
    }

    return (int)total;
}

int proto25_build(
    uint8_t        *out_buf,
    size_t          out_buf_size,
    uint32_t        dst_node_id,
    uint32_t        group_id,
    uint8_t         pkt_type,
    uint8_t         prio,
    uint8_t         flags,
    uint8_t         hop_limit,
    uint8_t         frag_total,
    uint8_t         frag_index,
    const uint8_t  *payload,
    uint8_t         payload_len)
{
    return proto25_build_id(out_buf, out_buf_size, dst_node_id, group_id,
                            pkt_type, prio, flags, hop_limit,
                            proto25_next_pkt_id(), frag_total, frag_index,
                            payload, payload_len);
}

// ── Parse ─────────────────────────────────────────────────────
esp_err_t proto25_parse(
    const uint8_t *data,
    int            len,
    pkt25_hdr_t   *out_hdr,
    uint8_t       *out_payload,
    uint8_t       *out_payload_len)
{
    if (!data || len < (int)PKT25_HDR_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(out_hdr, data, PKT25_HDR_SIZE);

    if (out_hdr->magic != PKT25_MAGIC) {
        ESP_LOGD(TAG, "parse: bad magic 0x%04X", out_hdr->magic);
        return ESP_ERR_INVALID_ARG;
    }
    if (out_hdr->version != PKT25_VERSION) {
        ESP_LOGW(TAG, "parse: version mismatch %d", out_hdr->version);
        // Accept anyway for future-compat, just warn
    }

    uint8_t plen = out_hdr->payload_len;
    // B12: bound plen to the max fragment size BEFORE any copy. out_payload
    // is a PKT25_MAX_PAYLOAD-byte buffer on the caller's stack; a crafted or
    // corrupt header with plen up to 231 would otherwise overflow it.
    if (plen > PKT25_MAX_PAYLOAD) {
        ESP_LOGW(TAG, "parse: payload_len=%d > max %d, rejecting", plen, PKT25_MAX_PAYLOAD);
        return ESP_ERR_INVALID_SIZE;
    }
    if ((int)PKT25_HDR_SIZE + plen > len) {
        ESP_LOGE(TAG, "parse: payload_len=%d exceeds packet len=%d", plen, len);
        return ESP_ERR_INVALID_SIZE;
    }

    if (out_payload && plen > 0) {
        memcpy(out_payload, data + PKT25_HDR_SIZE, plen);
        if (out_hdr->flags & FLAG25_ENCRYPTED) {
            if (!g_crypto_on) {
                // Encrypted packet but no key → we can't read it. Leave the
                // ciphertext; the type dispatcher will just see garbage.
                ESP_LOGD(TAG, "encrypted pkt but no key set — dropping payload");
            } else {
                crypto_xform(out_hdr->src_node_id, out_hdr->packet_id,
                             out_hdr->frag_index, out_payload, plen);
            }
        }
    }
    *out_payload_len = plen;

    return ESP_OK;
}

// ── Relay dedup ───────────────────────────────────────────────
bool proto25_dedup_seen(uint32_t src_node_id, uint16_t packet_id, uint8_t frag_index)
{
    uint32_t now = now_ms();
    for (int i = 0; i < PKT25_DEDUP_SIZE; i++) {
        if (g_dedup[i].src_node_id == src_node_id &&
            g_dedup[i].packet_id  == packet_id &&
            g_dedup[i].frag_index == frag_index) {
            // Valid if seen within 60 seconds
            if (now - g_dedup[i].seen_ms < 60000) {
                return true;
            }
            // Expired — slot is effectively free
        }
    }
    return false;
}

void proto25_dedup_add(uint32_t src_node_id, uint16_t packet_id, uint8_t frag_index)
{
    g_dedup[g_dedup_head].src_node_id = src_node_id;
    g_dedup[g_dedup_head].packet_id   = packet_id;
    g_dedup[g_dedup_head].frag_index  = frag_index;
    g_dedup[g_dedup_head].seen_ms     = now_ms();
    g_dedup_head = (g_dedup_head + 1) % PKT25_DEDUP_SIZE;
}

// ── Rate limiter ──────────────────────────────────────────────
bool proto25_rate_limited(uint32_t src_node_id)
{
    uint32_t now = now_ms();
    for (int i = 0; i < RATE_TABLE_SIZE; i++) {
        if (g_rate[i].src_node_id == src_node_id) {
            // Пополнение: +1 токен за каждую полную секунду
            uint32_t gained = (now - g_rate[i].last_rx_ms) / RATE_REFILL_MS;
            if (gained > 0) {
                uint32_t t = (uint32_t)g_rate[i].tokens + gained;
                g_rate[i].tokens = t > RATE_BURST ? RATE_BURST : (uint8_t)t;
                g_rate[i].last_rx_ms += gained * RATE_REFILL_MS;
            }
            if (g_rate[i].tokens == 0) return true;   // бёрст исчерпан
            g_rate[i].tokens--;
            return false;
        }
    }
    // New node — find empty slot (or evict oldest)
    uint32_t oldest_ms = UINT32_MAX;
    int oldest_idx = 0;
    for (int i = 0; i < RATE_TABLE_SIZE; i++) {
        if (g_rate[i].src_node_id == 0) { oldest_idx = i; break; }
        if (g_rate[i].last_rx_ms < oldest_ms) {
            oldest_ms  = g_rate[i].last_rx_ms;
            oldest_idx = i;
        }
    }
    g_rate[oldest_idx].src_node_id = src_node_id;
    g_rate[oldest_idx].last_rx_ms  = now;
    g_rate[oldest_idx].tokens      = RATE_BURST - 1;  // этот пакет уже съел токен
    return false;
}
