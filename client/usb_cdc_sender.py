#!/usr/bin/env python3
"""
usb_cdc_sender.py

Envía frames JPEG o mensajes JSON de stats al ESP32-S3 por USB CDC.

Protocolo de trama:
    [0xAA 0x55 0xAA 0x55][LEN:uint32 LE][TYPE:uint8][PAYLOAD...]
    TYPE 0x01 → JPEG (modo screen)
    TYPE 0x02 → JSON (modo stats / notify)

Uso:
    python usb_cdc_sender.py --port /dev/cu.usbmodem* --mode screen
    python usb_cdc_sender.py --port /dev/cu.usbmodem* --mode stats
    python usb_cdc_sender.py --mode screen          # auto-detecta el puerto

Dependencias: pyserial, mss, Pillow, psutil
"""

import argparse
import glob
import io
import json
import struct
import sys
import time
from typing import Optional

import psutil
import serial
import serial.tools.list_ports

# ── Constantes del protocolo ──────────────────────────────────────────────────
MAGIC       = bytes([0xAA, 0x55, 0xAA, 0x55])
TYPE_JPEG   = 0x01
TYPE_JSON   = 0x02

# ── Resolución objetivo ───────────────────────────────────────────────────────
SCREEN_W = 480
SCREEN_H = 320

# Calidad JPEG (50-80 es un buen balance velocidad/calidad para USB CDC)
JPEG_QUALITY = 60

# Baud rate declarado (ignorado por CDC nativo, pero requerido por pyserial)
CDC_BAUD = 2_000_000

# Intervalo entre frames en modo screen (segundos)
SCREEN_INTERVAL = 1.0 / 15   # ~15 fps máximo

# Intervalo entre envíos de stats (segundos)
STATS_INTERVAL = 2.0


# ── Helpers de protocolo ──────────────────────────────────────────────────────

def build_frame(payload: bytes, frame_type: int) -> bytes:
    """Construye una trama completa con cabecera y payload."""
    header = MAGIC + struct.pack("<IB", len(payload), frame_type)
    return header + payload


def send_frame(port: serial.Serial, payload: bytes, frame_type: int) -> int:
    """Envía una trama y devuelve el número de bytes escritos."""
    frame = build_frame(payload, frame_type)
    written = port.write(frame)
    port.flush()
    return written


# ── Auto-detección del puerto ─────────────────────────────────────────────────

def find_esp32_port() -> Optional[str]:
    """
    Busca el puerto USB CDC del ESP32-S3.
    En macOS: /dev/cu.usbmodem*
    En Linux: /dev/ttyACM*
    En Windows: COMx con descripción CDC
    """
    # macOS — patrón típico del CDC nativo S3
    macos_ports = glob.glob("/dev/cu.usbmodem*")
    if macos_ports:
        print(f"[auto] puertos macOS encontrados: {macos_ports}")
        return macos_ports[0]

    # Linux
    linux_ports = glob.glob("/dev/ttyACM*")
    if linux_ports:
        print(f"[auto] puertos Linux encontrados: {linux_ports}")
        return linux_ports[0]

    # Windows / fallback: buscar por descripción en todos los puertos
    for info in serial.tools.list_ports.comports():
        desc = (info.description or "").lower()
        hwid = (info.hwid or "").lower()
        # Espressif VID = 0x303A, Sub-class CDC
        if "303a" in hwid or "esp32" in desc or "cdc" in desc:
            print(f"[auto] puerto encontrado: {info.device} ({info.description})")
            return info.device

    return None


# ── Modo screen ───────────────────────────────────────────────────────────────

def run_screen(port: serial.Serial) -> None:
    """
    Captura la pantalla principal, la escala a 480×320, la comprime como JPEG
    y la envía continuamente al ESP32.
    """
    try:
        import mss
        from PIL import Image
    except ImportError:
        print("ERROR: instala 'mss' y 'Pillow'  →  pip install mss Pillow")
        sys.exit(1)

    print(f"[screen] iniciando captura → {SCREEN_W}×{SCREEN_H} @ {JPEG_QUALITY}% JPEG")

    frames_sent  = 0
    bytes_total  = 0
    t_start      = time.monotonic()
    t_report     = t_start

    with mss.mss() as sct:
        # Capturar monitor principal (índice 1; índice 0 es el monitor combinado)
        monitor = sct.monitors[1]

        while True:
            t_frame = time.monotonic()

            # 1. Captura
            raw = sct.grab(monitor)
            img = Image.frombytes("RGB", raw.size, raw.rgb)

            # 2. Escalar al tamaño del display (480×320) conservando relación de aspecto
            img.thumbnail((SCREEN_W, SCREEN_H), Image.LANCZOS)

            # Crear fondo negro y pegar centrado
            canvas = Image.new("RGB", (SCREEN_W, SCREEN_H), (0, 0, 0))
            offset_x = (SCREEN_W - img.width)  // 2
            offset_y = (SCREEN_H - img.height) // 2
            canvas.paste(img, (offset_x, offset_y))

            # 3. Comprimir a JPEG en memoria
            buf = io.BytesIO()
            canvas.save(buf, format="JPEG", quality=JPEG_QUALITY, optimize=False)
            jpeg_bytes = buf.getvalue()

            # 4. Enviar
            written = send_frame(port, jpeg_bytes, TYPE_JPEG)
            frames_sent += 1
            bytes_total += written

            # 5. Reporte de throughput cada 5 segundos
            now = time.monotonic()
            if now - t_report >= 5.0:
                elapsed  = now - t_start
                fps      = frames_sent / elapsed
                kbps     = (bytes_total / 1024) / elapsed
                print(f"[screen] {frames_sent} frames  |  "
                      f"{fps:.1f} fps  |  "
                      f"{kbps:.1f} KB/s  |  "
                      f"frame actual: {len(jpeg_bytes)/1024:.1f} KB")
                t_report = now

            # 6. Respetar el intervalo entre frames
            elapsed_frame = time.monotonic() - t_frame
            sleep_time = SCREEN_INTERVAL - elapsed_frame
            if sleep_time > 0:
                time.sleep(sleep_time)


