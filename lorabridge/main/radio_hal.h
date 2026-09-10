#pragma once
// ============================================================
// Radio HAL — единый интерфейс поверх всех радио (V2.9)
//
// До V2.9 драйвер выбирался на компиляции: ровно один
// radio_ops_t попадал в бинарник. Теперь компилируются все, а
// нужный выбирается в рантайме по hwcfg()->radio.kind, и
// активный лежит в указателе `radio`. Один бинарник умеет и
// E220, и Ra-02, и SX1262 — что припаяно, то и поедет.
//
// Скорости оставлены в прежней семантике (её знают BLE-команды
// и приложение):
//   LORA_SF_SLOW (0) — дальнобой (у E220 2.4 кбит/с)
//   LORA_SF_FAST (5) — быстро   (у E220 19.2 кбит/с)
// SPI-радио отображают их в SF/BW-пресеты, подобранные под те же
// эфирные скорости, чтобы Ra-02 слышал E220 на одной частоте.
// ============================================================
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "hwcfg.h"

typedef struct {
    const char *name;
    esp_err_t (*init)(void);            // поднять шину и чип
    bool      (*is_alive)(void);        // чип отвечает
    esp_err_t (*configure)(int speed);  // полная настройка на пресете
    esp_err_t (*send)(const uint8_t *data, size_t len);   // блокирующий TX
    // Блокирующий опрос RX: вернёт число байт, 0 по таймауту, <0 при ошибке
    int       (*receive)(uint8_t *buf, size_t buf_size,
                         int *rssi_out, float *snr_out, int timeout_ms);
    esp_err_t (*set_speed)(int speed);
    esp_err_t (*set_tx_power)(uint8_t pwr_idx);  // 0=макс … 3=мин (legacy idx)
    // Listen-before-talk: true, если канал сейчас занят.
    // NULL у радио, которое не умеет слушать эфир (E220 в прозрачном режиме).
    bool      (*channel_busy)(void);
    // Состояние чипа одной строкой в лог: режим, частота и настройки
    // модема ЧИТАЮТСЯ ОБРАТНО ИЗ РЕГИСТРОВ, а не берутся из hwcfg.
    // «Приёмник молчит» — симптом сразу трёх разных болезней (чип выпал
    // из RX, разъехались частоты, разъехался SF), и различить их можно
    // только так. NULL — если драйвер этого не умеет.
    void      (*log_state)(void);
} radio_ops_t;

// ============================================================
// FSK
//
// Звонки и быстрая передача картинок идут не LoRa-модуляцией, а
// GFSK: он даёт нужный битрейт на коротком плече. Умеет это не
// каждое радио (E220 в прозрачном режиме — нет), и от этого
// напрямую зависит, доступны ли пользователю звонки по эфиру:
// если fsk_ops() вернул NULL, звонок возможен только через веб.
// ============================================================
#define FSK_PROFILE_LONGRANGE 0   // 2.4к — Codec2
#define FSK_PROFILE_STANDARD  1   // 19.2к — AMR-NB
#define FSK_PROFILE_HD        2   // 50к — AMR-WB

typedef struct {
    const char *name;
    esp_err_t (*enter)(int profile);   // LoRa → FSK
    esp_err_t (*exit)(void);           // обратно (нужен radio->configure)
    esp_err_t (*send)(const uint8_t *data, uint8_t len);   // ≤60 байт
    int       (*read)(uint8_t *buf, uint8_t buf_size);     // 0 = пусто
} fsk_ops_t;

// ============================================================
// Реестр
// ============================================================
// Выбирает драйверы по hwcfg(). Звать до lora_manager_init().
esp_err_t radio_registry_init(void);

// Активное радио. После radio_registry_init() никогда не NULL:
// если железо неизвестно, подставляется «пустое» радио, которое
// честно ничего не передаёт — устройство остаётся живым по BLE
// и вебу, а пользователь видит ошибку, а не немой кирпич.
extern const radio_ops_t *radio;

// Активный FSK-бэкенд или NULL, если это радио так не умеет.
const fsk_ops_t *fsk_ops(void);
static inline bool fsk_available(void) { return fsk_ops() != NULL; }

// Бэкенды (компилируются всегда, выбираются в рантайме).
extern const radio_ops_t radio_ops_e220;
extern const radio_ops_t radio_ops_sx127x;
extern const radio_ops_t radio_ops_sx126x;
extern const radio_ops_t radio_ops_sx128x;   // 2.4 ГГц — заглушка
extern const radio_ops_t radio_ops_halow;    // 802.11ah — заглушка
extern const radio_ops_t radio_ops_null;

extern const fsk_ops_t   fsk_ops_sx127x;
