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
#include "tx_api.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */

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
