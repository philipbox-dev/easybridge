#pragma once
// ============================================================
// Автономные устройства на LoRa (V2.9)
//
// Метеостанция на подоконнике, датчик дождя, реле в рюкзаке — узлы,
// к которым не подключён телефон. Они сами рассказывают о себе и
// шлют показания, а любой, кто их слышит, может нарисовать виджет и
// отправить команду. Ставится это одной прошивкой: роль и набор
// датчиков задаются JSON-конфигом в NVS, паять и пересобирать не надо.
//
// Устройство самоописываемое: манифест несёт имена полей, единицы и
// список команд. Приёмнику не нужно заранее знать, что это за железка
// — виджет строится из манифеста.
//
// Чего здесь сознательно НЕТ: исполняемых скриптов в эфире. Пакет от
// неаутентифицированного узла, который выполняется у всех, кто его
// услышал, — это удалённое исполнение кода по своей природе. Внешний
// вид виджета настраивается на сервере и раздаётся по вебу, где байты
// бесплатны, а доверие проверяемо (см. docs/DEVICES.md).
// ============================================================
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define DEVNODE_MANIFEST_VER   1
#define DEVNODE_MAX_FIELDS     8
#define DEVNODE_MAX_CMDS       4
#define DEVNODE_NAME_LEN       20

// Класс устройства — подсказка для интерфейса, не более.
#define DEV_CLASS_SENSOR    0
#define DEV_CLASS_ACTUATOR  1
#define DEV_CLASS_COMBO     2

#define DEV_FLAG_CMDS       (1 << 0)   // принимает команды
#define DEV_FLAG_BATTERY    (1 << 1)   // на батарее (беречь эфир)
#define DEV_FLAG_RETAINED   (1 << 2)   // показания имеют смысл и «на потом»

// Тип значения. Едет в каждом показании, поэтому виджет собирается
// даже у того, кто пропустил манифест.
#define DEVF_INT16      0
#define DEVF_UINT16     1
#define DEVF_INT32      2
#define DEVF_BOOL       3
#define DEVF_PERCENT    4

// Единицы измерения — таблицей, а не строкой: строка «°C» съела бы
// в эфире больше, чем само значение.
#define DEVU_NONE   0
#define DEVU_CELSIUS 1
#define DEVU_PERCENT 2
#define DEVU_HPA    3
#define DEVU_MM     4
#define DEVU_VOLT   5
#define DEVU_AMP    6
#define DEVU_LUX    7
#define DEVU_MPS    8
#define DEVU_PPM    9
#define DEVU_METER  10
#define DEVU_COUNT  11
#define DEVU_SECOND 12

// Откуда брать значение
#define DEVSRC_NONE     0
#define DEVSRC_ADC      1   // аналоговый вход (pin = канал ADC1)
#define DEVSRC_GPIO_IN  2   // цифровой вход
#define DEVSRC_COUNTER  3   // счётчик импульсов на входе (дождемер, анемометр)
#define DEVSRC_UPTIME   4
#define DEVSRC_BATTERY  5
#define DEVSRC_CHIP_TEMP 6
#define DEVSRC_RSSI     7   // как меня слышно — полезно при установке

// Что делает команда
#define DEVACT_NONE      0
#define DEVACT_GPIO_SET  1   // выставить уровень (arg: 0/1)
#define DEVACT_GPIO_PULSE 2  // импульс arg миллисекунд
#define DEVACT_REBOOT    3
#define DEVACT_REPORT    4   // прислать показания немедленно

typedef struct {
    uint8_t src;                    // DEVSRC_*
    uint8_t type;                   // DEVF_*
    uint8_t unit;                   // DEVU_*
    int8_t  scale;                  // значение × 10^scale (−2 → сотые)
    int8_t  pin;                    // канал/пин источника
    int32_t mul, div_;              // линейная поправка: v = raw * mul / div_
    char    name[DEVNODE_NAME_LEN];
} devnode_field_t;

typedef struct {
    uint8_t id;                     // код команды в эфире
    uint8_t action;                 // DEVACT_*
    int8_t  pin;
    uint8_t active_low;
    uint16_t default_arg;
    char    name[DEVNODE_NAME_LEN];
} devnode_cmd_t;

typedef struct {
    bool     enabled;               // роль «автономное устройство»
    uint8_t  dev_class;
    uint8_t  flags;
    uint16_t interval_s;            // как часто слать показания
    uint16_t manifest_every;        // раз в N показаний повторять манифест
    char     name[DEVNODE_NAME_LEN];
    uint8_t  n_fields, n_cmds;
    devnode_field_t fields[DEVNODE_MAX_FIELDS];
    devnode_cmd_t   cmds[DEVNODE_MAX_CMDS];
} devnode_cfg_t;

// Поднять подсистему. Если роль выключена, узел всё равно умеет
// принимать чужие DEV_*-пакеты и отдавать их телефону.
void devnode_init(void);
void devnode_note_rssi(int rssi);
bool devnode_enabled(void);
const devnode_cfg_t *devnode_cfg(void);

// Конфигурация из JSON (BLE-команда devcfg / веб-флешер).
esp_err_t devnode_apply_json(const char *json, char *err, size_t err_sz);
int       devnode_to_json(char *out, size_t out_sz);
esp_err_t devnode_reset(void);

// Отправить манифест/показания прямо сейчас.
void devnode_send_manifest(void);
void devnode_send_data(void);

// Приём. Возвращают true, если пакет разобран.
bool devnode_on_cmd(uint32_t src, const uint8_t *payload, uint8_t len);

// Разбор чужих пакетов в JSON для телефона (на обычном узле).
int devnode_manifest_to_json(uint32_t src, const uint8_t *payload, uint8_t len,
                             char *out, size_t out_sz);
int devnode_data_to_json(uint32_t src, int rssi, const uint8_t *payload,
                         uint8_t len, char *out, size_t out_sz);
int devnode_ack_to_json(uint32_t src, const uint8_t *payload, uint8_t len,
                        char *out, size_t out_sz);

const char *devnode_unit_name(uint8_t unit);
