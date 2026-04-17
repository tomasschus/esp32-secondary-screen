# ESP32 Secondary Screen

Pantalla secundaria basada en **JC3248W535EN** (ESP32-S3, 480×320 QSPI) que recibe contenido desde una PC vía Wi-Fi WebSocket o USB CDC nativo.

---

## Hardware

| Componente | Detalle |
|---|---|
| MCU | ESP32-S3 N16R8 (16 MB Flash, 8 MB PSRAM) |
| Display | 480×320 px, interfaz QSPI, controlador AXS15231B |
| Touch | Capacitivo I2C |
| Conectividad | Wi-Fi 802.11 b/g/n, USB nativo (sin bridge) |
| Módulo | JC3248W535EN |

---

## Stack técnico

| Capa | Solución |
|---|---|
| Display driver | Arduino_GFX + Arduino_ESP32QSPI |
| GUI | LVGL 9.2.2 (192 KB heap, sin OS) |
| JPEG decode | TJpgDec (buffer 120 KB en SPIRAM) |
| Transporte principal | Wi-Fi WebSocket — 20–35 FPS |
| Transporte alternativo | USB CDC nativo S3 — 10–18 FPS |
| Dev env | Arduino IDE 2.x (evitar PlatformIO: no soporta Core 3.x) |

> Siempre se envían frames JPEG comprimidos. Un frame RGB565 raw = 300 KB/frame → inviable sin compresión.

---

## Casos de uso

### 1. Monitor de sistema (PC stats)
Mostrar CPU, RAM, GPU, temperatura y red en tiempo real.
- Cliente PC envía métricas → el ESP32 renderiza con LVGL gauges/barras.
- Integrable con herramientas como HWiNFO64, AIDA64 o scripts Python con `psutil`.

### 2. Pantalla secundaria espejo / extensión de escritorio
El cliente PC captura una región de pantalla → encode JPEG → envía por WebSocket a ~25 FPS.
- Útil para: chat flotante, terminal, reproductor de música, panel de OBS, chat de stream.
- Solo requiere el cliente Python; el ESP32 actúa como display dumb.

### 3. Panel de notificaciones
El ESP32 recibe push de notificaciones (Slack, email, builds CI, alertas).
- Se actualiza solo cuando hay contenido nuevo (sin streaming continuo).
- Consumo energético mínimo: ideal si el ESP32 funciona con batería.
- **Slack:** cliente Python con `slack-bolt` en Socket Mode (WebSocket saliente, sin servidor público).
  Requiere crear una Slack App gratuita con permisos `im:read`, `channels:history`, `users:read`.

### 4. Panel de productividad personalizable
Widgets configurables: reloj, clima, calendario, Pomodoro timer, música en reproducción.
- Cada widget es una región LVGL actualizada por WebSocket desde el cliente PC.
- El usuario define layout vía archivo de configuración (JSON).

### 5. Overlay para gaming / streaming
Mostrar stats de OBS, chat de Twitch, alertas de stream, métricas de red sin ocupar monitor principal.
- Baja latencia requerida → Wi-Fi WebSocket con JPEG q=40–60.

---

## Arquitectura

```
┌─────────────────────────────┐        Wi-Fi / USB        ┌──────────────────────────┐
│          PC / Host          │ ─────────────────────────> │      ESP32-S3            │
│                             │                            │                          │
│  ┌──────────────────────┐   │   JPEG frames (WebSocket)  │  WebSocket server        │
│  │  Cliente Python      │   │ ─────────────────────────> │  TJpgDec pipeline        │
│  │  - captura pantalla  │   │                            │  Arduino_GFX render      │
│  │  - encode JPEG       │   │   o USB CDC (serial)       │  LVGL overlay (opcional) │
│  │  - envío WebSocket   │   │ ─────────────────────────> │                          │
│  └──────────────────────┘   │                            │  Display 480×320 QSPI    │
└─────────────────────────────┘                            └──────────────────────────┘
```

---

## Estructura del repositorio

