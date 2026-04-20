/**
  ******************************************************************************
  * @file           : c_debug.h
  * @brief          : USART2 비동기 DMA 기반 디버그 printf
  * @details        : FreeRTOS 뮤텍스로 보호되는 링버퍼 + DMA 전송 방식.
  *                   모든 태스크에서 printf() 호출 시 경쟁상태 없이 안전하게 출력.
  ******************************************************************************
  */
#ifndef C_DEBUG_H
#define C_DEBUG_H

#include "main.h"

/* ---------- 설정 --------------------------------------------------------- */
#define DEBUG_TX_BUF_SIZE   1024u   /**< 링버퍼 크기 (바이트) */

/* ---------- Public API --------------------------------------------------- */

/**
 * @brief  비동기 디버그 출력 초기화 (MX_USART2_UART_Init 이후 호출)
 */
void Debug_Init(void);

/**
 * @brief  DMA TX 완료 핸들러 — HAL_UART_TxCpltCallback 에서
 *         huart == &huart2 일 때 호출
 */
void Debug_TxCpltHandler(void);

#endif /* C_DEBUG_H */
