#!/usr/bin/env python3
"""
run_all.py — Launcher unificado para todos los clientes del ESP32 Secondary Screen.

Arranca screen_capture.py, system_stats.py y/o slack_notifier.py como subprocesos,
mostrando sus logs con prefijo coloreado y reiniciando automáticamente los procesos
que mueran (backoff exponencial).

Uso:
    python run_all.py --host 192.168.4.1 --mode mirror
    python run_all.py --host 192.168.4.1 --mode stats+notify
    python run_all.py --mode all --auto
    python run_all.py --auto

Modos (combinables con +):
    mirror   — screen_capture.py
    stats    — system_stats.py
    notify   — slack_notifier.py
    all      — los tres

Flags:
    --auto       Autodiscover la IP del ESP32 antes de arrancar
    --host       IP del ESP32 (requerido si no se usa --auto)
    --port       Puerto WebSocket (default: 8080)
    --mode       Modo(s) a arrancar (default: all)
"""

import argparse
import asyncio
import os
import signal
import subprocess
import sys
import time
from pathlib import Path
from typing import Optional


# ── Configuración de modos ────────────────────────────────────────────────────

# Directorio donde residen todos los scripts cliente
_CLIENT_DIR = Path(__file__).parent

# Definición de cada servicio: nombre mostrado, script, prefijo ANSI
_SERVICIOS = {
    "mirror": {
        "label":  "MIRROR",
        "script": "screen_capture.py",
        "color":  "\033[94m",   # azul
        "args":   [],           # se completarán con --host y --port
    },
    "stats": {
        "label":  "STATS",
        "script": "system_stats.py",
        "color":  "\033[92m",   # verde
        "args":   [],
    },
    "notify": {
        "label":  "SLACK",
        "script": "slack_notifier.py",
        "color":  "\033[95m",   # magenta
        "args":   [],           # no necesita --host (lo toma de ENV)
    },
}

_RESET   = "\033[0m"
_AMARILLO = "\033[93m"
_ROJO    = "\033[91m"
_CYAN    = "\033[96m"


# ── Utilidades de salida ──────────────────────────────────────────────────────

def _log(color: str, prefijo: str, msg: str) -> None:
    """Imprime una línea de log con prefijo coloreado."""
    ts = time.strftime("%H:%M:%S")
    print(f"{color}[{prefijo}]{_RESET} {_CYAN}{ts}{_RESET} {msg}", flush=True)


def _log_sistema(msg: str) -> None:
    _log(_AMARILLO, "RUN_ALL", msg)


# ── Lector de stdout/stderr del subproceso ────────────────────────────────────

async def _leer_salida(stream: asyncio.StreamReader, color: str, prefijo: str) -> None:
    """Lee líneas de un stream de subproceso e imprime con prefijo coloreado."""
    while True:
        try:
            linea = await stream.readline()
        except Exception:
            break
        if not linea:
            break
        texto = linea.decode("utf-8", errors="replace").rstrip()
        if texto:
            _log(color, prefijo, texto)


# ── Clase de gestión de un servicio ──────────────────────────────────────────

class _Servicio:
    """
    Gestiona el ciclo de vida de un subproceso cliente.
    Reinicia automáticamente con backoff exponencial si el proceso muere.
    """

    BACKOFF_INICIAL   = 1.0   # segundos
    BACKOFF_MAXIMO    = 30.0  # segundos
    BACKOFF_FACTOR    = 2.0

    def __init__(self, nombre: str, cmd: list[str], color: str, label: str):
        self.nombre  = nombre
        self.cmd     = cmd
        self.color   = color
        self.label   = label
        self.proceso: Optional[asyncio.subprocess.Process] = None
        self._backoff = self.BACKOFF_INICIAL
        self._activo  = True

    async def iniciar(self) -> None:
        """Inicia el subproceso y espera a que termine; reinicia si es necesario."""
        while self._activo:
            _log_sistema(f"Iniciando [{self.label}]: {' '.join(self.cmd)}")
            try:
                self.proceso = await asyncio.create_subprocess_exec(
                    *self.cmd,
                    stdout=asyncio.subprocess.PIPE,
                    stderr=asyncio.subprocess.STDOUT,
                    cwd=str(_CLIENT_DIR),
                )
            except FileNotFoundError as e:
                _log(self.color, self.label, f"ERROR: Script no encontrado — {e}")
                await asyncio.sleep(self._backoff)
                self._backoff = min(self._backoff * self.BACKOFF_FACTOR, self.BACKOFF_MAXIMO)
                continue

            # Leer salida mientras el proceso está vivo
            assert self.proceso.stdout is not None
            await _leer_salida(self.proceso.stdout, self.color, self.label)

            # Esperar que el proceso termine
            retcode = await self.proceso.wait()

            if not self._activo:
                break

            _log(
                _ROJO, self.label,
                f"Proceso terminado (código {retcode}). "
                f"Reiniciando en {self._backoff:.0f}s …"
            )
            await asyncio.sleep(self._backoff)
            self._backoff = min(self._backoff * self.BACKOFF_FACTOR, self.BACKOFF_MAXIMO)

    def resetear_backoff(self) -> None:
        """Resetea el backoff cuando el proceso lleva suficiente tiempo corriendo."""
        self._backoff = self.BACKOFF_INICIAL

    async def detener(self) -> None:
        """Detiene el proceso limpiamente."""
        self._activo = False
        if self.proceso and self.proceso.returncode is None:
            try:
                self.proceso.terminate()
                await asyncio.wait_for(self.proceso.wait(), timeout=5.0)
            except Exception:
                try:
                    self.proceso.kill()
                except Exception:
                    pass


# ── Autodiscovery ─────────────────────────────────────────────────────────────

