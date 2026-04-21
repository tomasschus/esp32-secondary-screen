#pragma once
/*
 * ui_layout.h
 *
 * Sistema de layout JSON configurable para LVGL.
 *
 * El cliente envía un mensaje WebSocket de tipo "t":"layout" con la
 * definición de widgets; ui_layout_apply() parsea ese JSON y recrea
 * la pantalla LVGL según la definición recibida.
 *
 * Tipos de widget soportados:
 *   "stats_panel"    — panel CPU/RAM/GPU/TEMP (reutiliza ui_stats_*)
 *   "clock"          — reloj HH:MM actualizado cada 60 s
 *   "label"          — etiqueta de texto estático configurable
 *   "notify_overlay" — card flotante de notificaciones (reutiliza ui_notify_*)
 *   "image"          — placeholder: rectángulo con borde (JPEG futuro)
 *
 * Integración con main.cpp:
 *   1. Llamar ui_layout_init() en setup(), DESPUÉS de lv_init() y de
 *      registrar el display LVGL.
 *   2. En el handler "t":"layout" del WebSocket, llamar
 *      ui_layout_apply(json, len).
 *   3. En el handler "t":"time", llamar ui_layout_set_time(epoch).
 */

#include <lvgl.h>
#include <stddef.h>
#include <stdint.h>

/* ── Tipos de widget ──────────────────────────────────────────────────────── */
typedef enum {
    UI_WIDGET_UNKNOWN        = 0,
    UI_WIDGET_STATS_PANEL    = 1,
    UI_WIDGET_CLOCK          = 2,
    UI_WIDGET_LABEL          = 3,
    UI_WIDGET_NOTIFY_OVERLAY = 4,
    UI_WIDGET_IMAGE          = 5,
} ui_widget_type_t;

/* ── API pública ──────────────────────────────────────────────────────────── */

/**
 * Inicializa el sistema de layout con el layout por defecto hardcodeado:
 * stats_panel centrado + notify_overlay en esquina superior derecha.
 * Debe llamarse en setup() después de que LVGL esté operativo.
 */
void ui_layout_init(void);

/**
 * Aplica un layout definido en JSON.
 * Borra todos los objetos del screen activo y recrea los widgets indicados.
 *
 * @param json  Buffer con el JSON completo (no necesita '\0' terminal).
 * @param len   Longitud del JSON en bytes.
 * @return true si el JSON se parseó y aplicó correctamente.
 *
 * Formato esperado:
 * {
 *   "t": "layout",
 *   "bg": "#0d0d0d",           // opcional — color de fondo
 *   "widgets": [
 *     {"type": "stats_panel", "x": 0,   "y": 0,   "w": 460, "h": 200},
 *     {"type": "clock",       "x": 380, "y": 280, "w": 90,  "h": 30, "font": 14},
 *     {"type": "label",       "x": 10,  "y": 290, "w": 200, "h": 25,
 *                              "text": "ESP32 Screen", "color": "#4a9eff", "font": 12},
 *     {"type": "notify_overlay"},
 *     {"type": "image",       "x": 0,   "y": 0,   "w": 480, "h": 320}
 *   ]
 * }
 */
bool ui_layout_apply(const char *json, size_t len);

/**
 * Guarda la IP del servidor WebSocket y la muestra en el layout por defecto.
 * Llamar una vez en setup() después de ws_server_start().
 * Persiste entre reinicios del layout (ui_layout_init la redibuija).
 *
 * @param ip  Cadena con la IP, ej: "192.168.0.138"
 */
void ui_layout_set_ip(const char *ip);

/**
 * Actualiza la hora interna usada por el widget clock.
 * Llamar cuando se reciba {"t":"time","epoch":1234567890} via WebSocket.
 *
 * @param epoch  Unix timestamp (segundos desde 1970-01-01 UTC).
 */
void ui_layout_set_time(uint32_t epoch);
