# STM32H7 ThreadX 네이티브 적용 가이드

> F446RE 프로젝트(system-threadx 브랜치) 마이그레이션 경험 기반  
> CMSIS-RTOS2 래퍼를 **사용하지 않고** ThreadX 네이티브 API를 직접 사용하는 방법

---

## 1. CubeMX 설정

### 1.1 Middleware 선택
- **Middleware and Software Packs → X-CUBE-AZRTOS-H7**
  - `ThreadX` 체크 ✅
  - `CMSIS RTOS2` / `CMSIS RTOS2 wrappers` → **선택 안 함** ❌

### 1.2 Timebase 설정 (중요)
| 항목 | 설정값 | 이유 |
|---|---|---|
| HAL Timebase | TIM6 (또는 사용하지 않는 TIM) | SysTick을 ThreadX 전용으로 해방 |
| SysTick | ThreadX 사용 | `tx_timer_interrupt()` 호출 |

> H7은 TIM6/TIM7이 기본 HAL Timebase로 적합. SysTick을 HAL에 주면 ThreadX tick이 깨진다.

### 1.3 Clock 설정
- SYSCLK 확인 후 `tx_initialize_low_level.S` 내 `SYSTEM_CLOCK` 값과 일치시킬 것  
  (아래 2.3 참고)

---

## 2. 프로젝트 생성 후 필수 수정

### 2.1 tx_user.h — tick 주파수 설정 (**가장 중요**)

```c
/* tx_user.h */

/* 1ms tick = 1000Hz — 이것이 없으면 기본값 100Hz(10ms tick)로 동작 */
#define TX_TIMER_TICKS_PER_SECOND    1000

/* 성능 최적화 (선택) */
#define TX_DISABLE_PREEMPTION_THRESHOLD
#define TX_DISABLE_NOTIFY_CALLBACKS
```

> ⚠️ **F446RE 함정**: `TX_TIMER_TICKS_PER_SECOND`를 설정하지 않으면 기본값 100이 되어  
> `tx_thread_sleep(1000)`이 1초가 아닌 **10초**로 동작한다.

### 2.2 cmake/stm32cubemx/CMakeLists.txt — CMSIS 래퍼 제거

CubeMX가 자동 생성한 cmake에서 CMSIS 래퍼 관련 항목 삭제:

```cmake
# 삭제 대상 (CubeMX가 자동 생성하는 경우 있음)
# - CMSIS_RTOS_ThreadX 라이브러리 타겟
# - Drivers/CMSIS/RTOS2/Include 경로
# - Middlewares/ST/cmsis_rtos_threadx 경로
# - MX_LINK_LIBS에서 CMSIS_RTOS_ThreadX

# 남길 것: ThreadX 타겟만
target_link_libraries(${CMAKE_PROJECT_NAME} ThreadX)
```

### 2.3 tx_initialize_low_level.S — SYSTEM_CLOCK 확인

H7 포트 경로: `Middlewares/ST/threadx/ports/cortex_m7/gnu/src/tx_initialize_low_level.S`

```asm
SYSTEM_CLOCK      =   550000000   ; H7 SYSCLK (Hz)에 맞게 수정
SYSTICK_CYCLES    =   ((SYSTEM_CLOCK / TX_TIMER_TICKS_PER_SECOND) - 1)
```

> F4 포트 경로: `ports/cortex_m4/gnu/src/tx_initialize_low_level.S`

---

## 3. app_threadx.c — 태스크 생성 패턴

