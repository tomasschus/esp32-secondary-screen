#pragma once
#include <stdint.h>
#include <stddef.h>

/* ── Protocolo de mensajes entrantes ────────────────────────────────────────
 *
 * Binario  → frame JPEG completo (pantalla completa 480×320)
 *            Puede llegar fragmentado; se ensambla antes de decodificar.
 *
 * Texto    → JSON con campo "t":
 *   {"t":"frame"}          — señal de que viene un JPEG (legacy, opcional)
 *   {"t":"notify", "app":"Slack", "title":"...", "body":"..."}
 *   {"t":"stats",  "cpu":42, "ram":61, "gpu":80, "temp":65}
 *   {"t":"ping"}           — keepalive, el ESP responde "pong"
 *   {"t":"layout", "bg":"#rrggbb", "widgets":[...]}
 *                          — redefine el layout LVGL en tiempo real
 *   {"t":"time", "epoch":1234567890}
 *                          — sincroniza la hora del reloj interno
 *
 * ────────────────────────────────────────────────────────────────────────── */

#define WS_SCREEN_W  480
#define WS_SCREEN_H  320
#define WS_JPEG_MAX  (120 * 1024)
#define WS_TEXT_MAX  (2  * 1024)

/* Payload de notificación */
typedef struct {
    char app[32];
    char title[64];
    char body[256];
} ws_notify_t;

/* Payload de stats del sistema */
typedef struct {
    uint8_t cpu;   /* porcentaje 0-100 */
    uint8_t ram;
    uint8_t gpu;
    int8_t  temp;  /* °C */
} ws_stats_t;

/* Callbacks */
typedef void (*ws_on_frame_t)(void);                  /* JPEG decodificado en buf */
typedef void (*ws_on_notify_t)(const ws_notify_t *n);
typedef void (*ws_on_stats_t)(const ws_stats_t *s);
typedef void (*ws_on_layout_t)(const char *json, size_t len); /* layout JSON crudo */
typedef void (*ws_on_time_t)(uint32_t epoch);                 /* sincronización hora */

/* API pública */
bool ws_server_start(uint16_t        *screen_buf,
                     ws_on_frame_t    on_frame,
                     ws_on_notify_t   on_notify,
                     ws_on_stats_t    on_stats,
                     ws_on_layout_t   on_layout,
                     ws_on_time_t     on_time);

void ws_server_stop(void);
bool ws_server_is_running(void);
bool ws_server_has_client(void);

/* Enviar texto al cliente conectado (ej: "pong") */
void ws_server_send(const char *text);
