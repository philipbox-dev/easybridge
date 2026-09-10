#pragma once
// ============================================================
// IMG-FSK — передача маленьких картинок (JPEG) по GFSK, V2.9.3.
// Тот же радиотракт, что у PTT-звонков (только SX127x-платы), но
// пакетный best-effort блок с избыточностью (несколько проходов),
// без реалтайма и без ACK. Захват канала: PKT25_IMG_START по LoRa →
// обе стороны в GFSK → блб кадров картинки → done-маркер → назад в LoRa.
// На платах без FSK всё — no-op (imgfsk_supported() = false).
// ============================================================
#include <stdint.h>
#include <stdbool.h>

// Лимит: держим кадр FSK ≤60 Б → полезная нагрузка 57 Б.
#define IMG_CHUNK        57
#define IMG_MAX_FRAMES   200               // 200×57 ≈ 11.4 КБ
#define IMG_MAX_BYTES    (IMG_CHUNK * IMG_MAX_FRAMES)

void imgfsk_init(void);
bool imgfsk_supported(void);
bool imgfsk_active(void);

// Приём: устройство собрало картинку от телефона (BLE-чанки img_tx) и
// готово блнуть её в эфир. profile: FSK_PROFILE_* (0/1/2). dst — адресат
// или NODE_BROADCAST.
void imgfsk_send(const uint8_t *data, uint16_t len, uint32_t dst, int profile);

// Вызывается из bridge при приёме PKT25_IMG_START — поднять FSK-приём.
void imgfsk_on_start(uint32_t src, int profile, uint8_t frame_total);
