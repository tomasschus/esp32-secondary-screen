/*
 * ui_layout.cpp
 *
 * Sistema de layout JSON configurable para LVGL 9.2.
 *
 * Flujo principal:
 *   ui_layout_apply() recibe el JSON completo, lo parsea con ArduinoJson,
 *   limpia el screen activo con lv_obj_clean() y recrea todos los widgets
 *   según la lista "widgets" del JSON.
 *
 * Gestión de tiempo:
 *   No hay RTC en el hardware. Si el cliente envía {"t":"time","epoch":N},
 *   ui_layout_set_time() guarda el epoch y la marca de millis() del momento.
 *   El ticker del clock calcula la hora actual como:
 *       epoch_base + (millis() - millis_base) / 1000
 *   Sin esa llamada, el clock muestra el tiempo transcurrido desde el boot
 *   en formato MM:SS como fallback visible.
 */
#include "../ui_layout.h"
#include "../ui_notifications.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <cstring>
#include <cstdio>

/* ── Estado de tiempo ─────────────────────────────────────────────────────── */

static uint32_t s_epoch_base  = 0;   /* epoch recibido del cliente (0 = no sincronizado) */
static uint32_t s_millis_base = 0;   /* millis() en el momento de recibir el epoch */

/* ── IP del servidor WebSocket ────────────────────────────────────────────── */
static char s_ip[24] = "";           /* guardada para redibujar en cada ui_layout_init() */

/* ── Estado del clock LVGL ────────────────────────────────────────────────── */

static lv_obj_t   *s_clock_label = nullptr;
static lv_timer_t *s_clock_timer = nullptr;

/* ── Helpers ──────────────────────────────────────────────────────────────── */

/**
 * Convierte una cadena "#rrggbb" en lv_color_t.
 * Si el formato no es válido, devuelve negro.
 */
static lv_color_t hex_to_lv_color(const char *hex) {
    if (!hex || hex[0] != '#' || strlen(hex) < 7) {
        return lv_color_hex(0x000000);
    }
    unsigned int r = 0, g = 0, b = 0;
    /* sscanf en Arduino puede no soportar %2x directamente; usamos strtoul parcial */
    char tmp[3] = {0, 0, 0};
    tmp[0] = hex[1]; tmp[1] = hex[2];
    r = (unsigned int)strtoul(tmp, nullptr, 16);
    tmp[0] = hex[3]; tmp[1] = hex[4];
    g = (unsigned int)strtoul(tmp, nullptr, 16);
    tmp[0] = hex[5]; tmp[1] = hex[6];
    b = (unsigned int)strtoul(tmp, nullptr, 16);
    return lv_color_make((uint8_t)r, (uint8_t)g, (uint8_t)b);
}

/**
 * Devuelve el puntero a la fuente Montserrat más cercana al tamaño pedido.
 * Sólo se mapean los tamaños compilados en lv_conf.h.
 */
static const lv_font_t *font_for_size(int size) {
    /* Solo los tamaños habilitados en lv_conf.h: 12, 14, 16 */
    if (size <= 12) return &lv_font_montserrat_12;
    if (size <= 14) return &lv_font_montserrat_14;
    return &lv_font_montserrat_16;
}

/* ── Ticker del reloj ─────────────────────────────────────────────────────── */

/**
 * Calcula la hora actual y la vuelca en el label del clock.
 * Si el epoch está sincronizado muestra HH:MM; si no, muestra
 * el tiempo transcurrido desde el boot en formato MM:SS como fallback.
 */
static void clock_tick_cb(lv_timer_t *t) {
    (void)t;
    if (!s_clock_label) return;

    char buf[8];
    if (s_epoch_base != 0) {
        /* Hora sincronizada: epoch + tiempo transcurrido */
        uint32_t now_epoch = s_epoch_base + (millis() - s_millis_base) / 1000UL;
        /* Calcular HH:MM en UTC (sin librería de calendario) */
        uint32_t day_sec = now_epoch % 86400UL;
        uint8_t  hh      = (uint8_t)(day_sec / 3600);
        uint8_t  mm      = (uint8_t)((day_sec % 3600) / 60);
        snprintf(buf, sizeof(buf), "%02u:%02u", hh, mm);
    } else {
        /* Fallback: tiempo desde boot en MM:SS */
        uint32_t s_total = millis() / 1000UL;
        uint8_t  mm      = (uint8_t)((s_total / 60) % 60);
        uint8_t  ss      = (uint8_t)(s_total % 60);
        snprintf(buf, sizeof(buf), "%02u:%02u", mm, ss);
    }
    lv_label_set_text(s_clock_label, buf);
}

/* ── Creadores de widgets ─────────────────────────────────────────────────── */

/**
 * Crea el panel de estadísticas del sistema (CPU/RAM/GPU/TEMP).
 * Reutiliza ui_stats_init() de ui_notifications.cpp, que ya sabe
 * leer lv_screen_active() y crear el contenedor centrado.
 *
 * Si se especifican x/y/w/h en el JSON se aplican DESPUÉS de la init
 * para sobreescribir el posicionado por defecto.
 */
