# c_debug 포팅 가이드

FreeRTOS + STM32 HAL 환경에서 DMA 링버퍼 기반 비동기 printf 디버그 출력 모듈.

## 개요

| 항목 | 내용 |
|------|------|
| 파일 | `c_debug.c`, `c_debug.h` |
| 동작 방식 | `printf()` → `_write()` 재정의 → 링버퍼 복사 → DMA TX |
| 동기화 | FreeRTOS 뮤텍스 (`xSemaphoreCreateMutex`) |
| 오버플로우 | 초과 바이트 무시 (블로킹 없음, 크래시 없음) |
| ISR 사용 | 불가 (설계상 태스크 컨텍스트 전용) |

## 아키텍처

```
┌──────────┐    ┌──────────┐    ┌───────────┐    ┌──────────┐
│ Task A   │    │ _write() │    │  링버퍼   │    │ DMA TX   │
│ printf() ├───►│ 뮤텍스   ├───►│ s_txBuf[] ├───►│ UART DR  │──► TX핀
│          │    │ 획득/해제│    │ 1024 byte │    │          │
├──────────┤    └──────────┘    └───────────┘    └────┬─────┘
│ Task B   │                                          │
│ printf() ├─── (대기) ──►                             │
└──────────┘                                          │
                                                      ▼
                                          ┌───────────────────┐
                                          │ DMA TC 인터럽트   │
                                          │   → UART TC 인터럽트│
                                          │     → TxCpltCallback│
                                          │       → 다음 청크  │
                                          └───────────────────┘
```

## 필수 조건

- STM32 HAL 드라이버
- FreeRTOS (CMSIS-RTOS V2 또는 네이티브 API)
- UART + DMA TX 채널 (CubeMX에서 설정)
- C 라이브러리: Newlib (`configUSE_NEWLIB_REENTRANT=1`) 또는 Picolibc

## 포팅 체크리스트

### 1단계: CubeMX 설정

| 설정 항목 | 위치 | 값 |
|-----------|------|-----|
| UART 활성화 | Connectivity → USARTx | Asynchronous, 원하는 Baud Rate |
| DMA TX 추가 | USARTx → DMA Settings → Add | USARTx_TX, Memory To Peripheral, Normal |
| DMA 인터럽트 | NVIC Settings | DMA Streamx 인터럽트 **Enable** |
| UART 글로벌 인터럽트 | NVIC Settings | USARTx global interrupt **Enable** |
| DMA 인터럽트 우선순위 | NVIC Settings | 5 이상 (FreeRTOS 관리 범위 내) |
| UART 인터럽트 우선순위 | NVIC Settings | 5 이상 (FreeRTOS 관리 범위 내) |
| FreeRTOS Newlib | FREERTOS → Advanced | `USE_NEWLIB_REENTRANT` = Enabled |

> **중요**: DMA 인터럽트와 UART 글로벌 인터럽트가 **모두** 필요합니다.
> HAL 내부에서 DMA TC → UART TC 순서로 체인 호출되므로 하나라도 빠지면
> `HAL_UART_TxCpltCallback()`이 호출되지 않아 출력이 멈춥니다.

> **인터럽트 우선순위**: `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` (기본 5) 이상이어야
> FreeRTOS 관리 범위에 포함됩니다. 이보다 높은 우선순위(숫자가 작은)를 사용하면
> FreeRTOS 커널과 충돌할 수 있습니다.

