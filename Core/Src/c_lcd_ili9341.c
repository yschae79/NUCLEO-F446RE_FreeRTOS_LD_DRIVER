/**
 * @file    c_lcd_ili9341.c
 * @brief   ILI9341 LCD 드라이버 구현 (SPI + DMA, Queue 아키텍처)
 * @details Landscape 320×240, RGB565.
 *          전용 Display 태스크가 Message Queue에서 커맨드를 수신하여 렌더링.
 *          모든 공개 API는 큐에 커맨드를 넣고 즉시 리턴 (호출 태스크 비차단).
 *          SPI, 라인버퍼, 폰트 등 내부 리소스는 Display 태스크만 접근.
 *          - 명령/소량 데이터: blocking HAL_SPI_Transmit
 *          - 대량 데이터(>32B): DMA + 바이너리 세마포어 대기
 */
#include "c_lcd_ili9341.h"
#include "tx_api.h"
#include <string.h>

/* ── 외부 핸들 ─────────────────────────────────────────────────────────── */
extern SPI_HandleTypeDef LCD_SPI_INSTANCE;

/* ── 내부 상수 ─────────────────────────────────────────────────────────── */
/** @brief DMA 전환 임계값 (이하 blocking, 초과 DMA) */
#define DMA_THRESHOLD   32u

/** @brief DMA 세마포어 대기 타임아웃 (ms) */
#define DMA_TIMEOUT_MS  500u

/* ── 내부 변수 ─────────────────────────────────────────────────────────── */
/** @brief DMA 전송 완료 동기화용 바이너리 세마포어 */
static TX_SEMAPHORE    s_dmaSem;

/** @brief DMA 라인 버퍼 (1 스캔라인 = 320 × 2바이트 = 640바이트) */
static uint8_t s_lineBuf[LCD_WIDTH * 2];

/** @brief 현재 선택된 폰트 (Display 태스크 전용) */
static const Font_t *s_font = NULL;

/**
 * @brief 커맨드를 포인터로 전달하는 TX_QUEUE (1 ULONG = 포인터 크기)
 * @note  TX_QUEUE 메시지 크기 최대 = 64B, LCD_Cmd_t = 80B 이상이므로 포인터 방식 사용
 */
static TX_QUEUE        s_cmdQueue;
static ULONG           s_cmdQueueBuf[LCD_QUEUE_SIZE];

/** @brief LCD 커맨드 풀 — 생산자가 슬롯을 빌린 후 포인터를 큐에 전송 */
static LCD_Cmd_t       s_cmdPool[LCD_QUEUE_SIZE];
static TX_SEMAPHORE    s_poolSem;          /**< 자유 슬롯 개수, 초기값 = LCD_QUEUE_SIZE */
static UINT            s_poolIdx;          /**< 다음 할당 인덱스 */

/** @brief Display 태스크 TCB + 스택 */
static TX_THREAD       s_displayTask;
static ULONG           s_displayStack[LCD_TASK_STACK];

/* ── GPIO 헬퍼 (인라인) ────────────────────────────────────────────────── */

/** @brief DC 핀을 명령 모드로 설정 (LOW) */
static inline void DC_Command(void)
{
    HAL_GPIO_WritePin(LCD_PIN_DC_PORT, LCD_PIN_DC_PIN, GPIO_PIN_RESET);
}

/** @brief DC 핀을 데이터 모드로 설정 (HIGH) */
static inline void DC_Data(void)
{
    HAL_GPIO_WritePin(LCD_PIN_DC_PORT, LCD_PIN_DC_PIN, GPIO_PIN_SET);
}

/** @brief RST 핀 LOW */
static inline void RST_Low(void)
{
    HAL_GPIO_WritePin(LCD_PIN_RST_PORT, LCD_PIN_RST_PIN, GPIO_PIN_RESET);
}

/** @brief RST 핀 HIGH */
static inline void RST_High(void)
{
    HAL_GPIO_WritePin(LCD_PIN_RST_PORT, LCD_PIN_RST_PIN, GPIO_PIN_SET);
}

/** @brief CS 핀 LOW (SPI 선택) */
static inline void CS_Select(void)
{
    HAL_GPIO_WritePin(LCD_PIN_CS_PORT, LCD_PIN_CS_PIN, GPIO_PIN_RESET);
}

/** @brief CS 핀 HIGH (SPI 해제) */
static inline void CS_Deselect(void)
{
    HAL_GPIO_WritePin(LCD_PIN_CS_PORT, LCD_PIN_CS_PIN, GPIO_PIN_SET);
}

