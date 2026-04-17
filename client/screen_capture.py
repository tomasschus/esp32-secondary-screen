#!/usr/bin/env python3
"""
screen_capture.py — Captura pantalla y envía frames JPEG al ESP32.

Uso:
    python screen_capture.py --host 192.168.4.1
    python screen_capture.py --host 192.168.1.50 --port 8080 --quality 55 --fps 25
    python screen_capture.py --host 192.168.4.1 --region 0 0 960 540  # mitad superior izquierda

Argumentos:
    --host      IP del ESP32 (requerido)
    --port      Puerto WebSocket (default: 8080)
    --quality   Calidad JPEG 1-95 (default: 50)
    --fps       FPS objetivo (default: 25)
    --monitor   Índice del monitor a capturar (default: 1 = primario)
    --region    x y w h — región específica en vez del monitor completo
"""

import argparse
import asyncio
import time
import sys

import mss
import websockets
from PIL import Image
import io

SCREEN_W = 480
SCREEN_H = 320


def capture_frame(sct, monitor_def) -> bytes:
    """Captura una región, la escala a 480×320 y devuelve JPEG bytes."""
    img = sct.grab(monitor_def)
    pil = Image.frombytes("RGB", img.size, img.bgra, "raw", "BGRX")
    pil = pil.resize((SCREEN_W, SCREEN_H), Image.LANCZOS)
    buf = io.BytesIO()
    pil.save(buf, format="JPEG", quality=args.quality, optimize=False)
    return buf.getvalue()


async def stream(host: str, port: int, monitor_def: dict):
    uri = f"ws://{host}:{port}/ws"
    interval = 1.0 / args.fps

    print(f"Conectando a {uri} …")
    async with websockets.connect(uri, max_size=None) as ws:
        print("Conectado. Enviando frames (Ctrl+C para detener)")
        with mss.mss() as sct:
            frame_count = 0
            t_start = time.monotonic()
            while True:
                t0 = time.monotonic()
                jpeg = capture_frame(sct, monitor_def)
                await ws.send(jpeg)
                frame_count += 1

                elapsed = time.monotonic() - t0
                sleep_for = interval - elapsed
                if sleep_for > 0:
                    await asyncio.sleep(sleep_for)

                # Log cada 5 segundos
                if time.monotonic() - t_start >= 5.0:
                    fps_real = frame_count / 5.0
                    print(f"  {fps_real:.1f} FPS  |  {len(jpeg)/1024:.1f} KB/frame")
                    frame_count = 0
                    t_start = time.monotonic()


def build_monitor_def(sct) -> dict:
    if args.region:
        x, y, w, h = args.region
        return {"left": x, "top": y, "width": w, "height": h}
    monitors = sct.monitors  # [0] = all, [1] = primary, [2] = secondary, ...
    idx = min(args.monitor, len(monitors) - 1)
    return monitors[idx]


def main():
    with mss.mss() as sct:
        monitor_def = build_monitor_def(sct)
        print(f"Capturando: {monitor_def}")

    try:
        asyncio.run(stream(args.host, args.port, monitor_def))
    except KeyboardInterrupt:
        print("\nDetenido.")
    except websockets.exceptions.ConnectionClosedError as e:
        print(f"Conexión cerrada: {e}")
        sys.exit(1)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Captura pantalla → ESP32")
    parser.add_argument("--host",    required=True, help="IP del ESP32")
    parser.add_argument("--port",    type=int, default=8080)
    parser.add_argument("--quality", type=int, default=50, help="Calidad JPEG (1-95)")
    parser.add_argument("--fps",     type=int, default=25, help="FPS objetivo")
    parser.add_argument("--monitor", type=int, default=1, help="Índice de monitor")
    parser.add_argument("--region",  type=int, nargs=4, metavar=("X","Y","W","H"),
                        help="Región específica en píxeles")
    args = parser.parse_args()
    main()
