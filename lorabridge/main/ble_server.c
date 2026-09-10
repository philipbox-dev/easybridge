#include "ble_server.h"
#include "config.h"
#include "identity.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "BLE";

static ble_rx_cb_t  ble_rx_callback  = NULL;
static uint16_t     conn_handle       = BLE_HS_CONN_HANDLE_NONE;
static uint16_t     notify_handle     = 0;
static bool         subscribed        = false;

// ── Notify queue ──────────────────────────────────────────────
// V2.7.1: входящие BLE-команды больше НЕ исполняются в задаче NimBLE.
// bridge_on_ble_rx тяжёлый (nodes-дамп 2КБ на стеке, NVS, PTT до 2с) —
// nimble_host периодически переполнялся. Теперь rx_chr_access только
// копирует команду в очередь, а разбирает её задача ble_rx (8КБ стека).
#define RX_QUEUE_LEN   8
#define RX_MAX_LEN     512
typedef struct { char s[RX_MAX_LEN]; size_t len; } rx_msg_t;
static QueueHandle_t s_rx_queue = NULL;

// ble_server_notify() is called from bridge_on_ble_rx() which runs
// inside the NimBLE host task's GATT write callback.
// Calling ble_gatts_notify_custom() from there causes a recursive
// ble_hs_lock() deadlock. Fix: post to a queue, drain from own task.
// V2.6: очередь короче, элемент больше — длинные JSON (nodes dump,
// длинный текст, voice-чанки) раньше обрезались на 512 и на MTU.
// V2.9.5: 8 → 16. PTT-звонок генерит ~20 evt:ptt_audio/с; глубины 8 не
// хватало при любой конкуренции (nodes-дамп, ptt_state) — аудио дропалось.
#define NOTIFY_QUEUE_LEN 16
#define NOTIFY_MAX_LEN   2048
// Одна GATT-нотификация ограничена MTU-3 (у нас MTU 256) —
// длинные JSON шлём кусками, приложение склеивает по скобкам.
#define NOTIFY_CHUNK      180

typedef struct { char s[NOTIFY_MAX_LEN]; } notify_msg_t;

static QueueHandle_t s_notify_queue = NULL;

// V2.9.5: диагностика потока notify — сколько сообщений выкинули из-за
// полной очереди и сколько ОБОРВАЛИ на середине (нехватка mbuf). Обрыв
// на середине хуже дропа: приложение склеивает чанки по балансу скобок,
// недосланный JSON рассинхронизирует сборщик и глушит ВСЕ события после.
static volatile uint32_t s_ntf_dropped = 0;
static volatile uint32_t s_ntf_aborted = 0;
uint32_t ble_server_notify_dropped(void) { return s_ntf_dropped; }
uint32_t ble_server_notify_aborted(void) { return s_ntf_aborted; }

static void notify_task(void *arg)
{
    // V2.6.2: msg 2КБ — static, а не на стеке задачи (был overflow).
    // Единственный потребитель очереди, так что static безопасен.
    static notify_msg_t msg;
    while (1) {
        if (xQueueReceive(s_notify_queue, &msg, portMAX_DELAY) != pdTRUE) continue;

        if (conn_handle == BLE_HS_CONN_HANDLE_NONE || !subscribed || !notify_handle) {
            continue;
        }

        size_t total = strlen(msg.s);
        for (size_t off = 0; off < total; off += NOTIFY_CHUNK) {
            size_t chunk = total - off;
            if (chunk > NOTIFY_CHUNK) chunk = NOTIFY_CHUNK;

            // V2.9.5: упорно ретраим и mbuf, и notify — бросить сообщение
            // на середине нельзя (рассинхрон сборщика в приложении).
            struct os_mbuf *om = NULL;
            int rc = -1;
            for (int try = 0; try < 12; try++) {
                om = ble_hs_mbuf_from_flat(msg.s + off, chunk);
                if (!om) { vTaskDelay(pdMS_TO_TICKS(25)); continue; }
                rc = ble_gatts_notify_custom(conn_handle, notify_handle, om);
                if (rc == 0) break;
                // notify_custom сам освобождает om при ошибке
                vTaskDelay(pdMS_TO_TICKS(25));
            }
            if (rc != 0) {
                ESP_LOGE(TAG, "Notify failed after retries (off=%u/%u)",
                         (unsigned)off, (unsigned)total);
                s_ntf_aborted++;
                break;
            }
            if (off + chunk < total) vTaskDelay(pdMS_TO_TICKS(15));
        }
    }
}

// NUS UUID (Nordic UART Service)
static const ble_uuid128_t svc_uuid = BLE_UUID128_INIT(
    0x9E,0xCA,0xDC,0x24,0x0E,0xE5,0xA9,0xE0,
    0x93,0xF3,0xA3,0xB5,0x01,0x00,0x40,0x6E
);
static const ble_uuid128_t rx_uuid = BLE_UUID128_INIT(
    0x9E,0xCA,0xDC,0x24,0x0E,0xE5,0xA9,0xE0,
    0x93,0xF3,0xA3,0xB5,0x02,0x00,0x40,0x6E
);
static const ble_uuid128_t tx_uuid = BLE_UUID128_INIT(
    0x9E,0xCA,0xDC,0x24,0x0E,0xE5,0xA9,0xE0,
    0x93,0xF3,0xA3,0xB5,0x03,0x00,0x40,0x6E
);

