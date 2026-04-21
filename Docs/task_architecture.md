# 태스크 아키텍처 설계

## 태스크 구성 (총 4개 + 시스템 2개)

| 태스크 | 역할 | 우선순위 | 비고 |
|--------|------|----------|------|
| Task 1 (제어) | LD/TEC PID 제어 | **높음** | HRTimer PWM + ADC/DMA 피드백, 지연 허용 불가 |
| Task 2 (UI) | 화면 레이아웃 관리 | 보통 | 상태바, 페이지 전환, 주기적 갱신 |
| Task 3 (통신) | RS-422/485, RS-232, FDCAN | 중간 | Modbus RTU 프로토콜, 이벤트 플래그 기반 |
| DisplayTask (내부) | SPI 렌더링 엔진 | 보통 | LCD 드라이버 내부, `LCD_Init()`에서 자동 생성 |

## 데이터 흐름

```
Task 1 (제어)  ─── 제어 데이터 ──→  Task 2 (UI)  ──→ LCD Queue ──→ DisplayTask
Task 3 (통신)  ─── 통신 상태  ──→  Task 2 (UI)  ──→ LCD Queue ─┘
```

### 역할 분리 원칙

- **무엇을** 그릴지 (UI 로직) → **Task 2 (UI)**
- **어떻게** 그릴지 (SPI 렌더링) → **DisplayTask** (변경 불필요)
- 데이터 생산 → **Task 1**, **Task 3**

## DisplayTask의 `osWaitForever` 유지 이유

DisplayTask는 순수 렌더링 엔진이므로, 큐 timeout을 두고 주기적 갱신 로직을 넣으면 안 됨.

- ❌ DisplayTask에 UI 로직 혼합 → 결합도 증가, 드라이버 비대화
- ✅ UI Task가 주기적으로 `LCD_DrawString()` 등 공개 API 호출 → 깔끔한 분리

## UI Task 예시

```c
void UiTaskEntry(void *argument)
{
    (void)argument;
    LCD_Init();
    osDelay(300);  /* DisplayTask HW 초기화 대기 */
    LCD_FillScreen(LCD_BLACK);
    LCD_SetFont(&Font_16x24);
    LCD_DrawString(16, 16, "LD Controller", LCD_CYAN, LCD_BLACK);
    LCD_SetFont(&Font_8x16);

    char buf[64];
    for (;;) {
        /* Task1/Task3로부터 공유 데이터 읽기 */
        float ld_temp  = GetSharedLdTemp();
        float tec_curr = GetSharedTecCurr();

        snprintf(buf, sizeof(buf), "LD Temp: %.2f C  ", ld_temp);
        LCD_DrawString(16, 60, buf, LCD_GREEN, LCD_BLACK);

        snprintf(buf, sizeof(buf), "TEC I:  %.3f A  ", tec_curr);
        LCD_DrawString(16, 80, buf, LCD_YELLOW, LCD_BLACK);

        /* 상태바 갱신, 페이지 전환 로직 등 */

        osDelay(200);  /* 5Hz 화면 갱신 */
    }
}
```

## 힙 사용량 추정 (configTOTAL_HEAP_SIZE = 32,768B)

| 항목 | 스택 | TCB | 소계 |
|------|------|-----|------|
| Task 1 (제어) | 2,048B | ~168B | ~2,216B |
| Task 2 (UI) | 2,048B | ~168B | ~2,216B |
| Task 3 (통신) | 2,048B | ~168B | ~2,216B |
| DisplayTask | 2,048B | ~168B | ~2,216B |
| Idle Task | 512B | ~168B | ~680B |
| Timer Task | ~1,024B | ~168B | ~1,192B |
| 큐/세마포어 등 | - | - | ~1,500B |
| **합계** | | | **~12,236B (37%)** |

32KB 힙 대비 여유 충분. 80MHz Cortex-M4에서 4개 태스크는 가벼운 수준.

## 태스크 간 데이터 전달 방식 (향후 결정 필요)

| 방식 | 장점 | 단점 | 적합한 경우 |
|------|------|------|-------------|
| Queue | 데이터 복사, 경쟁 없음 | 복사 오버헤드 | 커맨드/이벤트 전달 |
| 공유 구조체 + Mutex | 최신 값만 유지, 가벼움 | 잠금 필요 | 센서값 등 상태 공유 |
| Thread Flags | 매우 가벼움 | 데이터 전달 불가 | 이벤트 알림 |

**권장:** Task 1 → Task 2 데이터 전달은 공유 구조체 + Mutex (최신 값만 필요), Task 3 수신 이벤트는 Thread Flags + 공유 버퍼.

---

## Task 1 (제어) 상세 설계

### 하드웨어 구성

- **HRTimer PWM**: LD 구동용 고속 PWM 출력 (지연 절대 불허)
- **ADC + DMA**: 피드백 전압/전류 연속 변환 → DMA로 메모리 전송
- PID 연산 → PWM Duty 갱신

### 타이밍 전략: HW 타이머 + `osThreadFlagsWait`

`osDelay()`는 틱 해상도(1ms) + 스케줄링 지터가 있어 고정 주기 제어에 부적합.

