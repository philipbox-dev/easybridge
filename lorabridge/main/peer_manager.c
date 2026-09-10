#include "peer_manager.h"
#include "lora_manager.h"
#include "bridge.h"
#include "ble_server.h"
#include "protocol.h"
#include "nodedb.h"
#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char     *TAG        = "PEER";
static peer_status_cb_t status_cb = NULL;
static bool            peer_online = false;
static int             peer_rssi   = 0;
static uint32_t        last_seen   = 0;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void peer_manager_on_ping(void)
{
    last_seen = now_ms();
    if (!peer_online) {
        peer_online = true;
        ESP_LOGI(TAG, "Peer online (got ping)");
        if (status_cb) status_cb(true, peer_rssi);
    }

    // ★ Асинхронная отправка pong
    uint8_t buf[PACKET_HEADER_SIZE + 4];
    int len = protocol_build_packet(buf, sizeof(buf), PEER_DEVICE_ID,
                                    PKT_HEARTBEAT_PONG, protocol_next_seq(), 1, 0, NULL, 0);
    if (len > 0) lora_manager_send_async(buf, len);
}

void peer_manager_on_pong(int rssi)
{
    last_seen  = now_ms();
    peer_rssi  = rssi;
    if (!peer_online) {
        peer_online = true;
        ESP_LOGI(TAG, "Peer online (got pong) rssi=%d", rssi);
        if (status_cb) status_cb(true, rssi);
    }
}

void peer_manager_on_discovery_resp(const char *name, int rssi)
{
    last_seen   = now_ms();
    peer_rssi   = rssi;
    peer_online = true;
    ESP_LOGI(TAG, "Peer: %s rssi=%d", name, rssi);
    if (status_cb) status_cb(true, rssi);
}

void peer_manager_send_ping(void)
{
    uint8_t buf[PACKET_HEADER_SIZE + 4];
    int len = protocol_build_packet(buf, sizeof(buf), PEER_DEVICE_ID,
                                    PKT_HEARTBEAT_PING, protocol_next_seq(), 1, 0, NULL, 0);
    if (len > 0) lora_manager_send(buf, len);
}

void peer_manager_send_discovery(void)
{
    uint8_t buf[PACKET_HEADER_SIZE + 4];
    int len = protocol_build_packet(buf, sizeof(buf), BROADCAST_ID,
                                    PKT_DISCOVERY_REQ, protocol_next_seq(), 1, 0, NULL, 0);
    if (len > 0) lora_manager_send(buf, len);
    ESP_LOGI(TAG, "Discovery sent");
}

bool peer_manager_is_online(void) { return peer_online; }
int  peer_manager_get_rssi(void)  { return peer_rssi;   }

static void heartbeat_task(void *arg)
{
    uint32_t initial_delay = 10000 + (MY_DEVICE_ID * 5000) + (esp_random() % 5000);
    ESP_LOGI(TAG, "Initial heartbeat delay: %lu ms", (unsigned long)initial_delay);
    vTaskDelay(pdMS_TO_TICKS(initial_delay));

    while (1) {
        uint32_t interval = HEARTBEAT_INTERVAL_MS;
        interval = interval * 80 / 100 + (esp_random() % (interval * 40 / 100));

        vTaskDelay(pdMS_TO_TICKS(interval));

        // B13: без периодического вызова статусы узлов вечно ONLINE и
        // полную nodedb (32 узла) невозможно очистить под новые.
        nodedb_expire_stale();

        // Живость пира держится широковещательным NODE_HELLO (proto25), а
        // не legacy-пингом: тот уходит юникастом на PEER_DEVICE_ID (0x02),
        // а реальный legacy-ID узла — node_id & 0xFF (bridge_init), так что
        // приёмник отбрасывал такой пинг как «not for us» и пир уходил в
        // офлайн через HEARTBEAT_TIMEOUT_MS сразу после стартового hello.
        bridge_announce();

        peer_manager_send_ping();   // legacy-совместимость со старой прошивкой

        if (peer_online && (now_ms() - last_seen > HEARTBEAT_TIMEOUT_MS)) {
            peer_online = false;
            ESP_LOGW(TAG, "Peer offline");
            if (status_cb) status_cb(false, 0);
        }
    }
}

void peer_manager_init(peer_status_cb_t cb)
{
    status_cb   = cb;
    peer_online = false;
    peer_rssi   = 0;
    last_seen   = 0;

    xTaskCreatePinnedToCore(heartbeat_task, "heartbeat", 4096, NULL, 2, NULL, 0);

    ESP_LOGI(TAG, "Peer manager init");
}