```c
/* USER CODE BEGIN Includes */
#include "main.h"
#include "tx_api.h"
/* USER CODE END Includes */

/* USER CODE BEGIN PFP */
extern TX_THREAD MainTaskHandle;
extern TX_THREAD TestTask2Handle;
void StartDefaultTask(ULONG argument);
void TestTask2Entry(ULONG argument);

/* 정적 스택 선언 (ULONG 단위 — sizeof() 시 자동으로 바이트 크기 계산됨) */
static ULONG s_mainTaskStack[512];    /* 512 × 4 = 2048 bytes */
static ULONG s_testTask2Stack[512];
/* USER CODE END PFP */

UINT App_ThreadX_Init(VOID *memory_ptr)
{
    UINT ret = TX_SUCCESS;
    TX_BYTE_POOL *byte_pool = (TX_BYTE_POOL*)memory_ptr;

    /* USER CODE BEGIN App_ThreadX_Init */
    (void)byte_pool;  /* 정적 스택 사용 시 byte_pool 불필요 */

    tx_thread_create(&TestTask2Handle, "TestTask2", TestTask2Entry, 0,
                     s_testTask2Stack, sizeof(s_testTask2Stack),
                     4, 4, TX_NO_TIME_SLICE, TX_AUTO_START);

    tx_thread_create(&MainTaskHandle, "MainTask", StartDefaultTask, 0,
                     s_mainTaskStack, sizeof(s_mainTaskStack),
                     4, 4, TX_NO_TIME_SLICE, TX_AUTO_START);
    /* USER CODE END App_ThreadX_Init */
    return ret;
}

void MX_ThreadX_Init(void)
{
    /* USER CODE BEGIN Before_Kernel_Start */
    /* USER CODE END Before_Kernel_Start */

    tx_kernel_enter();  /* osKernelStart() 절대 사용 금지 */
}
```

---

## 4. main.c — 태스크 선언 및 본문 패턴

### 4.1 전역 변수 (USER CODE BEGIN PV)

```c
/* USER CODE BEGIN PV */
TX_THREAD MainTaskHandle;
TX_THREAD TestTask2Handle;
/* USER CODE END PV */
```

### 4.2 함수 선언 (USER CODE BEGIN PFP)

```c
/* USER CODE BEGIN PFP */
void StartDefaultTask(ULONG argument);
void TestTask2Entry(ULONG argument);
/* USER CODE END PFP */
```

### 4.3 태스크 본문 위치 규칙 (**CubeMX 재생성 보호**)

```c
/* USER CODE BEGIN 4 */

void TestTask2Entry(ULONG argument)
{
    (void)argument;
    for (;;) {
        /* ... */
        tx_thread_sleep(1000);  /* 1초 */
    }
}

void StartDefaultTask(ULONG argument)
{
    /* USER CODE BEGIN 5 */
    (void)argument;
    /* ... */
    for (;;) {
        tx_thread_sleep(500);
    }
    /* USER CODE END 5 */
}

/* USER CODE END 4 */   ← 반드시 이 앞에 위치해야 재생성 시 보존
```

> ⚠️ **F446RE 함정**: 태스크 함수가 `USER CODE END 4` 밖에 있으면  
> CubeMX 재생성 시 **함수 전체가 삭제**되어 링크 에러 발생.

---

## 5. 드라이버별 ThreadX 네이티브 API 대응표

| FreeRTOS / CMSIS-RTOS2 | ThreadX 네이티브 |
|---|---|
| `TaskHandle_t` / `osThreadId_t` | `TX_THREAD` |
| `xTaskCreate()` / `osThreadNew()` | `tx_thread_create()` |
| `vTaskDelay(ms)` / `osDelay(ms)` | `tx_thread_sleep(ms)` ※tick=1000Hz 시 |
| `SemaphoreHandle_t` / `osSemaphoreId_t` | `TX_SEMAPHORE` |
| `xSemaphoreGive()` / `osSemaphoreRelease()` | `tx_semaphore_put()` |
| `xSemaphoreTake()` / `osSemaphoreAcquire()` | `tx_semaphore_get()` |
| `MutexHandle_t` / `osMutexId_t` | `TX_MUTEX` |
| `xSemaphoreTakeRecursive()` / `osMutexAcquire()` | `tx_mutex_get()` |
| `xSemaphoreGiveRecursive()` / `osMutexRelease()` | `tx_mutex_put()` |
| `QueueHandle_t` / `osMessageQueueId_t` | `TX_QUEUE` |
| `xQueueSend()` / `osMessageQueuePut()` | `tx_queue_send()` |
| `xQueueReceive()` / `osMessageQueueGet()` | `tx_queue_receive()` |
| `EventGroupHandle_t` / `osEventFlagsId_t` | `TX_EVENT_FLAGS_GROUP` |
| `xEventGroupSetBits()` / `osEventFlagsSet()` | `tx_event_flags_set()` |
| `xEventGroupWaitBits()` / `osEventFlagsWait()` | `tx_event_flags_get()` |
| `osKernelStart()` | `tx_kernel_enter()` |
| `portMAX_DELAY` / `osWaitForever` | `TX_WAIT_FOREVER` |
| `NULL (no wait)` / `0` | `TX_NO_WAIT` |

