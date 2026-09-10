#pragma once
// ============================================================
// Easy Bridge Protocol v2.5
// Binary packet format — NOT backward compatible with v1/v2
// ============================================================
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// ── Magic & Version ──────────────────────────────────────────
#define PKT25_MAGIC         0xEB25
#define PKT25_VERSION       0x01

// ── Node IDs ─────────────────────────────────────────────────
#define NODE_BROADCAST      0xFFFFFFFF   // All nodes
#define NODE_SOS_ALL        0xFFFFFFFE   // SOS override (all, ignores blocklist)
#define GROUP_NONE          0x00000000   // Direct/no-group message

// ── Packet types ─────────────────────────────────────────────
#define PKT25_TEXT_MSG      0x01
#define PKT25_TEXT_ACK      0x02
#define PKT25_READ_RECEIPT  0x03
#define PKT25_GPS_BEACON    0x04
#define PKT25_NODE_HELLO    0x05   // Announce presence
#define PKT25_NODE_BYE      0x06   // Graceful disconnect
#define PKT25_SOS           0x07   // Emergency (bypasses all filters)
#define PKT25_SOS_ACK       0x08
#define PKT25_PANIC         0x09   // Forces target phone to scream
#define PKT25_CHAN_SWITCH    0x0A   // Announce channel/SF migration
#define PKT25_CHAN_SWITCH_ACK 0x0B
#define PKT25_GROUP_JOIN    0x0C
#define PKT25_GROUP_LEAVE   0x0D
#define PKT25_GROUP_INFO    0x0E   // Group metadata broadcast
#define PKT25_QUICK_PHRASE  0x0F   // Pre-coded short phrase (1 byte payload)
#define PKT25_PING          0x10
#define PKT25_PONG          0x11
#define PKT25_VOICE_MSG     0x12   // Голосовое сообщение (AMR-NB), V2.6
#define PKT25_PTT_START     0x13   // Захват канала под FSK-звонок, V2.7
#define PKT25_PTT_END       0x14   // Завершение FSK-звонка
#define PKT25_IMG_START     0x15   // Захват канала под FSK-передачу картинки, V2.9.3
// ── Автономные устройства (V2.9) ─────────────────────────────
// Датчик или реле на МК, к которому НЕ подключён телефон. Такой узел
// сам рассказывает о себе (DEV_HELLO) и шлёт показания (DEV_DATA);
// любой, кто это слышит, может показать виджет и отправить команду.
#define PKT25_DEV_HELLO     0x16   // Манифест: кто я и что умею
#define PKT25_DEV_DATA      0x17   // Показания
#define PKT25_DEV_CMD       0x18   // Команда устройству (реле, режим)
#define PKT25_DEV_ACK       0x19   // Результат выполнения команды

// ── Flags ────────────────────────────────────────────────────
#define FLAG25_ENCRYPTED    (1 << 0)   // AES-128 payload
#define FLAG25_RELAY        (1 << 1)   // Relayed by intermediate node
#define FLAG25_GROUP        (1 << 2)   // group_id is valid
#define FLAG25_FRAG_MORE    (1 << 3)   // More fragments follow
#define FLAG25_ACK_REQ      (1 << 4)   // Request delivery ACK
#define FLAG25_SOS_OVERRIDE (1 << 5)   // Bypass all blocklists/group filters
#define FLAG25_COMPRESSED   (1 << 6)   // Reserved

// ── Priority levels (TX queue ordering) ──────────────────────
#define PRIO25_SOS      0   // Immediate, interrupts TX, broadcast
#define PRIO25_PANIC    0   // Same level as SOS
#define PRIO25_TEXT     1   // Normal chat
#define PRIO25_ACK      1
#define PRIO25_GPS      2   // Droppable if queue full
#define PRIO25_HELLO    3
#define PRIO25_RELAY    2

// ── Limits ───────────────────────────────────────────────────
#define PKT25_MAX_PAYLOAD   155     // LoRa 180 - 25 (header) = 155 bytes
#define PKT25_MAX_SIZE      180     // Max LoRa packet (E220-400T30D)
#define PKT25_MAX_FRAGS     8       // Max fragments per message
#define PKT25_MAX_TEXT      (PKT25_MAX_PAYLOAD * PKT25_MAX_FRAGS)  // ~1240 bytes raw
#define PKT25_TEXT_LIMIT    850     // Enforced display limit (UTF-8 safe)
#define PKT25_HOP_MAX       5       // Max mesh hops
// Голос: AMR-NB 4.75kbps ≈ 600 Б/с → 8 сек ≈ 4.8КБ. У фрагментов
// голоса свой бюджет (не PKT25_MAX_FRAGS) — маска сборки uint64.
#define PKT25_VOICE_MAX     6144    // Макс байт на голосовое
#define PKT25_VOICE_FRAGS   40      // Макс фрагментов (40×155 = 6200)
#define PKT25_DEDUP_SIZE    96      // Packet ID cache for relay dedup
                                    // (per-fragment: 1 voice msg = up to 40 slots)
