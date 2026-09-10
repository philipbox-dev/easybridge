#!/usr/bin/env python3
"""Извлекает таблицу профилей железа из main/hwcfg.c в JSON.

Зачем: таблица пинов на странице прошивки должна показывать ровно то,
что зашито в прошивку. Держать её второй копией в HTML — гарантированный
способ однажды показать человеку пины от другой платы и получить
«почему-то не работает». Поэтому единственный источник правды —
`k_profiles[]` в hwcfg.c, а этот скрипт её оттуда достаёт.

    python3 tools/gen_profiles.py            # → tools/flasher/web/profiles.json
    python3 tools/gen_profiles.py --check    # только проверить, что парсится

Парсер понимает ровно тот диалект, которым написана таблица:
designated initializers, вложенные скобки и макросы RP_UNSET / DP_UNSET /
TP_UNSET / NO_BTN / OLED_I2C. Если таблица начнёт использовать что-то
ещё, скрипт упадёт с внятной ошибкой, а не соврёт молча.
"""

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HWCFG_C = ROOT / "main" / "hwcfg.c"
HWCFG_H = ROOT / "main" / "hwcfg.h"
BOARD_H = ROOT / "main" / "boards" / "board.h"
OUT = ROOT / "tools" / "flasher" / "web" / "profiles.json"


class ParseError(RuntimeError):
    pass


# ── Разбор перечислений ─────────────────────────────────────
def parse_enums(text):
    """{'RADIO_E220': 1, 'BAND_868': 2, ...} из всех enum в файле.

    Значения нужны не сами по себе, а чтобы обратно превратить их в
    имена: в JSON едут человеческие «SX127x» и «868», а не 2 и 2.
    """
    out = {}
    # Комментарии снимаем до разбиения по запятым: запятая внутри
    # «// EBYTE E220-xxxTxxD, UART» иначе разрезала бы элемент пополам
    # и половина имён просто не попала бы в таблицу.
    text = strip_comments(text)
    for body in re.findall(r"typedef\s+enum\s*\{(.*?)\}\s*\w+\s*;", text, re.S):
        value = 0
        for item in body.split(","):
            item = item.strip()
            if not item:
                continue
            m = re.match(r"^([A-Za-z_]\w*)\s*(?:=\s*(\w+))?$", item)
            if not m:
                continue
            if m.group(2) is not None:
                value = int(m.group(2), 0)
            out[m.group(1)] = value
            value += 1
    return out


def parse_defines(text):
    out = {}
    for name, val in re.findall(r"^\s*#define\s+(\w+)\s+(\d+)\s*$", text, re.M):
        out[name] = int(val)
    return out


# ── Разбор инициализаторов ──────────────────────────────────
def extract_table(text):
    marker = "static const hwcfg_t k_profiles[] = {"
    start = text.find(marker)
    if start < 0:
        raise ParseError("не нашёл 'static const hwcfg_t k_profiles[] = {' в hwcfg.c")
    i = start + len(marker) - 1          # на открывающей скобке
    depth, j = 0, i
    while j < len(text):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                return text[i + 1:j]
        j += 1
    raise ParseError("таблица профилей не закрыта скобкой")


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def expand_macros(text, source):
    """Раскрывает макросы-сокращения из hwcfg.c.

    Их определения берутся из самого файла, а не дублируются здесь:
    иначе добавленный в RP_UNSET пин пришлось бы править в двух местах.
    """
    m = re.search(r"^#define\s+OLED_I2C\(([^)]*)\)\s+((?:.*\\\n)*.*)$", source, re.M)
    if not m:
        raise ParseError("в hwcfg.c нет макроса OLED_I2C")
    params = [p.strip() for p in m.group(1).split(",")]
    body = m.group(2).replace("\\\n", " ")

    def sub_oled(match):
        args = [a.strip() for a in match.group(1).split(",")]
        result = body
        for p, a in zip(params, args):
            result = re.sub(r"\b%s\b" % re.escape(p), a, result)
        return result

    text = re.sub(r"\bOLED_I2C\(([^)]*)\)", sub_oled, text)

    # *_UNSET разворачиваются ПОСЛЕ OLED_I2C: его тело само содержит
    # DP_UNSET, и в обратном порядке он бы остался неразвёрнутым.
    for name in ("RP_UNSET", "DP_UNSET", "TP_UNSET", "NO_BTN"):
        m = re.search(r"^#define\s+%s\s+((?:.*\\\n)*.*)$" % name, source, re.M)
        if not m:
            raise ParseError("в hwcfg.c нет макроса %s" % name)
        body = m.group(1).replace("\\\n", " ")
        text = re.sub(r"\b%s\b" % name, body, text)

    return text


def split_top_level(text):
    """Режет по запятым верхнего уровня, уважая скобки и строки."""
    parts, depth, buf, in_str = [], 0, [], False
    i = 0
    while i < len(text):
        ch = text[i]
        if in_str:
            buf.append(ch)
            if ch == "\\" and i + 1 < len(text):
                buf.append(text[i + 1])
                i += 2
                continue
            if ch == '"':
                in_str = False
        elif ch == '"':
            in_str = True
            buf.append(ch)
        elif ch in "{[(":
            depth += 1
            buf.append(ch)
        elif ch in "}])":
            depth -= 1
            buf.append(ch)
        elif ch == "," and depth == 0:
            parts.append("".join(buf).strip())
            buf = []
        else:
            buf.append(ch)
        i += 1
    tail = "".join(buf).strip()
    if tail:
        parts.append(tail)
    return parts


