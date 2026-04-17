#pragma once
#include <stdint.h>
#include <stddef.h>
#include "ws_server.h"   /* reutiliza ws_notify_t, ws_stats_t y constantes */

/* ── Protocolo de trama CDC ──────────────────────────────────────────────────
 *
 * Cada mensaje se envía con la siguiente estructura:
 *
 *   [MAGIC 4B][LEN 4B LE][TYPE 1B][PAYLOAD ...]
 *
 *   MAGIC  : 0xAA 0x55 0xAA 0x55  (4 bytes fijos, sincronización)
 *   LEN    : uint32_t little-endian, longitud del PAYLOAD en bytes
 *   TYPE   : 0x01 = JPEG frame, 0x02 = JSON (notify / stats)
 *   PAYLOAD: LEN bytes de datos
 *
 * ────────────────────────────────────────────────────────────────────────── */

/* Bytes de cabecera */
#define CDC_MAGIC_0     0xAAu
#define CDC_MAGIC_1     0x55u
#define CDC_MAGIC_2     0xAAu
#define CDC_MAGIC_3     0x55u
#define CDC_MAGIC_SIZE  4u
#define CDC_LEN_SIZE    4u
#define CDC_TYPE_SIZE   1u
#define CDC_HEADER_SIZE (CDC_MAGIC_SIZE + CDC_LEN_SIZE + CDC_TYPE_SIZE)  /* 9 bytes */

/* Tipos de frame */
#define CDC_TYPE_JPEG   0x01u
#define CDC_TYPE_JSON   0x02u

/* Límites de payload (iguales al protocolo WS) */
#define CDC_JPEG_MAX    WS_JPEG_MAX   /* 120 KB */
#define CDC_TEXT_MAX    WS_TEXT_MAX   /* 2 KB   */

/* Baud rate para USB CDC nativo (valor informativo; el CDC nativo lo ignora) */
#define CDC_BAUD_RATE   2000000u

/* ── Callbacks (misma semántica que ws_server.h) ────────────────────────── */
typedef void (*usb_cdc_on_frame_t)(void);                    /* JPEG decodificado */
typedef void (*usb_cdc_on_notify_t)(const ws_notify_t *n);
typedef void (*usb_cdc_on_stats_t)(const ws_stats_t  *s);

/* ── API pública ─────────────────────────────────────────────────────────── */

/**
 * Inicializa el canal USB CDC.
 *
 * @param screen_buf  Buffer RGB565 de 480×320 píxeles (el mismo que usa ws_server).
 * @param on_frame    Callback invocado tras decodificar un JPEG en screen_buf.
 * @param on_notify   Callback invocado al recibir una notificación.
 * @param on_stats    Callback invocado al recibir estadísticas del sistema.
 * @return true si la inicialización fue exitosa.
 */
bool usb_cdc_start(uint16_t          *screen_buf,
                   usb_cdc_on_frame_t  on_frame,
                   usb_cdc_on_notify_t on_notify,
                   usb_cdc_on_stats_t  on_stats);

/**
 * Procesa los bytes disponibles en el puerto USB CDC.
 * Debe llamarse desde loop() en cada iteración.
 */
void usb_cdc_update(void);

/** Detiene el canal CDC y libera recursos. */
void usb_cdc_stop(void);

/** Devuelve true si hay un host conectado al puerto CDC. */
bool usb_cdc_is_connected(void);
