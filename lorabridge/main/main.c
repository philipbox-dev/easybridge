#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_timer.h"

#include "config.h"
#include "identity.h"
#include "battery.h"
#include "led.h"
#include "lora_manager.h"
#include "ble_server.h"
#include "protocol.h"
#include "peer_manager.h"
#include "bridge.h"
#include "display.h"
#include "hwcfg.h"
#include "panel.h"
#include "touch.h"
#include "radio_hal.h"
#include "devnode.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    // NVS (identity нужен NVS — инициализируем первым)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
    nvs_flash_init();
        }

        // ★ hwcfg ПЕРВЫМ после NVS: всё остальное — пины, радио,
        // дисплей, батарея — спрашивает железо у него (V2.9).
        hwcfg_init();
        radio_registry_init();

        // Идентичность (node_id + имя из NVS) и батарея
        identity_init();
        battery_init();
        led_init();

    ESP_LOGI(TAG, "=============================");
    ESP_LOGI(TAG, "  EasyBridge - %s", identity_ble_name());
    ESP_LOGI(TAG, "  Плата: %s (hw=%d, профиль %s)",
             hwcfg()->name, hwcfg()->hw_id, hwcfg()->profile);
    ESP_LOGI(TAG, "  Радио: %s на %s, %lu Гц", radio->name,
             hwcfg_band_name(hwcfg()->radio.band),
             (unsigned long)hwcfg()->radio.freq_hz);
    ESP_LOGI(TAG, "  Net:   0x%04X", NETWORK_ID);
    ESP_LOGI(TAG, "=============================");

        // Протокол
        protocol_init();

        // Дисплей и тач
        display_init();
        touch_init();

        // Автономное устройство (датчик/реле). До bridge: если роль
        // включена, узел должен быть готов отвечать на команды сразу,
        // как только заработает радио.
        devnode_init();

        // ★ Bridge ПЕРЕД LoRa — чтобы очереди были готовы до первого RX
        bridge_init();

        // ★ Peer manager ПЕРЕД LoRa — чтобы очереди были готовы
        peer_manager_init(bridge_on_peer_status);

        // BLE
        ret = ble_server_init(bridge_on_ble_rx);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "BLE init FAILED!");
            display_show_error("BLE ERROR", "Init failed");
        }

        // ★ LoRa ПОСЛЕДНИМ — после всех обработчиков
        ret = lora_manager_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "LoRa init FAILED!");
            display_show_error("LoRa ERROR", radio->name);
        } else {
            lora_manager_set_rx_callback(bridge_on_lora_rx);
        }

        vTaskDelay(pdMS_TO_TICKS(2000));

        // Показать idle после splash
        display_show_idle(identity_ble_name(), "???", false, 0,
                          lora_manager_get_speed(), 0, 0);

        if (lora_manager_is_healthy()) {
            peer_manager_send_discovery();  // legacy discovery (backward compat)
            vTaskDelay(pdMS_TO_TICKS(500));
            bridge_announce();              // proto25 NODE_HELLO broadcast
        }

        ESP_LOGI(TAG, "System ready!");
}
