/**
 * @file lv_conf.h
 * Configuración de LVGL 9.x para JC3248W535EN (ESP32-S3, 480x320 landscape)
 */

#ifndef LV_CONF_H
#define LV_CONF_H

#if 1

#ifndef __ASSEMBLY__
#include <stdint.h>
#endif

/*====================
   COLOR SETTINGS
 *====================*/
#define LV_COLOR_DEPTH 16
/* 0 = RGB565 normal. Si el fondo sale verde, probar 1 (byte swap). */
#define LV_COLOR_16_SWAP 0

/*=========================
   MEMORY SETTINGS
 *=========================*/
/* 192 KB: deja espacio al stack Wi-Fi */
#define LV_MEM_SIZE (192U * 1024U)

/*====================
   HAL SETTINGS
 *====================*/
#define LV_DEF_REFR_PERIOD 10
#define LV_DPI_DEF 130

/*=================
 * OPERATING SYSTEM
 *=================*/
/* Sin OS (Arduino loop) */
#define LV_USE_OS 0

/*========================
 * RENDERING CONFIGURATION
 *========================*/
/* Stride/align 1 evita texto torcido (issue lvgl #5090) */
#define LV_DRAW_BUF_STRIDE_ALIGN 1
#define LV_DRAW_BUF_ALIGN        1
#define LV_USE_DRAW_SW           1

#if LV_USE_DRAW_SW == 1
#define LV_DRAW_SW_DRAW_UNIT_CNT  1
#define LV_USE_DRAW_ARM2D_SYNC    0
#define LV_DRAW_SW_COMPLEX        1
#if LV_DRAW_SW_COMPLEX == 1
#define LV_DRAW_SW_SHADOW_CACHE_SIZE 0
#define LV_DRAW_SW_CIRCLE_CACHE_SIZE 4
#endif
#endif

/*====================
   LOG / ASSERT
 *====================*/
#define LV_USE_LOG              0
#define LV_USE_ASSERT_NULL      1
#define LV_USE_ASSERT_MALLOC    1
#define LV_USE_ASSERT_STYLE     0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ       0
#define LV_ASSERT_HANDLER_INCLUDE <stdint.h>
#define LV_ASSERT_HANDLER while (1);

/*================
 * FONTS (solo 12, 14, 16 — fuente uniforme, sin glitches)
 *================*/
#define LV_FONT_MONTSERRAT_8   0
#define LV_FONT_MONTSERRAT_10  0
#define LV_FONT_MONTSERRAT_12  1
#define LV_FONT_MONTSERRAT_14  1
#define LV_FONT_MONTSERRAT_16  1
#define LV_FONT_MONTSERRAT_18  0
#define LV_FONT_MONTSERRAT_20  0
#define LV_FONT_MONTSERRAT_22  0
#define LV_FONT_MONTSERRAT_24  0
#define LV_FONT_MONTSERRAT_26  0
#define LV_FONT_MONTSERRAT_28  0
#define LV_FONT_MONTSERRAT_30  0
#define LV_FONT_MONTSERRAT_32  0
#define LV_FONT_MONTSERRAT_34  0
#define LV_FONT_MONTSERRAT_36  0
#define LV_FONT_MONTSERRAT_38  0
#define LV_FONT_MONTSERRAT_40  0
#define LV_FONT_MONTSERRAT_42  0
#define LV_FONT_MONTSERRAT_44  0
#define LV_FONT_MONTSERRAT_46  0
#define LV_FONT_MONTSERRAT_48  0

#define LV_FONT_MONTSERRAT_12_SUBPX       0
#define LV_FONT_MONTSERRAT_28_COMPRESSED  0
#define LV_FONT_DEJAVU_16_PERSIAN_HEBREW  0
#define LV_FONT_SIMSUN_14_CJK             0
#define LV_FONT_SIMSUN_16_CJK             0
#define LV_FONT_UNSCII_8                  0
#define LV_FONT_UNSCII_16                 0
#define LV_FONT_CUSTOM_DECLARE
#define LV_FONT_DEFAULT &lv_font_montserrat_14
#define LV_FONT_FMT_TXT_LARGE   0
#define LV_USE_FONT_COMPRESSED  0
#define LV_USE_FONT_PLACEHOLDER 1

/*=================
 *  TEXT SETTINGS
 *=================*/
