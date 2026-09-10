#include "nodedb.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "NODEDB";

static node_entry_t s_nodes[NODEDB_MAX_NODES];

void nodedb_init(void)
{
    memset(s_nodes, 0, sizeof(s_nodes));
    ESP_LOGI(TAG, "Init: capacity=%d nodes", NODEDB_MAX_NODES);
}

node_entry_t *nodedb_find(uint32_t node_id)
{
    for (int i = 0; i < NODEDB_MAX_NODES; i++) {
        if (s_nodes[i].in_use && s_nodes[i].node_id == node_id) {
            return &s_nodes[i];
        }
    }
    return NULL;
}

node_entry_t *nodedb_get_or_create(uint32_t node_id)
{
    node_entry_t *e = nodedb_find(node_id);
    if (e) return e;

    // Find free slot
    for (int i = 0; i < NODEDB_MAX_NODES; i++) {
        if (!s_nodes[i].in_use) {
            memset(&s_nodes[i], 0, sizeof(node_entry_t));
            s_nodes[i].in_use    = true;
            s_nodes[i].node_id   = node_id;
            s_nodes[i].status    = NODE_STATUS_UNKNOWN;
            ESP_LOGI(TAG, "New node: 0x%08X (slot %d)", node_id, i);
            return &s_nodes[i];
        }
    }

    // DB full — evict oldest offline node
    uint32_t oldest_ms = UINT32_MAX;
    int      oldest_i  = -1;
    for (int i = 0; i < NODEDB_MAX_NODES; i++) {
        if (s_nodes[i].status == NODE_STATUS_OFFLINE &&
            s_nodes[i].last_seen_ms < oldest_ms) {
            oldest_ms = s_nodes[i].last_seen_ms;
            oldest_i  = i;
        }
    }
    if (oldest_i >= 0) {
        ESP_LOGW(TAG, "DB full, evicting 0x%08X", s_nodes[oldest_i].node_id);
        memset(&s_nodes[oldest_i], 0, sizeof(node_entry_t));
        s_nodes[oldest_i].in_use  = true;
        s_nodes[oldest_i].node_id = node_id;
        return &s_nodes[oldest_i];
    }

    ESP_LOGE(TAG, "NodeDB full, cannot add 0x%08X", node_id);
    return NULL;
}

void nodedb_update_signal(uint32_t node_id, int rssi, float snr, uint8_t hops)
{
    node_entry_t *e = nodedb_get_or_create(node_id);
    if (!e) return;
    e->rssi         = rssi;
    e->snr          = snr;
    e->hop_count    = hops;
    e->last_seen_ms = (uint32_t)(esp_timer_get_time() / 1000);
    e->status       = NODE_STATUS_ONLINE;
}

void nodedb_update_gps(uint32_t node_id, int32_t lat, int32_t lon, int16_t alt, uint8_t batt)
{
    node_entry_t *e = nodedb_get_or_create(node_id);
    if (!e) return;
    e->lat  = lat;
    e->lon  = lon;
    e->alt  = alt;
    e->batt = batt;
}

void nodedb_set_name(uint32_t node_id, const char *name)
{
    node_entry_t *e = nodedb_get_or_create(node_id);
    if (!e) return;
    strncpy(e->name, name, NODE_NAME_LEN - 1);
    e->name[NODE_NAME_LEN - 1] = '\0';
}

void nodedb_set_hw(uint32_t node_id, uint8_t hw)
{
    node_entry_t *e = nodedb_get_or_create(node_id);
    if (!e) return;
    e->hw = hw;
}

void nodedb_set_status(uint32_t node_id, node_status_t status)
{
    node_entry_t *e = nodedb_find(node_id);
    if (e) e->status = status;
}

void nodedb_expire_stale(void)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    for (int i = 0; i < NODEDB_MAX_NODES; i++) {
        if (!s_nodes[i].in_use) continue;
        uint32_t age_ms = now_ms - s_nodes[i].last_seen_ms;
        if (age_ms > NODE_TIMEOUT_MS) {
            if (s_nodes[i].status != NODE_STATUS_OFFLINE) {
                ESP_LOGI(TAG, "Node 0x%08X timed out", s_nodes[i].node_id);
                s_nodes[i].status = NODE_STATUS_OFFLINE;
            }
        } else if (age_ms > 120000) {
            s_nodes[i].status = NODE_STATUS_AWAY;
        }
    }
}

int nodedb_count_online(void)
{
    int n = 0;
    for (int i = 0; i < NODEDB_MAX_NODES; i++) {
        if (s_nodes[i].in_use && s_nodes[i].status == NODE_STATUS_ONLINE) n++;
    }
    return n;
}

node_entry_t *nodedb_get_all(void)
{
    return s_nodes;
}
