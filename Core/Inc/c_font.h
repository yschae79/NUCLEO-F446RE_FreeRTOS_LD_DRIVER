/**
 * @file    c_font.h
 * @brief   비트맵 폰트 구조체 및 폰트 인스턴스 선언
 * @details ILI9341 LCD용 고정폭 ASCII 비트맵 폰트.
 *          - Font_8x16: 8×16 픽셀, 40×15줄 (320×240 Landscape)
 *          - Font_16x24: 16×24 픽셀, 20×10줄 (320×240 Landscape)
 *          - ASCII 0x20~0x7E (95문자), MSB-first 스캔
 */
#ifndef C_FONT_H
#define C_FONT_H

#include <stdint.h>

/** @brief 비트맵 폰트 디스크립터 */
typedef struct {
    uint8_t         width;      /**< 문자 폭 (픽셀) */
    uint8_t         height;     /**< 문자 높이 (픽셀) */
    uint8_t         bytes_per_line; /**< 행당 바이트 수 (ceil(width/8)) */
    const uint8_t  *data;       /**< 비트맵 배열 포인터 */
} Font_t;

/** @brief 8×16 고정폭 폰트 (ASCII 0x20~0x7E) */
extern const Font_t Font_8x16;

/** @brief 16×24 고정폭 폰트 (ASCII 0x20~0x7E) */
extern const Font_t Font_16x24;

#endif /* C_FONT_H */
