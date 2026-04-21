/**
 * @file    c_adc.c
 * @brief   ADC1 Circular DMA 더블버퍼 드라이버 구현
 * @details TIM2 TRGO(10kHz) → ADC1 불연속 스캔(10채널) → DMA2_Stream0 Circular
 *
 *          더블버퍼 동작:
 *            HT 발생: s_adcDmaBuf[0..9]  유효 → g_adcResult 복사
 *            TC 발생: s_adcDmaBuf[10..19] 유효 → g_adcResult 복사
 *            DMA Circular 모드이므로 TC 이후 자동 wrap, 수동 재시작 불필요
 */
#include "c_adc.h"
#include "main.h"
#include "tx_api.h"
#include <string.h>

/* ── 외부 핸들 선언 ──────────────────────────────────────────────────── */
/* ADC_INSTANCE, ADC_TIM_INSTANCE 매크로가 c_adc.h에서 정의되어 있으므로
   전처리기가 아래를 각각 hadc1, htim2 로 치환함 */
extern ADC_HandleTypeDef ADC_INSTANCE;
extern TIM_HandleTypeDef ADC_TIM_INSTANCE;

/* ── 내부 상수 ───────────────────────────────────────────────────────── */
/** @brief DMA Half-Transfer 이벤트 플래그 비트 */
#define FLAG_HT     (1U << 0)
/** @brief DMA Transfer-Complete 이벤트 플래그 비트 */
#define FLAG_TC     (1U << 1)

/* ── 내부 변수 ───────────────────────────────────────────────────────── */

/**
 * @brief DMA Circular 타겟 버퍼 (DMA가 직접 기록)
 * @note  volatile: CPU 캐시 최적화를 방지하여 항상 최신 메모리 값을 읽도록 함
 */
static volatile uint16_t s_adcDmaBuf[ADC_DMA_BUF_SIZE];

/** @brief ADC 이벤트 플래그 그룹 — HT/TC 신호 전달 */
static TX_EVENT_FLAGS_GROUP s_adcFlags;
static TX_THREAD            s_adcTcb;
static ULONG                s_adcStack[ADC_TASK_STACK];

/* ── 공유 결과 버퍼 정의 ─────────────────────────────────────────────── */
volatile uint16_t g_adcResult[ADC_CH_COUNT];

/* ── 내부 함수 선언 ──────────────────────────────────────────────────── */
static void AdcTask(ULONG argument);

/* ═══════════════════════════════════════════════════════════════════════ */
/* 공개 API 구현                                                           */
/* ═══════════════════════════════════════════════════════════════════════ */

/**
 * @brief  ADC 드라이버 초기화
 * @details 버퍼 초기화 후 ADC 태스크를 생성함.
 *          DMA 및 타이머 시작은 태스크 내부에서 수행되므로
 *          osKernelInitialize() 이후에 호출해야 함.
 */
void ADC_Init(void)
{
    memset((void *)s_adcDmaBuf, 0, sizeof(s_adcDmaBuf));
    memset((void *)g_adcResult,  0, sizeof(g_adcResult));
    tx_event_flags_create(&s_adcFlags, "adc_flags");
    tx_thread_create(&s_adcTcb, "AdcTask", AdcTask, 0,
                     s_adcStack, sizeof(s_adcStack),
                     ADC_TASK_PRIO, ADC_TASK_PRIO,
                     TX_NO_TIME_SLICE, TX_AUTO_START);
}

/**
 * @brief  DMA Half-Transfer 완료 콜백 핸들러 (ISR 컨텍스트)
 * @note   NVIC 우선순위 5 → FreeRTOS configMAX_SYSCALL_INTERRUPT_PRIORITY 이하
 *         → osThreadFlagsSet ISR-safe 호출 가능
 */
void ADC_HalfCpltHandler(void)
{
    tx_event_flags_set(&s_adcFlags, FLAG_HT, TX_OR);
}

/**
 * @brief  DMA Transfer-Complete 완료 콜백 핸들러 (ISR 컨텍스트)
 */
void ADC_CpltHandler(void)
{
    tx_event_flags_set(&s_adcFlags, FLAG_TC, TX_OR);
}

/* ═══════════════════════════════════════════════════════════════════════ */
/* ADC 태스크                                                              */
/* ═══════════════════════════════════════════════════════════════════════ */

/**
 * @brief  ADC 태스크
 * @details 시작 시 DMA·타이머를 직접 구동하고, 이후 HT/TC 플래그를 대기하여
 *          유효한 버퍼 절반을 g_adcResult로 복사함.
 *
 *          HT 수신: s_adcDmaBuf[0..ADC_CH_COUNT-1]      → g_adcResult
 *          TC 수신: s_adcDmaBuf[ADC_CH_COUNT..BUF_SIZE-1] → g_adcResult
 *
 *          Circular 모드이므로 TC 이후 DMA가 자동으로 인덱스 0부터 재시작.
 */
static void AdcTask(ULONG argument)
{
    (void)argument;

    /* DMA Circular 시작 — HAL이 TC 인터럽트를 자동 활성화 */
    HAL_ADC_Start_DMA(&ADC_INSTANCE, (uint32_t *)s_adcDmaBuf, ADC_DMA_BUF_SIZE);

    /* HT 인터럽트 수동 활성화 (HAL_ADC_Start_DMA는 TC만 활성화하므로) */
    __HAL_DMA_ENABLE_IT(ADC_INSTANCE.DMA_Handle, DMA_IT_HT);

    /* TIM2 시작 → TRGO 발생 → ADC 불연속 트리거 10kHz 시작 */
    HAL_TIM_Base_Start(&ADC_TIM_INSTANCE);

    for (;;)
    {
        /* HT 또는 TC 중 어느 하나라도 수신될 때까지 블로킹 대기 */
        ULONG actual_flags = 0;
        tx_event_flags_get(&s_adcFlags, FLAG_HT | FLAG_TC,
                           TX_OR_CLEAR, &actual_flags, TX_WAIT_FOREVER);
        uint32_t flags = (uint32_t)actual_flags;

        if (flags & FLAG_HT)
        {
            /* 앞 절반 [0..ADC_CH_COUNT-1] 유효: DMA가 뒤 절반을 채우는 중 */
            for (uint32_t i = 0u; i < ADC_CH_COUNT; i++)
            {
                g_adcResult[i] = s_adcDmaBuf[i];
            }
        }

        if (flags & FLAG_TC)
        {
            /* 뒤 절반 [ADC_CH_COUNT..ADC_DMA_BUF_SIZE-1] 유효:
               DMA가 Circular wrap 후 앞 절반을 채우는 중 */
            for (uint32_t i = 0u; i < ADC_CH_COUNT; i++)
            {
                g_adcResult[i] = s_adcDmaBuf[ADC_CH_COUNT + i];
            }
        }
    }
}
