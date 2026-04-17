#pragma once

/* ── JC3248W535EN — ESP32-S3 N16R8 ─────────────────────────────────────────
 * Display: AXS15231B, 480x320, QSPI
 * Touch:   AXS15231B capacitivo, I2C
 * ────────────────────────────────────────────────────────────────────────── */

/* QSPI display */
#define TFT_CS    45
#define TFT_SCK   47
#define TFT_SDA0  21
#define TFT_SDA1  48
#define TFT_SDA2  40
#define TFT_SDA3  39
#define TFT_RST   GFX_NOT_DEFINED
#define TFT_BL    1

/* Touch I2C */
#define Touch_SCL   4
#define Touch_SDA   8
#define Touch_INT   3
#define Touch_ADDR  0x3B
