#include "lora_manager.h"
#include "radio_hal.h"
#include "config.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>

static const char      *TAG         = "LORA_MGR";
static lora_rx_cb_t     rx_callback = NULL;
static bool             healthy     = false;
static int              current_speed = LORA_SF_SLOW;
static volatile bool     suspended     = false;
static uint8_t          current_pwr_idx = E220_TX_POWER;

typedef struct {
    uint8_t data[256];
    size_t  len;
} tx_item_t;

#define TX_QUEUE_SIZE 8
static QueueHandle_t tx_queue = NULL;

// ★ TX задача
static void tx_task(void *arg)
{
    tx_item_t item;
    while (1) {
        if (xQueueReceive(tx_queue, &item, portMAX_DELAY) == pdTRUE) {
            if (!healthy) continue;
            while (suspended) vTaskDelay(pdMS_TO_TICKS(100));  // PTT владеет радио

            // ── Listen-before-talk (только радио с CAD: SX127x/SX126x) ──
            // Канал занят → случайный backoff и повторная проверка.
            // 6 попыток максимум, дальше шлём как есть (fail-open).
            if (radio->channel_busy) {
                for (int lbt = 0; lbt < 6 && radio->channel_busy(); lbt++) {
                    vTaskDelay(pdMS_TO_TICKS(30 + (esp_random() % 120)));
                }
            }

            esp_err_t ret = radio->send(item.data, item.len);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "TX failed");
            }
        }
    }
}

// ★ RX задача
//
// Раз в RX_REPORT_MS печатаем, сколько пакетов поймали и в каком
// состоянии чип. «Пиры не видят друг друга» — симптом, за которым
// стоят три разные причины (чип выпал из приёма, платы на разных
// частотах, платы на разных SF), и по логу одной платы они
// неразличимы. Эта строка различает их сразу.
#define RX_REPORT_MS 30000

static void rx_task(void *arg)
{
    uint8_t buf[256];
    int rssi;
    float snr;
    int      got = 0, bad = 0;
    uint32_t next_report = 0;

    while (1) {
        if (suspended) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        int len = radio->receive(buf, sizeof(buf), &rssi, &snr, 500);
        if (len > 0) {
            got++;
            if (rx_callback) rx_callback(buf, len, rssi, snr);
        } else if (len < 0) {
            bad++;
        }

        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (now >= next_report) {
            if (next_report) {
                ESP_LOGI(TAG, "приём за %ds: %d пакетов, %d битых",
                         RX_REPORT_MS / 1000, got, bad);
                if (radio->log_state) radio->log_state();
            }
            got = 0; bad = 0;
            next_report = now + RX_REPORT_MS;
        }
    }
}

esp_err_t lora_manager_init(void)
{
    tx_queue = xQueueCreate(TX_QUEUE_SIZE, sizeof(tx_item_t));
    if (!tx_queue) {
        ESP_LOGE(TAG, "Queue create failed");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = radio->init();
    if (ret != ESP_OK) {
        healthy = false;
        return ret;
    }

    // Probe the radio up to 3 times.
    // After a crash-reboot the module keeps power and may be mid-state.
    bool alive = false;
    for (int attempt = 0; attempt < 3 && !alive; attempt++) {
        if (attempt > 0) {
            ESP_LOGW(TAG, "%s not responding, retry %d/3...", radio->name, attempt + 1);
            vTaskDelay(pdMS_TO_TICKS(1500));
        }
        alive = radio->is_alive();
    }
    if (!alive) {
        // Don't hard-fail: the link may still work even if the
        // diagnostic probe returns nothing. Real TX/RX errors will
        // surface in radio->send() / radio->receive().
        ESP_LOGW(TAG, "%s probe failed — continuing anyway", radio->name);
    }

    ret = radio->configure(current_speed);
    if (ret != ESP_OK) return ret;

    healthy = true;

    // Результат создания задач проверяем. Без этого «нет приёма» выглядит
    // как исправная прошивка: tx_task на 4 КБ поднимается, rx_task на 8 КБ
    // не находит непрерывного блока — и LoRa рапортует ready, передавая в
    // эфир и не слыша ответа. К моменту вызова BT-контроллер, NimBLE и
    // шесть задач bridge/ptt/imgfsk уже забрали свою кучу.
    BaseType_t ok_tx = xTaskCreatePinnedToCore(tx_task, "lora_tx", 4096, NULL, 4, NULL, 0);
    BaseType_t ok_rx = xTaskCreatePinnedToCore(rx_task, "lora_rx", 8192, NULL, 3, NULL, 0);

    ESP_LOGI(TAG, "куча: %u свободно, наибольший блок %u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    if (ok_tx != pdPASS || ok_rx != pdPASS) {
        ESP_LOGE(TAG, "★ не создана задача %s%s — радио %s",
                 ok_tx != pdPASS ? "передачи " : "",
                 ok_rx != pdPASS ? "приёма" : "",
                 ok_rx != pdPASS ? "ГЛУХОЕ" : "немое");
        healthy = false;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "LoRa ready: %s (speed=%d)", radio->name, current_speed);
    return ESP_OK;
}

void lora_manager_set_rx_callback(lora_rx_cb_t cb)
{
    rx_callback = cb;
}

esp_err_t lora_manager_send(const uint8_t *data, size_t len)
{
    if (!healthy || !tx_queue) return ESP_ERR_INVALID_STATE;
    if (len > 200) return ESP_ERR_INVALID_SIZE;

    tx_item_t item;
    memcpy(item.data, data, len);
    item.len = len;

    if (xQueueSend(tx_queue, &item, pdMS_TO_TICKS(TX_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "TX queue full");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void lora_manager_send_async(const uint8_t *data, size_t len)
{
    if (!healthy || !tx_queue || len > 200) return;

    tx_item_t item;
    memcpy(item.data, data, len);
    item.len = len;

    if (xQueueSend(tx_queue, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "TX queue full (async)");
    }
}

bool lora_manager_is_healthy(void)
{
    return healthy && (tx_queue != NULL);
}

esp_err_t lora_manager_set_speed(int speed)
{
    if (!healthy) return ESP_ERR_INVALID_STATE;
    if (speed == current_speed) return ESP_OK;

    // Ждём пока TX очередь опустеет
    while (uxQueueMessagesWaiting(tx_queue) > 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelay(pdMS_TO_TICKS(500));

    current_speed = speed;
    esp_err_t ret = radio->set_speed(speed);

    ESP_LOGI(TAG, "Speed → %d", speed);
    return ret;
}

int lora_manager_get_speed(void)
{
    return current_speed;
}

esp_err_t lora_manager_set_tx_power(uint8_t pwr_idx)
{
    if (!healthy) return ESP_ERR_INVALID_STATE;
    if (pwr_idx > 3) return ESP_ERR_INVALID_ARG;
    if (pwr_idx == current_pwr_idx) return ESP_OK;

    while (uxQueueMessagesWaiting(tx_queue) > 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelay(pdMS_TO_TICKS(300));

    esp_err_t ret = radio->set_tx_power(pwr_idx);
    if (ret == ESP_OK) current_pwr_idx = pwr_idx;
    ESP_LOGI(TAG, "TX power → idx %u", pwr_idx);
    return ret;
}

int lora_manager_get_tx_power(void)
{
    return current_pwr_idx;
}

void lora_manager_suspend(void)
{
    suspended = true;
    vTaskDelay(pdMS_TO_TICKS(600));  // дать rx_task выйти из receive()
}

void lora_manager_resume(void)
{
    suspended = false;
}
