#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "config.h"

// ============================================================
// Заголовок пакета (10 байт)
// ============================================================
typedef struct __attribute__((packed)) {
    uint16_t network_id;
    uint8_t  from_id;
    uint8_t  to_id;
    uint8_t  pkt_type;
    uint16_t seq_num;
    uint8_t  frag_total;
    uint8_t  frag_index;
    uint8_t  payload_len;
} pkt_header_t;

// ============================================================
// Буфер сборки фрагментов одного сообщения
// ============================================================
#define FRAG_BUF_SIZE (MAX_PAYLOAD_PER_PACKET * MAX_FRAGMENTS)

typedef struct {
    uint16_t seq_num;
    uint8_t  frag_total;
    uint32_t received_mask;   // было uint8_t — переполнение при >8 фрагментов
    uint8_t  buf[FRAG_BUF_SIZE];
    uint16_t total_len;
    uint32_t first_frag_ms;
    bool     in_use;
} frag_session_t;

// ============================================================
// API
// ============================================================
void     protocol_init(void);
// V2.6.1: legacy-ID из node_id — раньше оба девайса были 0x01
// и отбрасывали ответы друг друга как своё эхо
void     protocol_set_my_id(uint8_t id);
uint8_t  protocol_my_id(void);
uint16_t protocol_next_seq(void);

int protocol_build_packet(
    uint8_t        *out_buf,
    size_t          out_buf_size,
    uint8_t         to_id,
    uint8_t         pkt_type,
    uint16_t        seq,
    uint8_t         frag_total,
    uint8_t         frag_index,
    const uint8_t  *payload,
    uint8_t         payload_len
);

esp_err_t protocol_parse_packet(
    const uint8_t *data,
    int            len,
    pkt_header_t  *out_header,
    uint8_t       *out_payload,
    uint8_t       *out_payload_len
);

bool protocol_reassemble(
    const pkt_header_t *hdr,
    const uint8_t      *payload,
    uint8_t             payload_len,
    uint8_t            *out_buf,
    uint16_t           *out_len
);
