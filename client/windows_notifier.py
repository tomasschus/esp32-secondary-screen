#!/usr/bin/env python3
"""
windows_notifier.py — Intercepta notificaciones de Windows y las manda al ESP32.

Monitorea wpndatabase.db buscando notificaciones nuevas de las apps configuradas
(por defecto: Claude). Cuando aparece una, la parsea y la envía como overlay al ESP32.

Uso:
    python windows_notifier.py --host 192.168.0.138
    python windows_notifier.py --host 192.168.0.138 --apps Claude Slack Discord
    python windows_notifier.py --host 192.168.0.138 --list-apps   # listar apps disponibles

Las notificaciones aparecen en el ESP32 como overlay de 8 segundos por encima
de cualquier contenido que esté mostrando (stats, mirror, etc.).
"""

import argparse
import asyncio
import json
import os
import shutil
import sqlite3
import tempfile
import time
import xml.etree.ElementTree as ET
import websockets

DB_PATH = os.path.expandvars(
    r"%LOCALAPPDATA%\Microsoft\Windows\Notifications\wpndatabase.db"
)

# ── Leer DB (copiando para evitar lock) ──────────────────────────────────────

def read_db(query: str, params=()):
    tmp = shutil.copy(DB_PATH, tempfile.mktemp(suffix=".db"))
    try:
        con = sqlite3.connect(tmp)
        cur = con.execute(query, params)
        rows = cur.fetchall()
        con.close()
        return rows
    finally:
        os.unlink(tmp)


# ── Parsear payload XML de notificación Windows ───────────────────────────────

def parse_toast(payload: bytes) -> tuple[str, str]:
    """Extrae (title, body) de un payload XML de toast de Windows."""
    try:
        root = ET.fromstring(payload.decode("utf-8", errors="replace"))
        texts = root.findall(".//text")
        title = texts[0].text.strip() if len(texts) > 0 and texts[0].text else ""
        body  = texts[1].text.strip() if len(texts) > 1 and texts[1].text else ""
        return title, body
    except Exception:
        return "Notificación", ""


# ── Obtener handler IDs de las apps pedidas ───────────────────────────────────

def get_handler_ids(app_names: list[str]) -> dict[int, str]:
    """Devuelve {handler_id: app_display_name} para las apps que matcheen."""
    rows = read_db("SELECT RecordId, PrimaryId FROM NotificationHandler")
    result = {}
    for record_id, primary_id in rows:
        for name in app_names:
            if name.lower() in primary_id.lower():
                # Usar solo el nombre corto como display name
                display = primary_id.split("!")[0].split("_")[0].split(".")[-1]
                result[record_id] = display
                break
    return result


def list_apps():
    rows = read_db(
        "SELECT nh.PrimaryId, COUNT(n.Id) as cnt "
        "FROM NotificationHandler nh "
        "LEFT JOIN Notification n ON n.HandlerId = nh.RecordId "
        "GROUP BY nh.RecordId ORDER BY cnt DESC LIMIT 30"
    )
    print("Apps con notificaciones en la base de datos:")
    for primary_id, cnt in rows:
        print(f"  {cnt:4d}  {primary_id}")


# ── Monitor principal ─────────────────────────────────────────────────────────

async def push_notification(host: str, port: int, app: str, title: str, body: str):
    payload = json.dumps({
        "t":     "notify",
        "app":   app[:31],
        "title": title[:63],
        "body":  body[:255],
    })
    try:
        async with websockets.connect(
            f"ws://{host}:{port}/ws", open_timeout=3, close_timeout=2
        ) as ws:
            await ws.send(payload)
        print(f"  → ESP32: [{app}] {title} | {body[:50]}")
    except Exception as e:
        print(f"  ! Error enviando al ESP32: {e}")


async def monitor(host: str, port: int, handler_ids: dict[int, str], interval: float):
    if not handler_ids:
        print("ERROR: no se encontraron las apps pedidas en la base de notificaciones.")
        print("Corré con --list-apps para ver las apps disponibles.")
        return

    print(f"Monitoreando notificaciones de: {list(handler_ids.values())}")
    print(f"Enviando al ESP32 en ws://{host}:{port}/ws")

    # ID de la última notificación vista
    rows = read_db(
        f"SELECT MAX(Id) FROM Notification WHERE HandlerId IN ({','.join('?'*len(handler_ids))})",
        list(handler_ids.keys())
    )
    last_id = rows[0][0] or 0
    print(f"Última notificación conocida: ID={last_id}. Esperando nuevas...\n")

    while True:
        await asyncio.sleep(interval)
        try:
            rows = read_db(
                f"SELECT n.Id, n.HandlerId, n.Payload "
                f"FROM Notification n "
                f"WHERE n.HandlerId IN ({','.join('?'*len(handler_ids))}) "
                f"AND n.Id > ? "
                f"ORDER BY n.Id ASC",
                list(handler_ids.keys()) + [last_id]
            )
            for notif_id, handler_id, payload in rows:
                last_id = notif_id
                app_name = handler_ids.get(handler_id, "App")
                title, body = parse_toast(payload)
                if title or body:
                    await push_notification(host, port, app_name, title, body)
        except Exception as e:
            print(f"  ! Error leyendo DB: {e}")


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    if args.list_apps:
        list_apps()
        return

    handler_ids = get_handler_ids(args.apps)
    try:
        asyncio.run(monitor(args.host, args.port, handler_ids, args.interval))
    except KeyboardInterrupt:
        print("\nDetenido.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Notificaciones Windows → ESP32")
    parser.add_argument("--host",      default="192.168.4.1", help="IP del ESP32")
    parser.add_argument("--port",      type=int, default=8080)
    parser.add_argument("--apps",      nargs="+", default=["Claude"],
                        help="Nombres parciales de apps a monitorear (default: Claude)")
    parser.add_argument("--interval",  type=float, default=1.0,
                        help="Segundos entre chequeos de la DB (default: 1)")
    parser.add_argument("--list-apps", action="store_true",
                        help="Listar apps disponibles y salir")
    args = parser.parse_args()
    main()
