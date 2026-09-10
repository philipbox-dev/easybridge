#!/usr/bin/env bash
#
# Собрать и проверить весь EasyBridge разом: прошивку на все платы,
# приложение и серверную часть.
#
# Три репозитория лежат в разных местах и собираются разными
# инструментами, и держать это в голове (или в трёх окнах терминала) —
# лишняя работа. Скрипт делает всё по очереди и в конце показывает
# сводку: что собралось, что упало и сколько тестов прошло.
#
#   ./build_all.sh                 # всё
#   ./build_all.sh fw              # только прошивка (4 платы)
#   ./build_all.sh app             # только Android
#   ./build_all.sh server          # только тесты сервера
#   ./build_all.sh fw app          # что перечислено
#   ./build_all.sh --images        # + merged-образы и таблицы пинов
#
# Ничего никуда не выкладывает: деплой — отдельное осознанное действие
# (см. ~/serverchik/deploy.py).

set -uo pipefail

FW_DIR="$(cd "$(dirname "$0")" && pwd)"
APP_DIR="${EASYLINK_DIR:-$HOME/AndroidStudioProjects/EasyLink}"
SRV_DIR="${SERVERCHIK_DIR:-$HOME/serverchik}"
IDF_EXPORT="${IDF_EXPORT:-$HOME/esp/esp-idf-v5.5.1/export.sh}"

# Плата → цель ESP-IDF. Профиль по умолчанию выбирается сборкой, но
# железо всё равно настраивается в рантайме (см. docs/HWCFG.md).
BOARDS=(
    "t3_v161:esp32"
    "c6_e220:esp32c6"
    "s3_e220:esp32s3"
    "s3_e22m33s:esp32s3"
)

WANT_IMAGES=0
TARGETS=()
for arg in "$@"; do
    case "$arg" in
        --images) WANT_IMAGES=1 ;;
        fw|app|server) TARGETS+=("$arg") ;;
        -h|--help) sed -n '3,20p' "$0" | sed 's/^# \?//'; exit 0 ;;
        *) echo "неизвестный аргумент: $arg (см. --help)"; exit 2 ;;
    esac
done
[ ${#TARGETS[@]} -eq 0 ] && TARGETS=(fw app server)

RESULTS=()
FAILED=0

say()  { printf '\n\033[1m══ %s\033[0m\n' "$*"; }
# printf выравнивает по байтам, а кириллица многобайтовая — считаем
# ширину сами, иначе колонка разъезжается на русских подписях.
pad()  { local w=13 n=${#1}; printf '  %s' "$1"
         while [ $n -lt $w ]; do printf ' '; n=$((n+1)); done; printf ' '; }
ok()   { RESULTS+=("  ✔ $*"); }
fail() { RESULTS+=("  ✘ $*"); FAILED=1; }
skip() { RESULTS+=("  – $*"); }

want() { [[ " ${TARGETS[*]} " == *" $1 "* ]]; }

# ── Прошивка ────────────────────────────────────────────────
build_fw() {
    say "Прошивка"
    if [ ! -f "$IDF_EXPORT" ]; then
        echo "  ESP-IDF не найден: $IDF_EXPORT"
        echo "  (путь можно задать переменной IDF_EXPORT)"
        skip "прошивка — нет ESP-IDF"
        return
    fi
    # shellcheck disable=SC1090
    source "$IDF_EXPORT" >/dev/null 2>&1

    cd "$FW_DIR" || return
    for entry in "${BOARDS[@]}"; do
        local board="${entry%%:*}" target="${entry##*:}"
        pad "$board"
        local out
        out=$(idf.py -B "build_$board" \
            -DSDKCONFIG="build_$board/sdkconfig" \
            -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.board_$board" \
            -DIDF_TARGET="$target" build 2>&1)
        if echo "$out" | grep -qE '^\S+: error:|FAILED:'; then
            echo "СБОРКА УПАЛА"
            echo "$out" | grep -E 'error:|FAILED:' | head -3 | sed 's/^/      /'
            fail "прошивка $board"
        else
            echo "$out" | grep 'easybridge.bin binary size' \
                | sed 's/.*Smallest app partition is [^ ]* bytes\. //'
            ok "прошивка $board"
        fi
    done

    # Таблицы пинов на странице прошивки берутся из hwcfg.c — держим их
    # в согласии с кодом при каждой сборке, а не когда вспомним.
    pad "профили"
    if python3 "$FW_DIR/tools/gen_profiles.py" >/dev/null 2>&1; then
        echo "profiles.json обновлён"
        ok "таблицы пинов"
    else
        echo "ГЕНЕРАТОР УПАЛ"
        python3 "$FW_DIR/tools/gen_profiles.py" 2>&1 | tail -2 | sed 's/^/      /'
        fail "таблицы пинов"
    fi

    if [ "$WANT_IMAGES" = 1 ]; then
        say "Merged-образы для флешера"
        if "$FW_DIR/tools/flasher/build_images.sh" >/dev/null 2>&1; then
            ok "образы флешера"
        else
            fail "образы флешера"
        fi
    fi
}

# ── Android ─────────────────────────────────────────────────
build_app() {
    say "Android (EasyLink)"
    if [ ! -x "$APP_DIR/gradlew" ]; then
        echo "  проект не найден: $APP_DIR"
        echo "  (путь можно задать переменной EASYLINK_DIR)"
        skip "приложение — не найдено"
        return
    fi
    cd "$APP_DIR" || return
    local out
    out=$(./gradlew :app:assembleDebug 2>&1)
    if echo "$out" | grep -q "BUILD SUCCESSFUL"; then
        local apk
        apk=$(ls -1 app/build/outputs/apk/debug/*.apk 2>/dev/null | head -1)
        echo "  APK: ${apk:-собран}"
        ok "приложение"
    else
        echo "$out" | grep -E '^e:|error:' | head -5 | sed 's/^/    /'
        fail "приложение"
    fi
}

# ── Сервер ──────────────────────────────────────────────────
build_server() {
    say "Веб-часть (тесты)"
    if [ ! -x "$SRV_DIR/run_lorachat_tests.sh" ]; then
        echo "  не найден: $SRV_DIR/run_lorachat_tests.sh"
        echo "  (путь можно задать переменной SERVERCHIK_DIR)"
        skip "сервер — не найден"
        return
    fi
    cd "$SRV_DIR" || return
    local out
    out=$("$SRV_DIR/run_lorachat_tests.sh" 2>&1)
    local line
    line=$(echo "$out" | grep -E '[0-9]+ (passed|failed)' | tail -1)
    echo "  ${line:-нет результата}"
    if echo "$line" | grep -q "failed"; then
        echo "$out" | grep -E '^FAILED' | head -5 | sed 's/^/    /'
        fail "тесты сервера"
    else
        ok "тесты сервера — ${line:-?}"
    fi
}

want fw     && build_fw
want app    && build_app
want server && build_server

say "Итог"
printf '%s\n' "${RESULTS[@]}"
if [ "$FAILED" = 1 ]; then
    echo
    echo "Что-то не собралось. Деплоить в таком виде не стоит."
    exit 1
fi
echo
echo "Всё готово. Деплой веб-части — отдельно: cd $SRV_DIR && python3 deploy.py"