# ── Modo stats ────────────────────────────────────────────────────────────────

def run_stats(port: serial.Serial) -> None:
    """
    Lee métricas del sistema con psutil y las envía cada STATS_INTERVAL segundos.
    """
    print(f"[stats] enviando métricas cada {STATS_INTERVAL}s")

    msgs_sent   = 0
    bytes_total = 0
    t_start     = time.monotonic()

    # Primera llamada a cpu_percent es siempre 0; descartarla
    psutil.cpu_percent(interval=None)

    while True:
        t0 = time.monotonic()

        # Recolectar métricas
        cpu  = int(psutil.cpu_percent(interval=None))
        ram  = int(psutil.virtual_memory().percent)

        # GPU: intentar con pynvml (NVIDIA) o gputil si están disponibles
        gpu = 0
        try:
            import pynvml  # type: ignore
            pynvml.nvmlInit()
            handle = pynvml.nvmlDeviceGetHandleByIndex(0)
            util   = pynvml.nvmlDeviceGetUtilizationRates(handle)
            gpu    = int(util.gpu)
        except Exception:
            pass   # Sin GPU detectable → 0

        # Temperatura: primer sensor disponible
        temp = 0
        try:
            temps = psutil.sensors_temperatures()
            for sensor_list in temps.values():
                if sensor_list:
                    temp = int(sensor_list[0].current)
                    break
        except Exception:
            pass   # No disponible en macOS/Windows sin drivers extra

        payload_dict = {
            "t":    "stats",
            "cpu":  cpu,
            "ram":  ram,
            "gpu":  gpu,
            "temp": temp,
        }
        payload_json = json.dumps(payload_dict, separators=(",", ":")).encode()

        written     = send_frame(port, payload_json, TYPE_JSON)
        msgs_sent  += 1
        bytes_total += written

        elapsed = time.monotonic() - t_start
        kbps    = (bytes_total / 1024) / elapsed if elapsed > 0 else 0
        print(f"[stats] #{msgs_sent:4d}  cpu={cpu:3d}%  ram={ram:3d}%  "
              f"gpu={gpu:3d}%  temp={temp:3d}°C  |  {kbps:.2f} KB/s")

        # Esperar hasta el siguiente ciclo
        elapsed_loop = time.monotonic() - t0
        sleep_time   = STATS_INTERVAL - elapsed_loop
        if sleep_time > 0:
            time.sleep(sleep_time)


# ── Main ──────────────────────────────────────────────────────────────────────

def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Envía frames JPEG o stats de sistema al ESP32-S3 por USB CDC."
    )
    parser.add_argument(
        "--port", "-p",
        default=None,
        help="Puerto serie del ESP32, ej: /dev/cu.usbmodem101  (auto-detecta si se omite)"
    )
    parser.add_argument(
        "--mode", "-m",
        choices=["screen", "stats"],
        required=True,
        help="Modo de operación: 'screen' envía JPEG, 'stats' envía métricas"
    )
    parser.add_argument(
        "--baud", "-b",
        type=int,
        default=CDC_BAUD,
        help=f"Baud rate declarado (ignorado por CDC nativo, default {CDC_BAUD})"
    )
    parser.add_argument(
        "--quality", "-q",
        type=int,
        default=JPEG_QUALITY,
        help=f"Calidad JPEG 1-95 (default {JPEG_QUALITY})"
    )
    return parser.parse_args()


def main() -> None:
    global JPEG_QUALITY

    args = parse_args()
    JPEG_QUALITY = max(1, min(95, args.quality))

    # Resolver el puerto
    port_name = args.port
    if not port_name:
        port_name = find_esp32_port()
        if not port_name:
            print("ERROR: no se encontró ningún puerto CDC del ESP32.")
            print("Pasa el puerto explícitamente con --port /dev/cu.usbmodem...")
            sys.exit(1)

    print(f"[main] abriendo {port_name} @ {args.baud} baud (CDC nativo)")
    try:
        ser = serial.Serial(
            port=port_name,
            baudrate=args.baud,
            timeout=1.0,
            write_timeout=5.0,
        )
    except serial.SerialException as e:
        print(f"ERROR al abrir el puerto: {e}")
        sys.exit(1)

    # Pequeña pausa para que el ESP32 procese el DTR/RTS
    time.sleep(0.5)
    print(f"[main] conectado. Modo: {args.mode}")

    try:
        if args.mode == "screen":
            run_screen(ser)
        elif args.mode == "stats":
            run_stats(ser)
    except KeyboardInterrupt:
        print("\n[main] interrumpido por el usuario")
    finally:
        ser.close()
        print("[main] puerto cerrado")


if __name__ == "__main__":
    main()
