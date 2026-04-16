# ESP32 Secondary Screen

Proyecto para usar un ESP32 como pantalla secundaria.

## Descripción

Este proyecto implementa una pantalla secundaria basada en ESP32 que puede recibir contenido desde una PC u otro dispositivo y mostrarlo en un display conectado al microcontrolador.

## Hardware

- ESP32 (módulo a definir: ESP32-WROOM, ESP32-S3, etc.)
- Display (a definir: TFT SPI, OLED I2C, ST7789, ILI9341, etc.)
- Cables / PCB / fuente de alimentación

## Software

- Entorno de desarrollo: _(Arduino IDE / PlatformIO / ESP-IDF)_
- Librerías principales: _(por definir: TFT_eSPI, LVGL, etc.)_

## Estructura del repositorio

```
.
├── src/            # Código fuente del firmware
├── include/        # Headers
├── lib/            # Librerías locales
├── docs/           # Documentación, esquemáticos, imágenes
├── platformio.ini  # Config de PlatformIO (si aplica)
└── README.md
```

## Cómo empezar

1. Clonar el repo:
   ```bash
   git clone https://github.com/<tu-usuario>/esp32-secondary-screen.git
   cd esp32-secondary-screen
   ```
2. Abrir el proyecto en tu entorno (PlatformIO / Arduino IDE).
3. Configurar credenciales Wi-Fi si corresponde (ver `secrets.h.example`).
4. Compilar y flashear al ESP32.

## Roadmap

- [ ] Definir hardware (módulo ESP32 + display)
- [ ] Prototipo de conexión y test del display
- [ ] Protocolo de comunicación con la PC (USB serial / Wi-Fi / BLE)
- [ ] Cliente en la PC para enviar la señal
- [ ] Optimización de rendimiento y refresco

## Licencia

_(Definir licencia: MIT, GPL-3.0, etc.)_
