#!/usr/bin/env python3
"""
discovery.py — Autodiscovery del ESP32 en la red local.

Estrategias (en orden de preferencia):
    1. Conexión directa a 192.168.4.1:8080 (ESP32 en modo AP).
    2. mDNS/Zeroconf: busca el servicio _esp32screen._tcp en la red.
    3. Scan TCP en paralelo del /24 local, verificando el hello JSON del ESP32.

API pública:
    async def discover(timeout: float = 5.0) -> str | None

Uso como script:
    python discovery.py           # imprime IP o "No encontrado"
    python discovery.py --wait    # espera hasta encontrarlo (útil al bootear)
"""

import argparse
import asyncio
import ipaddress
import json
import socket
import sys
import time
from typing import Optional

import websockets

# IP fija del ESP32 cuando actúa como Access Point
AP_IP   = "192.168.4.1"
WS_PORT = 8080
WS_PATH = "/ws"

# JSON de bienvenida que envía el ESP32 al conectar un cliente
HELLO_T = "hello"
HELLO_W = 480
HELLO_H = 320


# ── Utilidades de conexión ────────────────────────────────────────────────────

async def _probe_websocket(ip: str, port: int, timeout: float) -> bool:
    """
    Intenta abrir una conexión WebSocket a ip:port/ws y verifica que el primer
    mensaje recibido sea el JSON hello correcto del ESP32.
    Devuelve True si el servidor responde como ESP32.
    """
    uri = f"ws://{ip}:{port}{WS_PATH}"
    try:
        async with websockets.connect(
            uri,
            open_timeout=timeout,
            close_timeout=1.0,
            max_size=4096,
        ) as ws:
            # El ESP32 envía {"t":"hello","w":480,"h":320} nada más conectar
            raw = await asyncio.wait_for(ws.recv(), timeout=timeout)
            if isinstance(raw, (bytes, bytearray)):
                raw = raw.decode("utf-8", errors="ignore")
            data = json.loads(raw)
            return (
                data.get("t") == HELLO_T
                and data.get("w") == HELLO_W
                and data.get("h") == HELLO_H
            )
    except Exception:
        return False


# ── Estrategia 1: IP fija del modo AP ────────────────────────────────────────

async def _try_ap_ip(timeout: float) -> Optional[str]:
    """Prueba la IP fija del modo AP (192.168.4.1)."""
    if await _probe_websocket(AP_IP, WS_PORT, timeout):
        return AP_IP
    return None


# ── Estrategia 2: mDNS con Zeroconf ──────────────────────────────────────────

async def _try_mdns(timeout: float) -> Optional[str]:
    """
    Busca el servicio _esp32screen._tcp via mDNS usando la librería zeroconf.
    Si zeroconf no está instalado, esta estrategia se salta silenciosamente.
    """
    try:
        from zeroconf import ServiceBrowser, Zeroconf  # type: ignore
        from zeroconf.asyncio import AsyncZeroconf      # type: ignore
    except ImportError:
        # zeroconf no instalado, saltar esta estrategia
        return None

    resultado: list[str] = []
    encontrado = asyncio.Event()

    class _Listener:
        def add_service(self, zc, type_, name):
            info = zc.get_service_info(type_, name)
            if info and info.addresses:
                ip = socket.inet_ntoa(info.addresses[0])
                resultado.append(ip)
                encontrado.set()

        def remove_service(self, zc, type_, name):
            pass

        def update_service(self, zc, type_, name):
            pass

    azc = AsyncZeroconf()
    try:
        browser = ServiceBrowser(azc.zeroconf, "_esp32screen._tcp.local.", _Listener())
        try:
            await asyncio.wait_for(encontrado.wait(), timeout=timeout)
        except asyncio.TimeoutError:
            pass
    finally:
        await azc.async_close()

    if resultado:
        ip = resultado[0]
        # Verificar que realmente es nuestro ESP32
        if await _probe_websocket(ip, WS_PORT, timeout=2.0):
            return ip
    return None


# ── Estrategia 3: Scan TCP del /24 ───────────────────────────────────────────

