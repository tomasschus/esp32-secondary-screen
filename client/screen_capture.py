#!/usr/bin/env python3
"""
screen_capture.py — Captura pantalla y envía frames JPEG al ESP32.

Uso:
    python screen_capture.py --host 192.168.4.1
    python screen_capture.py --host 192.168.1.50 --quality 55 --fps 25
    python screen_capture.py --host 192.168.4.1 --region 0 0 960 540
    python screen_capture.py --host 192.168.4.1 --window "YouTube Music"

Argumentos:
    --host      IP del ESP32 (requerido)
    --port      Puerto WebSocket (default: 8080)
    --quality   Calidad JPEG 1-95 (default: 50)
    --fps       FPS objetivo (default: 25)
    --monitor   Índice del monitor a capturar (default: 1 = primario)
    --region    x y w h — región específica en vez del monitor completo
    --window    Título parcial de ventana a capturar (aunque esté minimizada)
"""

import argparse
import asyncio
import io
import sys
import time

import mss
import websockets
from PIL import Image

SCREEN_W = 480
SCREEN_H = 320


# ── Captura por ventana (Windows) ────────────────────────────────────────────

def find_window(title_fragment: str):
    """Devuelve el HWND de la primera ventana cuyo título contenga title_fragment."""
    import ctypes
    import ctypes.wintypes

    found = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.wintypes.HWND, ctypes.wintypes.LPARAM)
    def enum_cb(hwnd, _):
        length = ctypes.windll.user32.GetWindowTextLengthW(hwnd)
        if length:
            buf = ctypes.create_unicode_buffer(length + 1)
            ctypes.windll.user32.GetWindowTextW(hwnd, buf, length + 1)
            if title_fragment.lower() in buf.value.lower():
                found.append((hwnd, buf.value))
        return True

    ctypes.windll.user32.EnumWindows(enum_cb, 0)
    return found


def capture_window(hwnd) -> bytes | None:
    """Captura una ventana por HWND usando PrintWindow (funciona minimizada)."""
    import ctypes
    import ctypes.wintypes
    import win32gui
    import win32ui
    import win32con

    # Restaurar temporalmente si está minimizada para poder capturarla
    placement = win32gui.GetWindowPlacement(hwnd)
    was_minimized = placement[1] == win32con.SW_SHOWMINIMIZED
    if was_minimized:
        win32gui.ShowWindow(hwnd, win32con.SW_SHOWNOACTIVATE)
        time.sleep(0.05)

    try:
        left, top, right, bottom = win32gui.GetWindowRect(hwnd)
        w = right - left
        h = bottom - top
        if w <= 0 or h <= 0:
            return None

        hwnd_dc   = win32gui.GetWindowDC(hwnd)
        mfc_dc    = win32ui.CreateDCFromHandle(hwnd_dc)
        save_dc   = mfc_dc.CreateCompatibleDC()
        bmp       = win32ui.CreateBitmap()
        bmp.CreateCompatibleBitmap(mfc_dc, w, h)
        save_dc.SelectObject(bmp)

        # PW_RENDERFULLCONTENT = 2 → captura contenido WebView/hardware-accelerated
        result = ctypes.windll.user32.PrintWindow(hwnd, save_dc.GetSafeHdc(), 2)
        if not result:
            ctypes.windll.user32.PrintWindow(hwnd, save_dc.GetSafeHdc(), 0)

        bmp_info = bmp.GetInfo()
        bmp_bits = bmp.GetBitmapBits(True)
        img = Image.frombuffer("RGB", (bmp_info["bmWidth"], bmp_info["bmHeight"]),
                               bmp_bits, "raw", "BGRX", 0, 1)

        save_dc.DeleteDC()
        mfc_dc.DeleteDC()
        win32gui.ReleaseDC(hwnd, hwnd_dc)
        win32gui.DeleteObject(bmp.GetHandle())

    finally:
        if was_minimized:
            win32gui.ShowWindow(hwnd, win32con.SW_SHOWMINNOACTIVE)

    img = img.resize((SCREEN_W, SCREEN_H), Image.LANCZOS)
    buf = io.BytesIO()
    img.save(buf, format="JPEG", quality=args.quality, optimize=False)
    return buf.getvalue()


# ── Captura de pantalla / región ─────────────────────────────────────────────

