#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    SCREEN_SPLASH,
    SCREEN_IDLE,
    SCREEN_RX_MSG,
    SCREEN_TX_SENDING,
    SCREEN_TX_OK,
    SCREEN_TX_FAIL,
    SCREEN_ERROR,
    SCREEN_PTT,       // V2.9: экран FSK-звонка
} display_screen_t;

void display_init(void);

void display_show_splash(void);
void display_show_idle(const char *my_name, const char *peer_name,
                       bool peer_online, int rssi, int sf,
                       int msg_sent, int msg_recv);
void display_show_rx_message(const char *from, int rssi, const char *text);
void display_show_tx_sending(void);
void display_show_tx_ok(void);
void display_show_tx_fail(void);
void display_show_error(const char *title, const char *desc);

void display_on_peer_status(const char *peer_name, bool online, int rssi);
void display_on_msg_received(const char *from, int rssi, const char *text);
void display_on_msg_sending(void);
void display_on_msg_sent(void);
void display_on_msg_failed(void);
void display_update_idle(const char *my_name, const char *peer_name,
                         bool peer_online, int rssi, int sf,
                         int msg_sent, int msg_recv);

// ── PTT-звонок (V2.9) ────────────────────────────────────────
void display_ptt_show(const char *mode_name);  // вход в звонок
void display_ptt_activity(int dir);            // 0=эфир, 1=передача, 2=приём
void display_ptt_end(void);                    // назад в idle