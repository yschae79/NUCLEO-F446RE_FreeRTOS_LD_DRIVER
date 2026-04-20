# c_lcd_ili9341 포팅 가이드

FreeRTOS + STM32 HAL 환경에서 ILI9341 TFT LCD를 SPI DMA로 구동하는 드라이버.

## 개요

| 항목 | 내용 |
|------|------|
| 파일 | `c_lcd_ili9341.c`, `c_lcd_ili9341.h`, `c_font.c`, `c_font.h` |
| 해상도 | 320×240 Landscape (RGB565, 16bpp) |
| 인터페이스 | SPI Master TX Only + DMA |
| 아키텍처 | **전용 Display 태스크 + Message Queue** |
| 동기화 | 바이너리 세마포어 (DMA 완료), osMessageQueue (커맨드 전달) |
| 멀티태스크 | 안전 — 모든 공개 API는 큐 Put으로 동작 |

## 아키텍처

```
┌──────────┐                     ┌──────────────┐
│ Task A   │  LCD_DrawString()   │              │
│          ├─────────────────────►              │
├──────────┤  LCD_FillScreen()   │  Message     │     ┌──────────────┐
│ Task B   ├─────────────────────►  Queue       ├────►│ Display Task │
├──────────┤  LCD_SetFont()      │  (8 cmds)    │     │              │
│ Task C   ├─────────────────────►              │     │ SPI+DMA 전송 │
└──────────┘                     └──────────────┘     │ (단독 접근)  │
                                                      └──────┬───────┘
                                                             │
                                                      ┌──────▼───────┐
                                                      │  ILI9341 LCD │
                                                      │  320×240     │
                                                      └──────────────┘
```

### 동작 흐름

1. 호출 태스크가 `LCD_DrawString()` 등 공개 API 호출
2. API 내부에서 `LCD_Cmd_t` 구조체를 구성하고 큐에 Put
3. 호출 태스크는 **즉시 리턴** (큐 가득 차면 대기)
4. Display 태스크가 큐에서 커맨드를 수신
5. 커맨드 타입에 따라 내부 렌더링 함수 호출 (SPI DMA 전송)

### 뮤텍스 vs Queue 비교

| 항목 | 뮤텍스 | Queue (채택) |
|------|--------|--------------|
| SPI 접근 | 여러 태스크 교대 점유 | **Display 태스크 단독 접근** |
| 호출 태스크 | SPI 전송 완료까지 Block | 큐에 넣고 즉시 리턴 |
| 내부 버퍼 보호 | 뮤텍스로 감싸야 함 | 불필요 (단일 소유자) |
| H7 포트 시 | SPI 코드가 각 태스크에 분산 | Display 태스크만 교체 |

## 헤더 설정 매크로 (`c_lcd_ili9341.h`)

### 하드웨어 설정

| 매크로 | 기본값 | 설명 |
|--------|--------|------|
| `LCD_SPI_INSTANCE` | `hspi2` | SPI 핸들 변수명 |
| `LCD_PIN_DC_PORT` | `LCD_DC_GPIO_Port` | DC 핀 GPIO 포트 (CubeMX 생성) |
| `LCD_PIN_DC_PIN` | `LCD_DC_Pin` | DC 핀 번호 |
| `LCD_PIN_RST_PORT` | `LCD_RST_GPIO_Port` | RST 핀 GPIO 포트 |
| `LCD_PIN_RST_PIN` | `LCD_RST_Pin` | RST 핀 번호 |
| `LCD_PIN_LED_PORT` | `LCD_LED_GPIO_Port` | 백라이트 핀 GPIO 포트 |
| `LCD_PIN_LED_PIN` | `LCD_LED_Pin` | 백라이트 핀 번호 |

### 태스크 / Queue 설정

| 매크로 | 기본값 | 설명 |
|--------|--------|------|
| `LCD_QUEUE_SIZE` | `8` | 커맨드 큐 깊이 |
| `LCD_TASK_STACK` | `512` | Display 태스크 스택 (word, ×4=바이트) |
| `LCD_TASK_PRIO` | `osPriorityNormal` | Display 태스크 우선순위 |
| `LCD_TEXT_MAX` | `64` | DrawString 텍스트 최대 길이 (NULL 포함) |

### 예: H7에서 SPI1 사용 시

