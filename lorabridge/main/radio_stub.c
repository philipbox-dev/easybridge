// ============================================================
// Заглушки радио, которые уже описаны в hwcfg, но ещё не
// написаны: LoRa 2.4 ГГц (SX128x) и 802.11ah HaLow.
//
// Смысл заглушек не в том, чтобы «было»: устройство с таким
// профилем поднимается целиком — BLE, веб, интерфейс, — и
// честно говорит, что эфир недоступен. Это позволяет собирать
// и отлаживать всё остальное на железе, которого ещё нет, и
// не городить #ifdef по всему проекту.
//
// Что нужно дописать, когда дойдут руки:
//   SX128x — команды почти как у SX126x (SetPacketType,
//            SetModulationParams…), но своя карта регистров и
//            FLRC/GFSK-режимы сверх LoRa;
//   HaLow  — MM6108 работает не как «радио с пакетами», а как
//            сетевой интерфейс: ему нужен свой путь (esp_netif),
//            а не radio_ops_t. См. docs/HALOW_BRIDGE.md.
// ============================================================
#include "radio_hal.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "RADIO-STUB";

static bool s_warned_2g4, s_warned_halow;

static esp_err_t stub_init_2g4(void)
{
    ESP_LOGW(TAG, "SX128x (2.4 ГГц) ещё не реализован — эфира не будет");
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t stub_init_halow(void)
{
    ESP_LOGW(TAG, "HaLow ещё не реализован — эфира не будет");
    return ESP_ERR_NOT_SUPPORTED;
}

static bool      stub_alive(void)           { return false; }
static esp_err_t stub_configure(int s)      { (void)s; return ESP_ERR_NOT_SUPPORTED; }
static esp_err_t stub_set_speed(int s)      { (void)s; return ESP_ERR_NOT_SUPPORTED; }
static esp_err_t stub_set_power(uint8_t p)  { (void)p; return ESP_ERR_NOT_SUPPORTED; }

static esp_err_t stub_send_2g4(const uint8_t *d, size_t n)
{
    (void)d; (void)n;
    if (!s_warned_2g4) { s_warned_2g4 = true; ESP_LOGW(TAG, "TX в 2.4 ГГц запрошен, драйвера нет"); }
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t stub_send_halow(const uint8_t *d, size_t n)
{
    (void)d; (void)n;
    if (!s_warned_halow) { s_warned_halow = true; ESP_LOGW(TAG, "TX в HaLow запрошен, драйвера нет"); }
    return ESP_ERR_NOT_SUPPORTED;
}

static int stub_receive(uint8_t *b, size_t n, int *r, float *s, int timeout_ms)
{
    (void)b; (void)n; (void)r; (void)s;
    // Отдаём время планировщику: rx_task крутится в цикле и без
    // этой задержки съел бы ядро целиком.
    vTaskDelay(pdMS_TO_TICKS(timeout_ms > 0 ? timeout_ms : 100));
    return 0;
}

const radio_ops_t radio_ops_sx128x = {
    .name = "SX128x (2.4G, заглушка)",
    .init = stub_init_2g4, .is_alive = stub_alive, .configure = stub_configure,
    .send = stub_send_2g4, .receive = stub_receive,
    .set_speed = stub_set_speed, .set_tx_power = stub_set_power,
    .channel_busy = NULL,
};

const radio_ops_t radio_ops_halow = {
    .name = "HaLow (заглушка)",
    .init = stub_init_halow, .is_alive = stub_alive, .configure = stub_configure,
    .send = stub_send_halow, .receive = stub_receive,
    .set_speed = stub_set_speed, .set_tx_power = stub_set_power,
    .channel_busy = NULL,
};
