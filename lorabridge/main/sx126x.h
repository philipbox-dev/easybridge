#pragma once
// ============================================================
// SX126x (SX1262/SX1268/LLCC68) minimal LoRa driver
// Target module: EBYTE E22-400M33S (SX1268 + 2W PA, TXEN/RXEN)
// Command-based SPI interface — see SX1268 datasheet rev 1.1
// ⚠ UNTESTED ON HARDWARE — written for the E22-400M33S bring-up
// ============================================================
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

// ── Opcodes ──────────────────────────────────────────────────
#define SX126X_SET_SLEEP            0x84
#define SX126X_SET_STANDBY          0x80
#define SX126X_SET_TX               0x83
#define SX126X_SET_RX               0x82
#define SX126X_SET_CAD              0xC5
#define SX126X_SET_PACKET_TYPE      0x8A
#define SX126X_SET_RF_FREQUENCY     0x86
#define SX126X_SET_PA_CONFIG        0x95
#define SX126X_SET_TX_PARAMS        0x8E
#define SX126X_SET_MODULATION       0x8B
#define SX126X_SET_PACKET_PARAMS    0x8C
#define SX126X_SET_CAD_PARAMS       0x88
#define SX126X_SET_BUF_BASE         0x8F
#define SX126X_WRITE_BUFFER         0x0E
#define SX126X_READ_BUFFER          0x1E
#define SX126X_WRITE_REGISTER       0x0D
#define SX126X_READ_REGISTER        0x1D
#define SX126X_GET_IRQ_STATUS       0x12
#define SX126X_CLR_IRQ_STATUS       0x02
#define SX126X_SET_DIO_IRQ_PARAMS   0x08
#define SX126X_GET_RX_BUF_STATUS    0x13
#define SX126X_GET_PACKET_STATUS    0x14
#define SX126X_GET_STATUS           0xC0
#define SX126X_SET_REGULATOR_MODE   0x96
#define SX126X_CALIBRATE            0x89
#define SX126X_SET_DIO2_AS_RF_SW    0x9D
#define SX126X_SET_DIO3_AS_TCXO     0x97

// ── Registers ────────────────────────────────────────────────
#define SX126X_REG_SYNC_WORD_MSB    0x0740
#define SX126X_REG_SYNC_WORD_LSB    0x0741
#define SX126X_REG_OCP              0x08E7

// ── IRQ bits ─────────────────────────────────────────────────
#define SX126X_IRQ_TX_DONE          (1 << 0)
#define SX126X_IRQ_RX_DONE          (1 << 1)
#define SX126X_IRQ_CRC_ERR          (1 << 6)
#define SX126X_IRQ_CAD_DONE         (1 << 7)
#define SX126X_IRQ_CAD_DETECTED     (1 << 8)
#define SX126X_IRQ_TIMEOUT          (1 << 9)

// ── API (mirrors sx1278.h so the HAL adapters stay symmetric) ─
esp_err_t sx126x_init(void);
bool      sx126x_is_alive(void);
esp_err_t sx126x_configure(uint32_t freq_hz, int sf, int bw_khz, int cr, int tx_dbm);
esp_err_t sx126x_send(const uint8_t *data, size_t len);
esp_err_t sx126x_start_receive(void);
int       sx126x_read_packet(uint8_t *buf, size_t buf_size);
int       sx126x_get_rssi(void);
float     sx126x_get_snr(void);
bool      sx126x_cad_detect(void);