def parse_value(text, consts):
    text = text.strip()
    if text.startswith("{"):
        if not text.endswith("}"):
            raise ParseError("незакрытая скобка в %r" % text[:60])
        inner = text[1:-1]
        # Массив от структуры отличаем по первому элементу: у структуры
        # он назначенный (.field = ...), у массива — просто значение.
        first = next((i for i in split_top_level(inner) if i.strip()), "")
        if first and not first.lstrip().startswith("."):
            return parse_array(inner, consts)
        return parse_object(inner, consts)
    if text.startswith('"'):
        # Склейка соседних строковых литералов, как в C.
        return "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', text))
    return eval_const(text, consts)


def eval_const(text, consts):
    """Числа, имена констант и простые выражения вроде -1 или 1 << 3."""
    expr = text.strip()
    if expr in consts:
        return consts[expr]
    resolved = re.sub(r"\b[A-Za-z_]\w*\b",
                      lambda m: str(consts.get(m.group(0), m.group(0))), expr)
    if re.fullmatch(r"[-+*/() \t0-9x<>a-fA-FuU]+", resolved):
        try:
            return int(eval(resolved.replace("u", "").replace("U", ""), {}, {}))
        except Exception:
            pass
    raise ParseError("не могу вычислить значение %r" % text)


def parse_object(body, consts):
    """`.a = 1, .b = { .c = 2 }` → {'a': 1, 'b': {'c': 2}}.

    Поздний designator перекрывает ранний — ровно как в C99, на этом
    и держится приём «сначала RP_UNSET, потом реальные пины».
    """
    obj = {}
    for item in split_top_level(body):
        if not item:
            continue
        m = re.match(r"^\.\s*(\w+)\s*=\s*(.*)$", item, re.S)
        if not m:
            raise ParseError("ожидался '.поле = значение', получил %r" % item[:60])
        obj[m.group(1)] = parse_value(m.group(2), consts)
    return obj


def parse_array(body, consts):
    return [parse_value(item, consts)
            for item in split_top_level(body) if item.strip()]


# ── Приведение к формату страницы ───────────────────────────
# Как подписывать пины в таблице и в каком порядке их показывать.
RADIO_PIN_LABELS = [
    ("sck", "SCK"), ("miso", "MISO"), ("mosi", "MOSI"), ("cs", "NSS (CS)"),
    ("rst", "RST"), ("busy", "BUSY"), ("dio0", "DIO0"), ("dio1", "DIO1"),
    ("dio2", "DIO2"), ("txen", "TXEN"), ("rxen", "RXEN"),
    ("tx", "RXD модуля ← ESP TX"), ("rx", "TXD модуля → ESP RX"),
    ("aux", "AUX"), ("m0", "M0"), ("m1", "M1"),
]
DISPLAY_PIN_LABELS = [
    ("sda", "SDA"), ("scl", "SCL"), ("sck", "SCK"), ("mosi", "MOSI"),
    ("miso", "MISO"), ("cs", "CS"), ("dc", "DC"), ("rst", "RST"), ("bl", "BL"),
]
TOUCH_PIN_LABELS = [
    ("sck", "SCK"), ("mosi", "MOSI"), ("miso", "MISO"), ("cs", "CS"),
    ("sda", "SDA"), ("scl", "SCL"), ("irq", "IRQ"), ("rst", "RST"),
]


def pin_rows(pins, labels):
    return [{"label": label, "gpio": pins[key]}
            for key, label in labels
            if isinstance(pins.get(key), int) and pins[key] >= 0]


# Под какой чип собран профиль. В самой структуре hwcfg этого нет
# (там лежит модель чипа, на котором конфиг записан, а не для
# которого он задуман), поэтому связь задаётся здесь — явной
# таблицей, а не угадыванием: неизвестный префикс уронит генератор,
# и про новый профиль не забудут.
CHIP_BY_PREFIX = {
    "s3_": {"target": "esp32s3", "family": "ESP32-S3"},
    "c6_": {"target": "esp32c6", "family": "ESP32-C6"},
    "t3_": {"target": "esp32",   "family": "ESP32"},
}

# У каких профилей есть готовый merged-образ (см. build_images.sh).
# Остальные — рантайм-конфигурация поверх образа для того же чипа.
PREBUILT = {"s3_e220", "c6_e220", "t3_v161", "s3_e22m33s"}


def chip_for(profile_id):
    for prefix, info in CHIP_BY_PREFIX.items():
        if profile_id.startswith(prefix):
            return info
    raise ParseError(
        "не знаю, под какой чип профиль '%s' — добавь префикс в "
        "CHIP_BY_PREFIX в tools/gen_profiles.py" % profile_id)