def capture_frame(sct, monitor_def) -> bytes:
    img = sct.grab(monitor_def)
    pil = Image.frombytes("RGB", img.size, img.bgra, "raw", "BGRX")
    pil = pil.resize((SCREEN_W, SCREEN_H), Image.LANCZOS)
    buf = io.BytesIO()
    pil.save(buf, format="JPEG", quality=args.quality, optimize=False)
    return buf.getvalue()


# ── Streaming ────────────────────────────────────────────────────────────────

async def stream_once(host: str, port: int, capture_fn):
    """Un intento de streaming. Levanta excepción si se corta."""
    uri = f"ws://{host}:{port}/ws"
    interval = 1.0 / args.fps

    async with websockets.connect(uri, max_size=None, open_timeout=5) as ws:
        print("Conectado. Enviando frames (Ctrl+C para detener)")
        frame_count = 0
        t_start = time.monotonic()
        while True:
            t0 = time.monotonic()
            jpeg = capture_fn()
            if jpeg:
                await ws.send(jpeg)
                frame_count += 1

            elapsed = time.monotonic() - t0
            sleep_for = interval - elapsed
            if sleep_for > 0:
                await asyncio.sleep(sleep_for)

            if time.monotonic() - t_start >= 5.0:
                fps_real = frame_count / 5.0
                kb = len(jpeg) / 1024 if jpeg else 0
                print(f"  {fps_real:.1f} FPS  |  {kb:.1f} KB/frame")
                frame_count = 0
                t_start = time.monotonic()


async def stream_loop(host: str, port: int, capture_fn):
    """Streaming con auto-reconexión."""
    uri = f"ws://{host}:{port}/ws"
    print(f"Conectando a {uri} …")
    while True:
        try:
            await stream_once(host, port, capture_fn)
        except KeyboardInterrupt:
            raise
        except Exception as e:
            print(f"  Conexión perdida: {e} — reconectando en 2s…")
            await asyncio.sleep(2)


# ── Helpers ───────────────────────────────────────────────────────────────────

def build_monitor_def(sct) -> dict:
    if args.region:
        x, y, w, h = args.region
        return {"left": x, "top": y, "width": w, "height": h}
    monitors = sct.monitors
    idx = min(args.monitor, len(monitors) - 1)
    return monitors[idx]


def main():
    # ── Modo ventana ──────────────────────────────────────────────────────────
    if args.window:
        matches = find_window(args.window)
        if not matches:
            print(f"ERROR: no se encontró ventana con '{args.window}'")
            print("Ventanas abiertas:")
            for hwnd, title in find_window(""):
                if title.strip():
                    print(f"  {hwnd}  {title}")
            sys.exit(1)

        hwnd, title = matches[0]
        print(f"Capturando ventana: '{title}' (HWND={hwnd})")

        try:
            import win32gui  # noqa — verificar que pywin32 está instalado
        except ImportError:
            print("ERROR: instalar pywin32:  pip install pywin32")
            sys.exit(1)

        capture_fn = lambda: capture_window(hwnd)
        try:
            asyncio.run(stream_loop(args.host, args.port, capture_fn))
        except KeyboardInterrupt:
            print("\nDetenido.")
        return

    # ── Modo pantalla / región ────────────────────────────────────────────────
    with mss.mss() as sct:
        monitor_def = build_monitor_def(sct)
        print(f"Capturando: {monitor_def}")

        def capture_fn():
            return capture_frame(sct, monitor_def)

        try:
            asyncio.run(stream_loop(args.host, args.port, capture_fn))
        except KeyboardInterrupt:
            print("\nDetenido.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Captura pantalla → ESP32")
    parser.add_argument("--host",    required=True, help="IP del ESP32")
    parser.add_argument("--port",    type=int, default=8080)
    parser.add_argument("--quality", type=int, default=50, help="Calidad JPEG (1-95)")
    parser.add_argument("--fps",     type=int, default=25, help="FPS objetivo")
    parser.add_argument("--monitor", type=int, default=1, help="Índice de monitor")
    parser.add_argument("--region",  type=int, nargs=4, metavar=("X","Y","W","H"),
                        help="Región específica en píxeles")
    parser.add_argument("--window",  type=str, help="Título parcial de ventana a capturar")
    args = parser.parse_args()
    main()