/* ── SPI 전송 헬퍼 ─────────────────────────────────────────────────────── */

/**
 * @brief  ILI9341 명령 전송 (1바이트, blocking)
 * @param  cmd  레지스터 명령 바이트
 */
static void SendCmd(uint8_t cmd)
{
    CS_Select();
    DC_Command();
    HAL_SPI_Transmit(&LCD_SPI_INSTANCE, &cmd, 1, HAL_MAX_DELAY);
    CS_Deselect();
}

/**
 * @brief  ILI9341 데이터 전송 (blocking, 소량)
 * @param  data  데이터 버퍼 포인터
 * @param  len   전송 길이 (바이트)
 */
static void SendData(const uint8_t *data, uint16_t len)
{
    CS_Select();
    DC_Data();
    HAL_SPI_Transmit(&LCD_SPI_INSTANCE, (uint8_t *)data, len, HAL_MAX_DELAY);
    CS_Deselect();
}

/**
 * @brief  단일 데이터 바이트 전송 (blocking)
 * @param  val  데이터 값
 */
static void SendData8(uint8_t val)
{
    CS_Select();
    DC_Data();
    HAL_SPI_Transmit(&LCD_SPI_INSTANCE, &val, 1, HAL_MAX_DELAY);
    CS_Deselect();
}

/**
 * @brief  DMA로 대량 데이터 전송 후 완료 대기
 * @param  data  데이터 버퍼 포인터
 * @param  len   전송 길이 (바이트)
 */
static void SendDataDMA(uint8_t *data, uint16_t len)
{
    CS_Select();
    DC_Data();
    HAL_SPI_Transmit_DMA(&LCD_SPI_INSTANCE, data, len);
    tx_semaphore_get(&s_dmaSem, DMA_TIMEOUT_MS);
    CS_Deselect();
}

/* ── ILI9341 초기화 시퀀스 ─────────────────────────────────────────────── */

/**
 * @brief  ILI9341 레지스터 초기화 (내부 함수)
 */
static void ILI9341_RegInit(void)
{
    /* Power Control A */
    SendCmd(0xCB);
    {
        const uint8_t d[] = {0x39, 0x2C, 0x00, 0x34, 0x02};
        SendData(d, sizeof(d));
    }

    /* Power Control B */
    SendCmd(0xCF);
    {
        const uint8_t d[] = {0x00, 0xC1, 0x30};
        SendData(d, sizeof(d));
    }

    /* Driver Timing Control A */
    SendCmd(0xE8);
    {
        const uint8_t d[] = {0x85, 0x00, 0x78};
        SendData(d, sizeof(d));
    }

    /* Driver Timing Control B */
    SendCmd(0xEA);
    {
        const uint8_t d[] = {0x00, 0x00};
        SendData(d, sizeof(d));
    }

    /* Power On Sequence Control */
    SendCmd(0xED);
    {
        const uint8_t d[] = {0x64, 0x03, 0x12, 0x81};
        SendData(d, sizeof(d));
    }

    /* Pump Ratio Control */
    SendCmd(0xF7);
    SendData8(0x20);

    /* Power Control 1 */
    SendCmd(0xC0);
    SendData8(0x23);

    /* Power Control 2 */
    SendCmd(0xC1);
    SendData8(0x10);

    /* VCOM Control 1 */
    SendCmd(0xC5);
    {
        const uint8_t d[] = {0x3E, 0x28};
        SendData(d, sizeof(d));
    }

    /* VCOM Control 2 */
    SendCmd(0xC7);
    SendData8(0x86);

    /* Memory Access Control — Landscape: MV=1, BGR=1 */
    SendCmd(0x36);
    SendData8(0x28);

    /* Pixel Format — RGB565 */
    SendCmd(0x3A);
    SendData8(0x55);

    /* Frame Rate Control — 70Hz */
    SendCmd(0xB1);
    {
        const uint8_t d[] = {0x00, 0x18};
        SendData(d, sizeof(d));
    }

    /* Display Function Control */
    SendCmd(0xB6);
    {
        const uint8_t d[] = {0x08, 0x82, 0x27};
        SendData(d, sizeof(d));
    }

    /* 3Gamma Function Disable */
    SendCmd(0xF2);
    SendData8(0x00);

    /* Gamma Set — Curve 1 */
    SendCmd(0x26);
    SendData8(0x01);

    /* Positive Gamma Correction */
    SendCmd(0xE0);
    {
        const uint8_t d[] = {
            0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1,
            0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00
        };
        SendData(d, sizeof(d));
    }

    /* Negative Gamma Correction */
    SendCmd(0xE1);
    {
        const uint8_t d[] = {
            0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1,
            0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F
        };
        SendData(d, sizeof(d));
    }
}

