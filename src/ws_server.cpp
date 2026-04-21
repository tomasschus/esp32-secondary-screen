/*
 * ws_server.cpp
 *
 * Servidor WebSocket para pantalla secundaria.
 *
 * Modos Wi-Fi soportados:
 *   - AP propio (SCREEN_AP_SSID) si secrets.h no define WIFI_SSID
 *   - STA conectado a la red del usuario si WIFI_SSID está definido
 *
 * Puerto: 8080, path: /ws
 *
 * Mensajes binarios  → JPEG completo (480×320), puede llegar fragmentado
 * Mensajes de texto  → JSON {"t":"notify"|"stats"|"ping"}
 */
#include "ws_server.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <TJpg_Decoder.h>
#include <WiFi.h>
#include <cstring>

/* Credenciales — si no existe secrets.h, arranca en modo AP */
#if __has_include("secrets.h")
#  include "secrets.h"
#  define USE_STA_MODE
#else
#  define WIFI_SSID     ""
#  define WIFI_PASSWORD ""
#endif

#define SCREEN_AP_SSID  "ESP32-SCREEN"
#define SCREEN_AP_PASS  "esp32screen"
#define WS_PORT         8080

static AsyncWebServer     *s_server     = nullptr;
static AsyncWebSocket     *s_ws         = nullptr;
static uint16_t           *s_screen_buf = nullptr;
static ws_on_frame_t       s_on_frame   = nullptr;
static ws_on_notify_t      s_on_notify  = nullptr;
static ws_on_stats_t       s_on_stats   = nullptr;
static ws_on_layout_t      s_on_layout     = nullptr;
static ws_on_time_t        s_on_time       = nullptr;
static ws_on_disconnect_t  s_on_disconnect = nullptr;
static bool                s_has_client    = false;
static char                s_ip_str[24]    = "0.0.0.0";
static uint8_t            *s_jpeg_buf   = nullptr;
static char               *s_text_buf   = nullptr;

/* ── TJpgDec callback: copia pixels RGB565 al buffer de pantalla ─────────── */
static bool jpeg_output_cb(int16_t x, int16_t y, uint16_t w, uint16_t h,
                            uint16_t *bitmap) {
    if (!s_screen_buf) return false;
    if ((uint16_t)(y + h) > WS_SCREEN_H || (uint16_t)(x + w) > WS_SCREEN_W)
        return true;
    for (uint16_t row = 0; row < h; row++) {
        memcpy(s_screen_buf + (y + row) * WS_SCREEN_W + x,
               bitmap + row * w,
               (size_t)w * 2);
    }
    return true;
}

/* ── Parsers de mensajes de texto ────────────────────────────────────────── */
static void handle_notify(const char *json, size_t len) {
    if (!s_on_notify) return;
    JsonDocument doc;
    if (deserializeJson(doc, json, len) != DeserializationError::Ok) return;

    static ws_notify_t n;
    strlcpy(n.app,   doc["app"]   | "?",   sizeof(n.app));
    strlcpy(n.title, doc["title"] | "",    sizeof(n.title));
    strlcpy(n.body,  doc["body"]  | "",    sizeof(n.body));
    s_on_notify(&n);
}

static void handle_stats(const char *json, size_t len) {
    if (!s_on_stats) return;
    JsonDocument doc;
    if (deserializeJson(doc, json, len) != DeserializationError::Ok) return;

    static ws_stats_t s;
    s.cpu  = doc["cpu"]  | 0;
    s.ram  = doc["ram"]  | 0;
    s.gpu  = doc["gpu"]  | 0;
    s.temp = doc["temp"] | 0;
    s_on_stats(&s);
}

static void handle_layout(const char *json, size_t len) {
    if (s_on_layout) s_on_layout(json, len);
}

static void handle_time(const char *json, size_t len) {
    if (!s_on_time) return;
    JsonDocument doc;
    if (deserializeJson(doc, json, len) != DeserializationError::Ok) return;
    uint32_t epoch = doc["epoch"] | 0;
    if (epoch > 0) s_on_time(epoch);
}

