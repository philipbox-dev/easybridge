#pragma once
// ============================================================
// PTT — полудуплексные FSK-«звонки» (V2.7, только SX127x-платы).
// Захват канала: PKT25_PTT_START по LoRa → обе стороны в GFSK →
// стрим AMR-кадров с телефона → PTT_END/watchdog → назад в LoRa.
// На платах без FSK все функции — no-op (ptt_supported() = false).
// ============================================================
#include <stdint.h>
#include <stdbool.h>

void ptt_init(void);
bool ptt_supported(void);
bool ptt_active(void);

// initiator=true: мы начали звонок (шлём PTT_START по LoRa)
// mode: FSK_PROFILE_* (0=дальнобой, 1=стандарт, 2=HD) — задаёт звонящий
void ptt_begin(bool initiator, uint32_t peer_node_id, int mode);

// V2.9: анонс «у этого звонка есть веб-плечо».
// Телефон-шлюз зовёт это сразу после ptt_begin(initiator=true), передав
// список веб-участников: те, у кого нет интернета, увидят, кто с ними
// говорит, хотя этих людей нет в эфире как узлов.
void ptt_announce_web(const char *names, uint8_t count);
void ptt_end(bool local);                       // local=true: мы вешаем трубку
void ptt_queue_audio(const uint8_t *data, int len);  // AMR-кадры с телефона

// V2.9.2: вызываемый принял звонок — шлём FSK-маркер ACCEPT звонящему,
// чтобы его телефон показал «соединено», а не «ожидание ответа».
void ptt_accept_call(void);