// ── GATT callbacks ────────────────────────────────────────────
static int tx_chr_access(uint16_t conn_hdl, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return 0;
}

static void ble_rx_task(void *arg)
{
    static rx_msg_t msg;   // единственный потребитель — static безопасен
    while (1) {
        if (xQueueReceive(s_rx_queue, &msg, portMAX_DELAY) != pdTRUE) continue;
        if (ble_rx_callback) ble_rx_callback(msg.s, msg.len);
    }
}

static int rx_chr_access(uint16_t conn_hdl, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        rx_msg_t msg = {0};
        if (len > RX_MAX_LEN - 1) len = RX_MAX_LEN - 1;
        ble_hs_mbuf_to_flat(ctxt->om, msg.s, len, NULL);
        msg.s[len] = '\0';
        msg.len = len;
        ESP_LOGI(TAG, "BLE RX: %.120s", msg.s);
        if (s_rx_queue && xQueueSend(s_rx_queue, &msg, 0) != pdTRUE) {
            ESP_LOGW(TAG, "BLE RX queue full — команда дропнута");
        }
    }
    return 0;
}

// ── GATT service ──────────────────────────────────────────────
static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid       = &tx_uuid.u,
                .access_cb  = tx_chr_access,
                .val_handle = &notify_handle,
                .flags      = BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid      = &rx_uuid.u,
                .access_cb = rx_chr_access,
                .flags     = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            { 0 }
        }
    },
    { 0 }
};

// ── GAP events ────────────────────────────────────────────────
static int gap_event_handler(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                conn_handle = event->connect.conn_handle;
                subscribed  = false;
                ESP_LOGI(TAG, "Connected: handle=%d", conn_handle);
            } else {
                conn_handle = BLE_HS_CONN_HANDLE_NONE;
                ble_server_start_adv();
            }
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "Disconnected: reason=%d", event->disconnect.reason);
            conn_handle = BLE_HS_CONN_HANDLE_NONE;
            subscribed  = false;
            ble_server_start_adv();
            break;

        case BLE_GAP_EVENT_MTU:
            ESP_LOGI(TAG, "MTU: %d", event->mtu.value);
            break;

        case BLE_GAP_EVENT_SUBSCRIBE:
            subscribed = (event->subscribe.cur_notify != 0);
            ESP_LOGI(TAG, "Notify %s", subscribed ? "enabled" : "disabled");
            break;

        default:
            break;
    }
    return 0;
}

// ── Advertise ─────────────────────────────────────────────────
void ble_server_start_adv(void)
{
    struct ble_gap_adv_params adv_params = {0};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    struct ble_hs_adv_fields fields = {0};
    fields.flags             = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name              = (uint8_t *)identity_ble_name();
    fields.name_len          = strlen(identity_ble_name());
    fields.name_is_complete  = 1;

    ble_gap_adv_set_fields(&fields);
    ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                      &adv_params, gap_event_handler, NULL);
    ESP_LOGI(TAG, "Advertising as \"%s\"", identity_ble_name());
}

// ── NimBLE host task ──────────────────────────────────────────
static void ble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_server_start_adv();
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "BLE reset: %d", reason);
}

// ── Public API ────────────────────────────────────────────────
esp_err_t ble_server_init(ble_rx_cb_t rx_cb)
{
    ble_rx_callback = rx_cb;

    s_notify_queue = xQueueCreate(NOTIFY_QUEUE_LEN, sizeof(notify_msg_t));
    if (!s_notify_queue) {
        ESP_LOGE(TAG, "Failed to create notify queue");
        return ESP_ERR_NO_MEM;
    }
    xTaskCreatePinnedToCore(notify_task, "ble_notify", 6144, NULL, 5, NULL, 1);

    s_rx_queue = xQueueCreate(RX_QUEUE_LEN, sizeof(rx_msg_t));
    xTaskCreatePinnedToCore(ble_rx_task, "ble_rx", 8192, NULL, 4, NULL, 1);

    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    ble_svc_gap_device_name_set(identity_ble_name());

    ble_gatts_count_cfg(gatt_svcs);
    ble_gatts_add_svcs(gatt_svcs);

    nimble_port_freertos_init(ble_host_task);

    ESP_LOGI(TAG, "BLE init OK");
    return ESP_OK;
}

void ble_server_notify(const char *json)
{
    if (!s_notify_queue) return;

    notify_msg_t msg;
    strncpy(msg.s, json, NOTIFY_MAX_LEN - 1);
    msg.s[NOTIFY_MAX_LEN - 1] = '\0';

    // Non-blocking: drop if queue full (caller is often in ISR/BLE context)
    if (xQueueSend(s_notify_queue, &msg, 0) != pdTRUE) {
        s_ntf_dropped++;
        ESP_LOGW(TAG, "Notify queue full, dropped: %.40s", json);
    }
}

bool ble_server_is_connected(void)
{
    return (conn_handle != BLE_HS_CONN_HANDLE_NONE);
}

void ble_server_set_connected_name(const char *name) { (void)name; }
