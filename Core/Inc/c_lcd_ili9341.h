/**
 * @file    c_lcd_ili9341.h
 * @brief   ILI9341 LCD 드라이버 공개 API (SPI + DMA, Queue 아키텍처)
 * @details 2.4" 240×320 TFT LCD, Landscape 320×240, RGB565.
 *          전용 Display 태스크 + Message Queue로 멀티태스크 안전성 보장.
 *          헤더 매크로로 SPI 인스턴스, GPIO 핀, 큐/태스크 설정 가능.
 */
#ifndef C_LCD_ILI9341_H
#define C_LCD_ILI9341_H

#include "main.h"
#include "c_font.h"
#include <stdint.h>

/* ── 하드웨어 설정 ─────────────────────────────────────────────────────── */

/**
 * @brief  SPI 핸들 인스턴스 이름 (CubeMX 생성 변수명)
 * @note   예: hspi2 (F446RE), hspi1 (H7) 등
 */
#ifndef LCD_SPI_INSTANCE
#define LCD_SPI_INSTANCE        hspi2
#endif

/** @brief DC (Data/Command) 핀 포트 */
#ifndef LCD_PIN_DC_PORT
#define LCD_PIN_DC_PORT         LCD_DC_GPIO_Port
#endif
/** @brief DC 핀 번호 */
#ifndef LCD_PIN_DC_PIN
#define LCD_PIN_DC_PIN          LCD_DC_Pin
#endif

/** @brief RST (Reset) 핀 포트 */
#ifndef LCD_PIN_RST_PORT
#define LCD_PIN_RST_PORT        LCD_RST_GPIO_Port
#endif
/** @brief RST 핀 번호 */
#ifndef LCD_PIN_RST_PIN
#define LCD_PIN_RST_PIN         LCD_RST_Pin
#endif

/** @brief LED (Backlight) 핀 포트 */
#ifndef LCD_PIN_LED_PORT
#define LCD_PIN_LED_PORT        LCD_LED_GPIO_Port
#endif
/** @brief LED 핀 번호 */
#ifndef LCD_PIN_LED_PIN
#define LCD_PIN_LED_PIN         LCD_LED_Pin
#endif

/** @brief CS (Chip Select) 핀 포트 (Software NSS) */
#ifndef LCD_PIN_CS_PORT
#define LCD_PIN_CS_PORT         LCD_CS_GPIO_Port
#endif
/** @brief CS 핀 번호 */
#ifndef LCD_PIN_CS_PIN
#define LCD_PIN_CS_PIN          LCD_CS_Pin
#endif

/* ── 화면 크기 (Landscape) ─────────────────────────────────────────────── */
#define LCD_WIDTH   320u
#define LCD_HEIGHT  240u

/* ── Display 태스크 / Queue 설정 ───────────────────────────────────────── */

/** @brief 커맨드 큐 깊이 */
#ifndef LCD_QUEUE_SIZE
#define LCD_QUEUE_SIZE          8
#endif

/** @brief Display 태스크 스택 크기 (word 단위, ×4 = 바이트) */
#ifndef LCD_TASK_STACK
#define LCD_TASK_STACK          512
#endif

/** @brief Display 태스크 우선순위 */
#ifndef LCD_TASK_PRIO
#define LCD_TASK_PRIO           4u     /**< ThreadX 우선순위 (낮을수록 높음) */
#endif

/** @brief DrawString 텍스트 최대 길이 (NULL 포함) */
#ifndef LCD_TEXT_MAX
#define LCD_TEXT_MAX            64
#endif

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

/* ── 커맨드 타입 (Queue 내부용) ────────────────────────────────────────── */

/** @brief LCD 커맨드 종류 */
typedef enum {
    LCD_CMD_FILL_SCREEN,    /**< 전체 화면 단색 채움 */
    LCD_CMD_FILL_RECT,      /**< 사각형 영역 단색 채움 */
    LCD_CMD_DRAW_PIXEL,     /**< 단일 픽셀 그리기 */
    LCD_CMD_DRAW_STRING,    /**< 문자열 출력 */
    LCD_CMD_SET_FONT,       /**< 폰트 변경 */
    LCD_CMD_BACKLIGHT,      /**< 백라이트 ON/OFF */
} LCD_CmdType_t;

/** @brief LCD 커맨드 구조체 (Queue 메시지) */
typedef struct {
    LCD_CmdType_t type;
    union {
        struct { uint16_t color; } fillScreen;
        struct { uint16_t x, y, w, h, color; } fillRect;
        struct { uint16_t x, y, color; } drawPixel;
        struct { uint16_t x, y, fg, bg; const Font_t *font; char text[LCD_TEXT_MAX]; } drawString;
        struct { const Font_t *font; } setFont;
        struct { uint8_t on; } backlight;
    };
} LCD_Cmd_t;

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
