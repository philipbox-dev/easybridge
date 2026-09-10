#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

// ============================================================
// Регистры SX1278
// ============================================================
#define REG_FIFO                 0x00
#define REG_OP_MODE              0x01
#define REG_FRF_MSB              0x06
#define REG_FRF_MID              0x07
#define REG_FRF_LSB              0x08
#define REG_PA_CONFIG            0x09
#define REG_PA_DAC               0x4D
#define REG_LNA                  0x0C
#define REG_FIFO_ADDR_PTR        0x0D
#define REG_FIFO_TX_BASE_ADDR    0x0E
#define REG_FIFO_RX_BASE_ADDR    0x0F
#define REG_FIFO_RX_CURRENT_ADDR 0x10
#define REG_IRQ_FLAGS_MASK       0x11
#define REG_IRQ_FLAGS            0x12
#define REG_RX_NB_BYTES          0x13
#define REG_PKT_SNR_VALUE        0x19
#define REG_PKT_RSSI_VALUE       0x1A
#define REG_RSSI_VALUE           0x1B
#define REG_MODEM_CONFIG_1       0x1D
#define REG_MODEM_CONFIG_2       0x1E
#define REG_PREAMBLE_MSB         0x20
#define REG_PREAMBLE_LSB         0x21
#define REG_PAYLOAD_LENGTH       0x22
#define REG_MODEM_CONFIG_3       0x26
#define REG_DETECTION_OPTIMIZE   0x31
#define REG_DETECTION_THRESHOLD  0x37
#define REG_SYNC_WORD            0x39
#define REG_DIO_MAPPING_1        0x40
#define REG_VERSION              0x42

// ============================================================
// Режимы работы
// ============================================================
#define MODE_LONG_RANGE  0x80
#define MODE_SLEEP       0x00
#define MODE_STDBY       0x01
#define MODE_TX          0x03
#define MODE_RX_CONT     0x05
#define MODE_RX_SINGLE   0x06
#define MODE_CAD         0x07

// ============================================================
// IRQ флаги
// ============================================================
#define IRQ_RX_DONE           0x40
#define IRQ_TX_DONE           0x08
#define IRQ_PAYLOAD_CRC_ERROR 0x20
#define IRQ_CAD_DONE          0x04
#define IRQ_CAD_DETECTED      0x01

// ============================================================
// API
// ============================================================
esp_err_t sx1278_init(void);
bool      sx1278_is_alive(void);
esp_err_t sx1278_configure(uint32_t freq, int sf, int bw, int cr, int tx_power);
esp_err_t sx1278_send(const uint8_t *data, size_t len);
esp_err_t sx1278_start_receive(void);

// Состояние чипа в лог: режим, реальная частота из FRF, SF/BW/CR из
// ModemConfig, флаги IRQ. Всё читается из регистров.
void      sx1278_log_state(void);
int       sx1278_read_packet(uint8_t *buf, size_t buf_size);
int       sx1278_get_rssi(void);
float     sx1278_get_snr(void);
void      sx1278_reset(void);
uint8_t   sx1278_read_reg(uint8_t addr);

// LBT
int       sx1278_get_current_rssi(void);
bool      sx1278_is_channel_free(int threshold_dbm);
bool      sx1278_cad_detect(void);

// ── FSK-режим (PTT-звонки, V2.7) ─────────────────────────────
// GFSK 19.2 кбит/с, dev 25 кГц, пакеты ≤60 байт (FIFO 64).
// enter переводит чип LoRa→FSK, exit — обратно (нужен reconfigure
// через radio_ops.configure после выхода).
// Профили: 0=дальнобой 2.4к (Codec2), 1=стандарт 19.2к (AMR-NB),
//           2=HD 50к (AMR-WB)
#define FSK_PROFILE_LONGRANGE 0
#define FSK_PROFILE_STANDARD  1
#define FSK_PROFILE_HD        2
esp_err_t sx1278_fsk_enter(int profile);
esp_err_t sx1278_fsk_exit(void);
esp_err_t sx1278_fsk_send(const uint8_t *data, uint8_t len);   // ≤60
int       sx1278_fsk_read(uint8_t *buf, uint8_t buf_size);     // 0 = пусто
