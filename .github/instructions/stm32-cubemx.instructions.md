---
description: "STM32CubeMX 생성 CMake 프로젝트에서 코드 분석, 수정, 문제 해결 시 준수해야 할 지침. STM32 HAL, IOC, CubeMX USER CODE, GPIO, 타이머, 인터럽트, 빌드, 커밋 관련 작업 시 자동 적용."
applyTo:
  - "**/*.c"
  - "**/*.h"
  - "**/*.ioc"
  - "**/CMakeLists.txt"
  - "**/*.s"
  - "**/*.ld"
---

# STM32 CubeMX 프로젝트 에이전트 지침

## 프로젝트 배경

- **목적**: Laser Diode (LD) Driver 기본 기능 사전 검증 — 실제 PCB 제작 전 테스트용
- **테스트 보드**: NUCLEO-F446RE (STM32F446RE)
- **최종 타깃**: 실제 LD Driver 제품은 STM32H7 MCU 사용 예정 → 코드 이식성을 항상 고려
- **FreeRTOS**: 본 프로젝트에서 처음 적용 → **안정성 확보가 최우선 순위**
  - 성능 최적화보다 동작 신뢰성을 우선
  - FreeRTOS API 사용 시 검증된 패턴만 적용
  - 태스크 스택 오버플로우, 우선순위 역전 등 일반적인 RTOS 문제에 주의

---

## 핵심 원칙

### 1. 추측성 답변 절대 금지
- 확실하지 않은 정보를 추측하거나 가정하여 답변하는 것을 절대 금지합니다.
- 불확실한 사항이 있을 경우 반드시 사용자에게 명확히 알리고 확인을 요청해야 합니다.
- ❌ "아마도 이 함수는 ~할 것으로 보입니다"
- ✅ "소스코드를 확인한 결과, [파일명]의 [라인번호]에서 다음과 같이 구현되어 있습니다"

### 2. 소스코드 필수 확인
모든 답변 전에 현재 프로젝트의 실제 소스코드를 반드시 확인합니다.

### 2-1. STM32 하드웨어 리소스 분석 — 3단계 필수 절차
하드웨어 관련 질문(GPIO, 타이머, 주변장치 등)이 있을 경우 반드시 다음 순서로 확인합니다.

1. **IOC 파일 확인** (`NUCLEO-F446RE_FreeRTOS_LD_DRIVER.ioc`) — 핀 할당, 주변장치 설정, 클럭 설정
2. **초기화 코드 확인** (`main.c`, `stm32f4xx_hal_msp.c`) — `MX_*_Init()`, `HAL_*_MspInit()`
3. **응용 코드 확인** (`Core/Src/*.c`) — 실제 제어 로직, 인터럽트 핸들러, 콜백

- ❌ IOC 파일을 확인하지 않고 핀 할당을 추측하는 것
- ❌ HAL 라이브러리 기본값을 실제 프로젝트 설정으로 가정하는 것
- ❌ STM32CubeMX 자동생성 코드와 사용자 코드를 혼동하는 것

### 3. 증거 기반 답변
모든 답변에 반드시 파일명, 라인 번호, 코드 인용을 제시합니다.

---

## 프로젝트 구조

- **MCU**: STM32F446RE (Cortex-M4, 180MHz, 512KB Flash, LQFP64)
- **보드**: NUCLEO-F446RE
- **OS**: FreeRTOS (CMSIS-RTOS V2)
- **빌드**: STM32CubeMX + CMake + Ninja + starm-clang (NEWLIB)
- **최적화**: C Release `-O3`, CXX Release `-Oz`, Debug `-Og -g3`
- **STARM 툴체인**: `STARM_NEWLIB` (`--config=newlib.cfg`)
- **Newlib Reentrant**: `configUSE_NEWLIB_REENTRANT=1` (FreeRTOS 태스크별 `_impure_ptr` 관리)
- **플래시**: STM32CubeProgrammer (SWD)
- **링커 스크립트**: `STM32F446XX_FLASH.ld`

### 소스파일 네이밍 규칙
애플리케이션 소스파일은 반드시 `c_` 접두사로 시작합니다.
- ✅ `c_ld_driver.c`, `c_debug.c`, `c_adc.c`, `c_hrtim.c`
- ❌ `ld_driver.c`, `Debug.c`
- 예외: CubeMX 자동생성 파일 (`main.c`, `stm32f4xx_it.c`, `stm32f4xx_hal_msp.c`, `stm32f4xx_hal_timebase_tim.c`, `system_stm32f4xx.c`, `freertos.c` 등)