```
.
├── src/
│   ├── main.cpp               # Init display QSPI + LVGL + loop principal
│   ├── ws_server.cpp          # WebSocket server + recepción JPEG fragmentada
│   ├── jpeg_pipeline.cpp      # TJpgDec decode → RGB565 → display
│   ├── usb_cdc.cpp            # Canal USB CDC alternativo (a implementar)
│   └── ui/                    # Widgets LVGL (stats, notificaciones, etc.)
├── include/
│   ├── lv_conf.h              # Config LVGL calibrada para JC3248W535EN
│   └── secrets.h.example      # Plantilla para credenciales Wi-Fi
├── client/
│   ├── screen_capture.py      # Captura pantalla → JPEG → WebSocket
│   ├── system_stats.py        # CPU/RAM/GPU → WebSocket
│   ├── slack_notifier.py      # Slack bot (Socket Mode) → push al ESP32
│   └── requirements.txt       # mss, Pillow, websockets, psutil, slack-bolt
├── docs/
│   └── specs-and-references.md
└── README.md
```

---

## Roadmap

### Fase 1 — Core display pipeline (base de todo)
- [x] Portar `main.cpp` del repo GPS: init display QSPI + LVGL
- [x] Portar `maps_ws_server.cpp`: WebSocket server + recepción JPEG fragmentada + TJpgDec
- [ ] Verificar render a pantalla completa (480×320, sin artefactos) ← pendiente flashear
- [ ] Medir FPS real con JPEG q=50

### Fase 2 — Cliente PC básico
- [x] Script Python: captura región de pantalla con `mss`
- [x] Encode JPEG con Pillow (calidad configurable)
- [x] Envío por WebSocket con `websockets` o `asyncio`
- [x] Medición de latencia extremo a extremo (`client/latency_test.py` — p50/p95/p99 por calidad JPEG)

### Fase 3 — Casos de uso específicos
- [x] Widget sistema: CPU/RAM/GPU con `psutil`
- [x] Notificaciones Slack: `slack_notifier.py` con Socket Mode → push al ESP32
- [x] Layout LVGL configurable por JSON (`client/send_layout.py` + `src/ui/ui_layout.cpp`)

### Fase 4 — Transporte USB CDC
- [x] Implementar `usb_cdc.cpp` con USB Serial nativo S3
- [x] Cliente Python vía `pyserial` (`client/usb_cdc_sender.py`)
- [ ] Comparar latencia vs Wi-Fi ← pendiente medir en hardware

### Fase 5 — Optimización y pulido
- [ ] Delta frames: enviar solo regiones modificadas (reduce ancho de banda ~60%)
- [ ] Protocolo binario propio para overhead mínimo
- [ ] OTA firmware update desde el cliente PC
- [ ] App de configuración (Wi-Fi credentials, layout, brillo)

### Extras completados
- [x] `client/discovery.py` — autodiscovery AP → mDNS → scan /24
- [x] `client/run_all.py` — launcher unificado con autorestart y logs coloreados
- [x] `arduino_libraries.txt` + `sketch.yaml` — build reproducible con Arduino CLI
- [x] `client/layouts/` — layouts JSON predefinidos (default, minimal, mirror)

---

## Mejoras potenciales

| Mejora | Impacto | Complejidad |
|---|---|---|
| Delta frames (dirty regions) | +40% FPS efectivo | Media |
| JPEG progresivo (decode parcial) | Reduce latencia percibida | Alta |
| Compresión LZ4 para frames no-foto | Mejor compresión que JPEG en texto/UI | Media |
| Doble buffer LVGL en PSRAM | Elimina tearing | Baja |
| Modo sleep + wake-on-notify | Bajo consumo para pantalla de notificaciones | Media |
| mDNS autodiscovery | Sin IP fija, conexión automática del cliente | Baja |
| Layout hot-reload | Cambiar widgets sin reflashear | Media |

---

## Dependencias de referencia

