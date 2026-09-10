#include "led.h"
#include "config.h"
#include "hwcfg.h"
#include "driver/gpio.h"
#include "esp_timer.h"

// Светодиод описан в hwcfg: пин и полярность. Плат без него
// хватает, поэтому все функции молча ничего не делают, если
// пин не задан — вызывающему коду проверять не нужно.
static esp_timer_handle_t s_off_timer = NULL;
static bool s_held = false;   // led_set(true) держит поверх пульсов
static int  s_pin  = -1;
static bool s_active_low = false;

static void drive(bool on)
{
    if (s_pin < 0) return;
    gpio_set_level(s_pin, s_active_low ? !on : on);
}

static void off_cb(void *arg)
{
    (void)arg;
    if (!s_held) drive(false);
}

void led_init(void)
{
    s_pin = hwcfg()->led_pin;
    s_active_low = hwcfg()->led_active_low;
    if (s_pin < 0) return;

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << s_pin),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    drive(false);

    const esp_timer_create_args_t args = { .callback = off_cb, .name = "led_off" };
    esp_timer_create(&args, &s_off_timer);
}

void led_set(bool on)
{
    s_held = on;
    drive(on);
}

void led_pulse(uint32_t ms)
{
    if (s_pin < 0) return;
    drive(true);
    if (s_off_timer) {
        esp_timer_stop(s_off_timer);
        esp_timer_start_once(s_off_timer, (uint64_t)ms * 1000);
    }
}
