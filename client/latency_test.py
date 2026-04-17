#!/usr/bin/env python3
"""
latency_test.py — Benchmark de latencia extremo a extremo para el ESP32.

Genera frames JPEG sintéticos (Pillow, sin captura real) y los envía por
WebSocket al ESP32, midiendo latencia de round-trip mediante ping/pong
estándar del protocolo WebSocket.

Métricas reportadas por calidad JPEG:
    - Tamaño medio del frame (KB)
    - Latencia media de envío (ms)
    - p50 / p95 / p99 de latencia
    - FPS sostenido
    - Throughput (KB/s)

Uso:
    python latency_test.py --host 192.168.4.1
    python latency_test.py --host 192.168.4.1 --frames 200 --qualities 30,50,70,85
"""

import argparse
import asyncio
import io
import random
import statistics
import sys
import time

import websockets
from PIL import Image, ImageDraw

SCREEN_W = 480
SCREEN_H = 320


# ── Generación de frames sintéticos ──────────────────────────────────────────

def _generate_frame(quality: int, seed: int) -> bytes:
    """
    Genera un frame JPEG 480×320 sintético con Pillow.
    Varía el contenido frame a frame (usando seed) para evitar que el compresor
    JPEG trivialice el benchmark con un fondo completamente plano.
    """
    rng = random.Random(seed)

    # Fondo de color aleatorio
    bg_color = (rng.randint(0, 255), rng.randint(0, 255), rng.randint(0, 255))
    img = Image.new("RGB", (SCREEN_W, SCREEN_H), bg_color)
    draw = ImageDraw.Draw(img)

    # Varios rectángulos y elipses para simular UI real
    for _ in range(8):
        x0 = rng.randint(0, SCREEN_W - 40)
        y0 = rng.randint(0, SCREEN_H - 30)
        x1 = x0 + rng.randint(20, 120)
        y1 = y0 + rng.randint(10, 80)
        color = (rng.randint(0, 255), rng.randint(0, 255), rng.randint(0, 255))
        if rng.random() > 0.5:
            draw.rectangle([x0, y0, x1, y1], fill=color)
        else:
            draw.ellipse([x0, y0, x1, y1], fill=color)

    buf = io.BytesIO()
    img.save(buf, format="JPEG", quality=quality, optimize=False)
    return buf.getvalue()


def _prebuild_frames(n: int, quality: int) -> list[bytes]:
    """Pre-genera N frames para no contaminar las mediciones con tiempo de CPU."""
    frames = []
    for i in range(n):
        frames.append(_generate_frame(quality, seed=i))
    return frames


# ── Benchmark de una calidad ──────────────────────────────────────────────────

async def _benchmark_quality(
    host: str,
    port: int,
    frames: list[bytes],
    quality: int,
) -> dict:
    """
    Envía todos los frames y mide latencia de round-trip usando ping/pong
    WebSocket estándar (RFC 6455). El servidor ESP32 responde automáticamente
    a los pings sin necesidad de soporte adicional en el firmware.

    Devuelve un dict con las métricas calculadas.
    """
    uri = f"ws://{host}:{port}/ws"
    latencias_ms: list[float] = []   # ms por frame (send + pong)
    bytes_enviados = 0

    print(f"  Calidad {quality}%  |  {len(frames)} frames  |  conectando a {uri} …")

    async with websockets.connect(uri, max_size=None) as ws:
        t_total_inicio = time.monotonic()

        for i, jpeg in enumerate(frames):
            # Medimos round-trip: envío del frame + ping/pong de confirmación
            t0 = time.monotonic()
            await ws.send(jpeg)
            # ping() devuelve un Future que se resuelve cuando llega el pong
            pong_waiter = await ws.ping()
            await pong_waiter
            t1 = time.monotonic()

            latencias_ms.append((t1 - t0) * 1000.0)
            bytes_enviados += len(jpeg)

            # Progreso cada 10 frames
            if (i + 1) % 10 == 0:
                print(f"    [{i+1}/{len(frames)}] lat={latencias_ms[-1]:.1f} ms  "
                      f"size={len(jpeg)/1024:.1f} KB")

        t_total = time.monotonic() - t_total_inicio

    latencias_ms.sort()
    n = len(latencias_ms)

    def percentil(p: float) -> float:
        idx = int(p / 100 * n)
        return latencias_ms[min(idx, n - 1)]

    tam_medio_kb  = bytes_enviados / n / 1024
    lat_media_ms  = statistics.mean(latencias_ms)
    p50           = percentil(50)
    p95           = percentil(95)
    p99           = percentil(99)
    fps           = n / t_total
    throughput    = bytes_enviados / 1024 / t_total   # KB/s

    return {
        "quality":      quality,
        "n_frames":     n,
        "tam_medio_kb": tam_medio_kb,
        "lat_media_ms": lat_media_ms,
        "p50_ms":       p50,
        "p95_ms":       p95,
        "p99_ms":       p99,
        "fps":          fps,
        "throughput_kbs": throughput,
    }


