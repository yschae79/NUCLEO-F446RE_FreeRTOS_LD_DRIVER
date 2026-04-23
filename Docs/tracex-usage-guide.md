# TraceX 사용 가이드 — NUCLEO-F446RE / Azure ThreadX

## 개요

TraceX는 Azure RTOS(ThreadX)에 내장된 실시간 이벤트 트레이싱 시스템입니다.
태스크 생성/종료, 뮤텍스 획득/해제, 세마포어, 이벤트 플래그 등 수백 가지 RTOS 이벤트를
마이크로초 단위 타임스탬프와 함께 링버퍼에 기록한 뒤, PC의 **TraceX GUI 툴**로 시각화합니다.

본 프로젝트에서는 버퍼가 가득 차면 UART2 DMA를 통해 자동 덤프하고,
PC의 `tracex_recv.py`가 `.trx` 파일로 저장하는 방식을 사용합니다.

---

## 1. 필요한 소프트웨어

| 항목 | 설명 |
|------|------|
| **TraceX GUI** | Microsoft Azure RTOS TraceX (무료, GitHub 제공) |
| **Python 3.x** | PC 수신 스크립트 실행 |
| **pyserial** | `pip install pyserial` |
| **UART-USB 드라이버** | ST-LINK 가상 COM 포트 (NUCLEO 내장) |

TraceX GUI 다운로드:
<https://github.com/eclipse-threadx/tracex/releases>

---

## 2. 펌웨어 측 설정 (현재 프로젝트 기준)

### 2-1. `Core/Inc/tx_user.h` — TraceX 컴파일 옵션

```c
/* TraceX 이벤트 트레이스 활성화 */
#define TX_ENABLE_EVENT_TRACE

/* 타임스탬프 소스: DWT 사이클 카운터
   SYSCLK 180MHz → 분해능 ~5.6ns */
#define TX_TRACE_TIME_SOURCE    (*((ULONG *)0xE0001004U))   /* DWT->CYCCNT */
#define TX_TRACE_TIME_MASK      0xFFFFFFFFUL

/* notify callback 사용을 위해 아래 주석 유지 (주석 해제 금지) */
/* #define TX_DISABLE_NOTIFY_CALLBACKS */
```

> **주의**: `TX_DISABLE_NOTIFY_CALLBACKS`를 활성화하면 `tx_trace_buffer_full_notify()`가
> 동작하지 않아 자동 덤프가 중단됩니다.

---

### 2-2. `Core/Src/main.c` — DWT 사이클 카운터 활성화

`SystemClock_Config()` 직후, `USER CODE BEGIN SysInit` 블록 안에 위치합니다.

```c
/* USER CODE BEGIN SysInit */
/* TraceX 타임스탬프용 DWT 사이클 카운터 활성화 */
CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;  /* DWT 전체 활성화 */
DWT->CYCCNT = 0U;                                  /* 카운터 초기화    */
DWT->CTRL   |= DWT_CTRL_CYCCNTENA_Msk;            /* CYCCNT 시작      */
/* USER CODE END SysInit */
```

> **주의**: 이 코드가 없으면 `TX_TRACE_TIME_SOURCE`가 항상 0을 반환하여
> TraceX GUI에서 "time source is static" 경고가 발생합니다.

---

### 2-3. `Core/Src/app_threadx.c` — TraceX 초기화 및 덤프 태스크

#### 버퍼 및 관련 변수

```c
#define TRACE_BUFFER_SIZE    (16u * 1024u)   /* 16KB ≈ ~500 이벤트 */
#define TRACE_FULL_FLAG      (0x01u)

static UCHAR              s_traceBuffer[TRACE_BUFFER_SIZE];
static TX_EVENT_FLAGS_GROUP s_traceFlags;
static TX_THREAD          s_traceDumpTask;
static ULONG              s_traceDumpStack[256];  /* 1KB */
```

#### 프레임 프로토콜 (바이너리 스트림 구분자)

```
[MAGIC_START 8B][size 4B LE][raw trace buffer (16KB)][MAGIC_END 8B]
```

