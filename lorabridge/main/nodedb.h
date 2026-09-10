#pragma once
// ============================================================
// NodeDB — RAM-based node registry, max 32 active nodes
// Tracks all known peers, their GPS, signal, status
// ============================================================
#include <stdint.h>
#include <stdbool.h>

#define NODEDB_MAX_NODES    32
#define NODE_TIMEOUT_MS     300000   // 5 min without heartbeat = offline
#define NODE_NAME_LEN       24

typedef enum {
    NODE_STATUS_UNKNOWN = 0,
    NODE_STATUS_ONLINE,
    NODE_STATUS_AWAY,       // Last seen > 2 min
    NODE_STATUS_OFFLINE,
} node_status_t;

typedef struct {
    uint32_t      node_id;
    char          name[NODE_NAME_LEN];
    node_status_t status;
    int32_t       lat;          // degrees × 1e7
    int32_t       lon;
    int16_t       alt;
    uint8_t       batt;         // battery % (0xFF = no battery)
    uint8_t       hw;           // HW_ID_* hardware type
    int           rssi;         // last heard RSSI (dBm)
    float         snr;
    uint8_t       hop_count;    // hops away from us
    uint32_t      last_seen_ms; // esp_timer ms
    uint32_t      last_seq;     // last packet_id for rate limit
    uint32_t      last_pkt_ms;  // timestamp of last packet
    uint32_t      last_hello_ms; // когда мы сами отвечали ему hello
    bool          in_use;
} node_entry_t;

void          nodedb_init(void);
node_entry_t *nodedb_find(uint32_t node_id);
node_entry_t *nodedb_get_or_create(uint32_t node_id);
void          nodedb_update_signal(uint32_t node_id, int rssi, float snr, uint8_t hops);
void          nodedb_update_gps(uint32_t node_id, int32_t lat, int32_t lon, int16_t alt, uint8_t batt);
void          nodedb_set_name(uint32_t node_id, const char *name);
void          nodedb_set_hw(uint32_t node_id, uint8_t hw);
void          nodedb_set_status(uint32_t node_id, node_status_t status);
void          nodedb_expire_stale(void);     // Call periodically
int           nodedb_count_online(void);
node_entry_t *nodedb_get_all(void);         // Returns pointer to array[NODEDB_MAX_NODES]
