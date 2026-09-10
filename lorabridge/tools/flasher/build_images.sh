#!/usr/bin/env bash
#
# Склеить merged-образы для веб-флешера и flash.py.
#
# ESP Web Tools умеет лить только один файл с нулевого смещения, а
# idf.py оставляет три (bootloader, таблица разделов, приложение) —
# esptool merge-bin собирает их обратно в один образ с правильными
# дырками между ними. Смещения и режим флеша берём из flasher_args.json
# соответствующей сборки, чтобы не разъезжались при смене платы.
#
#   ./build_images.sh              # все платы, у которых есть сборка
#   ./build_images.sh t3_v161      # только перечисленные
#
# Результат кладётся в web/images/ — рядом с manifest_*.json.

set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
FW_DIR="$(cd "$HERE/../.." && pwd)"
OUT="$HERE/web/images"

BOARDS=(t3_v161 c6_e220 s3_e220 s3_e22m33s)
[ $# -gt 0 ] && BOARDS=("$@")

mkdir -p "$OUT"

# esptool 4.x (тот, что внутри ESP-IDF) и 5.x называют одно и то же
# по-разному: merge_bin/--flash_mode против merge-bin/--flash-mode.
# Скрипт запускают и из окружения IDF, и из системного python — берём
# имена по фактической версии, а не по тому, кто как его позвал.
if python3 -m esptool version 2>/dev/null | grep -qE '^v?5\.'; then
    MERGE=merge-bin; F_MODE=--flash-mode; F_FREQ=--flash-freq; F_SIZE=--flash-size
else
    MERGE=merge_bin; F_MODE=--flash_mode; F_FREQ=--flash_freq; F_SIZE=--flash_size
fi

FAILED=0
for board in "${BOARDS[@]}"; do
    build="$FW_DIR/build_$board"
    args="$build/flasher_args.json"
    printf '  %-12s ' "$board"
    if [ ! -f "$args" ]; then
        echo "нет сборки ($build) — пропуск"
        continue
    fi

    # chip/флеш-настройки и список файлов — из самой сборки
    read -r chip mode freq size < <(python3 - "$args" <<'PY'
import json, sys
a = json.load(open(sys.argv[1]))
s = a["flash_settings"]
print(a["extra_esptool_args"]["chip"], s["flash_mode"], s["flash_freq"], s["flash_size"])
PY
)
    mapfile -t parts < <(python3 - "$args" <<'PY'
import json, sys
for off, f in sorted(json.load(open(sys.argv[1]))["flash_files"].items(), key=lambda kv: int(kv[0], 16)):
    print(off); print(f)
PY
)

    if ! out=$(cd "$build" && python3 -m esptool --chip "$chip" "$MERGE" \
            -o "$OUT/easybridge_$board.bin" \
            "$F_MODE" "$mode" "$F_FREQ" "$freq" "$F_SIZE" "$size" \
            "${parts[@]}" 2>&1); then
        echo "СБОРКА ОБРАЗА УПАЛА"
        echo "$out" | tail -5 | sed 's/^/      /'
        FAILED=1
        continue
    fi
    echo "$(du -h "$OUT/easybridge_$board.bin" | cut -f1)  →  web/images/easybridge_$board.bin"
done

exit $FAILED
