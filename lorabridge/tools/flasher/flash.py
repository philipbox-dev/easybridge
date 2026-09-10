#!/usr/bin/env python3
"""EasyBridge flasher — прошивает merged-образ нужной платы.

Использование:
    python flash.py                        # автоопределение чипа + выбор платы
    python flash.py --board t3_v161        # явная плата
    python flash.py --board s3_e220 --name Philip   # + записать имя юзера в NVS
    python flash.py --port /dev/ttyUSB0 --list      # список плат

Требует: pip install esptool
Образы берутся из images/ (собрать: ./build_images.sh).
"""
import argparse
import csv
import io
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).parent
IMAGES = HERE / "web" / "images"

# board → (chip, human name)
BOARDS = {
    "s3_e220":    ("esp32s3", "ESP32-S3 + E220-400T30D (433 МГц, 30 dBm)"),
    "c6_e220":    ("esp32c6", "ESP32-C6 + E220-400T30D (433 МГц, 30 dBm)"),
    "t3_v161":    ("esp32",   "LilyGO T3 v1.6.1 (SX1276, 868/915 МГц, батарея)"),
    "s3_e22m33s": ("esp32s3", "ESP32-S3 + E22-400M33S (SX1268, 433 МГц, 33 dBm)"),
}

# Дефолтная single-app таблица разделов ESP-IDF
NVS_OFFSET = 0x9000
NVS_SIZE   = 0x6000


def esptool(*args, port=None, chip=None):
    cmd = [sys.executable, "-m", "esptool"]
    if chip:
        cmd += ["--chip", chip]
    if port:
        cmd += ["--port", port]
    cmd += list(args)
    print("$", " ".join(cmd))
    return subprocess.run(cmd, capture_output=False)


def detect_chip(port):
    """esptool chip_id → строка типа 'esp32s3'."""
    cmd = [sys.executable, "-m", "esptool"]
    if port:
        cmd += ["--port", port]
    cmd += ["chip_id"]
    out = subprocess.run(cmd, capture_output=True, text=True).stdout
    for line in out.splitlines():
        if "Detecting chip type" in line or "Chip is" in line:
            low = line.lower()
            for chip in ("esp32-s3", "esp32-c6", "esp32"):
                if chip in low:
                    return chip.replace("-", "")
    return None


def make_nvs_image(name: str) -> bytes:
    """Генерирует NVS-образ с identity/name через nvs_partition_gen."""
    try:
        from esp_idf_nvs_partition_gen import nvs_partition_gen  # IDF 5.3+
    except ImportError:
        nvs_partition_gen = None

    csv_content = (
        "key,type,encoding,value\n"
        "identity,namespace,,\n"
        f"name,data,string,{name}\n"
    )
    with tempfile.TemporaryDirectory() as td:
        csv_path = Path(td) / "identity.csv"
        bin_path = Path(td) / "identity.bin"
        csv_path.write_text(csv_content)
        if nvs_partition_gen is not None:
            args = argparse.Namespace(
                input=str(csv_path), output=str(bin_path),
                size=hex(NVS_SIZE), version=2, outdir=td,
            )
            nvs_partition_gen.generate(args)
        else:
            # fallback: утилита из ESP-IDF в PATH
            r = subprocess.run(
                ["nvs_partition_gen.py", "generate", str(csv_path),
                 str(bin_path), hex(NVS_SIZE)],
                capture_output=True, text=True)
            if r.returncode != 0:
                sys.exit(f"nvs_partition_gen не найден: установите esp-idf-nvs-partition-gen\n{r.stderr}")
        return bin_path.read_bytes()


def main():
    ap = argparse.ArgumentParser(description="EasyBridge flasher")
    ap.add_argument("--board", choices=BOARDS.keys())
    ap.add_argument("--port", help="/dev/ttyUSB0, /dev/ttyACM0, COM5…")
    ap.add_argument("--name", help="Имя юзера — записать в NVS после прошивки")
    ap.add_argument("--list", action="store_true", help="Показать платы и выйти")
    args = ap.parse_args()

    if args.list:
        for b, (chip, desc) in BOARDS.items():
            img = IMAGES / f"easybridge_{b}.bin"
            mark = "✓" if img.exists() else "✗ (нет образа)"
            print(f"  {b:12s} {chip:8s} {desc}  {mark}")
        return

    board = args.board
    if not board:
        detected = detect_chip(args.port)
        candidates = [b for b, (chip, _) in BOARDS.items() if chip == detected]
        if len(candidates) == 1:
            board = candidates[0]
            print(f"Чип {detected} → плата {board}")
        elif candidates:
            print(f"Чип {detected}. Выберите плату:")
            for i, b in enumerate(candidates):
                print(f"  [{i + 1}] {b} — {BOARDS[b][1]}")
            board = candidates[int(input("> ")) - 1]
        else:
            sys.exit("Чип не определён — укажите --board явно")

    chip, desc = BOARDS[board]
    image = IMAGES / f"easybridge_{board}.bin"
    if not image.exists():
        sys.exit(f"Нет образа {image} — соберите: ./build_images.sh")

    print(f"Прошиваем {desc}")
    r = esptool("write_flash", "0x0", str(image), port=args.port, chip=chip)
    if r.returncode != 0:
        sys.exit("Прошивка не удалась")

    if args.name:
        print(f"Записываем имя '{args.name}' в NVS…")
        nvs = make_nvs_image(args.name)
        with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
            f.write(nvs)
            nvs_path = f.name
        esptool("write_flash", hex(NVS_OFFSET), nvs_path, port=args.port, chip=chip)

    print("Готово! Устройство перезагрузится само.")


if __name__ == "__main__":
    main()
