/*
 * ui_notifications.cpp
 *
 * Widget LVGL para mostrar notificaciones push y stats del sistema.
 *
 * Layout de notificación (aparece 8 s, luego se oculta):
 *
 *   ┌────────────────────────────┐
 *   │ 🔔  Slack                  │
 *   │ Juan → #general            │
 *   │ "Oye, ¿viste el PR?"       │
 *   └────────────────────────────┘
 *
 * Layout de stats (pantalla completa):
 *
 *   CPU  [████████░░] 82%
 *   RAM  [██████░░░░] 61%
 *   GPU  [████░░░░░░] 40%
 *   TEMP  65 °C
 */
#include "../ui_notifications.h"
#include <Arduino.h>

/* ── Notificaciones ──────────────────────────────────────────────────────── */

static lv_obj_t *s_notif_card  = nullptr;
static lv_obj_t *s_lbl_app     = nullptr;
static lv_obj_t *s_lbl_title   = nullptr;
static lv_obj_t *s_lbl_body    = nullptr;
static lv_timer_t *s_hide_timer = nullptr;

static void hide_notif_cb(lv_timer_t *t) {
    (void)t;
    if (s_notif_card) lv_obj_add_flag(s_notif_card, LV_OBJ_FLAG_HIDDEN);
    lv_timer_delete(s_hide_timer);
    s_hide_timer = nullptr;
}

void ui_notify_init(void) {
    lv_obj_t *scr = lv_screen_active();

    /* Card flotante en la esquina superior derecha */
    s_notif_card = lv_obj_create(scr);
    lv_obj_set_size(s_notif_card, 300, 90);
    lv_obj_align(s_notif_card, LV_ALIGN_TOP_RIGHT, -8, 8);
    lv_obj_set_style_bg_color(s_notif_card, lv_color_hex(0x1e1e1e), 0);
    lv_obj_set_style_border_color(s_notif_card, lv_color_hex(0x4a9eff), 0);
    lv_obj_set_style_border_width(s_notif_card, 1, 0);
    lv_obj_set_style_radius(s_notif_card, 8, 0);
    lv_obj_set_style_pad_all(s_notif_card, 8, 0);
    lv_obj_set_flex_flow(s_notif_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(s_notif_card, LV_OBJ_FLAG_HIDDEN);

    s_lbl_app = lv_label_create(s_notif_card);
    lv_obj_set_style_text_font(s_lbl_app, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_lbl_app, lv_color_hex(0x4a9eff), 0);

    s_lbl_title = lv_label_create(s_notif_card);
    lv_obj_set_style_text_font(s_lbl_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_title, lv_color_hex(0xffffff), 0);
    lv_label_set_long_mode(s_lbl_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_lbl_title, 280);

    s_lbl_body = lv_label_create(s_notif_card);
    lv_obj_set_style_text_font(s_lbl_body, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_lbl_body, lv_color_hex(0xaaaaaa), 0);
    lv_label_set_long_mode(s_lbl_body, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_lbl_body, 280);
}

void ui_notify_show(const ws_notify_t *n) {
    if (!s_notif_card) return;

    lv_label_set_text(s_lbl_app,   n->app);
    lv_label_set_text(s_lbl_title, n->title);
    lv_label_set_text(s_lbl_body,  n->body);

    lv_obj_clear_flag(s_notif_card, LV_OBJ_FLAG_HIDDEN);

    /* Auto-ocultar a los 8 segundos */
    if (s_hide_timer) lv_timer_reset(s_hide_timer);
    else s_hide_timer = lv_timer_create(hide_notif_cb, 8000, nullptr);
    lv_timer_set_repeat_count(s_hide_timer, 1);
}

/* ── Stats del sistema ───────────────────────────────────────────────────── */

static lv_obj_t *s_bar_cpu  = nullptr;
static lv_obj_t *s_bar_ram  = nullptr;
static lv_obj_t *s_bar_gpu  = nullptr;
static lv_obj_t *s_lbl_cpu  = nullptr;
static lv_obj_t *s_lbl_ram  = nullptr;
static lv_obj_t *s_lbl_gpu  = nullptr;
static lv_obj_t *s_lbl_temp = nullptr;

static lv_obj_t *make_stat_row(lv_obj_t *parent, const char *label,
                                lv_obj_t **out_bar, lv_obj_t **out_lbl) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 2, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *lbl_name = lv_label_create(row);
    lv_label_set_text(lbl_name, label);
    lv_obj_set_style_text_font(lbl_name, &lv_font_montserrat_14, 0);
    lv_obj_set_width(lbl_name, 50);

    lv_obj_t *bar = lv_bar_create(row);
    lv_obj_set_size(bar, 280, 16);
    lv_bar_set_range(bar, 0, 100);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x4a9eff), LV_PART_INDICATOR);
    *out_bar = bar;

    lv_obj_t *lbl_val = lv_label_create(row);
    lv_obj_set_style_text_font(lbl_val, &lv_font_montserrat_14, 0);
    lv_obj_set_width(lbl_val, 45);
    *out_lbl = lbl_val;

    return row;
}

