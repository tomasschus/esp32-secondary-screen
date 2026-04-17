#!/usr/bin/env python3
"""
send_layout.py — Envía un layout JSON al ESP32 via WebSocket.

Uso:
    python send_layout.py --host 192.168.4.1 --layout layouts/default.json
    python send_layout.py --host 192.168.1.50 --layout layouts/minimal.json
    python send_layout.py --host 192.168.4.1 --layout layouts/mirror.json

    # Sincronizar hora además de enviar el layout:
    python send_layout.py --host 192.168.4.1 --layout layouts/default.json --sync-time

Argumentos:
    --host        IP del ESP32 (obligatorio)
    --layout      Ruta al archivo JSON de layout (obligatorio)
    --port        Puerto WebSocket del ESP32 (defecto: 8080)
    --sync-time   Si se incluye, envía también {"t":"time","epoch":...} con
                  la hora Unix actual antes del layout
"""

import argparse
import asyncio
import json
import sys
import time

try:
    import websockets
except ImportError:
    print("ERROR: falta 'websockets'. Instalar con: pip install websockets")
    sys.exit(1)


async def send(host: str, port: int, layout_path: str, sync_time: bool) -> None:
    # Leer y validar el archivo de layout
    try:
        with open(layout_path, "r", encoding="utf-8") as f:
            raw = f.read()
    except FileNotFoundError:
        print(f"ERROR: no se encontró el archivo '{layout_path}'")
        sys.exit(1)
    except OSError as e:
        print(f"ERROR al leer '{layout_path}': {e}")
        sys.exit(1)

    # Verificar que es JSON válido y que tiene el campo correcto
    try:
        layout_obj = json.loads(raw)
    except json.JSONDecodeError as e:
        print(f"ERROR: JSON inválido en '{layout_path}': {e}")
        sys.exit(1)

    if layout_obj.get("t") != "layout":
        print(f"Aviso: el JSON no tiene \"t\": \"layout\" — se envía de todas formas")

    uri = f"ws://{host}:{port}/ws"
    print(f"Conectando a {uri} …")

    try:
        async with websockets.connect(uri, max_size=None) as ws:
            # Esperar el mensaje de bienvenida del ESP32
            try:
                hello = await asyncio.wait_for(ws.recv(), timeout=5.0)
                print(f"ESP32: {hello}")
            except asyncio.TimeoutError:
                print("Aviso: no se recibió mensaje de bienvenida (timeout 5s)")

            # Enviar hora de sincronización si se solicitó
            if sync_time:
                epoch = int(time.time())
                time_msg = json.dumps({"t": "time", "epoch": epoch})
                await ws.send(time_msg)
                print(f"Hora enviada: epoch={epoch}")

            # Enviar el layout (reserializado para compacto y sin BOM)
            layout_compact = json.dumps(layout_obj, separators=(",", ":"))
            await ws.send(layout_compact)
            widgets = layout_obj.get("widgets", [])
            tipos   = [w.get("type", "?") for w in widgets]
            print(f"Layout enviado: {len(widgets)} widget(s) — {tipos}")

            # Pequeña pausa para que el ESP32 procese antes de cerrar
            await asyncio.sleep(0.5)

    except websockets.exceptions.ConnectionRefusedError:
        print(f"ERROR: conexión rechazada en {uri}")
        sys.exit(1)
    except websockets.exceptions.WebSocketException as e:
        print(f"ERROR WebSocket: {e}")
        sys.exit(1)
    except OSError as e:
        print(f"ERROR de red: {e}")
        sys.exit(1)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Envía un layout JSON al ESP32 via WebSocket"
    )
    parser.add_argument("--host",      required=True,
                        help="IP del ESP32 (ej: 192.168.4.1)")
    parser.add_argument("--layout",    required=True,
                        help="Ruta al archivo JSON de layout")
    parser.add_argument("--port",      type=int, default=8080,
                        help="Puerto WebSocket (defecto: 8080)")
    parser.add_argument("--sync-time", action="store_true",
                        help="Enviar {'t':'time','epoch':N} antes del layout")
    args = parser.parse_args()

    try:
        asyncio.run(send(args.host, args.port, args.layout, args.sync_time))
    except KeyboardInterrupt:
        print("\nInterrumpido.")


if __name__ == "__main__":
    main()
