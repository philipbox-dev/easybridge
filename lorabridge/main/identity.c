#include "identity.h"
#include "config.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "nvs.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "IDENTITY";

#define ID_NVS_NS    "identity"
#define ID_KEY_NAME  "name"

static uint32_t s_node_id = 0;
static char     s_name[32]     = {0};
static char     s_ble_name[48] = {0};

static void rebuild_ble_name(void)
{
    if (s_name[0]) {
        snprintf(s_ble_name, sizeof(s_ble_name), "%s-%s", BLE_NAME_PREFIX, s_name);
    } else {
        snprintf(s_ble_name, sizeof(s_ble_name), "%s-%04X",
                 BLE_NAME_PREFIX, (unsigned)(s_node_id & 0xFFFF));
    }
}

void identity_init(void)
{
    // node_id from BT MAC bytes 2-5 (same derivation bridge.c always used)
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    s_node_id = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) |
                ((uint32_t)mac[4] << 8)  |  (uint32_t)mac[5];
    if (s_node_id == 0) s_node_id = 0xDEADBEEF;

    nvs_handle_t h;
    if (nvs_open(ID_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t sz = sizeof(s_name);
        nvs_get_str(h, ID_KEY_NAME, s_name, &sz);
        nvs_close(h);
    }
    rebuild_ble_name();
    ESP_LOGI(TAG, "node_id=0x%08lX name='%s' ble='%s' hw=%d",
             (unsigned long)s_node_id, s_name, s_ble_name, BOARD_HW_ID);
}

uint32_t    identity_node_id(void)  { return s_node_id; }
const char *identity_name(void)     { return s_name; }
const char *identity_ble_name(void) { return s_ble_name; }
uint8_t     identity_hw_id(void)    { return BOARD_HW_ID; }

void identity_set_name(const char *name)
{
    strncpy(s_name, name, sizeof(s_name) - 1);
    s_name[sizeof(s_name) - 1] = '\0';
    rebuild_ble_name();

    nvs_handle_t h;
    if (nvs_open(ID_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, ID_KEY_NAME, s_name);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "Name persisted: '%s'", s_name);
    } else {
        ESP_LOGE(TAG, "NVS open failed — name not persisted");
    }
}