async def _autodiscover(timeout: float = 10.0) -> Optional[str]:
    """
    Llama al módulo discovery.py para encontrar el ESP32 en la red.
    Importa discovery directamente para reutilizar la lógica sin subprocess extra.
    """
    try:
        # Añadir el directorio cliente al path de importación
        if str(_CLIENT_DIR) not in sys.path:
            sys.path.insert(0, str(_CLIENT_DIR))
        import discovery  # type: ignore
        return await discovery.discover(timeout=timeout)
    except ImportError:
        # Si discovery.py no está disponible, intentar como subprocess
        _log_sistema("discovery.py no importable, ejecutando como subprocess …")
        try:
            proc = await asyncio.create_subprocess_exec(
                sys.executable, str(_CLIENT_DIR / "discovery.py"),
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE,
            )
            stdout, _ = await asyncio.wait_for(proc.communicate(), timeout=timeout + 2)
            ip = stdout.decode().strip()
            if ip and ip != "No encontrado":
                return ip
        except Exception as e:
            _log_sistema(f"Error en autodiscovery: {e}")
        return None


# ── Parseo de modos ───────────────────────────────────────────────────────────

def _parsear_modos(modo_str: str) -> list[str]:
    """
    Convierte la cadena de modo a lista de nombres de servicio.
    Acepta 'all', 'mirror', 'stats', 'notify' y combinaciones con '+'.
    """
    partes = [p.strip().lower() for p in modo_str.split("+")]
    if "all" in partes:
        return list(_SERVICIOS.keys())

    validos = set(_SERVICIOS.keys())
    resultado = []
    for p in partes:
        if p not in validos:
            print(f"ERROR: Modo desconocido '{p}'. Válidos: {', '.join(validos)}, all")
            sys.exit(1)
        if p not in resultado:
            resultado.append(p)
    return resultado


# ── Construcción de comandos ──────────────────────────────────────────────────

def _construir_cmd(nombre: str, host: str, port: int) -> list[str]:
    """Devuelve la lista argv para invocar el script de cada servicio."""
    script = str(_CLIENT_DIR / _SERVICIOS[nombre]["script"])
    base   = [sys.executable, script]

    if nombre in ("mirror", "stats"):
        return base + ["--host", host, "--port", str(port)]
    elif nombre == "notify":
        # slack_notifier toma host/port desde ENV; no recibe argumentos CLI
        return base
    return base


# ── Gestor de señales ─────────────────────────────────────────────────────────

_servicios_activos: list[_Servicio] = []


def _manejador_sigint(signum, frame) -> None:
    """Captura Ctrl+C y delega el shutdown al loop de asyncio."""
    _log_sistema("Ctrl+C recibido, deteniendo todos los procesos …")
    for srv in _servicios_activos:
        srv._activo = False
        if srv.proceso and srv.proceso.returncode is None:
            try:
                srv.proceso.terminate()
            except Exception:
                pass


# ── Punto de entrada ──────────────────────────────────────────────────────────

async def _run(host: str, port: int, modos: list[str]) -> None:
    global _servicios_activos

    # Configurar ENV para slack_notifier si no está ya definido
    if "notify" in modos:
        os.environ.setdefault("ESP32_HOST", host)
        os.environ.setdefault("ESP32_PORT", str(port))

    # Crear servicios
    servicios = []
    for nombre in modos:
        defn  = _SERVICIOS[nombre]
        cmd   = _construir_cmd(nombre, host, port)
        srv   = _Servicio(nombre, cmd, defn["color"], defn["label"])
        servicios.append(srv)

    _servicios_activos = servicios

    # Instalar manejador de señal
    signal.signal(signal.SIGINT, _manejador_sigint)
    if hasattr(signal, "SIGTERM"):
        signal.signal(signal.SIGTERM, _manejador_sigint)

    _log_sistema(
        f"Arrancando {len(servicios)} servicio(s): "
        f"{', '.join('[' + s.label + ']' for s in servicios)}"
    )

    # Arrancar todos los servicios en paralelo
    tareas = [asyncio.create_task(srv.iniciar()) for srv in servicios]

    try:
        await asyncio.gather(*tareas)
    except asyncio.CancelledError:
        pass

    # Asegurar parada limpia de todos
    await asyncio.gather(*[srv.detener() for srv in servicios], return_exceptions=True)
    _log_sistema("Todos los procesos detenidos.")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Launcher unificado para ESP32 Secondary Screen"
    )
    parser.add_argument(
        "--host", default=None,
        help="IP del ESP32 (requerido a menos que se use --auto)"
    )
    parser.add_argument(
        "--port", type=int, default=8080,
        help="Puerto WebSocket del ESP32 (default: 8080)"
    )
    parser.add_argument(
        "--mode", default="all",
        help="Modo(s) a arrancar: mirror, stats, notify, all (combinables con +). "
             "Default: all"
    )
    parser.add_argument(
        "--auto", action="store_true",
        help="Autodiscover la IP del ESP32 antes de arrancar"
    )
    args = parser.parse_args()

    modos = _parsear_modos(args.mode)
    host  = args.host

    # ── Autodiscovery ──────────────────────────────────────────────────────────
    if args.auto:
        _log_sistema("Buscando ESP32 en la red …")
        found = asyncio.run(_autodiscover(timeout=10.0))
        if not found:
            print("ERROR: No se encontró ningún ESP32 en la red. "
                  "Usa --host para especificar la IP manualmente.")
            sys.exit(1)
        host = found
        _log_sistema(f"ESP32 encontrado en {host}")

    if not host:
        parser.error("Especifica --host <IP> o usa --auto para autodiscovery")

    _log_sistema(f"Target: {host}:{args.port}  |  Modos: {'+'.join(modos)}")

    try:
        asyncio.run(_run(host, args.port, modos))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
