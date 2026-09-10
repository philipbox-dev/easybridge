#include "protocol.h"

static uint8_t g_legacy_id = MY_DEVICE_ID;
void protocol_set_my_id(uint8_t id)
{
    if (id == 0x00 || id == BROADCAST_ID) id = 0x01;
    g_legacy_id = id;
}
uint8_t protocol_my_id(void) { return g_legacy_id; }
#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "PROTO";

static uint16_t     seq_counter = 0;
static frag_session_t frag_sess = {0};

void protocol_init(void)
{
    seq_counter = 0;
    memset(&frag_sess, 0, sizeof(frag_sess));
    ESP_LOGI(TAG, "Init. my_id=0x%02X peer_id=0x%02X net=0x%04X",
             protocol_my_id(), PEER_DEVICE_ID, NETWORK_ID);
}

uint16_t protocol_next_seq(void)
{
    return ++seq_counter;
}

int protocol_build_packet(
    uint8_t       *out_buf,
    size_t         out_buf_size,
    uint8_t        to_id,
    uint8_t        pkt_type,
    uint16_t       seq,
    uint8_t        frag_total,
    uint8_t        frag_index,
    const uint8_t *payload,
    uint8_t        payload_len)
{
    int total = PACKET_HEADER_SIZE + payload_len;
    if (total > (int)out_buf_size) {
        ESP_LOGE(TAG, "Buffer too small");
        return -1;
    }

    pkt_header_t *h = (pkt_header_t *)out_buf;
    h->network_id  = NETWORK_ID;
    h->from_id     = protocol_my_id();
    h->to_id       = to_id;
    h->pkt_type    = pkt_type;
    h->seq_num     = seq;
    h->frag_total  = frag_total;
    h->frag_index  = frag_index;
    h->payload_len = payload_len;

    if (payload && payload_len > 0) {
        memcpy(out_buf + PACKET_HEADER_SIZE, payload, payload_len);
    }

    return total;
}

esp_err_t protocol_parse_packet(
    const uint8_t *data,
    int            len,
    pkt_header_t  *out_header,
    uint8_t       *out_payload,
    uint8_t       *out_payload_len)
{
    if (len < PACKET_HEADER_SIZE) {
        ESP_LOGW(TAG, "Too short: %d < %d", len, PACKET_HEADER_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(out_header, data, PACKET_HEADER_SIZE);

    // ★ Вся диагностика теперь через LOGW — всегда видна
    if (out_header->network_id != NETWORK_ID) {
        ESP_LOGW(TAG, "REJECT: wrong net=0x%04X (expect 0x%04X) from=0x%02X",
                 out_header->network_id, NETWORK_ID, out_header->from_id);
        return ESP_ERR_INVALID_ARG;
    }

    if (out_header->from_id == protocol_my_id()) {
        ESP_LOGW(TAG, "REJECT: echo from self (from=0x%02X == my_id=0x%02X)",
                 out_header->from_id, protocol_my_id());
        return ESP_ERR_INVALID_ARG;
    }

    if (out_header->to_id != protocol_my_id() &&
        out_header->to_id != BROADCAST_ID) {
        ESP_LOGW(TAG, "REJECT: not for us (to=0x%02X, my_id=0x%02X, bcast=0x%02X)",
                 out_header->to_id, protocol_my_id(), BROADCAST_ID);
        return ESP_ERR_INVALID_ARG;
        }

        uint8_t plen = out_header->payload_len;
    if (PACKET_HEADER_SIZE + plen > len) {
        plen = len - PACKET_HEADER_SIZE;
    }

    if (out_payload && plen > 0) {
        memcpy(out_payload, data + PACKET_HEADER_SIZE, plen);
    }
    if (out_payload_len) {
        *out_payload_len = plen;
    }

    return ESP_OK;
}

bool protocol_reassemble(
    const pkt_header_t *hdr,
    const uint8_t      *payload,
    uint8_t             payload_len,
    uint8_t            *out_buf,
    uint16_t           *out_len)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

    // Однофрагментное сообщение — сразу готово
    if (hdr->frag_total == 1) {
        memcpy(out_buf, payload, payload_len);
        *out_len = payload_len;
        return true;
    }

    // Проверка таймаута текущей сессии
    if (frag_sess.in_use) {
        if (now_ms - frag_sess.first_frag_ms > FRAGMENT_TIMEOUT_MS) {
            ESP_LOGW(TAG, "Fragment timeout, dropping seq=%d", frag_sess.seq_num);
            memset(&frag_sess, 0, sizeof(frag_sess));
        }
    }

    // Новая сессия или другой seq_num
    if (!frag_sess.in_use || frag_sess.seq_num != hdr->seq_num) {
        memset(&frag_sess, 0, sizeof(frag_sess));
        frag_sess.in_use        = true;
        frag_sess.seq_num       = hdr->seq_num;
        frag_sess.frag_total    = hdr->frag_total;
        frag_sess.first_frag_ms = now_ms;
        ESP_LOGI(TAG, "New frag session: seq=%d, total=%d", hdr->seq_num, hdr->frag_total);
    }

    // Вычисляем offset
    uint16_t offset = hdr->frag_index * MAX_PAYLOAD_PER_PACKET;
    if (offset + payload_len > FRAG_BUF_SIZE) {
        ESP_LOGE(TAG, "Fragment overflow: offset=%d, len=%d", offset, payload_len);
        memset(&frag_sess, 0, sizeof(frag_sess));
        return false;
    }

    // Копируем данные фрагмента
    memcpy(frag_sess.buf + offset, payload, payload_len);
    frag_sess.received_mask |= (1 << hdr->frag_index);

    // ★ Обновляем total_len — максимальный offset + длина
    uint16_t end_pos = offset + payload_len;
    if (end_pos > frag_sess.total_len) {
        frag_sess.total_len = end_pos;
    }

    ESP_LOGI(TAG, "Fragment %d/%d seq=%d stored (mask=0x%02X, total_len=%d)",
             hdr->frag_index + 1, hdr->frag_total, hdr->seq_num,
             frag_sess.received_mask, frag_sess.total_len);

    // Проверяем — все ли фрагменты получены
    uint32_t expected_mask = (1u << hdr->frag_total) - 1u;
    if ((frag_sess.received_mask & expected_mask) == expected_mask) {
        memcpy(out_buf, frag_sess.buf, frag_sess.total_len);
        *out_len = frag_sess.total_len;

        ESP_LOGI(TAG, "Reassembled: %d bytes from %d fragments",
                 frag_sess.total_len, hdr->frag_total);

        memset(&frag_sess, 0, sizeof(frag_sess));
        return true;
    }

    return false;
}
