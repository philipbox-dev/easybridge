#include "battery.h"
#include "config.h"
#include "esp_log.h"

// Батарея описывается в hwcfg: ADC-юнит, канал и коэффициент
// делителя приходят из профиля платы, а не из board-заголовка.
// На плате без батареи задача монитора просто не запускается.
#define BATT_ADC_UNIT      ((adc_unit_t)(hwcfg()->batt.adc_unit - 1))
#define BATT_ADC_CHANNEL   ((adc_channel_t)hwcfg()->batt.adc_channel)
#define BATT_DIVIDER_RATIO (hwcfg()->batt.divider_x100 / 100.0f)


#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "BATTERY";

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t         s_cali;
static bool                      s_cali_ok = false;
static volatile int              s_mv      = -1;   // battery millivolts
static volatile uint8_t          s_percent = BATT_NONE;

// Li-ion voltage → % piecewise curve (resting voltage)
static uint8_t mv_to_percent(int mv)
{
    static const struct { int mv; uint8_t pct; } curve[] = {
        { 4200, 100 }, { 4060, 90 }, { 3980, 80 }, { 3920, 70 },
        { 3870, 60 },  { 3820, 50 }, { 3790, 40 }, { 3770, 30 },
        { 3740, 20 },  { 3680, 10 }, { 3450, 5 },  { 3000, 0 },
    };
    if (mv >= curve[0].mv) return 100;
    for (size_t i = 1; i < sizeof(curve) / sizeof(curve[0]); i++) {
        if (mv >= curve[i].mv) {
            // linear interpolation inside the segment
            int span_mv  = curve[i - 1].mv - curve[i].mv;
            int span_pct = curve[i - 1].pct - curve[i].pct;
            return curve[i].pct + (uint8_t)((mv - curve[i].mv) * span_pct / span_mv);
        }
    }
    return 0;
}

static void battery_task(void *arg)
{
    while (1) {
        int raw = 0, sum = 0, samples = 0;
        for (int i = 0; i < 8; i++) {
            if (adc_oneshot_read(s_adc, BATT_ADC_CHANNEL, &raw) == ESP_OK) {
                sum += raw; samples++;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (samples > 0) {
            int avg_raw = sum / samples;
            int mv_adc = 0;
            if (s_cali_ok &&
                adc_cali_raw_to_voltage(s_cali, avg_raw, &mv_adc) == ESP_OK) {
                // calibrated path
            } else {
                mv_adc = avg_raw * 3300 / 4095;  // rough fallback
            }
            s_mv = (int)(mv_adc * BATT_DIVIDER_RATIO);
            s_percent = mv_to_percent(s_mv);
            ESP_LOGD(TAG, "%d mV → %d%%", s_mv, s_percent);
        }
        vTaskDelay(pdMS_TO_TICKS(30000));  // every 30 s
    }
}

void battery_init(void)
{
    if (!hwcfg()->batt.present) {
        // Батареи нет — s_percent так и останется BATT_NONE, и
        // NODE_HELLO уедет без процентов, как и раньше.
        ESP_LOGI(TAG, "батареи на этой плате нет");
        return;
    }
    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = BATT_ADC_UNIT };
    if (adc_oneshot_new_unit(&unit_cfg, &s_adc) != ESP_OK) {
        ESP_LOGE(TAG, "ADC init failed");
        return;
    }
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = ADC_ATTEN_DB_12,      // full 0-3.3V range
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    adc_oneshot_config_channel(s_adc, BATT_ADC_CHANNEL, &chan_cfg);

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id  = BATT_ADC_UNIT,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    s_cali_ok = (adc_cali_create_scheme_line_fitting(&cali_cfg, &s_cali) == ESP_OK);
#elif ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id  = BATT_ADC_UNIT,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    s_cali_ok = (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) == ESP_OK);
#endif

    xTaskCreate(battery_task, "battery", 3072, NULL, 2, NULL);
    ESP_LOGI(TAG, "Battery monitor started (cali=%d)", (int)s_cali_ok);
}

uint8_t battery_get_percent(void)    { return s_percent; }
int     battery_get_millivolts(void) { return s_mv; }