def name_of(enum_names, value, prefix):
    for name, val in enum_names.items():
        if val == value and name.startswith(prefix):
            return name
    return str(value)


def build(profile, names):
    radio = profile.get("radio", {})
    radio2 = profile.get("radio2", {})
    disp = profile.get("disp", {})
    touch = profile.get("touch", {})
    batt = profile.get("batt", {})

    def radio_json(r):
        if not r or not r.get("kind"):
            return None
        return {
            "chip": names["radio"].get(r.get("kind", 0), "?"),
            "band": names["band"].get(r.get("band", 0), "?"),
            "freq_hz": r.get("freq_hz", 0),
            "max_dbm": r.get("max_dbm", 0),
            "bus": "UART" if names["radio"].get(r.get("kind")) == "E220" else "SPI",
            "fsk": bool(r.get("supports_fsk", 0)),
            "pins": pin_rows(r.get("pins", {}), RADIO_PIN_LABELS),
        }

    pid = profile.get("profile", "?")
    chip = chip_for(pid)
    out = {
        "id": pid,
        "target": chip["target"],
        "family": chip["family"],
        "prebuilt": pid in PREBUILT,
        "name": profile.get("name", "?"),
        "hw_id": profile.get("hw_id", 0),
        "radio": radio_json(radio),
        "radio2": radio_json(radio2),
        "display": None,
        "touch": None,
        "battery": bool(batt.get("present", 0)),
        "led": profile.get("led_pin", -1),
    }
    if disp.get("kind"):
        out["display"] = {
            "kind": names["panel"].get(disp["kind"], "?"),
            "size": "%dx%d" % (disp.get("width", 0), disp.get("height", 0)),
            "bus": "I2C" if disp.get("sda", -1) >= 0 else "SPI",
            "i2c_addr": disp.get("i2c_addr", 0),
            "pins": pin_rows(disp, DISPLAY_PIN_LABELS),
        }
    if touch.get("kind"):
        out["touch"] = {
            "kind": names["touch"].get(touch["kind"], "?"),
            "pins": pin_rows(touch, TOUCH_PIN_LABELS),
        }
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true",
                    help="только разобрать и отчитаться, ничего не писать")
    ap.add_argument("-o", "--out", type=Path, default=OUT)
    args = ap.parse_args()

    src_c = HWCFG_C.read_text(encoding="utf-8")
    consts = {}
    consts.update(parse_enums(HWCFG_H.read_text(encoding="utf-8")))
    consts.update(parse_defines(HWCFG_H.read_text(encoding="utf-8")))
    consts.update(parse_defines(BOARD_H.read_text(encoding="utf-8")))

    body = strip_comments(extract_table(strip_comments(src_c)))
    body = expand_macros(body, src_c)
    profiles = [parse_object(item[1:-1].strip(), consts)
                for item in split_top_level(body)
                if item.strip().startswith("{")]

    if not profiles:
        raise ParseError("таблица профилей пуста")

    names = {
        "radio": {v: n.replace("RADIO_", "") for n, v in consts.items() if n.startswith("RADIO_") and n != "RADIO_KIND_MAX"},
        "band":  {v: n.replace("BAND_", "")  for n, v in consts.items() if n.startswith("BAND_")  and n != "BAND_MAX"},
        "panel": {v: n.replace("PANEL_", "") for n, v in consts.items() if n.startswith("PANEL_") and n != "PANEL_KIND_MAX"},
        "touch": {v: n.replace("TOUCH_", "") for n, v in consts.items() if n.startswith("TOUCH_") and n != "TOUCH_KIND_MAX"},
    }
    # Красивые имена вместо машинных: в таблице их читает человек.
    names["radio"].update({consts["RADIO_SX127X"]: "SX127x", consts["RADIO_SX126X"]: "SX126x",
                           consts["RADIO_SX128X"]: "SX128x", consts["RADIO_LR11XX"]: "LR11xx",
                           consts["RADIO_HALOW"]: "HaLow"})
    names["band"].update({consts["BAND_2G4"]: "2.4 ГГц", consts["BAND_433"]: "433 МГц",
                          consts["BAND_470"]: "470 МГц", consts["BAND_868"]: "868 МГц",
                          consts["BAND_915"]: "915 МГц", consts["BAND_HALOW_SUB1"]: "HaLow sub-1G"})

    data = {
        "generated_by": "tools/gen_profiles.py — не править руками, "
                        "источник: main/hwcfg.c",
        "profiles": [build(p, names) for p in profiles],
    }

    if args.check:
        for p in data["profiles"]:
            radio = p["radio"] or {}
            print("%-20s %-22s %-8s %-10s пинов: %d" % (
                p["id"], p["name"], radio.get("chip", "-"),
                radio.get("band", "-"), len(radio.get("pins", []))))
        print("\nвсего профилей: %d" % len(data["profiles"]))
        return 0

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(data, ensure_ascii=False, indent=1),
                        encoding="utf-8")
    print("→ %s (%d профилей)" % (args.out, len(data["profiles"])))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except ParseError as e:
        print("gen_profiles: %s" % e, file=sys.stderr)
        sys.exit(1)
