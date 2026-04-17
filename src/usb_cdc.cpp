/*
 * usb_cdc.cpp
 *
 * Canal USB CDC de alta velocidad para pantalla secundaria.
 *
 * El ESP32-S3 expone un puerto CDC nativo mediante el USB integrado.
 * Con ARDUINO_USB_CDC_ON_BOOT=1 (Arduino Core 3.x) el objeto Serial
 * apunta directamente al CDC de hardware; no se necesita UART ni
 * SoftwareSerial.
 *
 * Protocolo de trama:
 *   [0xAA 0x55 0xAA 0x55][LEN:uint32 LE][TYPE:uint8][PAYLOAD...]
 *   TYPE 0x01 → JPEG de pantalla completa (480×320)
 *   TYPE 0x02 → JSON {"t":"notify"|"stats",...}
 *
 * El receptor acumula bytes en un buffer PSRAM, detecta la cabecera y
 * procesa el payload cuando el frame está completo.
 */
#include "usb_cdc.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <TJpg_Decoder.h>
#include <esp_heap_caps.h>
#include <cstring>

/* ── Estado del módulo ───────────────────────────────────────────────────── */
static uint16_t             *s_screen_buf = nullptr;
static usb_cdc_on_frame_t    s_on_frame   = nullptr;
static usb_cdc_on_notify_t   s_on_notify  = nullptr;
static usb_cdc_on_stats_t    s_on_stats   = nullptr;
static bool                  s_running    = false;

/* Buffer de recepción: tamaño máximo = cabecera + JPEG más grande posible */
#define RX_BUF_SIZE  (CDC_HEADER_SIZE + CDC_JPEG_MAX)
static uint8_t  *s_rx_buf     = nullptr;   /* buffer circular de recepción  */
static size_t    s_rx_head    = 0;          /* siguiente posición de escritura */
static size_t    s_rx_tail    = 0;          /* siguiente posición de lectura  */

/* ── Helpers del buffer circular ─────────────────────────────────────────── */
static inline size_t rx_used(void) {
    return (s_rx_head >= s_rx_tail)
           ? (s_rx_head - s_rx_tail)
           : (RX_BUF_SIZE - s_rx_tail + s_rx_head);
}

static inline size_t rx_free(void) {
    return RX_BUF_SIZE - 1 - rx_used();   /* -1 para distinguir lleno/vacío */
}

/* Lee un byte del buffer sin avanzar tail (peek). offset=0 es el más antiguo. */
static inline uint8_t rx_peek(size_t offset) {
    return s_rx_buf[(s_rx_tail + offset) % RX_BUF_SIZE];
}

/* Descarta 'n' bytes del frente del buffer. */
static inline void rx_consume(size_t n) {
    s_rx_tail = (s_rx_tail + n) % RX_BUF_SIZE;
}

/* Copia 'n' bytes del buffer circular a dst (sin avanzar tail). */
static void rx_copy_to(uint8_t *dst, size_t n) {
    for (size_t i = 0; i < n; i++) {
        dst[i] = rx_peek(i);
    }
}

/* ── TJpgDec callback: vuelca el tile RGB565 al buffer de pantalla ─────── */
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

/* ── Procesado de payload JPEG ───────────────────────────────────────────── */
static void process_jpeg(const uint8_t *data, uint32_t len) {
    if (!s_screen_buf || !s_on_frame) return;

    TJpgDec.setCallback(jpeg_output_cb);
    JRESULT r = TJpgDec.drawJpg(0, 0, data, len);
    if (r == JDR_OK) {
        s_on_frame();
    } else {
        Serial.printf("[CDC] JPEG error %d\n", (int)r);
    }
}

/* ── Procesado de payload JSON ───────────────────────────────────────────── */
static void process_json(const char *json, size_t len) {
    /* Detección rápida del tipo antes de parsear */
    const char *t = strstr(json, "\"t\":\"");
    if (!t) {
        Serial.println("[CDC] JSON sin campo \"t\", ignorado");
        return;
    }
    t += 5;  /* apunta al valor del campo "t" */

    if (strncmp(t, "notify", 6) == 0) {
        if (!s_on_notify) return;
        JsonDocument doc;
        if (deserializeJson(doc, json, len) != DeserializationError::Ok) {
            Serial.println("[CDC] JSON notify: error al parsear");
            return;
        }
        static ws_notify_t n;
        strlcpy(n.app,   doc["app"]   | "?", sizeof(n.app));
        strlcpy(n.title, doc["title"] | "",  sizeof(n.title));
        strlcpy(n.body,  doc["body"]  | "",  sizeof(n.body));
        s_on_notify(&n);

    } else if (strncmp(t, "stats", 5) == 0) {
        if (!s_on_stats) return;
        JsonDocument doc;
        if (deserializeJson(doc, json, len) != DeserializationError::Ok) {
            Serial.println("[CDC] JSON stats: error al parsear");
            return;
        }
        static ws_stats_t s;
        s.cpu  = doc["cpu"]  | 0;
        s.ram  = doc["ram"]  | 0;
        s.gpu  = doc["gpu"]  | 0;
        s.temp = doc["temp"] | 0;
        s_on_stats(&s);

    } else {
        Serial.printf("[CDC] tipo JSON desconocido, ignorado\n");
    }
}

