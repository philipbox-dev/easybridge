#pragma once
// ============================================================
// jsonlite — минимальный разбор JSON (V2.9)
//
// Вынесено из hwcfg.c, когда парсер понадобился второму модулю
// (devnode). Тащить cJSON ради двух команд не хочется, а держать две
// копии разбора — верный способ однажды получить конфиг, который
// применяется по-разному в зависимости от того, кто его прочитал.
//
// Парсер толерантный: неизвестные ключи игнорируются, отсутствующие
// поля оставляют значение по умолчанию. Это осознанно — приложение
// шлёт патч («поменяй один пин»), а не всю простыню целиком.
// ============================================================
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

const char *jl_skip_ws(const char *p, const char *e);
const char *jl_skip_string(const char *p, const char *e);
const char *jl_skip_value(const char *p, const char *e);

// Значение члена `key` объекта, начинающегося в obj. NULL, если нет.
const char *jl_member(const char *obj, const char *e, const char *key);

bool jl_int(const char *obj, const char *e, const char *key, long *out);
bool jl_u32(const char *obj, const char *e, const char *key, uint32_t *out);
bool jl_str(const char *obj, const char *e, const char *key, char *buf, size_t sz);

// Поле-вид: принимает и число, и человеческое имя (регистронезависимо).
bool jl_kind(const char *obj, const char *e, const char *key,
             const char *const *names, int count, uint8_t *out);

void jl_pin(const char *obj, const char *e, const char *key, int8_t *dst);
void jl_u8 (const char *obj, const char *e, const char *key, uint8_t *dst);
void jl_u16(const char *obj, const char *e, const char *key, uint16_t *dst);