| 항목 | 값 |
|------|----|
| MAGIC_START | `54 52 43 58 FF FE FD FC` |
| MAGIC_END   | `FC FD FE FF 54 52 43 58` |
| size        | 4바이트 리틀엔디안 (`TRACE_BUFFER_SIZE`) |

#### `App_ThreadX_Init()` 초기화 순서

```c
/* 1. 이벤트 플래그 생성 */
tx_event_flags_create(&s_traceFlags, "trace_flags");

/* 2. TraceX 시작 (레지스트리 슬롯 40개) */
tx_trace_enable(s_traceBuffer, TRACE_BUFFER_SIZE, 40);

/* 3. 버퍼 가득 콜백 등록 */
tx_trace_buffer_full_notify(TraceBufFullCb);

/* 4. TraceDump 태스크 생성 (priority=10, 최저) */
tx_thread_create(&s_traceDumpTask, "TraceDump", TraceDumpEntry, 0,
                 s_traceDumpStack, sizeof(s_traceDumpStack),
                 10, 10, TX_NO_TIME_SLICE, TX_AUTO_START);
```

#### `TraceBufFullCb` — 버퍼 가득 콜백

```c
/* SysTick ISR 컨텍스트에서 호출됨 — 블록하는 API 사용 금지 */
static VOID TraceBufFullCb(VOID *buffer)
{
    tx_event_flags_set(&s_traceFlags, TRACE_FULL_FLAG, TX_OR);
}
```

#### `TraceDumpEntry` — 덤프 태스크 동작 흐름

```
1. tx_event_flags_get() 대기          ← 버퍼 가득 신호 수신
2. tx_trace_disable()                 ← 새 이벤트 기록 중단
3. Debug_SendBinary(MAGIC_START)      ┐
4. Debug_SendBinary(&size, 4)         │ UART2 DMA로 프레임 전송
5. Debug_SendBinary(s_traceBuffer)    │
6. Debug_SendBinary(MAGIC_END)        ┘
7. tx_trace_enable()                  ← 트레이스 재시작
8. tx_trace_buffer_full_notify()      ← 콜백 재등록
9. 1번으로 반복
```

---

### 2-4. `Core/Src/c_debug.c` — `Debug_SendBinary()`

바이너리 프레임 전송 함수. **뮤텍스를 프레임 전체 구간 동안 단 한 번 획득**하여
`printf()` 출력이 TraceX 바이너리 프레임 중간에 끼어들지 못하도록 원자성을 보장합니다.

```c
void Debug_SendBinary(const uint8_t *data, uint32_t len)
{
    tx_mutex_get(&s_txMutex, TX_WAIT_FOREVER);  /* 프레임 전체 구간 락 */
    while (offset < len) {
        /* 링버퍼 여유 대기 (mutex 보유 중 sleep → DMA ISR이 drain) */
        while (RingFree() == 0u) {
            tx_thread_sleep(1);
        }
        /* 링버퍼에 복사 + DMA 시작 */
    }
    tx_mutex_put(&s_txMutex);
}
```

> DMA TxCplt ISR은 뮤텍스를 사용하지 않으므로, 뮤텍스를 보유한 채 `sleep`해도
> 링버퍼 드레인이 정상 동작합니다.

---

## 3. PC 수신 스크립트 — `tools/tracex_recv.py`

### 설치

```bash
pip install pyserial
```

### 사용법

```bash
# Windows
python tools/tracex_recv.py COM3 230400

# Linux / macOS
python tools/tracex_recv.py /dev/ttyUSB0 230400
```

> baud rate를 생략하면 기본값 `115200`이 사용됩니다.
> CubeMX에서 설정한 USART2 baud rate와 반드시 일치시켜야 합니다.

### 동작

```
수신 스트림
    ├─ MAGIC_START 이전 바이트 → [LOG] 접두사로 터미널 출력 (printf)
    └─ MAGIC_START 감지        → 프레임 수집 → dump_NNNN_YYYYMMDD_HHMMSS.trx 저장
```