---

## 6. TX_QUEUE 크기 제한 주의사항

ThreadX `TX_QUEUE`의 메시지 크기는 **최대 16 ULONG = 64바이트**.  
구조체가 64바이트를 초과하면 **포인터 기반 풀 패턴** 사용:

```c
/* 정적 커맨드 풀 + 1-ULONG 큐에 포인터 전달 */
static MyCmd_t    s_cmdPool[QUEUE_SIZE];
static TX_SEMAPHORE s_poolSem;           /* 자유 슬롯 카운터 */
static UINT         s_poolIdx;
static TX_QUEUE     s_cmdQueue;
static ULONG        s_cmdQueueBuf[QUEUE_SIZE];  /* TX_1_ULONG */

/* 할당 */
tx_semaphore_get(&s_poolSem, TX_WAIT_FOREVER);
MyCmd_t *p = &s_cmdPool[s_poolIdx++];
/* ... 필드 채움 ... */
ULONG msg = (ULONG)p;
tx_queue_send(&s_cmdQueue, &msg, TX_WAIT_FOREVER);

/* 수신 후 반환 */
ULONG msg;
tx_queue_receive(&s_cmdQueue, &msg, TX_WAIT_FOREVER);
MyCmd_t *p = (MyCmd_t *)msg;
/* ... 처리 ... */
tx_semaphore_put(&s_poolSem);
```

---

## 7. ISR 내 ThreadX API 사용 규칙

ISR에서는 `TX_NO_WAIT`만 사용 가능 (블로킹 불가):

```c
/* ISR (DMA 완료 등) */
void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi)
{
    tx_semaphore_put(&s_dmaSem);  /* put은 ISR에서 OK */
}

/* ISR에서 절대 사용 금지 */
// tx_semaphore_get(&sem, TX_WAIT_FOREVER);  ❌
// tx_thread_sleep(100);                     ❌
```

---

## 8. H7 특이사항 (F4와 차이점)

### 8.1 캐시 관리 (DMA 사용 시 필수)
H7은 D-Cache가 기본 활성화되어 있어 DMA 버퍼는 반드시 캐시 정합성 처리 필요:

```c
/* DMA 전송 전 캐시 flush */
SCB_CleanDCache_by_Addr((uint32_t *)buf, len);

/* DMA 수신 후 캐시 invalidate */
SCB_InvalidateDCache_by_Addr((uint32_t *)buf, len);
```

또는 DMA 버퍼를 `__attribute__((section(".DMA_buffer")))` + 비캐시 영역 배치.

### 8.2 ThreadX 포트 경로
```
Middlewares/ST/threadx/ports/cortex_m7/gnu/src/
```
- `tx_thread_schedule.S`
- `tx_initialize_low_level.S`  ← SYSTEM_CLOCK 수정 필요

### 8.3 MPU 설정
ThreadX는 스택 오버플로 감지를 위해 MPU 활용 가능.  
H7 CubeMX에서 MPU 설정 시 ThreadX 스택 영역을 별도 region으로 지정 권장.

### 8.4 우선순위
ThreadX: **낮은 숫자 = 높은 우선순위** (FreeRTOS와 반대).  
- `0` = 최고 우선순위  
- 일반 앱 태스크: `3`~`5` 권장  
- ISR 관련 태스크 (DMA 완료 처리 등): `2`~`3`

---

## 9. 검증 체크리스트

- [ ] `TX_TIMER_TICKS_PER_SECOND = 1000` 설정 확인
- [ ] HAL Timebase가 SysTick이 아닌 TIM 사용 확인
- [ ] `tx_kernel_enter()` 호출 확인 (`osKernelStart()` 아님)
- [ ] 모든 태스크 함수가 `USER CODE END 4` 앞에 위치하는지 확인
- [ ] CMSIS-RTOS2 헤더(`cmsis_os2.h`) include 없는지 확인
- [ ] cmake에서 CMSIS_RTOS_ThreadX 타겟 제거 확인
- [ ] ISR 내 TX API는 `TX_NO_WAIT` 또는 `_put` 계열만 사용 확인
- [ ] H7: DMA 버퍼 캐시 flush/invalidate 처리 확인