static void create_stats_panel(lv_obj_t *scr,
                                int x, int y, int w, int h,
                                bool has_pos) {
    (void)scr; /* ui_stats_init usa lv_screen_active() internamente */
    ui_stats_init();

    if (has_pos) {
        /* El contenedor que crea ui_stats_init es el último hijo del screen */
        lv_obj_t *cont = lv_obj_get_child(lv_screen_active(),
                                          lv_obj_get_child_count(lv_screen_active()) - 1);
        if (cont) {
            lv_obj_set_pos(cont, x, y);
            lv_obj_set_size(cont, w, h);
        }
    }
}

/**
 * Crea un reloj digital HH:MM (o MM:SS fallback) actualizado cada 60 s.
 */
static void create_clock(lv_obj_t *scr,
                          int x, int y, int w, int h, int font_size) {
    /* Cancelar timer anterior si existe */
    if (s_clock_timer) {
        lv_timer_delete(s_clock_timer);
        s_clock_timer = nullptr;
    }
    s_clock_label = nullptr;

    lv_obj_t *cont = lv_obj_create(scr);
    lv_obj_set_pos(cont, x, y);
    lv_obj_set_size(cont, w, h);
    lv_obj_set_style_bg_color(cont, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 2, 0);
    lv_obj_set_style_radius(cont, 4, 0);

    s_clock_label = lv_label_create(cont);
    lv_obj_set_style_text_font(s_clock_label, font_for_size(font_size), 0);
    lv_obj_set_style_text_color(s_clock_label, lv_color_hex(0xffffff), 0);
    lv_obj_center(s_clock_label);

    /* Mostrar hora inmediatamente */
    clock_tick_cb(nullptr);

    /* Timer: actualiza cada 60 segundos (1 minuto) */
    s_clock_timer = lv_timer_create(clock_tick_cb, 60000, nullptr);
}

/**
 * Crea un label de texto estático con color y fuente configurables.
 */
static void create_label(lv_obj_t *scr,
                          int x, int y, int w, int h,
                          const char *text,
                          const char *color_hex,
                          int font_size) {
    lv_obj_t *lbl = lv_label_create(scr);
    lv_obj_set_pos(lbl, x, y);
    lv_obj_set_size(lbl, w, h);
    lv_obj_set_style_text_font(lbl, font_for_size(font_size), 0);

    lv_color_t col = (color_hex && color_hex[0] == '#')
                     ? hex_to_lv_color(color_hex)
                     : lv_color_hex(0xffffff);
    lv_obj_set_style_text_color(lbl, col, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_label_set_text(lbl, text ? text : "");
}

/**
 * Inicializa el overlay de notificaciones.
 * Si se especifican x/y/w/h sobreescribe la posición por defecto del card.
 * Reutiliza ui_notify_init() de ui_notifications.cpp.
 */
static void create_notify_overlay(lv_obj_t *scr, bool has_pos,
                                   int x, int y, int w, int h) {
    (void)scr;
    ui_notify_init();

    if (has_pos) {
        /* El card es el último hijo añadido por ui_notify_init */
        lv_obj_t *card = lv_obj_get_child(lv_screen_active(),
                                           lv_obj_get_child_count(lv_screen_active()) - 1);
        if (card) {
            lv_obj_set_pos(card, x, y);
            lv_obj_set_size(card, w, h);
        }
    }
}

/**
 * Crea un placeholder para imagen: rectángulo oscuro con borde punteado.
 * Cuando llegue el JPEG por WebSocket se dibujará encima en el canvas GFX.
 */
static void create_image_placeholder(lv_obj_t *scr,
                                      int x, int y, int w, int h) {
    lv_obj_t *box = lv_obj_create(scr);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, w, h);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x0a0a0a), 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_radius(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);

    /* Texto indicador centrado */
    lv_obj_t *lbl = lv_label_create(box);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x444444), 0);
    lv_label_set_text(lbl, "[image]");
    lv_obj_center(lbl);
}

/* ── Limpieza de estado de widgets con ciclo de vida propio ──────────────── */

/**
 * Invalida los punteros de objetos LVGL que pueden quedar colgados
 * tras lv_obj_clean(). Debe llamarse ANTES de limpiar el screen.
 */
static void reset_widget_state(void) {
    /* El timer del clock referencia s_clock_label; eliminarlo primero */
    if (s_clock_timer) {
        lv_timer_delete(s_clock_timer);
        s_clock_timer = nullptr;
    }
    s_clock_label = nullptr;
    /* Los punteros internos de ui_notifications también quedan inválidos;
       se reinicializan con ui_notify_init() / ui_stats_init() en la
       siguiente llamada a create_*. */
}

/* ── Layout por defecto ───────────────────────────────────────────────────── */

/* ── API pública ──────────────────────────────────────────────────────────── */

void ui_layout_set_ip(const char *ip) {
    if (ip) strlcpy(s_ip, ip, sizeof(s_ip));
}