```c
#define LCD_SPI_INSTANCE  hspi1
#define LCD_TASK_STACK    1024        /* 4KB — H7은 여유롭게 */
#include "c_lcd_ili9341.h"
```

## 커맨드 구조체

```c
typedef enum {
    LCD_CMD_FILL_SCREEN,
    LCD_CMD_FILL_RECT,
    LCD_CMD_DRAW_PIXEL,
    LCD_CMD_DRAW_STRING,
    LCD_CMD_SET_FONT,
    LCD_CMD_BACKLIGHT,
} LCD_CmdType_t;

typedef struct {
    LCD_CmdType_t type;
    union { ... };              /* 커맨드별 파라미터 */
} LCD_Cmd_t;                    /* 크기: ~80바이트 */
```

> `LCD_Cmd_t.drawString.text[]`는 **구조체 내부 64B 고정 버퍼**에 복사됩니다.
> 호출자의 스택 버퍼가 리턴 후 해제되어도 안전합니다.

## 메모리 사용량

| 항목 | 크기 | 비고 |
|------|------|------|
| `LCD_Cmd_t` × 큐 깊이 | ~80B × 8 = 640B | Queue 내부 버퍼 |
| `s_lineBuf` | 640B | 1 스캔라인 (320px × 2B) |
| Display 태스크 스택 | 512 × 4 = 2048B | `LCD_TASK_STACK` |
| DMA 세마포어 | ~80B | FreeRTOS 세마포어 |
| **합계** | **~3.4KB** | |

## 필수 조건

- STM32 HAL 드라이버
- FreeRTOS (CMSIS-RTOS V2)
- SPI Master TX Only + DMA TX 채널 (CubeMX에서 설정)
- 3개 GPIO: DC, RST, LED (백라이트)
- NSS: Hardware Output 또는 Software (현재 Hardware)

## 포팅 체크리스트

### 1단계: CubeMX 설정

| 설정 항목 | 위치 | 값 |
|-----------|------|-----|
| SPI 활성화 | Connectivity → SPIx | Master, Transmit Only, Hardware NSS |
| SPI 파라미터 | SPIx → Parameter | Mode 0 (CPOL=0, CPHA=0), 8bit, MSB First |
| SPI 클럭 | SPIx → Parameter | Prescaler 2~4 (10~20MHz 권장) |
| DMA TX 추가 | SPIx → DMA Settings | SPIx_TX, M→P, Normal |
| DMA 인터럽트 | NVIC | DMA Stream 인터럽트 **Enable** |
| SPI 인터럽트 | NVIC | SPIx global interrupt **Enable** |
| DC 핀 | GPIO | Output Push-Pull |
| RST 핀 | GPIO | Output Push-Pull, 초기값 **HIGH** |
| LED 핀 | GPIO | Output Push-Pull |

> **인터럽트 우선순위**: `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` (기본 5) 이상

### 2단계: 헤더 매크로 설정

SPI, GPIO 핀이 기본값과 다른 경우에만 `c_lcd_ili9341.h` 포함 전에 재정의:

```c
#define LCD_SPI_INSTANCE   hspi1
#define LCD_PIN_DC_PORT    GPIOA
#define LCD_PIN_DC_PIN     GPIO_PIN_3
/* ... 필요한 매크로만 재정의 */
#include "c_lcd_ili9341.h"
```

### 3단계: main.c 통합 (USER CODE 블록 내)

```c
/* USER CODE BEGIN Includes */
#include "c_lcd_ili9341.h"
/* USER CODE END Includes */
```

```c
/* USER CODE BEGIN 4 */
void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi == &LCD_SPI_INSTANCE) {
        LCD_DmaCpltHandler();
    }
}
/* USER CODE END 4 */
```

```c
/* USER CODE BEGIN 5 — 태스크에서 호출 */
LCD_Init();                /* Display 태스크 + 큐 생성, HW 초기화 자동 진행 */
osDelay(300);              /* HW 초기화 완료 대기 (RST + RegInit + SleepOut) */
LCD_FillScreen(LCD_BLACK);
LCD_SetFont(&Font_16x24);
LCD_DrawString(16, 16, "Hello LCD", LCD_CYAN, LCD_BLACK);
/* USER CODE END 5 */
```

### 4단계: 빌드 시스템 등록