저장 위치: `tools/tracex_dumps/` (`.gitignore`에 의해 버전 관리 제외)

---

## 4. TraceX GUI에서 .trx 파일 열기

1. TraceX GUI 실행
2. **File → Open** → 저장된 `dump_NNNN_*.trx` 파일 선택
3. 뷰 구성:

| 뷰 | 설명 |
|----|------|
| **Execution Profile** | 태스크별 CPU 점유율 |
| **Thread Timeline** | 시간축 기준 태스크 실행/대기 상태 |
| **Event List** | 이벤트 목록 (타임스탬프 포함) |
| **Performance Statistics** | 문맥전환 횟수, 선점 횟수 등 |

---

## 5. 레지스트리 슬롯 수 조정

`tx_trace_enable()` 3번째 인자 `40`은 TraceX가 이름을 추적할 수 있는 오브젝트 수입니다.

```c
tx_trace_enable(s_traceBuffer, TRACE_BUFFER_SIZE, 40);
//                                                 ↑
//            스레드/뮤텍스/세마포어/이벤트플래그/타이머 총합
```

현재 프로젝트 오브젝트 목록 (여유 충분):

| 오브젝트 | 종류 |
|---------|------|
| MainTask | TX_THREAD |
| TestTask2 | TX_THREAD |
| TraceDump | TX_THREAD |
| Timer Thread (내부) | TX_THREAD |
| Idle Thread (내부) | TX_THREAD |
| dbg_mtx | TX_MUTEX |
| dbg_sem | TX_SEMAPHORE |
| trace_flags | TX_EVENT_FLAGS_GROUP |
| adc_task (c_adc.c) | TX_THREAD |

오브젝트를 추가할 경우 슬롯 수를 함께 늘려야 합니다 (최소: 오브젝트 수 + 여유 5개).

---

## 6. 버퍼 크기와 이벤트 수

TraceX 이벤트 한 항목의 크기는 헤더 포함 **32바이트**입니다.

$$\text{최대 이벤트 수} = \frac{\text{TRACE\_BUFFER\_SIZE} - \text{헤더(64B)} - \text{레지스트리}}{32}$$

현재 설정(`16KB`, 슬롯 40개) 기준:

$$\frac{16384 - 64 - (40 \times 52)}{32} \approx 437 \text{개 이벤트}$$

이벤트 수를 늘리려면 `TRACE_BUFFER_SIZE`를 키우면 됩니다 (단, RAM 여유 확인 필요).

---

## 7. 주의사항 및 트러블슈팅

| 증상 | 원인 및 해결 |
|------|-------------|
| TraceX GUI에서 타임스탬프가 모두 0 | DWT CYCCNT 미활성화 → `main.c` `USER CODE BEGIN SysInit` 확인 |
| "time source is static" 경고 | 동일 원인. DWT 활성화 후 재빌드 |
| `.trx` 파일이 저장되지 않음 | baud rate 불일치 또는 pyserial 미설치 확인 |
| MAGIC_END 불일치 경고 | UART 오버런 발생 → baud rate 낮추거나 흐름 제어 고려 |
| TraceDump 태스크가 덤프 후 멈춤 | `tx_trace_buffer_full_notify()` 재등록 누락 확인 |
| printf가 .trx 파일 중간에 섞임 | `Debug_SendBinary()`의 뮤텍스 획득 위치 확인 (루프 밖에 있어야 함) |

---

## 8. 파일 위치 정리

```
프로젝트 루트
├── Core/Inc/tx_user.h              ← TX_ENABLE_EVENT_TRACE, 타임소스 정의
├── Core/Src/main.c                 ← DWT CYCCNT 활성화
├── Core/Src/app_threadx.c          ← tx_trace_enable, TraceDumpEntry
├── Core/Src/c_debug.c              ← Debug_SendBinary (바이너리 전송)
└── tools/
    ├── tracex_recv.py              ← PC 수신 스크립트
    └── tracex_dumps/               ← .trx 저장 위치 (.gitignore 제외)
```