void ui_layout_init(void) {
    Serial.println("[Layout] Aplicando layout por defecto");

    /* Construir JSON dinámico para incluir la IP si está disponible */
    char json[512];
    if (s_ip[0] != '\0') {
        char ip_text[48];
        snprintf(ip_text, sizeof(ip_text), "ws://%s:8080", s_ip);
        snprintf(json, sizeof(json),
            "{"
            "\"t\":\"layout\","
            "\"bg\":\"#0d0d0d\","
            "\"widgets\":["
            "{\"type\":\"stats_panel\",\"x\":10,\"y\":10,\"w\":460,\"h\":200},"
            "{\"type\":\"label\","
             "\"x\":10,\"y\":295,\"w\":460,\"h\":20,"
             "\"text\":\"%s\","
             "\"color\":\"#334455\","
             "\"font\":12},"
            "{\"type\":\"notify_overlay\"}"
            "]}",
            ip_text);
    } else {
        snprintf(json, sizeof(json),
            "{"
            "\"t\":\"layout\","
            "\"bg\":\"#0d0d0d\","
            "\"widgets\":["
            "{\"type\":\"stats_panel\",\"x\":10,\"y\":10,\"w\":460,\"h\":200},"
            "{\"type\":\"notify_overlay\"}"
            "]}");
    }
    ui_layout_apply(json, strlen(json));
}

bool ui_layout_apply(const char *json, size_t len) {
    if (!json || len == 0) {
        Serial.println("[Layout] ERROR: JSON vacío");
        return false;
    }

    /* Parsear JSON — usamos un JsonDocument de tamaño generoso en heap */
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json, len);
    if (err != DeserializationError::Ok) {
        Serial.printf("[Layout] ERROR: deserializeJson -> %s\n", err.c_str());
        return false;
    }

    /* Verificar que sea tipo "layout" */
    const char *tipo = doc["t"] | "";
    if (strcmp(tipo, "layout") != 0) {
        Serial.printf("[Layout] ERROR: tipo inesperado '%s'\n", tipo);
        return false;
    }

    /* Invalidar objetos con ciclo de vida propio antes de limpiar */
    reset_widget_state();

    /* Limpiar todos los objetos del screen activo */
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);

    /* Color de fondo */
    const char *bg_hex = doc["bg"] | "#0d0d0d";
    lv_obj_set_style_bg_color(scr, hex_to_lv_color(bg_hex), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* Iterar sobre la lista de widgets */
    JsonArray widgets = doc["widgets"].as<JsonArray>();
    if (widgets.isNull()) {
        Serial.println("[Layout] Aviso: no hay widgets en el JSON");
        return true;
    }

    int count = 0;
    for (JsonObject w : widgets) {
        const char *type_str = w["type"] | "";

        /* Posición y tamaño — opcionales según el widget */
        int  x        = w["x"] | 0;
        int  y        = w["y"] | 0;
        int  ww       = w["w"] | 0;
        int  wh       = w["h"] | 0;
        bool has_pos  = w.containsKey("x") || w.containsKey("y")
                        || w.containsKey("w") || w.containsKey("h");

        if (strcmp(type_str, "stats_panel") == 0) {
            create_stats_panel(scr, x, y, ww, wh, has_pos);
            Serial.printf("[Layout]   stats_panel @ (%d,%d) %dx%d\n", x, y, ww, wh);

        } else if (strcmp(type_str, "clock") == 0) {
            int font_size = w["font"] | 16;
            create_clock(scr, x, y, ww ? ww : 90, wh ? wh : 30, font_size);
            Serial.printf("[Layout]   clock @ (%d,%d) font=%d\n", x, y, font_size);

        } else if (strcmp(type_str, "label") == 0) {
            const char *text      = w["text"]  | "";
            const char *color_hex = w["color"] | "#ffffff";
            int         font_size = w["font"]  | 14;
            create_label(scr, x, y, ww ? ww : 200, wh ? wh : 30,
                         text, color_hex, font_size);
            Serial.printf("[Layout]   label '%s' @ (%d,%d)\n", text, x, y);

        } else if (strcmp(type_str, "notify_overlay") == 0) {
            create_notify_overlay(scr, has_pos, x, y, ww, wh);
            Serial.println("[Layout]   notify_overlay");

        } else if (strcmp(type_str, "image") == 0) {
            create_image_placeholder(scr, x, y,
                                     ww ? ww : 480, wh ? wh : 320);
            Serial.printf("[Layout]   image placeholder @ (%d,%d) %dx%d\n",
                          x, y, ww, wh);

        } else {
            Serial.printf("[Layout]   Aviso: tipo desconocido '%s', ignorado\n",
                          type_str);
        }
        count++;
    }

    Serial.printf("[Layout] Layout aplicado: %d widget(s)\n", count);
    return true;
}

void ui_layout_set_time(uint32_t epoch) {
    s_epoch_base  = epoch;
    s_millis_base = millis();
    Serial.printf("[Layout] Hora sincronizada: epoch=%lu\n",
                  (unsigned long)epoch);

    /* Actualizar el clock en pantalla inmediatamente si existe */
    if (s_clock_label) {
        clock_tick_cb(nullptr);
    }
}
