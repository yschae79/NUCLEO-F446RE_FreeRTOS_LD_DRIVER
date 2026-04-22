/**
  ******************************************************************************
  * @file           : c_debug.h
  * @brief          : 비동기 DMA 기반 디버그 printf
  * @details        : FreeRTOS 뮤텍스로 보호되는 링버퍼 + DMA 전송 방식.
  *                   모든 태스크에서 printf() 호출 시 경쟁상태 없이 안전하게 출력.
  *                   헤더 매크로로 UART 인스턴스, 버퍼 크기, 오버플로우 정책 설정 가능.
  ******************************************************************************
  */
#ifndef C_DEBUG_H
#define C_DEBUG_H

#include "main.h"

/* ---------- 설정 --------------------------------------------------------- */

/**
 * @brief  UART 핸들 인스턴스 이름 (CubeMX 생성 변수명)
 * @note   예: huart2 (F446RE), huart3 (H7) 등
 */
#ifndef DEBUG_UART_INSTANCE
#define DEBUG_UART_INSTANCE     huart2
#endif

/**
 * @brief  링버퍼 크기 (바이트)
 * @note   링버퍼가 가득 차면 오버플로우 정책에 따라 동작
 */
#ifndef DEBUG_TX_BUF_SIZE
#define DEBUG_TX_BUF_SIZE       1024u
#endif

/**
 * @brief  오버플로우 정책
 *         0 = DROP  — 링버퍼 여유 없으면 초과 바이트 무시 (기본)
 *         1 = BLOCK — 링버퍼에 여유가 생길 때까지 대기 (DMA 완료 대기)
 */
#ifndef DEBUG_OVERFLOW_BLOCK
#define DEBUG_OVERFLOW_BLOCK    0
#endif

/* ---------- Public API --------------------------------------------------- */

/**
 * @brief  비동기 디버그 출력 초기화 (MX_USARTx_UART_Init 이후 호출)
 */
void Debug_Init(void);

/**
 * @brief  DMA TX 완료 핸들러 — HAL_UART_TxCpltCallback 에서
 *         huart == &DEBUG_UART_INSTANCE 일 때 호출
 */
void Debug_TxCpltHandler(void);

/**
 * @brief  Binary 데이터를 UART2 DMA를 통해 전송
 * @note   printf 스트림과 충돌 없이 안전하게 전송 (mutex 사용)
 * @note   태스크 컨텍스트 전용 — ISR에서 호출 금지
 * @param  data  전송할 데이터 포인터
 * @param  len   전송할 바이트 수
 */
void Debug_SendBinary(const uint8_t *data, uint32_t len);

#endif /* C_DEBUG_H */