#define LV_TXT_ENC                       LV_TXT_ENC_UTF8
#define LV_TXT_BREAK_CHARS               " ,.;:-_)]}"
#define LV_TXT_LINE_BREAK_LONG_LEN       0
#define LV_TXT_LINE_BREAK_LONG_PRE_MIN_LEN  3
#define LV_TXT_LINE_BREAK_LONG_POST_MIN_LEN 3
#define LV_TXT_COLOR_CMD                 "#"
#define LV_USE_BIDI                      0
#define LV_USE_ARABIC_PERSIAN_CHARS      0

/*===================
 *  WIDGETS
 *===================*/
#define LV_USE_ANIMIMG      0
#define LV_USE_ARC          1
#define LV_USE_BAR          1
#define LV_USE_BUTTON       1
#define LV_USE_BUTTONMATRIX 1
#define LV_USE_CALENDAR     0
#define LV_USE_CANVAS       1
#define LV_USE_CHART        0
#define LV_USE_CHECKBOX     0
#define LV_USE_COLORWHEEL   0
#define LV_USE_DROPDOWN     0
#define LV_USE_IMAGE        1
#define LV_USE_IMAGEBUTTON  0
#define LV_USE_KEYBOARD     1
#define LV_USE_LABEL        1
#define LV_USE_LED          0
#define LV_USE_LINE         1
#define LV_USE_LIST         1
#define LV_USE_MENU         0
#define LV_USE_MSGBOX       0
#define LV_USE_ROLLER       0
#define LV_USE_SCALE        1
#define LV_USE_SLIDER       0
#define LV_USE_SPAN         0
#define LV_USE_SPINBOX      0
#define LV_USE_SPINNER      1
#define LV_USE_SWITCH       0
#define LV_USE_TABLE        0
#define LV_USE_TABVIEW      0
#define LV_USE_TEXTAREA     1
#define LV_USE_TILEVIEW     0
#define LV_USE_WIN          0

/*================
 * THEME
 *================*/
#define LV_USE_THEME_DEFAULT 1
#if LV_USE_THEME_DEFAULT
#define LV_THEME_DEFAULT_DARK            1
#define LV_THEME_DEFAULT_GROW            0
#define LV_THEME_DEFAULT_TRANSITION_TIME 0
#endif
#define LV_USE_THEME_SIMPLE 0
#define LV_USE_THEME_MONO   0

/*===================
 * LAYOUT
 *===================*/
#define LV_USE_FLEX 1
#define LV_USE_GRID 1

/*====================
 * 3RD PARTY LIBRARIES
 *====================*/
#define LV_USE_FS_STDIO         0
#define LV_USE_FS_POSIX         0
#define LV_USE_FS_WIN32         0
#define LV_USE_FS_FATFS         0
#define LV_USE_FS_MEMFS         0
#define LV_USE_FS_LITTLEFS      0
#define LV_USE_FS_ARDUINO_SD    0
#define LV_USE_FS_ARDUINO_LITTLEFS 0
#define LV_USE_LIBPNG           0
#define LV_USE_BMP              0
#define LV_USE_TJPGD            0
#define LV_USE_LIBJPEG_TURBO    0
#define LV_USE_GIF              0
#define LV_USE_QRCODE           0
#define LV_USE_BARCODE          0
#define LV_USE_FREETYPE         0
#define LV_USE_TINY_TTF         0
#define LV_USE_RLOTTIE          0
#define LV_USE_VECTOR_GRAPHIC   0
#define LV_USE_LOTTIE           0
#define LV_USE_FFMPEG           0

/*==================
 * OTHERS
 *==================*/
#define LV_USE_SNAPSHOT       0
#define LV_USE_MONKEY         0
#define LV_USE_GRIDNAV        0
#define LV_USE_FRAGMENT       0
#define LV_USE_IMGFONT        0
#define LV_USE_OBSERVER       0
#define LV_USE_IME_PINYIN     0
#define LV_USE_FILE_EXPLORER  0
#define LV_USE_SYSMON         0
#define LV_USE_PROFILER       0
#define LV_USE_PERF_MONITOR   0
#define LV_USE_MEM_MONITOR    0

/*==================
 *  EXAMPLES / DEMOS
 *==================*/
#define LV_BUILD_EXAMPLES    0
#define LV_USE_DEMO_WIDGETS  0
#define LV_USE_DEMO_BENCHMARK 0
#define LV_USE_DEMO_STRESS   0
#define LV_USE_DEMO_MUSIC    0

#endif
#endif /* LV_CONF_H */
