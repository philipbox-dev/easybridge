#include "radio_hal.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stddef.h>

static const char *TAG = "RADIO";

// ── Пустое радио ────────────────────────────────────────────
// Не «ошибка инициализации», а осознанная заглушка: без неё
// каждый вызов radio->send() пришлось бы обкладывать проверкой
// на NULL, и один неизвестный чип ронял бы всю прошивку.
static esp_err_t null_init(void)              { return ESP_ERR_NOT_SUPPORTED; }
static bool      null_alive(void)             { return false; }
static esp_err_t null_configure(int s)        { (void)s; return ESP_ERR_NOT_SUPPORTED; }
static esp_err_t null_send(const uint8_t *d, size_t n) { (void)d; (void)n; return ESP_ERR_NOT_SUPPORTED; }
static int       null_receive(uint8_t *b, size_t n, int *r, float *s, int t)
{
    (void)b; (void)n; (void)r; (void)s;
    // Спим таймаут, иначе rx_task закрутится в busy-loop.
    vTaskDelay(pdMS_TO_TICKS(t > 0 ? t : 100));
    return 0;
}
static esp_err_t null_set_speed(int s)        { (void)s; return ESP_ERR_NOT_SUPPORTED; }
static esp_err_t null_set_power(uint8_t p)    { (void)p; return ESP_ERR_NOT_SUPPORTED; }

const radio_ops_t radio_ops_null = {
    .name = "нет радио",
    .init = null_init, .is_alive = null_alive, .configure = null_configure,
    .send = null_send, .receive = null_receive,
    .set_speed = null_set_speed, .set_tx_power = null_set_power,
    .channel_busy = NULL,
};

const radio_ops_t *radio = &radio_ops_null;
static const fsk_ops_t *s_fsk = NULL;

const fsk_ops_t *fsk_ops(void) { return s_fsk; }

esp_err_t radio_registry_init(void)
{
    const radio_cfg_t *r = &hwcfg()->radio;

    switch (r->kind) {
    case RADIO_E220:   radio = &radio_ops_e220;   break;
    case RADIO_SX127X: radio = &radio_ops_sx127x; break;
    case RADIO_SX126X: radio = &radio_ops_sx126x; break;
    case RADIO_SX128X: radio = &radio_ops_sx128x; break;
    case RADIO_HALOW:  radio = &radio_ops_halow;  break;
    default:
        ESP_LOGE(TAG, "неизвестный тип радио %u — работаем без эфира", r->kind);
        radio = &radio_ops_null;
        return ESP_ERR_NOT_SUPPORTED;
    }

    // FSK есть только там, где мы его реально реализовали. Флаг
    // supports_fsk в конфиге — про железо, а этот switch — про то,
    // написан ли драйвер; расходиться они не должны, но проверяем.
    s_fsk = NULL;
    if (r->kind == RADIO_SX127X && r->supports_fsk) s_fsk = &fsk_ops_sx127x;

    if (r->supports_fsk && !s_fsk) {
        ESP_LOGW(TAG, "%s помечен как FSK-способный, но драйвера FSK нет — "
                      "звонки по эфиру недоступны, только через веб",
                 hwcfg_radio_name(r->kind));
    }

    ESP_LOGI(TAG, "радио: %s, FSK: %s", radio->name, s_fsk ? s_fsk->name : "нет");
    return ESP_OK;
}
