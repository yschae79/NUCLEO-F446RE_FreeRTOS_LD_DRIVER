/**
  ******************************************************************************
  * @file           : c_debug.c
  * @brief          : 비동기 DMA 기반 디버그 printf
  * @details        : syscalls.c의 weak _write()를 재정의하여
  *                   printf()가 포맷된 전체 버퍼를 링버퍼에 복사한 뒤
  *                   DMA TX를 시작하도록 처리.
  *                   오버플로우 정책: DEBUG_OVERFLOW_BLOCK (c_debug.h) 참조.
  *                   ISR 컨텍스트에서 사용 불가 (설계상).
  *                   FreeRTOS 뮤텍스로 멀티태스크 동시 접근 보호.
  ******************************************************************************
  */

#include "c_debug.h"
#include "main.h"
#include "tx_api.h"
#include <string.h>

/* ----------------------- 외부 HAL 핸들 ---------------------------------- */
extern UART_HandleTypeDef DEBUG_UART_INSTANCE;

/* ----------------------- 링버퍼 ----------------------------------------- */
static uint8_t s_txBuf[DEBUG_TX_BUF_SIZE];

static volatile uint16_t s_head;        /**< 다음 쓰기 위치      */
static volatile uint16_t s_tail;        /**< 다음 DMA 읽기 위치  */
static volatile uint16_t s_dmaLen;      /**< 현재 DMA 전송 중인 바이트 수 */
static volatile uint8_t  s_dmaBusy;     /**< DMA TX 진행 중: 1  */

/* ----------------------- RTOS 동기화 ------------------------------------ */
static TX_MUTEX   s_txMutex;            /**< 링버퍼 동시 접근 보호 뮤텍스 */
static uint8_t    s_mutexReady;         /**< 1: 뮤텍스 초기화 완료 */

#if (DEBUG_OVERFLOW_BLOCK == 1)
static TX_SEMAPHORE s_txSpace;          /**< BLOCK 정책: 여유공간 알림 세마포어 */
#endif

/* ----------------------- Private 헬퍼 함수 ------------------------------ */

/**
 * @brief  링버퍼에 쓸 수 있는 여유 바이트 수 반환
 * @retval 여유 바이트 수
 */
static inline uint16_t RingFree(void)
{
    uint16_t h = s_head;
    uint16_t t = s_tail;
    /* full과 empty 구분을 위해 슬롯 1개 항상 낭비 */
    if (h >= t)
        return (DEBUG_TX_BUF_SIZE - 1u) - (h - t);
    else
        return (t - h) - 1u;
}

/**
 * @brief  tail 이후 연속 블록을 DMA 전송 시작
 * @note   s_dmaBusy == 0 이고 전송할 데이터가 있을 때만 호출
 */
static void StartDMA(void)
{
    uint16_t h = s_head;
    uint16_t t = s_tail;

    if (t == h) return;                 /* 전송할 데이터 없음 */

    uint16_t len;
    if (h > t) {
        len = h - t;                    /* 연속 블록: tail → head */
    } else {
        len = DEBUG_TX_BUF_SIZE - t;    /* 연속 블록: tail → 버퍼 끝 */
    }

    s_dmaLen  = len;
    s_dmaBusy = 1;
    HAL_UART_Transmit_DMA(&DEBUG_UART_INSTANCE, &s_txBuf[t], len);
}

/* ----------------------- Public API ------------------------------------- */

/**
 * @brief  비동기 디버그 출력 초기화
 * @note   MX_USART2_UART_Init() 이후, osKernelInitialize() 이전에 호출
 */
void Debug_Init(void)
{
    s_head    = 0;
    s_tail    = 0;
    s_dmaLen  = 0;
    s_dmaBusy = 0;
    s_mutexReady = 0;
    tx_mutex_create(&s_txMutex, "dbg_mtx", TX_NO_INHERIT);
    s_mutexReady = 1;
#if (DEBUG_OVERFLOW_BLOCK == 1)
    tx_semaphore_create(&s_txSpace, "dbg_sem", 0);
#endif
}

/**
 * @brief  DMA TX 완료 핸들러 (ISR 컨텍스트)
 * @note   HAL_UART_TxCpltCallback 에서 huart == &huart2 일 때 호출
 */
void Debug_TxCpltHandler(void)
{
    /* 방금 전송한 블록만큼 tail 전진 */
    uint16_t t = s_tail + s_dmaLen;
    if (t >= DEBUG_TX_BUF_SIZE)
        t -= DEBUG_TX_BUF_SIZE;
    s_tail    = t;
    s_dmaLen  = 0;
    s_dmaBusy = 0;

    /* 대기 중인 데이터가 있으면 다음 청크 시작 */
    if (s_head != s_tail) {
        StartDMA();
    }

#if (DEBUG_OVERFLOW_BLOCK == 1)
    /* BLOCK 정책: 여유공간 생겼음을 알림 (ThreadX ISR-safe) */
    tx_semaphore_put(&s_txSpace);
#endif
}

/* ----------------------- _write 재정의 (syscalls.c weak 함수 대체) ------- */

/**
 * @brief  _write() 재정의 — printf()가 DMA 링버퍼를 통해 출력되도록 처리
 * @param  file  파일 디스크립터 (미사용)
 * @param  ptr   출력할 데이터 포인터
 * @param  len   출력할 바이트 수
 * @retval 요청된 len (일부 드롭 시에도 동일)
 *
 * @note   ISR 컨텍스트에서 사용 불가 (설계상 — ISR에서 printf 미사용)
 * @note   링버퍼가 가득 찬 경우 초과 바이트는 무시됨
 */