### 소스코드 주석 규칙
소스코드 주석은 **한국어**로 작성하며, **Doxygen 규칙**을 적용합니다.
- 파일 헤더: `@file`, `@brief`, `@details` 태그 사용
- 함수: `@brief`, `@param`, `@retval`(또는 `@return`) 태그 사용
- 본문 주석(설명, 인라인)은 한국어로 작성
- ✅ `/** @brief USART3 디버그 DMA TX 완료 처리 */`
- ❌ `/* USART3 debug DMA TX complete */`
- 예외: CubeMX 자동생성 주석, 라이브러리 코드

### STM32CubeMX USER CODE 규칙
`main.c`, `stm32f4xx_it.c`, `stm32f4xx_hal_msp.c`에서 사용자 코드는 반드시 `USER CODE` 블록 내에 작성합니다.
CubeMX 재생성 시 USER CODE 블록만 보존됩니다.

```c
/* USER CODE BEGIN Includes */
#include "my_header.h"
/* USER CODE END Includes */
```

- ❌ USER CODE 블록 밖의 자동생성 코드 수정 (CubeMX에서 수정 필요)
- ❌ HAL 라이브러리 코드 직접 수정

### STM32F4 특수 고려사항
- **FPU**: 활성화됨 (`-mfpu=fpv4-sp-d16 -mfloat-abi=hard`): float 연산 시 하드웨어 FPU 사용
- **FreeRTOS 태스크**: `osThreadNew()` 사용, 스택 크기 단위는 word(4바이트)
- **FreeRTOS ISR**: 인터럽트 핸들러 내에서 FreeRTOS API 호출 시 반드시 `FromISR` 접미사 버전 사용
- **HAL 헤더**: `stm32f4xx_hal.h` (H7 계열과 혼동 금지)
- **CMSIS-RTOS V2**: `cmsis_os.h` 포함, `osKernelStart()` 이후 태스크 실행
- **DMA + UART**: `HAL_UART_Transmit_DMA()` 사용 시 버퍼가 전송 완료 전까지 유효해야 함

### ⚠️ starm-clang (NEWLIB) 주의사항
- `volatile` 없는 루프 변수는 컴파일러에 의해 제거될 수 있음
- 릴리즈 빌드(`-O3`)에서 중단점이 원하는 위치에서 동작하지 않을 수 있음
- 상태 플래그 변수에는 반드시 `volatile` 사용
- GCC(`gcc-arm-none-eabi`)와 동작 차이 주의 (특히 printf, malloc 동작)
- `configUSE_NEWLIB_REENTRANT=1` 필수 — 미설정 시 멀티태스크 printf 크래시 위험

---

## 코드 수정 지침

### 수정 전 필수 확인
1. ✅ 기존 코드의 정확한 동작 파악
2. ✅ 수정이 미치는 영향 범위 확인
3. ✅ USER CODE 블록 규칙 준수 확인
4. ✅ 사용자에게 수정 계획 설명 및 승인 받기

### 수정 후 필수 사항
1. ✅ 수정 내용 상세 설명
2. ✅ 수정한 파일과 라인 번호 명시
3. ✅ 변경 전후 코드 비교 제시

---

## 빌드 및 커밋 워크플로우

코드 수정 완료 후 반드시 다음 순서를 따릅니다:

1. **빌드**: `cube-cmake --build --preset Release`
2. **에러 확인 및 수정**: 빌드 실패 시 에러 분석 후 수정 → 재빌드
3. **커밋/Push 금지**: 빌드 성공 후에도 커밋과 Push는 절대 자동으로 수행하지 않습니다.
   사용자가 `/커밋` 또는 `/커밋-푸시` 명령을 명시적으로 실행해야만 수행합니다.

---

## 절대 금지 행동

1. ❌ 추측으로 답변 ("아마도", "일반적으로", "보통")
2. ❌ 소스코드를 직접 확인하지 않고 답변
3. ❌ 파일명, 라인번호, 코드 인용 없이 답변
4. ❌ 사용자 확인 없이 중요 코드 변경
5. ❌ "~인 것 같습니다", "~일 수도 있습니다" 사용