/* ── 내부 렌더링 함수 (Display 태스크 전용) ─────────────────────────────── */

/* Forward declarations */
static void Internal_FillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);

/**
 * @brief  전체 화면 단색 채움 (내부)
 * @param  color RGB565 색상값
 */
static void Internal_FillScreen(uint16_t color)
{
    Internal_FillRect(0, 0, LCD_WIDTH, LCD_HEIGHT, color);
}

/**
 * @brief  그리기 윈도우 설정 (Column/Page Address + Memory Write)
 */
static void SetWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    uint16_t x1 = x + w - 1;
    uint16_t y1 = y + h - 1;

    /* Column Address Set (0x2A) */
    SendCmd(0x2A);
    {
        uint8_t d[4] = {
            (uint8_t)(x >> 8), (uint8_t)(x & 0xFF),
            (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF)
        };
        SendData(d, 4);
    }

    /* Page Address Set (0x2B) */
    SendCmd(0x2B);
    {
        uint8_t d[4] = {
            (uint8_t)(y >> 8), (uint8_t)(y & 0xFF),
            (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFF)
        };
        SendData(d, 4);
    }

    /* Memory Write (0x2C) */
    SendCmd(0x2C);
}

/**
 * @brief  사각형 영역 단색 채움 (내부)
 */
static void Internal_FillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    if (x >= LCD_WIDTH || y >= LCD_HEIGHT) return;
    if (x + w > LCD_WIDTH)  w = LCD_WIDTH - x;
    if (y + h > LCD_HEIGHT) h = LCD_HEIGHT - y;

    SetWindow(x, y, w, h);

    /* 라인 버퍼를 해당 색상으로 채움 (Big-Endian: MSB first) */
    uint16_t linePixels = w;
    for (uint16_t i = 0; i < linePixels; i++) {
        s_lineBuf[i * 2]     = (uint8_t)(color >> 8);
        s_lineBuf[i * 2 + 1] = (uint8_t)(color & 0xFF);
    }

    uint16_t lineBytes = linePixels * 2;

    /* 라인 단위로 DMA 전송 */
    for (uint16_t row = 0; row < h; row++) {
        if (lineBytes > DMA_THRESHOLD) {
            SendDataDMA(s_lineBuf, lineBytes);
        } else {
            SendData(s_lineBuf, lineBytes);
        }
    }
}

/**
 * @brief  단일 픽셀 그리기 (내부)
 */
static void Internal_DrawPixel(uint16_t x, uint16_t y, uint16_t color)
{
    if (x >= LCD_WIDTH || y >= LCD_HEIGHT) return;

    SetWindow(x, y, 1, 1);
    uint8_t d[2] = {(uint8_t)(color >> 8), (uint8_t)(color & 0xFF)};
    SendData(d, 2);
}

/**
 * @brief  단일 문자 출력 (내부)
 */
static void Internal_DrawChar(uint16_t x, uint16_t y, char ch, uint16_t fg, uint16_t bg)
{
    if (s_font == NULL) return;
    if (ch < 0x20 || ch > 0x7E) ch = '?';
    if (x + s_font->width > LCD_WIDTH || y + s_font->height > LCD_HEIGHT) return;

    uint16_t charIdx = (uint16_t)(ch - 0x20);
    uint16_t bytesPerChar = (uint16_t)s_font->bytes_per_line * s_font->height;
    const uint8_t *bitmap = &s_font->data[charIdx * bytesPerChar];

    SetWindow(x, y, s_font->width, s_font->height);

    /* 한 행씩 비트맵 → RGB565 변환 후 전송 */
    for (uint16_t row = 0; row < s_font->height; row++) {
        const uint8_t *rowData = &bitmap[row * s_font->bytes_per_line];

        for (uint16_t col = 0; col < s_font->width; col++) {
            uint8_t byteIdx = col / 8;
            uint8_t bitIdx  = 7 - (col % 8);
            uint16_t c = (rowData[byteIdx] & (1 << bitIdx)) ? fg : bg;
            s_lineBuf[col * 2]     = (uint8_t)(c >> 8);
            s_lineBuf[col * 2 + 1] = (uint8_t)(c & 0xFF);
        }

        uint16_t rowBytes = s_font->width * 2;
        if (rowBytes > DMA_THRESHOLD) {
            SendDataDMA(s_lineBuf, rowBytes);
        } else {
            SendData(s_lineBuf, rowBytes);
        }
    }
}