# ── Tabla de resultados ───────────────────────────────────────────────────────

def _imprimir_tabla(resultados: list[dict]) -> None:
    """Imprime una tabla comparativa alineada en consola."""
    sep = "─" * 82
    header = (
        f"{'Cal':>4}  {'Frame(KB)':>9}  {'Lat.med(ms)':>11}  "
        f"{'p50(ms)':>8}  {'p95(ms)':>8}  {'p99(ms)':>8}  "
        f"{'FPS':>6}  {'KB/s':>8}"
    )
    print()
    print("  RESULTADOS FINALES")
    print(sep)
    print(header)
    print(sep)
    for r in resultados:
        print(
            f"  {r['quality']:>3}%"
            f"  {r['tam_medio_kb']:>8.2f}"
            f"  {r['lat_media_ms']:>10.1f}"
            f"  {r['p50_ms']:>8.1f}"
            f"  {r['p95_ms']:>8.1f}"
            f"  {r['p99_ms']:>8.1f}"
            f"  {r['fps']:>6.1f}"
            f"  {r['throughput_kbs']:>7.0f}"
        )
    print(sep)
    print()

    # Resumen textual
    mejor_fps  = max(resultados, key=lambda r: r["fps"])
    menor_lat  = min(resultados, key=lambda r: r["lat_media_ms"])
    menor_peso = min(resultados, key=lambda r: r["tam_medio_kb"])

    print("  RESUMEN")
    print(f"  - Mayor FPS:              calidad {mejor_fps['quality']}%  →  "
          f"{mejor_fps['fps']:.1f} FPS  ({mejor_fps['throughput_kbs']:.0f} KB/s)")
    print(f"  - Menor latencia media:   calidad {menor_lat['quality']}%  →  "
          f"{menor_lat['lat_media_ms']:.1f} ms")
    print(f"  - Frame más liviano:      calidad {menor_peso['quality']}%  →  "
          f"{menor_peso['tam_medio_kb']:.2f} KB/frame")
    print()


# ── Main ──────────────────────────────────────────────────────────────────────

async def _run(host: str, port: int, n_frames: int, qualities: list[int]) -> None:
    resultados: list[dict] = []

    for q in qualities:
        print(f"\n[Calidad {q}%]  Pre-generando {n_frames} frames …")
        frames = _prebuild_frames(n_frames, q)
        print(f"  Tamaño medio: {sum(len(f) for f in frames) / len(frames) / 1024:.2f} KB/frame")

        try:
            metrics = await _benchmark_quality(host, port, frames, q)
        except (websockets.exceptions.WebSocketException, OSError) as e:
            print(f"  ERROR en calidad {q}%: {e}")
            continue

        resultados.append(metrics)

    if not resultados:
        print("No se obtuvieron resultados.")
        sys.exit(1)

    _imprimir_tabla(resultados)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Benchmark de latencia extremo a extremo para ESP32 Secondary Screen"
    )
    parser.add_argument("--host",      required=True,      help="IP del ESP32")
    parser.add_argument("--port",      type=int, default=8080)
    parser.add_argument("--frames",    type=int, default=100,
                        help="Número de frames por calidad (default: 100)")
    parser.add_argument("--qualities", default="30,50,70",
                        help="Calidades JPEG a probar, separadas por coma (default: 30,50,70)")
    args = parser.parse_args()

    # Parsear lista de calidades
    try:
        qualities = [int(q.strip()) for q in args.qualities.split(",")]
    except ValueError:
        print("ERROR: --qualities debe ser una lista de enteros separados por coma")
        sys.exit(1)

    if not all(1 <= q <= 95 for q in qualities):
        print("ERROR: Las calidades deben estar entre 1 y 95")
        sys.exit(1)

    print(f"Benchmark ESP32 Secondary Screen")
    print(f"  Host:      {args.host}:{args.port}")
    print(f"  Frames:    {args.frames} por calidad")
    print(f"  Calidades: {qualities}")

    try:
        asyncio.run(_run(args.host, args.port, args.frames, qualities))
    except KeyboardInterrupt:
        print("\nAbortado por el usuario.")


if __name__ == "__main__":
    main()
