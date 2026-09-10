#!/usr/bin/env bash
# Прогон тестов LoRa-чата в изолированном окружении.
#
# Трогать системный Python и зависимости сайта не нужно: скрипт
# создаёт venv рядом с собой и ставит туда только то, что нужно
# модулю. Прод при этом не затрагивается вообще.
#
#   ./run_lorachat_tests.sh           # все тесты
#   ./run_lorachat_tests.sh -k floor  # только про арбитраж эфира
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
VENV="$ROOT/.lcvenv"

if [ ! -x "$VENV/bin/python" ]; then
    echo "→ создаю окружение в $VENV"
    python3 -m venv "$VENV"
    "$VENV/bin/pip" -q install --upgrade pip
    # websocket-client нужен только тестам: он ходит настоящим
    # сокетом к настоящему серверу, иначе главный сценарий продукта
    # (сообщение доходит в реальном времени) не проверяется вовсе.
    "$VENV/bin/pip" -q install flask flask-sqlalchemy flask-login flask-wtf \
        flask-sock cryptography pytest websocket-client
fi

cd "$ROOT"
exec "$VENV/bin/python" -m pytest \
    tests/test_lorachat.py \
    tests/test_lorachat_call.py \
    tests/test_lorachat_devices.py \
    tests/test_lorachat_ws.py \
    -q -p no:warnings "$@"