```
[HW Timer ISR]  ──(1kHz)──→  osThreadFlagsSet(controlTaskHandle, 0x01)
                                     │
[Task 1 루프]   ──────────→  osThreadFlagsWait(0x01, osFlagsWaitAny, osWaitForever)
                                     │
                              ADC 결과 읽기 → PID 연산 → PWM Duty 갱신
```

```c
/* HW 타이머 인터럽트 (예: TIM2, 1kHz) */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM2) {
        osThreadFlagsSet(controlTaskHandle, 0x01);
    }
}

/* Task 1 엔트리 */
void ControlTaskEntry(void *argument)
{
    (void)argument;
    for (;;) {
        osThreadFlagsWait(0x01, osFlagsWaitAny, osWaitForever);

        /* ADC DMA 버퍼에서 최신 값 읽기 */
        float voltage = ReadAdcVoltage();
        float current = ReadAdcCurrent();

        /* PID 연산 */
        float duty = PID_Compute(voltage, current);

        /* HRTimer PWM Duty 갱신 */
        SetPwmDuty(duty);

        /* UI Task용 공유 데이터 갱신 */
        UpdateSharedData(voltage, current, duty);
    }
}
```

### `osDelay` vs HW 타이머 + Thread Flags 비교

| 항목 | `osDelay(1)` | HW 타이머 + Flags |
|------|-------------|-------------------|
| 주기 정확도 | ±1ms 틱 해상도 + 스케줄링 지터 | HW 타이머 정밀도 (μs 단위) |
| 지터 | 수백 μs ~ 수 ms | 수 μs 이하 |
| CPU 점유 | 매 틱 스케줄러 개입 | 플래그 대기 시 sleep, ISR에서 즉시 깨움 |
| 적합 용도 | 느슨한 주기 (UI 갱신 등) | 고속 PID 제어 루프 |

---

## Task 3 (통신) 상세 설계

### 하드웨어 구성

| 인터페이스 | 용도 | 프로토콜 |
|-----------|------|----------|
| RS-422/485 | 외부 장비 통신 | Modbus RTU |
| RS-232 | 내부 (디버그/설정) | Modbus RTU |
| FDCAN | 외부 장비 통신 | Modbus RTU (over CAN) |

### 이벤트 플래그 기반 다중 대기 구조

여러 인터페이스를 하나의 태스크에서 처리하기 위해, 각 수신 콜백에서 Thread Flag를 설정.

```
[UART 422/485 RxCplt ISR] → osThreadFlagsSet(commTaskHandle, FLAG_422)
[UART 232 RxCplt ISR]     → osThreadFlagsSet(commTaskHandle, FLAG_232)
[FDCAN RxFifo ISR]        → osThreadFlagsSet(commTaskHandle, FLAG_CAN)
                                    │
[Task 3 루프] ────────────→ osThreadFlagsWait(FLAG_ALL, osFlagsWaitAny, osWaitForever)
                                    │
                            어느 채널? → 해당 Modbus RTU 프레임 파싱 → 응답
```

```c
#define FLAG_422   (1U << 0)
#define FLAG_232   (1U << 1)
#define FLAG_CAN   (1U << 2)
#define FLAG_ALL   (FLAG_422 | FLAG_232 | FLAG_CAN)

/* 수신 콜백 (ISR 컨텍스트) */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart == &huart_422) {
        osThreadFlagsSet(commTaskHandle, FLAG_422);
    } else if (huart == &huart_232) {
        osThreadFlagsSet(commTaskHandle, FLAG_232);
    }
}

void HAL_FDCAN_RxFifo0Callback(FDCAN_HandleTypeDef *hfdcan, uint32_t RxFifo0ITs)
{
    osThreadFlagsSet(commTaskHandle, FLAG_CAN);
}

/* Task 3 엔트리 */
void CommTaskEntry(void *argument)
{
    (void)argument;
    for (;;) {
        uint32_t flags = osThreadFlagsWait(FLAG_ALL, osFlagsWaitAny, osWaitForever);

        if (flags & FLAG_422) {
            Modbus_Process(&mb_422);   /* 422/485 프레임 파싱 + 응답 */
        }
        if (flags & FLAG_232) {
            Modbus_Process(&mb_232);   /* 232 프레임 파싱 + 응답 */
        }
        if (flags & FLAG_CAN) {
            Modbus_Process(&mb_can);   /* CAN 프레임 파싱 + 응답 */
        }
    }
}
```

### Modbus RTU 수신 전략

- UART: **Idle Line Detection** (`HAL_UARTEx_ReceiveToIdle_DMA`) → 프레임 끝 자동 감지
- FDCAN: Rx FIFO 콜백 → 메시지 단위 수신
- 각 채널별 독립 Modbus 컨텍스트 (`mb_422`, `mb_232`, `mb_can`)

### 태스크 분리 기준

현재 3개 인터페이스를 Task 3 하나로 처리하되, 아래 조건 발생 시 분리 고려:

| 조건 | 분리 방안 |
|------|----------|
| 422/485 트래픽이 고빈도 (>100 msg/s) | UART 전용 태스크 분리 |
| CAN 프레임 처리에 시간 소요 | CAN 전용 태스크 분리 |
| 프로토콜 응답 지연 요구사항 상이 | 인터페이스별 태스크 분리 |