- Firmware: [tomasschus/esp32](https://github.com/tomasschus/esp32) — mismo hardware, pipeline JPEG ya probado
- Cliente PC ref: [tuckershannon/ESP32-Desktop-Monitor](https://github.com/tuckershannon/ESP32-Desktop-Monitor)
- Display driver ref: [lovyan03/LovyanGFX](https://github.com/lovyan03/LovyanGFX)

---

## Setup rápido

```bash
git clone https://github.com/<tu-usuario>/esp32-secondary-screen.git
```

Copiar `include/secrets.h.example` → `include/secrets.h` y poner credenciales Wi-Fi.

### Opción A — Arduino IDE 2.x

1. **Instalar el core ESP32**
   - Abrir Preferences → Additional boards manager URLs, agregar:
     ```
     https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
     ```
   - Tools → Board → Boards Manager → buscar **"esp32 by Espressif Systems"** → instalar versión **>= 3.x**.

2. **Instalar librerías**
   - Ver `arduino_libraries.txt` para la lista completa con versiones.
   - Sketch → Include Library → Manage Libraries → buscar e instalar cada una.
   - **ESPAsyncWebServer y AsyncTCP:** usar el fork de **mathieucarbou**, no el de me-no-dev
     (el original no compila con Core 3.x). Si no aparece el fork correcto en el Library Manager,
     instalar manualmente desde ZIP:
     - <https://github.com/mathieucarbou/ESPAsyncWebServer>
     - <https://github.com/mathieucarbou/AsyncTCP>

3. **Seleccionar board y opciones**
   - Tools → Board → **ESP32S3 Dev Module**
   - Configurar las siguientes opciones en el menú Tools:

     | Opción | Valor |
     |---|---|
     | USB CDC On Boot | **Enabled** |
     | USB Mode | **Hardware CDC and JTAG** |
     | Flash Size | **16MB (128Mb)** |
     | Flash Mode | **QIO 80MHz** |
     | PSRAM | **OPI PSRAM** |
     | Partition Scheme | **App3M, FAT9M, 16MB** |
     | Upload Speed | 921600 |
     | Core Debug Level | None |

4. **Compilar y flashear**
   - Sketch → Upload (o Ctrl+U).

5. **Instalar dependencias del cliente PC**
   ```bash
   pip install -r client/requirements.txt
   python client/screen_capture.py --host <IP_ESP32>
   ```

---

### Opción B — Arduino CLI (terminal)

Requiere tener `arduino-cli` instalado y el archivo `sketch.yaml` incluido en el repositorio.

```bash
# Instalar core y librerías declaradas en sketch.yaml
arduino-cli core install esp32:esp32 \
  --additional-urls https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
arduino-cli lib install --profile esp32s3_screen

# Compilar
arduino-cli compile --profile esp32s3_screen .

# Flashear (reemplazar COM3 con el puerto real)
arduino-cli upload --profile esp32s3_screen --port COM3 .
```

> Las opciones de board, versiones de plataforma y librerías están declaradas en `sketch.yaml`.

---

## Librerías Arduino

Detalle completo (con notas de qué fork usar) en `arduino_libraries.txt`.

| Librería | Versión mínima | Autor / fork | Notas |
|---|---|---|---|
| Arduino_GFX_Library | >= 1.4.7 | moononournation | Driver QSPI AXS15231B. Library Manager. |
| ESPAsyncWebServer | >= 3.3.12 | **mathieucarbou** | Usar este fork. El original (me-no-dev) no compila con Core 3.x. |
| AsyncTCP | >= 3.3.1 | **mathieucarbou** | Dep. de ESPAsyncWebServer. Mismo fork. |
| ArduinoJson | >= 7.2.0 | bblanchon | API v7 (incompatible con v6). Library Manager. |
| TJpgDec | >= 1.1.0 | Bodmer | Decode JPEG → RGB565. Library Manager. |
| lvgl | >= 9.2.2 | lvgl | Requiere copiar `include/lv_conf.h`. Library Manager. |

---

## Licencia

MIT