int _write(int file, char *ptr, int len)
{
    (void)file;

    if (len <= 0) return 0;

    /* 스케줄러 시작 전에는 뮤텍스 없이 직접 접근 (Debug_Init 직후 등) */
    if (s_mutexReady && tx_thread_identify() != TX_NULL) {
        tx_mutex_get(&s_txMutex, TX_WAIT_FOREVER);
    }

#if (DEBUG_OVERFLOW_BLOCK == 1)
    /* BLOCK 정책: 전체 len 을 링버퍼에 넣을 때까지 반복 대기 */
    uint16_t remaining = (uint16_t)len;
    const char *src = ptr;

    while (remaining > 0) {
        uint16_t avail = RingFree();
        if (avail == 0) {
            /* 뮤텍스 해제 후 여유공간 대기 → 다시 획득 */
            if (s_mutexReady && tx_thread_identify() != TX_NULL) {
                tx_mutex_put(&s_txMutex);
            }
            tx_semaphore_get(&s_txSpace, TX_WAIT_FOREVER);
            if (s_mutexReady && tx_thread_identify() != TX_NULL) {
                tx_mutex_get(&s_txMutex, TX_WAIT_FOREVER);
            }
            continue;
        }

        uint16_t toWrite = (remaining <= avail) ? remaining : avail;

        /* 링버퍼에 복사 (랩어라운드 처리) */
        uint16_t h = s_head;
        uint16_t firstChunk = DEBUG_TX_BUF_SIZE - h;

        if (toWrite <= firstChunk) {
            memcpy(&s_txBuf[h], src, toWrite);
        } else {
            memcpy(&s_txBuf[h], src, firstChunk);
            memcpy(&s_txBuf[0], src + firstChunk, toWrite - firstChunk);
        }

        h += toWrite;
        if (h >= DEBUG_TX_BUF_SIZE)
            h -= DEBUG_TX_BUF_SIZE;
        s_head = h;

        src       += toWrite;
        remaining -= toWrite;

        /* DMA가 유휴 상태이면 즉시 전송 시작 */
        if (!s_dmaBusy) {
            StartDMA();
        }
    }
#else
    /* DROP 정책: 여유 없으면 초과 바이트 무시 */
    uint16_t avail = RingFree();
    uint16_t toWrite = ((uint16_t)len <= avail) ? (uint16_t)len : avail;

    /* 링버퍼에 복사 (랩어라운드 처리) */
    uint16_t h = s_head;
    uint16_t firstChunk = DEBUG_TX_BUF_SIZE - h;

    if (toWrite <= firstChunk) {
        memcpy(&s_txBuf[h], ptr, toWrite);
    } else {
        memcpy(&s_txBuf[h], ptr, firstChunk);
        memcpy(&s_txBuf[0], ptr + firstChunk, toWrite - firstChunk);
    }

    h += toWrite;
    if (h >= DEBUG_TX_BUF_SIZE)
        h -= DEBUG_TX_BUF_SIZE;
    s_head = h;

    /* DMA가 유휴 상태이면 즉시 전송 시작 */
    if (!s_dmaBusy) {
        StartDMA();
    }
#endif

    if (s_mutexReady && tx_thread_identify() != TX_NULL) {
        tx_mutex_put(&s_txMutex);
    }

    return len;     /* 일부 바이트가 드롭됐더라도 요청한 len 전체를 반환 */
}

/**
 * @brief  Binary 데이터를 UART2 DMA를 통해 전송 (TraceX 덤프 전용)
 * @note   동일한 버퍼 + DMA 인프라를 재사용
 *         printf와 충돌 없도록 mutex 로 보호
 *         큰 데이터를 DEBUG_TX_BUF_SIZE 첩크 단위로 분할 전송
 */
void Debug_SendBinary(const uint8_t *data, uint32_t len)
{
    if (data == NULL || len == 0u) return;
    if (!s_mutexReady || tx_thread_identify() == TX_NULL) return;

    uint32_t offset = 0;

    while (offset < len) {
        tx_mutex_get(&s_txMutex, TX_WAIT_FOREVER);

        /* 링버퍼 여유가 생길 때까지 대기 (블록 전달) */
        while (RingFree() == 0u) {
            tx_mutex_put(&s_txMutex);
            tx_thread_sleep(1);
            tx_mutex_get(&s_txMutex, TX_WAIT_FOREVER);
        }

        uint16_t avail   = RingFree();
        uint32_t remain  = len - offset;
        uint16_t toWrite = (remain <= (uint32_t)avail)
                           ? (uint16_t)remain
                           : avail;

        uint16_t h          = s_head;
        uint16_t firstChunk = (uint16_t)(DEBUG_TX_BUF_SIZE - h);

        if (toWrite <= firstChunk) {
            memcpy(&s_txBuf[h], data + offset, toWrite);
        } else {
            memcpy(&s_txBuf[h], data + offset, firstChunk);
            memcpy(&s_txBuf[0], data + offset + firstChunk,
                   toWrite - firstChunk);
        }

        h += toWrite;
        if (h >= DEBUG_TX_BUF_SIZE) h -= DEBUG_TX_BUF_SIZE;
        s_head = h;

        if (!s_dmaBusy) StartDMA();

        tx_mutex_put(&s_txMutex);

        offset += toWrite;
    }
}