> **USE_NEWLIB_REENTRANT**: Newlib의 `printf()`/`sprintf()` 계열 함수는 내부적으로
> `_impure_ptr`이라는 전역 구조체(`struct _reent`)를 통해 버퍼, errno, locale 등을
> 관리합니다. 이 옵션을 활성화하면 FreeRTOS가 **태스크마다 독립적인 `struct _reent`** 를
> TCB에 할당하고, 컨텍스트 스위칭 시 `_impure_ptr`을 해당 태스크의 구조체로 교체합니다.
> 비활성화 상태에서 여러 태스크가 동시에 `printf()`를 호출하면 **동일한 전역 `_reent` 구조체를
> 경쟁적으로 수정**하여 출력이 깨지거나 힙 메타데이터가 손상되어 HardFault가 발생할 수 있습니다.
>
> - CubeMX: FREERTOS → Config parameters → Advanced → `USE_NEWLIB_REENTRANT` = **Enabled**
> - 결과: `FreeRTOSConfig.h`에 `#define configUSE_NEWLIB_REENTRANT 1` 생성
> - 부작용: 태스크당 약 96바이트(아키텍처에 따라 다름) RAM 추가 소비

### 2단계: 코드 수정 (3곳)

#### 2-1. `c_debug.h` — HAL 헤더

```c
#include "main.h"
// 변경 불필요 — main.h가 해당 MCU의 HAL 헤더를 포함하므로 그대로 사용
```

#### 2-2. `c_debug.c` — UART 핸들 변경

```c
// 수정 전
extern UART_HandleTypeDef huart2;

// 수정 후 (예: USART3 사용 시)
extern UART_HandleTypeDef huart3;
```

`StartDMA()` 함수 내부도 동일하게 변경:

```c
// 수정 전
HAL_UART_Transmit_DMA(&huart2, &s_txBuf[t], len);

// 수정 후
HAL_UART_Transmit_DMA(&huart3, &s_txBuf[t], len);
```

#### 2-3. `c_debug.c` — STM32H7 DMA 버퍼 배치 (H7 전용)

STM32H7은 D-Cache가 있으므로 DMA 버퍼를 Non-Cacheable 영역에 배치해야 합니다.

```c
// F4 (캐시 없음) — 일반 SRAM
static uint8_t s_txBuf[DEBUG_TX_BUF_SIZE];

// H7 (D-Cache 있음) — D2 SRAM에 배치
__attribute__((section(".dma_buffer"), aligned(32)))
static uint8_t s_txBuf[DEBUG_TX_BUF_SIZE];
```

H7 사용 시 링커 스크립트에 `.dma_buffer` 섹션 추가 필요:

```ld
/* 링커 스크립트 예시 (STM32H7) */
.dma_buffer (NOLOAD) :
{
    . = ALIGN(32);
    *(.dma_buffer)
    . = ALIGN(32);
} >RAM_D2
```

### 3단계: main.c 통합 (USER CODE 블록 내)

```c
/* USER CODE BEGIN Includes */
#include "c_debug.h"
#include <stdio.h>
/* USER CODE END Includes */
```

```c
/* USER CODE BEGIN 2 */
Debug_Init();       // MX_USARTx_UART_Init() 이후, osKernelInitialize() 이전
/* USER CODE END 2 */
```

```c
/* USER CODE BEGIN 4 */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart == &huart2) {     // ← 사용하는 UART 핸들로 변경
        Debug_TxCpltHandler();
    }
}
/* USER CODE END 4 */
```

### 4단계: 빌드 시스템 등록

사용자 소스 파일은 **상위 `CMakeLists.txt`** 의 `target_sources`에 등록합니다.
`cmake/stm32cubemx/CMakeLists.txt`에 추가하면 CubeMX 재생성 시 사라집니다.

```cmake
# CMakeLists.txt (프로젝트 루트)
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    Core/Src/c_debug.c
)
```

## MCU별 포팅 차이 요약

| 항목 | STM32F4 | STM32H7 |
|------|---------|---------|
| DMA 버퍼 | 일반 SRAM | D2 SRAM (`.dma_buffer` 섹션) |
| Cache 정렬 | 불필요 | `aligned(32)` 필수 |
| Cache 관리 | 불필요 | `SCB_CleanDCache_by_Addr()` 고려 |
| HAL 헤더 | `stm32f4xx_hal.h` | `stm32h7xx_hal.h` |
| `_write()` | Newlib weak 재정의 | 동일 |
| FreeRTOS 뮤텍스 | 동일 | 동일 |