void ui_stats_init(void) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0d0d0d), 0);

    lv_obj_t *cont = lv_obj_create(scr);
    lv_obj_set_size(cont, 460, 200);
    lv_obj_align(cont, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(cont, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 12, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(cont, 10, 0);

    make_stat_row(cont, "CPU",  &s_bar_cpu, &s_lbl_cpu);
    make_stat_row(cont, "RAM",  &s_bar_ram, &s_lbl_ram);
    make_stat_row(cont, "GPU",  &s_bar_gpu, &s_lbl_gpu);

    /* Temperatura */
    lv_obj_t *row_temp = lv_obj_create(cont);
    lv_obj_set_size(row_temp, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row_temp, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row_temp, 0, 0);
    lv_obj_set_style_pad_all(row_temp, 2, 0);

    lv_obj_t *lbl_tname = lv_label_create(row_temp);
    lv_label_set_text(lbl_tname, "TEMP");
    lv_obj_set_style_text_font(lbl_tname, &lv_font_montserrat_14, 0);

    s_lbl_temp = lv_label_create(row_temp);
    lv_obj_align_to(s_lbl_temp, lbl_tname, LV_ALIGN_OUT_RIGHT_MID, 10, 0);
    lv_obj_set_style_text_font(s_lbl_temp, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_lbl_temp, lv_color_hex(0xff8c42), 0);
    lv_label_set_text(s_lbl_temp, "-- °C");
}

void ui_stats_update(const ws_stats_t *s) {
    if (!s_bar_cpu) return;

    lv_bar_set_value(s_bar_cpu, s->cpu, LV_ANIM_ON);
    lv_bar_set_value(s_bar_ram, s->ram, LV_ANIM_ON);
    lv_bar_set_value(s_bar_gpu, s->gpu, LV_ANIM_ON);

    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", s->cpu);  lv_label_set_text(s_lbl_cpu, buf);
    snprintf(buf, sizeof(buf), "%d%%", s->ram);  lv_label_set_text(s_lbl_ram, buf);
    snprintf(buf, sizeof(buf), "%d%%", s->gpu);  lv_label_set_text(s_lbl_gpu, buf);
    snprintf(buf, sizeof(buf), "%d °C", s->temp); lv_label_set_text(s_lbl_temp, buf);

    /* Color de temperatura: verde < 70, amarillo < 85, rojo >= 85 */
    lv_color_t tc = s->temp >= 85 ? lv_color_hex(0xff3333) :
                    s->temp >= 70 ? lv_color_hex(0xffcc00) :
                                    lv_color_hex(0x44dd88);
    lv_obj_set_style_text_color(s_lbl_temp, tc, 0);
}
