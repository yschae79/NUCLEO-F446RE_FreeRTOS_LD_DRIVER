/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_threadx.c
  * @author  MCD Application Team
  * @brief   ThreadX applicative file
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2021 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "app_threadx.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "main.h"
#include "c_adc.h"
#include "c_debug.h"
#include "tx_api.h"
#include "tx_trace.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/** @brief TraceX 덤프 버퍼 크기 (16KB = 약 500개 이벤트) */
#define TRACE_BUFFER_SIZE    (16u * 1024u)

/** @brief TraceDump 태스크 이벤트 플래그 */
#define TRACE_FULL_FLAG      (0x01u)
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */
/** @brief TraceX 이벤트 로깅 버퍼 */
static UCHAR s_traceBuffer[TRACE_BUFFER_SIZE];

/** @brief TraceDump 태스크 동기화용 이벤트 플래그 */
static TX_EVENT_FLAGS_GROUP s_traceFlags;

/** @brief TraceDump 태스크 TCB + 스택 */
static TX_THREAD s_traceDumpTask;
static ULONG     s_traceDumpStack[256];
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */
extern TX_THREAD MainTaskHandle;
extern TX_THREAD TestTask2Handle;
void StartDefaultTask(ULONG argument);
void TestTask2Entry(ULONG argument);
/* 태스크 스택 (워드 단위, 512 워드 = 2048 바이트) */
static ULONG s_mainTaskStack[512];
static ULONG s_testTask2Stack[512];

/* TraceX 프레임 마직 바이트 — printf와 binary 트레이스 패킷 구분 */
static const uint8_t s_traceMagicStart[8] = {0x54,0x52,0x43,0x58,0xFF,0xFE,0xFD,0xFC};
static const uint8_t s_traceMagicEnd[8]   = {0xFC,0xFD,0xFE,0xFF,0x54,0x52,0x43,0x58};

/**
 * @brief  TraceX 버퍼 가득 콜백 — SysTick ISR 컨텍스트 (TX_TIMER_PROCESS_IN_ISR)
 * @note   event flags set만 수행 (블록하는 API 금지)
 */
static VOID TraceBufFullCb(VOID *buffer)
{
    (void)buffer;
    tx_event_flags_set(&s_traceFlags, TRACE_FULL_FLAG, TX_OR);
}

/**
 * @brief  TraceDump 태스크 — 버퍼 가득 신호 대기 후 UART2로 덤프
 * @details 프로토콜: [START_MAGIC 8B][size 4B LE][raw buffer][END_MAGIC 8B]
 *          PC의 tracex_recv.py가 마직 패턴을 찾아 .trx 파일로 저장
 */
static void TraceDumpEntry(ULONG argument)
{
    (void)argument;
    ULONG flags;
    for (;;) {
        /* 버퍼 가득 신호 대기 */
        tx_event_flags_get(&s_traceFlags, TRACE_FULL_FLAG,
                           TX_AND_CLEAR, &flags, TX_WAIT_FOREVER);

        /* 새 이벤트가 시작되지 않도록 갖시적으로 트레이스 중단 */
        tx_trace_disable();

        /* 프레임 전송: [START_MAGIC][size 4B LE][raw][END_MAGIC] */
        uint32_t sz = TRACE_BUFFER_SIZE;
        Debug_SendBinary(s_traceMagicStart, sizeof(s_traceMagicStart));
        Debug_SendBinary((const uint8_t *)&sz, sizeof(sz));
        Debug_SendBinary(s_traceBuffer, TRACE_BUFFER_SIZE);
        Debug_SendBinary(s_traceMagicEnd, sizeof(s_traceMagicEnd));

        /* 트레이스 재시작 */
        tx_trace_enable(s_traceBuffer, TRACE_BUFFER_SIZE, 40);
        tx_trace_buffer_full_notify(TraceBufFullCb);
    }
}
/* USER CODE END PFP */

/**
  * @brief  Application ThreadX Initialization.
  * @param memory_ptr: memory pointer
  * @retval int
  */
UINT App_ThreadX_Init(VOID *memory_ptr)
{
  UINT ret = TX_SUCCESS;
  TX_BYTE_POOL *byte_pool = (TX_BYTE_POOL*)memory_ptr;

  /* USER CODE BEGIN App_ThreadX_Init */
  (void)byte_pool;

  /* 트레이스 이벤트 플래그 생성 */
  tx_event_flags_create(&s_traceFlags, "trace_flags");

  /* TraceX 트레이스 시작 — 오브젝트 레지스트리 40개
   * (ADC/Display/Main/TestTask2/TraceDump/Timer 스레드 포함) */
  tx_trace_enable(s_traceBuffer, TRACE_BUFFER_SIZE, 40);
  tx_trace_buffer_full_notify(TraceBufFullCb);

  /* TraceDump 태스크 — priority=10 (최저우선순위) */
  tx_thread_create(&s_traceDumpTask, "TraceDump", TraceDumpEntry, 0,
                   s_traceDumpStack, sizeof(s_traceDumpStack),
                   10, 10, TX_NO_TIME_SLICE, TX_AUTO_START);

  /* ADC 드라이버 초기화 (AdcTask 생성 포함) */
  ADC_Init();

  /* TestTask2: 1s 주기 ADC printf */
  tx_thread_create(&TestTask2Handle, "TestTask2", TestTask2Entry, 0,
                   s_testTask2Stack, sizeof(s_testTask2Stack),
                   4, 4, TX_NO_TIME_SLICE, TX_AUTO_START);

  /* MainTask: LCD 초기화 + 500ms ADC 모니터 */
  tx_thread_create(&MainTaskHandle, "MainTask", StartDefaultTask, 0,
                   s_mainTaskStack, sizeof(s_mainTaskStack),
                   4, 4, TX_NO_TIME_SLICE, TX_AUTO_START);

  /* USER CODE END App_ThreadX_Init */

  return ret;
}

/**
  * @brief  MX_ThreadX_Init
  * @param  None
  * @retval None
  */
void MX_ThreadX_Init(void)
{
  /* USER CODE BEGIN  Before_Kernel_Start */

  /* USER CODE END  Before_Kernel_Start */

  tx_kernel_enter();

  /* USER CODE BEGIN  Kernel_Start_Error */

  /* USER CODE END  Kernel_Start_Error */
}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */
