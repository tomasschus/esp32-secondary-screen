#pragma once
#include <lvgl.h>
#include "ws_server.h"

/* Inicializa la pantalla de notificaciones LVGL */
void ui_notify_init(void);

/* Muestra una notificación entrante (auto-oculta a los 8 s) */
void ui_notify_show(const ws_notify_t *n);

/* Inicializa el panel de stats del sistema */
void ui_stats_init(void);

/* Actualiza los valores de stats en pantalla */
void ui_stats_update(const ws_stats_t *s);
