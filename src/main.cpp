/*
 * main.cpp — ESP32 Secondary Screen
 *
 * Modos de operación (seleccionados por el cliente al conectar):
 *   1. Mirror / frame completo: recibe JPEG → dibuja directo al canvas (sin LVGL)
 *   2. Stats: recibe JSON stats → renderiza con LVGL
 *   3. Notificaciones: recibe JSON notify → overlay LVGL sobre el frame actual
 *
 * Hardware: JC3248W535EN (ESP32-S3 N16R8, display 480×320 QSPI AXS15231B)
 *
 * IMPORTANTE: no hay audio_mgr. El display se inicia directo.
 */
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <lvgl.h>

#include "dispcfg.h"
#include "pincfg.h"
#include "ui_layout.h"
#include "ui_notifications.h"
#include "ws_server.h"

/* ── Display ─────────────────────────────────────────────────────────────── */
static Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    TFT_CS, TFT_SCK, TFT_SDA0, TFT_SDA1, TFT_SDA2, TFT_SDA3);

static Arduino_GFX *panel =
    new Arduino_AXS15231B(bus, TFT_RST, 0, false, TFT_RES_W, TFT_RES_H);

static Arduino_Canvas *gfx =
    new Arduino_Canvas(TFT_RES_W, TFT_RES_H, panel, 0, 0, TFT_ROT);

/* Buffer de pantalla compartido con TJpgDec (RGB565, PSRAM) */
static uint16_t *s_screen_buf = nullptr;

static volatile bool s_frame_ready = false;
static volatile bool s_display_dirty = false;

/* ── LVGL ────────────────────────────────────────────────────────────────── */
static uint32_t lvgl_tick_cb(void) { return millis(); }

static void disp_flush_cb(lv_display_t *disp, const lv_area_t *area,
                           uint8_t *px_map) {
    gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)px_map,
                             lv_area_get_width(area), lv_area_get_height(area));
    s_display_dirty = true;
    lv_disp_flush_ready(disp);
}

/* ── Callbacks del servidor WebSocket ───────────────────────────────────── */
static void on_frame(void) {
    /* JPEG decodificado en s_screen_buf — copiar al canvas en el loop principal */
    s_frame_ready = true;
}

static void on_notify(const ws_notify_t *n) {
    Serial.printf("[Main] notif: %s | %s\n", n->app, n->title);
    ui_notify_show(n);
}

static void on_stats(const ws_stats_t *s) {
    ui_stats_update(s);
}

static void on_layout(const char *json, size_t len) {
    Serial.printf("[Main] layout recibido (%u bytes)\n", (unsigned)len);
    ui_layout_apply(json, len);
}

static void on_time(uint32_t epoch) {
    Serial.printf("[Main] time recibido: epoch=%lu\n", (unsigned long)epoch);
    ui_layout_set_time(epoch);
}

/* ── setup ───────────────────────────────────────────────────────────────── */
void setup() {
#ifdef ARDUINO_USB_CDC_ON_BOOT
    delay(3000);
#endif
    Serial.begin(115200);
    Serial.println("[Main] === Boot iniciado ===");
    Serial.printf("[Main] PSRAM total: %u bytes\n", ESP.getPsramSize());
    Serial.printf("[Main] Heap libre: %u bytes\n", ESP.getFreeHeap());

    /* Buffer de pantalla en PSRAM */
    Serial.println("[Main] Allocando screen_buf...");
    s_screen_buf = (uint16_t *)heap_caps_malloc(
        (size_t)WS_SCREEN_W * WS_SCREEN_H * 2,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_screen_buf) {
        Serial.println("[Main] ERROR: sin PSRAM para screen_buf");
        while (1);
    }
    memset(s_screen_buf, 0, (size_t)WS_SCREEN_W * WS_SCREEN_H * 2);
    Serial.println("[Main] screen_buf OK");

    /* Display */
    Serial.println("[Main] Iniciando display...");
    if (!gfx->begin(40000000UL)) {
        Serial.println("[Main] ERROR: display");
        while (1);
    }
    gfx->fillScreen(BLACK);
    gfx->flush();
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);
    Serial.println("[Main] Display OK, backlight ON");

    /* LVGL */
    Serial.println("[Main] Iniciando LVGL...");
    lv_init();
    lv_tick_set_cb(lvgl_tick_cb);

    uint32_t buf_px = (uint32_t)gfx->width() * gfx->height() / 10;
    Serial.printf("[Main] Allocando LVGL buf: %u bytes (heap libre: %u)\n",
                  buf_px * sizeof(lv_color_t), ESP.getFreeHeap());
    lv_color_t *lvgl_buf = (lv_color_t *)heap_caps_malloc(
        buf_px * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!lvgl_buf) {
        Serial.println("[Main] ERROR: buffer LVGL");
        while (1);
    }

    lv_display_t *disp = lv_display_create(gfx->width(), gfx->height());
    lv_display_set_flush_cb(disp, disp_flush_cb);
    lv_display_set_buffers(disp, lvgl_buf, NULL,
                           buf_px * sizeof(lv_color_t),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    Serial.println("[Main] LVGL OK");

    /* UI: inicializar con layout por defecto */
    Serial.println("[Main] Iniciando UI...");
    ui_layout_init();
    Serial.println("[Main] UI OK");

    /* Servidor WebSocket (incluye init Wi-Fi) */
    Serial.println("[Main] Iniciando servidor WS...");
    if (!ws_server_start(s_screen_buf, on_frame, on_notify, on_stats,
                         on_layout, on_time)) {
        Serial.println("[Main] ERROR: ws_server_start");
        while (1);
    }

    Serial.println("[Main] === Listo. Esperando cliente... ===");
}

/* ── loop ────────────────────────────────────────────────────────────────── */
#define FLUSH_INTERVAL_MS 50

void loop() {
    /* Volcar frame JPEG al canvas cuando haya uno nuevo */
    if (s_frame_ready) {
        s_frame_ready = false;
        gfx->draw16bitRGBBitmap(0, 0, s_screen_buf, WS_SCREEN_W, WS_SCREEN_H);
        s_display_dirty = true;
    }

    lv_task_handler();

    static uint32_t last_flush = 0;
    uint32_t now = millis();
    if (s_display_dirty || (now - last_flush) >= FLUSH_INTERVAL_MS) {
        s_display_dirty = false;
        last_flush = now;
        gfx->flush();
    }
}
