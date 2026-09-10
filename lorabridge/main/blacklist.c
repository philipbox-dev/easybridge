#include "blacklist.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "BLIST";
static const char *NVS_NS  = "bridge";
static const char *NVS_KEY = "blocklist";

static uint32_t s_list[BLACKLIST_MAX];
static int      s_count = 0;

void blacklist_init(void)
{
    memset(s_list, 0, sizeof(s_list));
    s_count = 0;
    blacklist_load();
}

bool blacklist_contains(uint32_t node_id)
{
    for (int i = 0; i < s_count; i++) {
        if (s_list[i] == node_id) return true;
    }
    return false;
}

void blacklist_add(uint32_t node_id)
{
    if (blacklist_contains(node_id)) return;
    if (s_count >= BLACKLIST_MAX) {
        ESP_LOGW(TAG, "Blacklist full");
        return;
    }
    s_list[s_count++] = node_id;
    blacklist_save();
    ESP_LOGI(TAG, "Blocked: 0x%08X", node_id);
}

void blacklist_remove(uint32_t node_id)
{
    for (int i = 0; i < s_count; i++) {
        if (s_list[i] == node_id) {
            s_list[i] = s_list[--s_count];
            s_list[s_count] = 0;
            blacklist_save();
            ESP_LOGI(TAG, "Unblocked: 0x%08X", node_id);
            return;
        }
    }
}

void blacklist_clear(void)
{
    memset(s_list, 0, sizeof(s_list));
    s_count = 0;
    blacklist_save();
}

void blacklist_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, NVS_KEY, s_list, s_count * sizeof(uint32_t));
    nvs_commit(h);
    nvs_close(h);
}

void blacklist_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(s_list);
    if (nvs_get_blob(h, NVS_KEY, s_list, &len) == ESP_OK) {
        s_count = (int)(len / sizeof(uint32_t));
        ESP_LOGI(TAG, "Loaded %d blocked nodes", s_count);
    }
    nvs_close(h);
}
