#!/usr/bin/env python3
"""
system_stats.py — Envía métricas del sistema al ESP32 cada N segundos.

Uso:
    python system_stats.py --host 192.168.4.1
    python system_stats.py --host 192.168.1.50 --interval 2

Métricas enviadas (JSON):
    {"t": "stats", "cpu": 42, "ram": 61, "gpu": 80, "temp": 65}

GPU:
    - macOS: intenta leer uso GPU vía `powermetrics` (requiere sudo).
      Si no está disponible, envía 0.
    - Para datos más ricos en macOS, instalar `gpu-py` o usar iStatistica.
"""

import argparse
import asyncio
import json
import subprocess
import sys
import time

import psutil
import websockets


def get_cpu_percent() -> int:
    return int(psutil.cpu_percent(interval=None))


def get_ram_percent() -> int:
    return int(psutil.virtual_memory().percent)


def get_cpu_temp() -> int:
    """Temperatura CPU en °C. Windows: WMI. Linux/macOS: psutil/sysctl."""
    # Windows via WMI
    if sys.platform == "win32":
        try:
            import wmi  # type: ignore
            w = wmi.WMI(namespace="root\\OpenHardwareMonitor")
            for sensor in w.Sensor():
                if sensor.SensorType == "Temperature" and "CPU" in sensor.Name:
                    return int(sensor.Value)
        except Exception:
            pass
        return 0
    # Linux / macOS
    try:
        temps = psutil.sensors_temperatures()
        if temps:
            for key in ("coretemp", "cpu_thermal", "acpitz", "k10temp"):
                if key in temps and temps[key]:
                    return int(temps[key][0].current)
        result = subprocess.run(
            ["sysctl", "-n", "machdep.xcpm.cpu_thermal_level"],
            capture_output=True, text=True, timeout=1
        )
        if result.returncode == 0:
            return int(result.stdout.strip())
    except Exception:
        pass
    return 0


def get_gpu_percent() -> int:
    """
    Uso GPU en macOS vía powermetrics (requiere sudo).
    Sin sudo, devuelve 0. Para uso sin sudo, instalar 'gpu-py':
        pip install gpu-py
    """
    try:
        import gpustat  # type: ignore
        stats = gpustat.GPUStatCollection.new_query()
        if stats.gpus:
            return int(stats.gpus[0].utilization)
    except ImportError:
        pass
    return 0


async def send_stats(host: str, port: int, interval: float):
    uri = f"ws://{host}:{port}/ws"
    print(f"Conectando a {uri} …")

    # Primer ciclo de psutil necesita un warmup
    psutil.cpu_percent(interval=0.1)

    async with websockets.connect(uri, max_size=None) as ws:
        print(f"Conectado. Enviando stats cada {interval}s (Ctrl+C para detener)")
        while True:
            payload = {
                "t":    "stats",
                "cpu":  get_cpu_percent(),
                "ram":  get_ram_percent(),
                "gpu":  get_gpu_percent(),
                "temp": get_cpu_temp(),
            }
            await ws.send(json.dumps(payload))
            print(f"  CPU {payload['cpu']}%  RAM {payload['ram']}%  "
                  f"GPU {payload['gpu']}%  {payload['temp']}°C")
            await asyncio.sleep(interval)


def main():
    try:
        asyncio.run(send_stats(args.host, args.port, args.interval))
    except KeyboardInterrupt:
        print("\nDetenido.")
    except websockets.exceptions.ConnectionClosedError as e:
        print(f"Conexión cerrada: {e}")
        sys.exit(1)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Stats del sistema → ESP32")
    parser.add_argument("--host",     required=True, help="IP del ESP32")
    parser.add_argument("--port",     type=int, default=8080)
    parser.add_argument("--interval", type=float, default=2.0,
                        help="Segundos entre actualizaciones (default: 2)")
    args = parser.parse_args()
    main()