/* ── WebSocket event handler ─────────────────────────────────────────────── */
static void on_ws_event(AsyncWebSocket *ws, AsyncWebSocketClient *client,
                        AwsEventType type, void *arg, uint8_t *data,
                        size_t len) {
    (void)ws;

    if (type == WS_EVT_CONNECT) {
        Serial.printf("[WS] cliente %u conectado\n", client->id());
        s_has_client = true;
        client->text("{\"t\":\"hello\",\"w\":480,\"h\":320}");
        return;
    }
    if (type == WS_EVT_DISCONNECT) {
        Serial.printf("[WS] cliente %u desconectado\n", client->id());
        s_has_client = false;
        if (s_on_disconnect) s_on_disconnect();
        return;
    }
    if (type != WS_EVT_DATA || len == 0) return;

    AwsFrameInfo *info = (AwsFrameInfo *)arg;

    /* ── JPEG binario ──────────────────────────────────────────────────── */
    if (info->message_opcode == WS_BINARY) {
        if (!s_screen_buf || !s_on_frame) return;

        if (!s_jpeg_buf) {
            s_jpeg_buf = (uint8_t *)heap_caps_malloc(
                WS_JPEG_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!s_jpeg_buf)
                s_jpeg_buf = (uint8_t *)heap_caps_malloc(
                    WS_JPEG_MAX, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (!s_jpeg_buf) {
                Serial.println("[WS] ERROR: sin memoria para jpeg_buf");
                return;
            }
        }

        if (info->index + len > WS_JPEG_MAX) {
            Serial.printf("[WS] JPEG muy grande (%llu bytes), descartado\n", info->len);
            return;
        }

        memcpy(s_jpeg_buf + info->index, data, len);
        if (info->index + len < info->len || !info->final) return;

        TJpgDec.setCallback(jpeg_output_cb);
        JRESULT r = TJpgDec.drawJpg(0, 0, s_jpeg_buf, (uint32_t)info->len);
        if (r == JDR_OK) {
            s_on_frame();
        } else {
            Serial.printf("[WS] JPEG error %d\n", (int)r);
        }
        return;
    }

    /* ── Texto JSON ────────────────────────────────────────────────────── */
    if (info->message_opcode == WS_TEXT) {
        if (!s_text_buf) {
            s_text_buf = (char *)heap_caps_malloc(
                WS_TEXT_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!s_text_buf)
                s_text_buf = (char *)heap_caps_malloc(
                    WS_TEXT_MAX, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (!s_text_buf) {
                Serial.println("[WS] ERROR: sin memoria para text_buf");
                return;
            }
        }

        if (info->index + len >= WS_TEXT_MAX) return;
        memcpy(s_text_buf + info->index, data, len);
        if (info->index + len < info->len || !info->final) return;

        size_t total = (size_t)info->len;
        s_text_buf[total] = '\0';

        /* Detectar tipo parseando el campo "t" con ArduinoJson
           (el strstr manual fallaba con JSON que usa ': ' con espacio) */
        JsonDocument hdr;
        if (deserializeJson(hdr, s_text_buf, total) != DeserializationError::Ok) return;
        const char *t = hdr["t"] | "";

        if      (strcmp(t, "notify") == 0) handle_notify(s_text_buf, total);
        else if (strcmp(t, "stats")  == 0) handle_stats(s_text_buf, total);
        else if (strcmp(t, "layout") == 0) handle_layout(s_text_buf, total);
        else if (strcmp(t, "time")   == 0) handle_time(s_text_buf, total);
        else if (strcmp(t, "ping")   == 0) ws_server_send("pong");
    }
}

/* ── ws_server_start ─────────────────────────────────────────────────────── */
bool ws_server_start(uint16_t           *screen_buf,
                     ws_on_frame_t       on_frame,
                     ws_on_notify_t      on_notify,
                     ws_on_stats_t       on_stats,
                     ws_on_layout_t      on_layout,
                     ws_on_time_t        on_time,
                     ws_on_disconnect_t  on_disconnect) {
    if (s_server) return true;
    if (!screen_buf || !on_frame) return false;

    s_screen_buf    = screen_buf;
    s_on_frame      = on_frame;
    s_on_notify     = on_notify;
    s_on_stats      = on_stats;
    s_on_layout     = on_layout;
    s_on_time       = on_time;
    s_on_disconnect = on_disconnect;

#ifdef USE_STA_MODE
    /* Conectar a red existente */
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.printf("[WS] Conectando a %s", WIFI_SSID);
    uint8_t retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 20) {
        delay(500);
        Serial.print(".");
        retries++;
    }
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("\n[WS] Wi-Fi falló, cambiando a modo AP");
        goto start_ap;
    }
    strlcpy(s_ip_str, WiFi.localIP().toString().c_str(), sizeof(s_ip_str));
    Serial.printf("\n[WS] IP: %s\n", s_ip_str);
    goto start_server;
#endif

start_ap:
    WiFi.mode(WIFI_AP);
    if (!WiFi.softAP(SCREEN_AP_SSID, SCREEN_AP_PASS, 1, 0, 4)) {
        Serial.println("[WS] ERROR: no se pudo iniciar AP");
        return false;
    }
    strlcpy(s_ip_str, WiFi.softAPIP().toString().c_str(), sizeof(s_ip_str));
    Serial.printf("[WS] AP '%s' OK, IP: %s\n", SCREEN_AP_SSID, s_ip_str);

start_server:
    s_server = new AsyncWebServer(WS_PORT);
    s_ws     = new AsyncWebSocket("/ws");
    s_ws->onEvent(on_ws_event);
    s_server->addHandler(s_ws);
    s_server->begin();

    Serial.printf("[WS] servidor en ws://...:%d/ws\n", WS_PORT);
    return true;
}

/* ── ws_server_send ──────────────────────────────────────────────────────── */
void ws_server_send(const char *text) {
    if (s_ws && s_has_client) s_ws->textAll(text);
}

/* ── ws_server_stop ──────────────────────────────────────────────────────── */
void ws_server_stop(void) {
    if (s_server) { s_server->end(); delete s_server; s_server = nullptr; }
    if (s_ws)     { delete s_ws;     s_ws     = nullptr; }
    if (s_jpeg_buf)  { heap_caps_free(s_jpeg_buf);  s_jpeg_buf  = nullptr; }
    if (s_text_buf)  { heap_caps_free(s_text_buf);  s_text_buf  = nullptr; }
    s_screen_buf = nullptr;
    s_on_frame   = nullptr;
    s_on_notify  = nullptr;
    s_on_stats   = nullptr;
    s_on_layout  = nullptr;
    s_on_time    = nullptr;
    s_has_client = false;
    WiFi.disconnect(true);
    WiFi.softAPdisconnect(true);
}

bool ws_server_is_running(void) { return s_server != nullptr; }
bool ws_server_has_client(void) { return s_has_client; }
const char *ws_server_get_ip(void) { return s_ip_str; }
