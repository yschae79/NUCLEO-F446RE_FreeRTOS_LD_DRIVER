/**
 * @file    c_lcd_ili9341.h
 * @brief   ILI9341 LCD 드라이버 공개 API (SPI + DMA)
 * @details 2.4" 240×320 TFT LCD, Landscape 320×240, RGB565.
 *          SPI2 + DMA1_Stream4, FreeRTOS 바이너리 세마포어 동기화.
 */
#ifndef C_LCD_ILI9341_H
#define C_LCD_ILI9341_H

#include "main.h"
#include "c_font.h"
#include <stdint.h>

/* ── 화면 크기 (Landscape) ─────────────────────────────────────────────── */
#define LCD_WIDTH   320u
#define LCD_HEIGHT  240u

/* ── RGB565 색상 매크로 ────────────────────────────────────────────────── */
#define LCD_BLACK       0x0000
#define LCD_WHITE       0xFFFF
#define LCD_RED         0xF800
#define LCD_GREEN       0x07E0
#define LCD_BLUE        0x001F
#define LCD_YELLOW      0xFFE0
#define LCD_CYAN        0x07FF
#define LCD_MAGENTA     0xF81F
#define LCD_ORANGE      0xFD20
#define LCD_GRAY        0x8410
#define LCD_DARKGRAY    0x4208

/** @brief RGB888 → RGB565 변환 매크로 */
#define LCD_RGB565(r, g, b) \
    ((uint16_t)(((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | (((b) & 0xF8) >> 3))

/* ── 공개 API ──────────────────────────────────────────────────────────── */

/**
 * @brief  LCD 초기화 (ILI9341 레지스터 설정 + 백라이트 ON)
 * @note   FreeRTOS 스케줄러 시작 후 태스크 컨텍스트에서 호출해야 함
 */
void LCD_Init(void);

/**
 * @brief  DMA 전송 완료 ISR 핸들러
 * @note   HAL_SPI_TxCpltCallback 내부에서 호출
 */
void LCD_DmaCpltHandler(void);

/**
 * @brief  전체 화면을 단색으로 채움
 * @param  color RGB565 색상값
 */
void LCD_FillScreen(uint16_t color);

/**
 * @brief  사각형 영역을 단색으로 채움
 * @param  x      시작 X 좌표
 * @param  y      시작 Y 좌표
 * @param  w      폭 (픽셀)
 * @param  h      높이 (픽셀)
 * @param  color  RGB565 색상값
 */
void LCD_FillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);

/**
 * @brief  단일 픽셀 그리기
 * @param  x      X 좌표
 * @param  y      Y 좌표
 * @param  color  RGB565 색상값
 */
void LCD_DrawPixel(uint16_t x, uint16_t y, uint16_t color);

/**
 * @brief  현재 폰트 설정
 * @param  font   Font_t 포인터 (Font_8x16 또는 Font_16x24)
 */
void LCD_SetFont(const Font_t *font);

/**
 * @brief  단일 문자 출력
 * @param  x   X 좌표
 * @param  y   Y 좌표
 * @param  ch  ASCII 문자 (0x20~0x7E)
 * @param  fg  전경색 (RGB565)
 * @param  bg  배경색 (RGB565)
 */
void LCD_DrawChar(uint16_t x, uint16_t y, char ch, uint16_t fg, uint16_t bg);

/**
 * @brief  문자열 출력 (자동 줄바꿈)
 * @param  x    시작 X 좌표
 * @param  y    시작 Y 좌표
 * @param  str  NULL 종료 문자열
 * @param  fg   전경색 (RGB565)
 * @param  bg   배경색 (RGB565)
 */
void LCD_DrawString(uint16_t x, uint16_t y, const char *str, uint16_t fg, uint16_t bg);

/**
 * @brief  백라이트 ON/OFF
 * @param  on  1=켜기, 0=끄기
 */
void LCD_Backlight(uint8_t on);

#endif /* C_LCD_ILI9341_H */
