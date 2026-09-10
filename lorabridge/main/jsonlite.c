#include "jsonlite.h"
#include <string.h>
#include <stdlib.h>

const char *jl_skip_ws(const char *p, const char *e)
{
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

const char *jl_skip_string(const char *p, const char *e)
{
    if (p >= e || *p != '"') return NULL;
    p++;
    while (p < e) {
        if (*p == '\\') { p += 2; continue; }
        if (*p == '"')  return p + 1;
        p++;
    }
    return NULL;
}

const char *jl_skip_value(const char *p, const char *e)
{
    p = jl_skip_ws(p, e);
    if (p >= e) return NULL;
    if (*p == '"') return jl_skip_string(p, e);
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (p < e) {
            if (*p == '"') { p = jl_skip_string(p, e); if (!p) return NULL; continue; }
            if (*p == '{' || *p == '[') depth++;
            else if (*p == '}' || *p == ']') { depth--; if (depth == 0) return p + 1; }
            p++;
        }
        return NULL;
    }
    while (p < e && *p != ',' && *p != '}' && *p != ']') p++;
    return p;
}

// Значение члена `key` объекта, начинающегося в obj. NULL, если нет.
const char *jl_member(const char *obj, const char *e, const char *key)
{
    if (!obj) return NULL;
    const char *p = jl_skip_ws(obj, e);
    if (p >= e || *p != '{') return NULL;
    p++;
    size_t klen = strlen(key);
    for (;;) {
        p = jl_skip_ws(p, e);
        if (p >= e || *p == '}') return NULL;
        const char *ks = p;
        const char *ke = jl_skip_string(p, e);
        if (!ke) return NULL;
        bool match = ((size_t)(ke - ks - 2) == klen) && memcmp(ks + 1, key, klen) == 0;
        p = jl_skip_ws(ke, e);
        if (p >= e || *p != ':') return NULL;
        p++;
        p = jl_skip_ws(p, e);
        if (match) return p;
        p = jl_skip_value(p, e);
        if (!p) return NULL;
        p = jl_skip_ws(p, e);
        if (p < e && *p == ',') p++;
    }
}

bool jl_int(const char *obj, const char *e, const char *key, long *out)
{
    const char *v = jl_member(obj, e, key);
    if (!v || v >= e) return false;
    if (*v == 't') { *out = 1; return true; }   // true
    if (*v == 'f') { *out = 0; return true; }   // false
    if (*v != '-' && (*v < '0' || *v > '9')) return false;
    *out = strtol(v, NULL, 10);
    return true;
}

bool jl_u32(const char *obj, const char *e, const char *key, uint32_t *out)
{
    const char *v = jl_member(obj, e, key);
    if (!v || v >= e) return false;
    if (*v < '0' || *v > '9') return false;
    *out = (uint32_t)strtoul(v, NULL, 10);
    return true;
}

bool jl_str(const char *obj, const char *e, const char *key, char *buf, size_t sz)
{
    const char *v = jl_member(obj, e, key);
    if (!v || v >= e || *v != '"') return false;
    v++;
    size_t i = 0;
    while (v < e && *v != '"' && i + 1 < sz) {
        if (*v == '\\' && v + 1 < e) v++;
        buf[i++] = *v++;
    }
    buf[i] = '\0';
    return true;
}

// Поле-вид: принимаем и число, и человеческое имя ("SX127x", "ST7789-SPI").
// Имена регистронезависимы: набирать их будет человек.
bool jl_kind(const char *obj, const char *e, const char *key,
                   const char *const *names, int count, uint8_t *out)
{
    long n;
    if (jl_int(obj, e, key, &n)) {
        if (n < 0 || n >= count) return false;
        *out = (uint8_t)n;
        return true;
    }
    char s[24];
    if (!jl_str(obj, e, key, s, sizeof(s))) return false;
    for (int i = 0; i < count; i++) {
        if (names[i] && strcasecmp(names[i], s) == 0) { *out = (uint8_t)i; return true; }
    }
    return false;
}

void jl_pin(const char *obj, const char *e, const char *key, int8_t *dst)
{
    long v;
    if (jl_int(obj, e, key, &v) && v >= -1 && v <= 60) *dst = (int8_t)v;
}

void jl_u8(const char *obj, const char *e, const char *key, uint8_t *dst)
{
    long v;
    if (jl_int(obj, e, key, &v) && v >= 0 && v <= 255) *dst = (uint8_t)v;
}

void jl_u16(const char *obj, const char *e, const char *key, uint16_t *dst)
{
    long v;
    if (jl_int(obj, e, key, &v) && v >= 0 && v <= 65535) *dst = (uint16_t)v;
}

