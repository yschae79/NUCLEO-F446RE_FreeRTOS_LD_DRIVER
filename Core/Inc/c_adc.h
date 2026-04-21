/**
 * @file    c_adc.h
 * @brief   ADC1 Circular DMA 더블버퍼 드라이버 공개 API
 * @details TIM2 TRGO(10kHz) → ADC1 불연속 스캔(10채널) → DMA2_Stream0 Circular
 *          HT/TC 더블버퍼 패턴으로 1kHz 주기 샘플링 결과를 g_adcResult에 갱신
 *
 *          채널 인덱스 매핑:
 *            [0]=CH8(PB0), [1]=CH9(PB1), [2]=CH10(PC0), [3]=CH11(PC1)
 *            [4]=CH12(PC2), [5]=CH13(PC3), [6]=CH14(PC4), [7]=CH15(PC5)
 *            [8]=TempSensor, [9]=Vrefint
 */
#ifndef C_ADC_H
#define C_ADC_H

#include "cmsis_os.h"
#include "stm32f4xx_ll_adc.h"
#include <stdint.h>

/* ── 설정 매크로 ─────────────────────────────────────────────────────── */

/** @brief ADC 인스턴스 핸들 */
#ifndef ADC_INSTANCE
#define ADC_INSTANCE        hadc1
#endif

/** @brief ADC 트리거 타이머 인스턴스 핸들 */
#ifndef ADC_TIM_INSTANCE
#define ADC_TIM_INSTANCE    htim2
#endif

/** @brief ADC 스캔 채널 수 (DMA 버퍼 절반 크기) */
#ifndef ADC_CH_COUNT
#define ADC_CH_COUNT        10u
#endif

/** @brief DMA Circular 버퍼 크기 = ADC_CH_COUNT × 2 (HT + TC 더블버퍼) */
#ifndef ADC_DMA_BUF_SIZE
#define ADC_DMA_BUF_SIZE    (ADC_CH_COUNT * 2u)
#endif

/** @brief ADC 태스크 스택 크기 (words) */
#ifndef ADC_TASK_STACK
#define ADC_TASK_STACK      256u
#endif

/** @brief ADC 태스크 우선순위 — Display·Printf보다 높게 설정 */
#ifndef ADC_TASK_PRIO
#define ADC_TASK_PRIO       osPriorityAboveNormal
#endif

/* ── 공유 결과 버퍼 ──────────────────────────────────────────────────── */

/**
 * @brief ADC 변환 결과 배열 [0..ADC_CH_COUNT-1]
 * @note  ADC 태스크가 1kHz 주기로 갱신. 외부에서 읽기 전용으로 사용.
 *        16비트 단위 접근은 Cortex-M4에서 원자적(atomic)이므로
 *        개별 채널 읽기 시 별도 동기화 불필요.
 */
extern volatile uint16_t g_adcResult[ADC_CH_COUNT];

/* ── 공개 API ────────────────────────────────────────────────────────── */

/**
 * @brief  ADC 드라이버 초기화 및 태스크 생성
 * @note   osKernelInitialize() 이후, osKernelStart() 이전에 호출.
 *         DMA·타이머 시작은 태스크 내부에서 수행.
 */
void ADC_Init(void);

/**
 * @brief  ADC DMA Half-Transfer 완료 콜백 핸들러
 * @note   main.c의 HAL_ADC_ConvHalfCpltCallback()에서 호출 (ISR 컨텍스트)
 */
void ADC_HalfCpltHandler(void);

/**
 * @brief  ADC DMA Transfer-Complete 완료 콜백 핸들러
 * @note   main.c의 HAL_ADC_ConvCpltCallback()에서 호출 (ISR 컨텍스트)
 */
void ADC_CpltHandler(void);

#endif /* C_ADC_H */