```cmake
# CMakeLists.txt (프로젝트 루트)
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    Core/Src/c_lcd_ili9341.c
    Core/Src/c_font.c
)
```

## 공개 API

| 함수 | 설명 | Queue 동작 |
|------|------|------------|
| `LCD_Init()` | 세마포어 + 큐 + Display 태스크 생성 | 직접 실행 |
| `LCD_DmaCpltHandler()` | DMA 완료 ISR 핸들러 | ISR에서 직접 호출 |
| `LCD_FillScreen(color)` | 전체 화면 단색 채움 | 큐 Put |
| `LCD_FillRect(x,y,w,h,color)` | 사각형 채움 | 큐 Put |
| `LCD_DrawPixel(x,y,color)` | 단일 픽셀 | 큐 Put |
| `LCD_SetFont(font)` | 폰트 변경 | 큐 Put |
| `LCD_DrawChar(x,y,ch,fg,bg)` | 단일 문자 | 큐 Put (DrawString으로 변환) |
| `LCD_DrawString(x,y,str,fg,bg)` | 문자열 출력 | 큐 Put (텍스트 복사) |
| `LCD_Backlight(on)` | 백라이트 ON/OFF | 큐 Put |

> 큐가 가득 찬 경우 `osWaitForever`로 대기합니다 (데이터 손실 방지).

## MCU별 포팅 차이 요약

| 항목 | STM32F4 | STM32H7 |
|------|---------|---------|
| DMA 버퍼 | 일반 SRAM | D2 SRAM (`.dma_buffer` 섹션) |
| Cache 정렬 | 불필요 | `aligned(32)` + `SCB_CleanDCache_by_Addr()` |
| SPI 클럭 | APB2/Prescaler | Kernel Clock/Prescaler |
| HAL 헤더 | `stm32f4xx_hal.h` | `stm32h7xx_hal.h` |

## 제한사항

| 항목 | 설명 |
|------|------|
| ISR 컨텍스트 | 공개 API 호출 불가 (`osMessageQueuePut`는 ISR에서 사용 불가) |
| 텍스트 길이 | `LCD_TEXT_MAX` (64) 초과 시 잘림 |
| 커맨드 지연 | 큐 기반이므로 호출 시점 ↔ 화면 갱신 사이 약간의 지연 발생 |
| 큐 오버플로우 | 큐 깊이(`LCD_QUEUE_SIZE`) 초과 시 호출 태스크 Block |
| 동기 응답 | 렌더링 완료 확인 API 없음 (fire-and-forget) |

## 트러블슈팅

| 증상 | 원인 | 해결 |
|------|------|------|
| 아무것도 표시 안 됨 | DMA/SPI 인터럽트 미활성화 | CubeMX NVIC에서 Enable |
| 아무것도 표시 안 됨 | `LCD_Init()` 미호출 | 태스크 내에서 호출 확인 |
| 아무것도 표시 안 됨 | RST 핀 초기값 LOW | CubeMX에서 GPIO 초기값 HIGH 설정 |
| 첫 화면만 나오고 멈춤 | `HAL_SPI_TxCpltCallback` 미구현 | main.c USER CODE 4에 추가 |
| 색상이 이상함 | MADCTL 또는 Pixel Format 불일치 | `ILI9341_RegInit()` 확인 |
| 글자가 잘림 | `LCD_TEXT_MAX` 부족 | 매크로 값 증가 |
| 커맨드 무시됨 | `LCD_Init()` 후 바로 커맨드 전송 | `osDelay(300)` 추가 (HW 초기화 대기) |
| HardFault (H7) | DMA 버퍼 DTCM 배치 | `.dma_buffer` 섹션으로 D2 SRAM 배치 |

## 검증 완료 환경

- **NUCLEO-F446RE** (STM32F446RE, Cortex-M4, 80MHz)
- **FreeRTOS** v10.3.1, CMSIS-RTOS V2, heap_4
- **starm-clang** (NEWLIB), `configUSE_NEWLIB_REENTRANT=1`
- **SPI2** + DMA1 Stream4, 20MHz, Hardware NSS
- **LCD**: ILI9341 2.4" TFT, 320×240 Landscape
- 2개 태스크 동시 LCD 출력 (MainTask + TestTask2) — Queue 기반 멀티태스크 안정성 확인
