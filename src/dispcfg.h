#pragma once

/* Resolución física del panel (portrait nativo del AXS15231B) */
#define TFT_RES_W  320
#define TFT_RES_H  480

/* Rotación del canvas GFX: 1 → landscape lógico 480x320 */
#define TFT_ROT    1

/* Alias portrait (usado en cálculos de touch) */
#define DISP_HOR_RES  TFT_RES_W
#define DISP_VER_RES  TFT_RES_H

typedef enum {
    DISP_ROT_LANDSCAPE = 0,
    DISP_ROT_PORTRAIT  = 1,
} disp_rot_t;