#define PKT25_RATE_LIMIT_MS 1500    // Min ms between sends from same node

// ── Header (25 bytes) ────────────────────────────────────────
typedef struct __attribute__((packed)) {
    uint16_t magic;         // 0xEB25
    uint8_t  version;       // 0x01
    uint8_t  flags;         // FLAG25_* bitmask
    uint32_t src_node_id;   // Source node (32-bit, derived from BT MAC)
    uint32_t dst_node_id;   // Destination (NODE_BROADCAST = 0xFFFFFFFF)
    uint32_t group_id;      // Group ID (0 = direct message)
    uint16_t packet_id;     // Unique per src, for dedup (rolling counter)
    uint8_t  hop_limit;     // Remaining hops allowed
    uint8_t  hop_count;     // Hops taken so far (for diagnostics)
    uint8_t  pkt_type;      // PKT25_* type
    uint8_t  prio;          // PRIO25_* priority
    uint8_t  frag_total;    // Total fragments (1 = no frag)
    uint8_t  frag_index;    // Fragment index (0-based)
    uint8_t  payload_len;   // Encrypted/raw payload length
} pkt25_hdr_t;  // 25 bytes

#define PKT25_HDR_SIZE sizeof(pkt25_hdr_t)

// ── GPS payload (20 bytes, fits in 1 fragment) ───────────────
typedef struct __attribute__((packed)) {
    int32_t  lat;       // degrees × 1e7  (±180° range)
    int32_t  lon;       // degrees × 1e7
    int16_t  alt;       // meters above sea level
    uint16_t speed;     // km/h × 10
    uint16_t heading;   // degrees × 10 (0–3599)
    uint8_t  batt;      // battery % (0–100)
    uint8_t  gflags;    // GPS_VALID(1)|MOVING(2)|CHARGING(4)|PRECISE(8)
} gps25_t;  // 16 bytes

// ── Quick phrase codes (1 byte) ───────────────────────────────
#define QP_YES          0x01  // "Да"
#define QP_NO           0x02  // "Нет"
#define QP_OK           0x03  // "Окей"
#define QP_COMING       0x04  // "Иду"
#define QP_HELP         0x05  // "Помоги"
#define QP_APPROACH     0x06  // "Подойди"
#define QP_STOP         0x07  // "Стой"
#define QP_RUN          0x08  // "Бежим"
#define QP_WAIT         0x09  // "Жди"
#define QP_HELLO        0x0A  // "Привет"

// ── Group info payload ────────────────────────────────────────
typedef struct __attribute__((packed)) {
    uint32_t group_id;
    char     name[24];
    uint8_t  member_count;
} group25_info_t;  // 29 bytes

// ── Channel switch payload ────────────────────────────────────
typedef struct __attribute__((packed)) {
    uint8_t  new_sf;        // Spreading factor
    uint32_t new_freq_khz;  // Frequency in kHz
    uint8_t  countdown_sec; // Seconds until migration
} chan_switch25_t;

// ── API ───────────────────────────────────────────────────────
void     proto25_init(uint32_t my_node_id);
uint32_t proto25_my_id(void);
uint16_t proto25_next_pkt_id(void);

// ── Payload encryption (V2.9) ─────────────────────────────────
// AES-128-CTR over payloads. key16 = 16-byte key (NULL disables → plaintext).
// When enabled, every built packet's payload is encrypted and flagged
// FLAG25_ENCRYPTED; parse() decrypts flagged payloads. Header stays clear.
void proto25_set_key(const uint8_t *key16);
bool proto25_crypto_enabled(void);

// Auto-assigns a fresh packet_id. Use for single-fragment packets.
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
    uint8_t         payload_len
);

// V2.9 (fix B1): explicit packet_id so every fragment of one message
// shares it — the receiver keys reassembly on (src, packet_id).
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
    uint8_t         payload_len
);

esp_err_t proto25_parse(
    const uint8_t *data,
    int            len,
    pkt25_hdr_t   *out_hdr,
    uint8_t       *out_payload,
    uint8_t       *out_payload_len
);

// Relay dedup — returns true if this exact fragment was already seen
// (storm guard). Keyed on (src, packet_id, frag_index): fragments of one
// message share packet_id (fix B1), so frag_index must be part of the key
// or frags 1..N would be mistaken for duplicates of frag 0.
bool proto25_dedup_seen(uint32_t src_node_id, uint16_t packet_id, uint8_t frag_index);
void proto25_dedup_add(uint32_t src_node_id, uint16_t packet_id, uint8_t frag_index);

// Rate limiter — returns true if node is sending too fast
bool proto25_rate_limited(uint32_t src_node_id);