## 설정 변경

### 링버퍼 크기

`c_debug.h`에서 조정:

```c
#define DEBUG_TX_BUF_SIZE   1024u   // 기본값
#define DEBUG_TX_BUF_SIZE   2048u   // 출력량이 많은 경우
#define DEBUG_TX_BUF_SIZE   512u    // RAM 절약 시
```

> 링버퍼가 가득 차면 초과 바이트는 무시됩니다 (블로킹 없음).
> 메시지 드롭이 발생하면 크기를 늘리거나 출력 빈도를 줄여야 합니다.

### 태스크 스택 크기

`printf()`는 내부적으로 `vsnprintf()`를 호출하며 300~500 바이트의 스택을 사용합니다.
printf를 호출하는 태스크의 스택은 **최소 512 word (2KB)** 를 권장합니다.

```c
const osThreadAttr_t Task_attributes = {
    .name = "MyTask",
    .stack_size = 512 * 4,      // 2KB
    .priority = (osPriority_t) osPriorityNormal,
};
```

## 제한사항

| 항목 | 설명 |
|------|------|
| ISR 컨텍스트 | `printf()` 사용 불가 — 뮤텍스 기반이므로 ISR에서 호출 시 크래시 |
| 오버플로우 | 링버퍼 초과 시 메시지 드롭 (알림 없음) |
| Baud Rate | 115200bps 기준 약 11.5KB/s — 초당 약 500줄 (20byte/줄) 출력 가능 |
| 스케줄러 시작 전 | 뮤텍스 없이 동작 (단일 컨텍스트이므로 안전) |
| `_write()` 충돌 | `syscalls.c`의 `_write()`는 weak이므로 c_debug.c 버전이 자동 우선 |

## 트러블슈팅

| 증상 | 원인 | 해결 |
|------|------|------|
| 첫 줄만 출력되고 멈춤 | UART 글로벌 인터럽트 미활성화 | CubeMX → NVIC에서 USARTx 인터럽트 Enable |
| 아무것도 출력 안 됨 | DMA 인터럽트 미활성화 | CubeMX → NVIC에서 DMA Stream 인터럽트 Enable |
| 아무것도 출력 안 됨 | `Debug_Init()` 미호출 | `main.c`에서 `MX_USARTx_Init()` 이후 호출 확인 |
| 아무것도 출력 안 됨 | `HAL_UART_TxCpltCallback` 미구현 | `main.c` USER CODE BEGIN 4에 콜백 추가 |
| 메시지가 깨짐/섞임 | `configUSE_NEWLIB_REENTRANT=0` | FreeRTOS Advanced → Enable 후 재생성 |
| 메시지 일부 누락 | 링버퍼 오버플로우 | `DEBUG_TX_BUF_SIZE` 증가 또는 출력 빈도 감소 |
| HardFault | ISR에서 printf 호출 | 태스크 컨텍스트에서만 사용 — ISR에서는 사용 금지 |
| HardFault (H7) | DMA 버퍼가 DTCM에 위치 | `.dma_buffer` 섹션으로 D2 SRAM에 배치 |
| 링크 에러 (undefined symbol) | 빌드에 c_debug.c 미등록 | **상위** CMakeLists.txt의 target_sources에 추가 |

## 검증 완료 환경

- **NUCLEO-F446RE** (STM32F446RE, Cortex-M4, 84MHz)
- **FreeRTOS** v10.3.1, CMSIS-RTOS V2, heap_4
- **starm-clang** (NEWLIB), `configUSE_NEWLIB_REENTRANT=1`
- **USART2** + DMA1 Stream6, 115200bps
- 2개 태스크 동시 printf (500ms 동일 주기) — 경쟁상태 안정성 확인
- 장시간 연속 출력 — 메시지 무결성 확인