def _detect_local_subnet() -> Optional[str]:
    """
    Detecta el prefijo /24 de la interfaz de red activa que tiene gateway
    usando netifaces. Si netifaces no está disponible, usa un fallback básico.
    Devuelve p.ej. "192.168.1" o None.
    """
    try:
        import netifaces  # type: ignore

        gateways = netifaces.gateways()
        default_gw = gateways.get("default", {})
        if not default_gw:
            return None

        # Tomar la primera interfaz con gateway por defecto (AF_INET = 2)
        for af, (gw_ip, iface, _) in default_gw.items():
            addrs = netifaces.ifaddresses(iface)
            ipv4_list = addrs.get(netifaces.AF_INET, [])
            if ipv4_list:
                local_ip = ipv4_list[0].get("addr", "")
                if local_ip:
                    # Devolver los tres primeros octetos
                    parts = local_ip.split(".")
                    if len(parts) == 4:
                        return ".".join(parts[:3])
    except Exception:
        pass

    # Fallback: socket trick (sin netifaces)
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("8.8.8.8", 80))
            local_ip = s.getsockname()[0]
            parts = local_ip.split(".")
            if len(parts) == 4:
                return ".".join(parts[:3])
    except Exception:
        pass

    return None


async def _scan_subnet(subnet_prefix: str, timeout: float) -> Optional[str]:
    """
    Escanea todas las IPs del /24 en paralelo buscando el ESP32.
    Utiliza asyncio para probar múltiples hosts simultáneamente.
    """
    # Semáforo para no abrir demasiadas conexiones a la vez
    sem = asyncio.Semaphore(50)

    resultado: list[str] = []
    encontrado = asyncio.Event()

    async def _check(ip: str) -> None:
        if encontrado.is_set():
            return
        async with sem:
            if encontrado.is_set():
                return
            if await _probe_websocket(ip, WS_PORT, timeout=1.5):
                resultado.append(ip)
                encontrado.set()

    tareas = [
        asyncio.create_task(_check(f"{subnet_prefix}.{i}"))
        for i in range(1, 255)
    ]

    try:
        await asyncio.wait_for(encontrado.wait(), timeout=timeout)
    except asyncio.TimeoutError:
        pass
    finally:
        # Cancelar tareas pendientes
        for t in tareas:
            if not t.done():
                t.cancel()
        await asyncio.gather(*tareas, return_exceptions=True)

    return resultado[0] if resultado else None


async def _try_scan(timeout: float) -> Optional[str]:
    """Detecta la subred local y lanza el scan."""
    subnet_prefix = _detect_local_subnet()
    if not subnet_prefix:
        print("  [Scan] No se pudo detectar la subred local.")
        return None
    print(f"  [Scan] Escaneando {subnet_prefix}.0/24 …")
    return await _scan_subnet(subnet_prefix, timeout)


# ── API pública ───────────────────────────────────────────────────────────────

async def discover(timeout: float = 5.0) -> Optional[str]:
    """
    Busca el ESP32 en la red usando tres estrategias en orden:
      1. IP fija del modo AP (192.168.4.1)
      2. mDNS (_esp32screen._tcp)
      3. Scan TCP del /24 local

    Devuelve la IP del ESP32 o None si no se encontró.
    """
    # Estrategia 1 — modo AP
    print("[Discovery] Probando IP fija del modo AP (192.168.4.1) …")
    ip = await _try_ap_ip(timeout=min(timeout, 3.0))
    if ip:
        print(f"[Discovery] ESP32 encontrado en {ip} (modo AP)")
        return ip

    # Estrategia 2 — mDNS
    print("[Discovery] Buscando vía mDNS (_esp32screen._tcp) …")
    ip = await _try_mdns(timeout=min(timeout, 4.0))
    if ip:
        print(f"[Discovery] ESP32 encontrado en {ip} (mDNS)")
        return ip

    # Estrategia 3 — scan de red
    print("[Discovery] Iniciando scan de red …")
    ip = await _try_scan(timeout=timeout)
    if ip:
        print(f"[Discovery] ESP32 encontrado en {ip} (scan de red)")
        return ip

    return None


# ── Script standalone ─────────────────────────────────────────────────────────

async def _main_async(wait: bool) -> None:
    if wait:
        print("Modo espera activo. Buscando ESP32 hasta encontrarlo (Ctrl+C para cancelar) …")
        backoff = 2.0
        while True:
            ip = await discover(timeout=5.0)
            if ip:
                print(ip)
                return
            print(f"  No encontrado, reintentando en {backoff:.0f}s …")
            await asyncio.sleep(backoff)
            backoff = min(backoff * 1.5, 30.0)
    else:
        ip = await discover(timeout=5.0)
        if ip:
            print(ip)
        else:
            print("No encontrado")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Autodiscovery del ESP32 Secondary Screen en la red local"
    )
    parser.add_argument(
        "--wait", action="store_true",
        help="Esperar hasta encontrar el ESP32 (útil al bootear)"
    )
    args = parser.parse_args()

    try:
        asyncio.run(_main_async(args.wait))
    except KeyboardInterrupt:
        print("\nAbortado.")
        sys.exit(1)


if __name__ == "__main__":
    main()