/**
 * @brief  문자열 출력 (내부, 자동 줄바꿈)
 */
static void Internal_DrawString(uint16_t x, uint16_t y, const char *str,
                                uint16_t fg, uint16_t bg, const Font_t *font)
{
    /* drawString 커맨드에 폰트가 지정되어 있으면 우선 적용 */
    if (font != NULL) {
        s_font = font;
    }
    if (s_font == NULL || str == NULL) return;

    uint16_t cx = x;
    uint16_t cy = y;

    while (*str) {
        if (*str == '\n') {
            cx = x;
            cy += s_font->height;
            str++;
            continue;
        }

        if (cx + s_font->width > LCD_WIDTH) {
            cx = x;
            cy += s_font->height;
        }
        if (cy + s_font->height > LCD_HEIGHT) break;

        Internal_DrawChar(cx, cy, *str, fg, bg);
        cx += s_font->width;
        str++;
    }
}

/**
 * @brief  백라이트 ON/OFF (내부)
 */
static void Internal_Backlight(uint8_t on)
{
    HAL_GPIO_WritePin(LCD_PIN_LED_PORT, LCD_PIN_LED_PIN,
                      on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/* ── Display 태스크 ────────────────────────────────────────────────────── */

/**
 * @brief  Display 태스크 엔트리 — 큐에서 커맨드를 수신하여 순차 렌더링
 * @param  argument  미사용
 */
static void DisplayTaskEntry(ULONG argument)
{
    (void)argument;

    /* 하드웨어 초기화 (태스크 컨텍스트에서 osDelay 사용 가능) */
    RST_Low();
    tx_thread_sleep(10);
    RST_High();
    tx_thread_sleep(120);

    ILI9341_RegInit();

    /* Sleep Out */
    SendCmd(0x11);
    tx_thread_sleep(120);

    /* Display ON */
    SendCmd(0x29);
    tx_thread_sleep(20);

    /* 백라이트 ON */
    Internal_Backlight(1);

    /* 기본 폰트 설정 */
    s_font = &Font_8x16;

    /* 커맨드 처리 루프 */
    LCD_Cmd_t cmd;
    for (;;) {
        ULONG msg = 0;
        if (tx_queue_receive(&s_cmdQueue, &msg, TX_WAIT_FOREVER) == TX_SUCCESS) {
            LCD_Cmd_t *p = (LCD_Cmd_t *)msg;
            cmd = *p;
            /* 풀 슬롯 반환 */
            tx_semaphore_put(&s_poolSem);
            switch (cmd.type) {
            case LCD_CMD_FILL_SCREEN:
                Internal_FillScreen(cmd.fillScreen.color);
                break;
            case LCD_CMD_FILL_RECT:
                Internal_FillRect(cmd.fillRect.x, cmd.fillRect.y,
                                  cmd.fillRect.w, cmd.fillRect.h,
                                  cmd.fillRect.color);
                break;
            case LCD_CMD_DRAW_PIXEL:
                Internal_DrawPixel(cmd.drawPixel.x, cmd.drawPixel.y,
                                   cmd.drawPixel.color);
                break;
            case LCD_CMD_DRAW_STRING:
                Internal_DrawString(cmd.drawString.x, cmd.drawString.y,
                                    cmd.drawString.text,
                                    cmd.drawString.fg, cmd.drawString.bg,
                                    cmd.drawString.font);
                break;
            case LCD_CMD_SET_FONT:
                if (cmd.setFont.font != NULL) {
                    s_font = cmd.setFont.font;
                }
                break;
            case LCD_CMD_BACKLIGHT:
                Internal_Backlight(cmd.backlight.on);
                break;
            }
        }
    }
}

/* ── 공개 API 구현 (큐 Put 래퍼) ──────────────────────────────────────── */

void LCD_Init(void)
{
    /* DMA 완료 세마포어 (초기값=0: 전송 전 잠긴 상태) */
    tx_semaphore_create(&s_dmaSem, "lcd_dma", 0);

    /* 커맨드 풀 + 포인터 큐 설정 */
    s_poolIdx = 0;
    tx_semaphore_create(&s_poolSem, "lcd_pool", (ULONG)LCD_QUEUE_SIZE);
    tx_queue_create(&s_cmdQueue, "lcd_q", TX_1_ULONG,
                    s_cmdQueueBuf, sizeof(s_cmdQueueBuf));

    /* Display 태스크 생성 */
    tx_thread_create(&s_displayTask, "DisplayTask", DisplayTaskEntry, 0,
                     s_displayStack, sizeof(s_displayStack),
                     LCD_TASK_PRIO, LCD_TASK_PRIO,
                     TX_NO_TIME_SLICE, TX_AUTO_START);
}

void LCD_DmaCpltHandler(void)
{
    tx_semaphore_put(&s_dmaSem);
}

/* 풀에서 커맨드 슬롯 할당 (단일 생산자 타고용 — 다중 생산자 필요 시 TX_MUTEX 추가) */
static LCD_Cmd_t *AllocCmd(void)
{
    tx_semaphore_get(&s_poolSem, TX_WAIT_FOREVER);
    UINT idx = s_poolIdx;
    s_poolIdx = (idx + 1u >= (UINT)LCD_QUEUE_SIZE) ? 0u : idx + 1u;
    return &s_cmdPool[idx];
}

void LCD_FillScreen(uint16_t color)
{
    LCD_Cmd_t *p = AllocCmd();
    p->type = LCD_CMD_FILL_SCREEN;
    p->fillScreen.color = color;
    ULONG msg = (ULONG)p;
    tx_queue_send(&s_cmdQueue, &msg, TX_WAIT_FOREVER);
}

void LCD_FillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    LCD_Cmd_t *p = AllocCmd();
    p->type = LCD_CMD_FILL_RECT;
    p->fillRect.x = x; p->fillRect.y = y;
    p->fillRect.w = w; p->fillRect.h = h;
    p->fillRect.color = color;
    ULONG msg = (ULONG)p;
    tx_queue_send(&s_cmdQueue, &msg, TX_WAIT_FOREVER);
}

void LCD_DrawPixel(uint16_t x, uint16_t y, uint16_t color)
{
    LCD_Cmd_t *p = AllocCmd();
    p->type = LCD_CMD_DRAW_PIXEL;
    p->drawPixel.x = x; p->drawPixel.y = y;
    p->drawPixel.color = color;
    ULONG msg = (ULONG)p;
    tx_queue_send(&s_cmdQueue, &msg, TX_WAIT_FOREVER);
}

void LCD_SetFont(const Font_t *font)
{
    LCD_Cmd_t *p = AllocCmd();
    p->type = LCD_CMD_SET_FONT;
    p->setFont.font = font;
    ULONG msg = (ULONG)p;
    tx_queue_send(&s_cmdQueue, &msg, TX_WAIT_FOREVER);
}

void LCD_DrawChar(uint16_t x, uint16_t y, char ch, uint16_t fg, uint16_t bg)
{
    LCD_Cmd_t *p = AllocCmd();
    p->type = LCD_CMD_DRAW_STRING;
    p->drawString.x = x; p->drawString.y = y;
    p->drawString.fg = fg; p->drawString.bg = bg;
    p->drawString.font = NULL;
    p->drawString.text[0] = ch;
    p->drawString.text[1] = '\0';
    ULONG msg = (ULONG)p;
    tx_queue_send(&s_cmdQueue, &msg, TX_WAIT_FOREVER);
}

void LCD_DrawString(uint16_t x, uint16_t y, const char *str, uint16_t fg, uint16_t bg)
{
    if (str == NULL) return;
    LCD_Cmd_t *p = AllocCmd();
    p->type = LCD_CMD_DRAW_STRING;
    p->drawString.x = x; p->drawString.y = y;
    p->drawString.fg = fg; p->drawString.bg = bg;
    p->drawString.font = NULL;
    strncpy(p->drawString.text, str, LCD_TEXT_MAX - 1);
    p->drawString.text[LCD_TEXT_MAX - 1] = '\0';
    ULONG msg = (ULONG)p;
    tx_queue_send(&s_cmdQueue, &msg, TX_WAIT_FOREVER);
}

void LCD_Backlight(uint8_t on)
{
    LCD_Cmd_t *p = AllocCmd();
    p->type = LCD_CMD_BACKLIGHT;
    p->backlight.on = on;
    ULONG msg = (ULONG)p;
    tx_queue_send(&s_cmdQueue, &msg, TX_WAIT_FOREVER);
}