/* ── Buffer lineal temporal para payload ─────────────────────────────────── */
/* Se aloca una vez y se reutiliza para evitar malloc en cada frame.          */
static uint8_t *s_payload_buf     = nullptr;
static size_t   s_payload_buf_cap = 0;

static bool ensure_payload_buf(size_t needed) {
    if (s_payload_buf && s_payload_buf_cap >= needed) return true;

    /* Liberar si ya existe (redimensión) */
    if (s_payload_buf) {
        heap_caps_free(s_payload_buf);
        s_payload_buf     = nullptr;
        s_payload_buf_cap = 0;
    }

    /* Intentar PSRAM primero */
    s_payload_buf = (uint8_t *)heap_caps_malloc(
        needed, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_payload_buf) {
        s_payload_buf = (uint8_t *)heap_caps_malloc(
            needed, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!s_payload_buf) {
        Serial.printf("[CDC] ERROR: no hay memoria para payload_buf (%zu bytes)\n", needed);
        return false;
    }
    s_payload_buf_cap = needed;
    return true;
}

/* ── Máquina de estados del parser de tramas ─────────────────────────────── */
typedef enum {
    STATE_HUNT,       /* buscando el magic 0xAA 0x55 0xAA 0x55 */
    STATE_HEADER,     /* acumulando los 9 bytes de cabecera       */
    STATE_PAYLOAD     /* acumulando los bytes del payload          */
} parser_state_t;

static parser_state_t s_state      = STATE_HUNT;
static uint32_t       s_frame_len  = 0;   /* longitud del payload esperada */
static uint8_t        s_frame_type = 0;   /* tipo de frame                 */

/* Constantes del magic para comparación rápida */
static const uint8_t MAGIC[4] = {
    CDC_MAGIC_0, CDC_MAGIC_1, CDC_MAGIC_2, CDC_MAGIC_3
};

/* ── usb_cdc_update ──────────────────────────────────────────────────────── */
void usb_cdc_update(void) {
    if (!s_running || !s_rx_buf) return;

    /* 1. Leer bytes disponibles y meterlos en el buffer circular */
    int avail = Serial.available();
    if (avail > 0) {
        size_t to_read = (size_t)avail;
        size_t space   = rx_free();
        if (to_read > space) {
            /* Buffer lleno: se descartarán bytes; forzar resincronización */
            to_read = space;
            s_state = STATE_HUNT;
            Serial.println("[CDC] WARN: buffer lleno, resincronizando");
        }

        /* Leer en dos tramos si el buffer circular se enrolla */
        size_t first = RX_BUF_SIZE - s_rx_head;
        if (to_read <= first) {
            Serial.readBytes((char *)(s_rx_buf + s_rx_head), to_read);
        } else {
            Serial.readBytes((char *)(s_rx_buf + s_rx_head), first);
            Serial.readBytes((char *)(s_rx_buf), to_read - first);
        }
        s_rx_head = (s_rx_head + to_read) % RX_BUF_SIZE;
    }

    /* 2. Procesar el contenido del buffer según la máquina de estados */
    bool progress = true;
    while (progress) {
        progress = false;

        switch (s_state) {

        /* ── HUNT: buscar 4 bytes de magic ──────────────────────────── */
        case STATE_HUNT: {
            size_t used = rx_used();
            if (used < CDC_MAGIC_SIZE) break;   /* esperar más bytes */

            /* Buscar el patrón MAGIC byte a byte */
            bool found = false;
            while (rx_used() >= CDC_MAGIC_SIZE) {
                if (rx_peek(0) == MAGIC[0] &&
                    rx_peek(1) == MAGIC[1] &&
                    rx_peek(2) == MAGIC[2] &&
                    rx_peek(3) == MAGIC[3]) {
                    found = true;
                    break;
                }
                rx_consume(1);   /* descartar byte no válido */
                progress = true;
            }

            if (found) {
                rx_consume(CDC_MAGIC_SIZE);   /* consumir el magic */
                s_state  = STATE_HEADER;
                progress = true;
                Serial.println("[CDC] magic encontrado");
            }
            break;
        }

        /* ── HEADER: leer 5 bytes restantes (LEN 4B + TYPE 1B) ─────── */
        case STATE_HEADER: {
            /* Ya consumimos el MAGIC; ahora esperamos LEN(4) + TYPE(1) = 5 bytes */
            const size_t REST = CDC_LEN_SIZE + CDC_TYPE_SIZE;   /* 5 */
            if (rx_used() < REST) break;

            /* Leer LEN (uint32 LE) */
            s_frame_len = (uint32_t)rx_peek(0)
                        | ((uint32_t)rx_peek(1) << 8)
                        | ((uint32_t)rx_peek(2) << 16)
                        | ((uint32_t)rx_peek(3) << 24);
            s_frame_type = rx_peek(4);
            rx_consume(REST);

            /* Validar */
            size_t max_len = (s_frame_type == CDC_TYPE_JPEG)
                             ? (size_t)CDC_JPEG_MAX
                             : (size_t)CDC_TEXT_MAX;

            if (s_frame_len == 0 || s_frame_len > max_len) {
                Serial.printf("[CDC] cabecera inválida: len=%u type=0x%02X, resincronizando\n",
                              (unsigned)s_frame_len, (unsigned)s_frame_type);
                s_state  = STATE_HUNT;
                progress = true;
                break;
            }

            s_state  = STATE_PAYLOAD;
            progress = true;
            break;
        }

        /* ── PAYLOAD: acumular hasta tener el frame completo ────────── */
        case STATE_PAYLOAD: {
            if (rx_used() < (size_t)s_frame_len) break;   /* datos incompletos */

            /* Asegurar buffer lineal */
            size_t alloc_size = (s_frame_type == CDC_TYPE_JSON)
                                ? s_frame_len + 1   /* +1 para '\0' */
                                : s_frame_len;
            if (!ensure_payload_buf(alloc_size)) {
                /* Sin memoria: descartar frame */
                rx_consume(s_frame_len);
                s_state  = STATE_HUNT;
                progress = true;
                break;
            }

            /* Copiar del buffer circular al buffer lineal */
            rx_copy_to(s_payload_buf, s_frame_len);
            rx_consume(s_frame_len);

            /* Despachar según tipo */
            if (s_frame_type == CDC_TYPE_JPEG) {
                process_jpeg(s_payload_buf, s_frame_len);
            } else if (s_frame_type == CDC_TYPE_JSON) {
                s_payload_buf[s_frame_len] = '\0';
                process_json((const char *)s_payload_buf, s_frame_len);
            } else {
                Serial.printf("[CDC] tipo de frame desconocido 0x%02X, descartado\n",
                              (unsigned)s_frame_type);
            }

            s_state  = STATE_HUNT;
            progress = true;
            break;
        }

        }   /* switch */
    }   /* while progress */
}

/* ── usb_cdc_start ───────────────────────────────────────────────────────── */
bool usb_cdc_start(uint16_t           *screen_buf,
                   usb_cdc_on_frame_t   on_frame,
                   usb_cdc_on_notify_t  on_notify,
                   usb_cdc_on_stats_t   on_stats) {
    if (s_running) return true;
    if (!screen_buf || !on_frame) {
        Serial.println("[CDC] ERROR: screen_buf y on_frame son obligatorios");
        return false;
    }

    s_screen_buf = screen_buf;
    s_on_frame   = on_frame;
    s_on_notify  = on_notify;
    s_on_stats   = on_stats;

    /* Alocar buffer circular de recepción en PSRAM */
    s_rx_buf = (uint8_t *)heap_caps_malloc(
        RX_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rx_buf) {
        s_rx_buf = (uint8_t *)heap_caps_malloc(
            RX_BUF_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!s_rx_buf) {
        Serial.println("[CDC] ERROR: sin memoria para rx_buf");
        return false;
    }
    s_rx_head = 0;
    s_rx_tail = 0;
    s_state   = STATE_HUNT;

    /* Configurar Serial (USB CDC nativo en Arduino Core 3.x) */
    Serial.setRxBufferSize(4096);   /* ampliar buffer del driver CDC */
    Serial.begin(CDC_BAUD_RATE);    /* en CDC nativo el baud se ignora por hw */

    /* Esperar a que el host abra el puerto (máx ~2 s) */
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0) < 2000) {
        delay(10);
    }

    s_running = true;
    Serial.println("[CDC] canal USB CDC listo");
    Serial.printf( "[CDC] buffer RX: %u bytes (%s)\n",
                   (unsigned)RX_BUF_SIZE,
                   heap_caps_check_integrity_addr((intptr_t)s_rx_buf, false)
                       ? "PSRAM" : "RAM interna");
    return true;
}

/* ── usb_cdc_stop ────────────────────────────────────────────────────────── */
void usb_cdc_stop(void) {
    if (!s_running) return;
    s_running = false;

    if (s_rx_buf)      { heap_caps_free(s_rx_buf);      s_rx_buf      = nullptr; }
    if (s_payload_buf) { heap_caps_free(s_payload_buf); s_payload_buf = nullptr; }

    s_payload_buf_cap = 0;
    s_rx_head         = 0;
    s_rx_tail         = 0;
    s_screen_buf      = nullptr;
    s_on_frame        = nullptr;
    s_on_notify       = nullptr;
    s_on_stats        = nullptr;
    s_state           = STATE_HUNT;

    Serial.println("[CDC] canal USB CDC detenido");
}

/* ── usb_cdc_is_connected ────────────────────────────────────────────────── */
bool usb_cdc_is_connected(void) {
    /* Serial (CDC nativo) se evalúa como bool true cuando el host
     * tiene el puerto abierto. */
    return s_running && (bool)Serial;
}
