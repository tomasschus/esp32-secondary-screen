#!/usr/bin/env python3
"""
slack_notifier.py — Recibe mensajes de Slack y los envía al ESP32.

Usa Slack Bolt con Socket Mode: conexión WebSocket saliente a Slack.
No requiere servidor público ni ngrok.

Configuración requerida (variables de entorno o archivo .env):
    SLACK_BOT_TOKEN  — token xoxb-... (OAuth, permisos: im:read, channels:history, users:read)
    SLACK_APP_TOKEN  — token xapp-... (Socket Mode, permisos: connections:write)
    ESP32_HOST       — IP del ESP32

Uso:
    export SLACK_BOT_TOKEN=xoxb-...
    export SLACK_APP_TOKEN=xapp-...
    export ESP32_HOST=192.168.4.1
    python slack_notifier.py

Cómo crear la Slack App:
    1. https://api.slack.com/apps → "Create New App" → "From scratch"
    2. "Socket Mode" → Enable → crear App-Level Token con scope: connections:write → copiar xapp-...
    3. "OAuth & Permissions" → Bot Token Scopes: im:read, channels:history, users:read
    4. "Event Subscriptions" → Enable → Subscribe to bot events: message.im, message.channels
    5. "Install to Workspace" → copiar Bot User OAuth Token (xoxb-...)
"""

import asyncio
import json
import os
import sys
import websockets

from slack_bolt import App
from slack_bolt.adapter.socket_mode import SocketModeHandler

# ── Configuración ─────────────────────────────────────────────────────────
BOT_TOKEN  = os.environ.get("SLACK_BOT_TOKEN", "")
APP_TOKEN  = os.environ.get("SLACK_APP_TOKEN", "")
ESP32_HOST = os.environ.get("ESP32_HOST", "192.168.4.1")
ESP32_PORT = int(os.environ.get("ESP32_PORT", "8080"))

if not BOT_TOKEN or not APP_TOKEN:
    print("ERROR: Definir SLACK_BOT_TOKEN y SLACK_APP_TOKEN como variables de entorno")
    sys.exit(1)


# ── Push al ESP32 (síncrono, se llama desde el hilo de Slack Bolt) ─────────
def push_to_esp32(app_name: str, title: str, body: str) -> None:
    payload = json.dumps({
        "t":     "notify",
        "app":   app_name[:31],
        "title": title[:63],
        "body":  body[:255],
    })
    try:
        asyncio.run(_send_ws(payload))
    except Exception as e:
        print(f"[ESP32] error enviando: {e}")


async def _send_ws(payload: str) -> None:
    uri = f"ws://{ESP32_HOST}:{ESP32_PORT}/ws"
    async with websockets.connect(uri, open_timeout=3, close_timeout=2) as ws:
        await ws.send(payload)


# ── App Slack Bolt ─────────────────────────────────────────────────────────
app = App(token=BOT_TOKEN)

# Cache de user IDs → nombres (evita llamadas repetidas a users.info)
_user_cache: dict[str, str] = {}


def resolve_user(client, user_id: str) -> str:
    if user_id in _user_cache:
        return _user_cache[user_id]
    try:
        info = client.users_info(user=user_id)
        name = info["user"].get("real_name") or info["user"].get("name", user_id)
    except Exception:
        name = user_id
    _user_cache[user_id] = name
    return name


@app.event("message")
def handle_message(event, client, logger):
    """Captura mensajes en DMs y canales donde el bot está invitado."""
    subtype = event.get("subtype")
    # Ignorar ediciones, borrados, mensajes del bot mismo
    if subtype in ("message_changed", "message_deleted", "bot_message"):
        return
    if event.get("bot_id"):
        return

    user_id  = event.get("user", "")
    text     = event.get("text", "")
    channel  = event.get("channel", "")
    chan_type = event.get("channel_type", "")

    user_name = resolve_user(client, user_id) if user_id else "Unknown"

    if chan_type == "im":
        title = f"DM de {user_name}"
    else:
        title = f"{user_name} → #{channel}"

    logger.info(f"Slack → ESP32: {title} | {text[:60]}")
    push_to_esp32("Slack", title, text)


@app.event("app_mention")
def handle_mention(event, client, logger):
    """Captura menciones al bot en cualquier canal."""
    user_id   = event.get("user", "")
    text      = event.get("text", "")
    channel   = event.get("channel", "")
    user_name = resolve_user(client, user_id) if user_id else "Unknown"

    title = f"@mención de {user_name}"
    logger.info(f"Slack mention → ESP32: {title}")
    push_to_esp32("Slack", title, text)


# ── Main ───────────────────────────────────────────────────────────────────
if __name__ == "__main__":
    print(f"Slack Notifier iniciando → ESP32 en {ESP32_HOST}:{ESP32_PORT}")
    handler = SocketModeHandler(app, APP_TOKEN)
    handler.start()
