/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
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
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include <string.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* DUAL_CORE_BOOT_SYNC_SEQUENCE: Define for dual core boot synchronization    */
/*                             demonstration code based on hardware semaphore */
/* This define is present in both CM7/CM4 projects                            */
/* To comment when developping/debugging on a single core                     */
#define DUAL_CORE_BOOT_SYNC_SEQUENCE

#if defined(DUAL_CORE_BOOT_SYNC_SEQUENCE)
#ifndef HSEM_ID_0
#define HSEM_ID_0 (0U) /* HW semaphore 0*/
#endif
#endif /* DUAL_CORE_BOOT_SYNC_SEQUENCE */
#define MAX30102_ADDR      (0x57 << 1)   // 0xAE: HAL pide dirección de 8 bits
#define REG_INT_STATUS_1   0x00
#define REG_FIFO_WR_PTR    0x04
#define REG_OVF_COUNTER    0x05
#define REG_FIFO_RD_PTR    0x06
#define REG_FIFO_DATA      0x07
#define REG_FIFO_CONFIG    0x08
#define REG_MODE_CONFIG    0x09
#define REG_SPO2_CONFIG    0x0A
#define REG_LED1_PA        0x0C   // LED rojo
#define REG_LED2_PA        0x0D   // LED infrarrojo
#define REG_PART_ID        0xFF
#define FS_HZ              25     // 100 muestras/s / promedio de 4
#define FINGER_THR         70000  // empírico: sin dedo ~40-51k, con dedo ~110k
#define REFRACT_SMP        8      // 8 muestras = 320 ms → máximo ~187 BPM
#define N_AVG              5      // intervalos para media recortada
#define PRINT_EVERY        25     // 25 muestras = 1 s entre impresiones
#define SETTLE_SMP         (2 * FS_HZ)   // 2 s de asentamiento tras poner dedo
#define STALE_SMP          (2 * FS_HZ)   // 2 s sin latido = señal débil
#define MLX_ADDR           (0x5A << 1)   // 0xB4
#define MLX_REG_TA         0x06          // temperatura ambiente
#define MLX_REG_TOBJ1      0x07          // temperatura de objeto
#define N_SPO2             5

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

I2C_HandleTypeDef hi2c1;
I2C_HandleTypeDef hi2c2;

UART_HandleTypeDef huart3;

/* USER CODE BEGIN PV */
float    dc = 0, ac = 0;
float    prev1 = 0, prev2 = 0;
float    env = 0;
uint32_t sample_idx = 0, last_peak = 0;
uint32_t intervals[N_AVG];
uint8_t  int_pos = 0, int_cnt = 0;
uint32_t finger_smp = 0;
uint8_t  beat_flag = 0;                 // 1 = hr_process detectó pico
uint32_t r_min, r_max, i_min, i_max;    // extremos en ventana de latido
float    r_sum = 0, i_sum = 0;          // sumas para DC
uint32_t w_cnt = 0;                     // muestras en ventana
float    ratios[N_SPO2];
uint8_t  rat_pos = 0, rat_cnt = 0;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MPU_Config(void);
static void MX_GPIO_Init(void);
static void MX_I2C2_Init(void);
static void MX_USART3_UART_Init(void);
static void MX_I2C1_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
void uart_print(const char *msg) {
    HAL_UART_Transmit(&huart3, (uint8_t*)msg, strlen(msg), 100);
}

HAL_StatusTypeDef max_write(uint8_t reg, uint8_t val) {
    return HAL_I2C_Mem_Write(&hi2c2, MAX30102_ADDR, reg,
                             I2C_MEMADD_SIZE_8BIT, &val, 1, 100);
}

HAL_StatusTypeDef max_read(uint8_t reg, uint8_t *val) {
    return HAL_I2C_Mem_Read(&hi2c2, MAX30102_ADDR, reg,
                            I2C_MEMADD_SIZE_8BIT, val, 1, 100);
}

HAL_StatusTypeDef max_init(void) {
    uint8_t v;
    uint32_t t0;

    // 1. Reset por software y espera a que termine
    if (max_write(REG_MODE_CONFIG, 0x40) != HAL_OK) return HAL_ERROR;
    t0 = HAL_GetTick();
    do {
        if (max_read(REG_MODE_CONFIG, &v) != HAL_OK) return HAL_ERROR;
        if (HAL_GetTick() - t0 > 100) return HAL_TIMEOUT;
    } while (v & 0x40);

    // 2. Leer estado limpia bandera de encendido
    max_read(REG_INT_STATUS_1, &v);

    // 3. Vaciar FIFO
    max_write(REG_FIFO_WR_PTR, 0x00);
    max_write(REG_OVF_COUNTER, 0x00);
    max_write(REG_FIFO_RD_PTR, 0x00);

    // 4. Configuración
    max_write(REG_FIFO_CONFIG, 0x50);
    max_write(REG_SPO2_CONFIG, 0x27);
    max_write(REG_LED1_PA, 0x24);
    max_write(REG_LED2_PA, 0x24);
    max_write(REG_MODE_CONFIG, 0x03);   // al final: arranca muestreo

    return HAL_OK;
}
uint8_t max_read_fifo(uint32_t *red, uint32_t *ir, uint8_t max_n) {
    static uint8_t raw[32 * 6];     // static: no ocupa pila
    uint8_t wr, rd, n;

    if (max_read(REG_FIFO_WR_PTR, &wr) != HAL_OK) return 0;
    if (max_read(REG_FIFO_RD_PTR, &rd) != HAL_OK) return 0;

    n = (wr - rd) & 0x1F;           // muestras pendientes
    if (n == 0) return 0;
    if (n > max_n) n = max_n;

    // Lectura en ráfaga: n muestras × 6 bytes
    if (HAL_I2C_Mem_Read(&hi2c2, MAX30102_ADDR, REG_FIFO_DATA,
                         I2C_MEMADD_SIZE_8BIT, raw, n * 6, 100) != HAL_OK)
        return 0;

    for (uint8_t i = 0; i < n; i++) {
        uint8_t *s = &raw[i * 6];
        red[i] = (((uint32_t)s[0] << 16) | ((uint32_t)s[1] << 8) | s[2]) & 0x3FFFF;
        ir[i]  = (((uint32_t)s[3] << 16) | ((uint32_t)s[4] << 8) | s[5]) & 0x3FFFF;
    }
    return n;
}
float hr_process(uint32_t ir_raw) {
    float x = (float)ir_raw;

    sample_idx++;

    // 1. Sin dedo: reiniciar estado
    if (ir_raw < FINGER_THR) {
        dc = x; ac = 0; env = 0;
        prev1 = prev2 = 0;
        last_peak = 0; int_pos = 0; int_cnt = 0;
        finger_smp = 0;
        w_cnt = 0; rat_pos = 0; rat_cnt = 0; beat_flag = 0;
        return -1.0f;
    }
    finger_smp++;

    // 2. Quitar DC e invertir
    dc += (x - dc) / 16.0f;
    ac = dc - x;

    // 3. Envolvente: sube instantáneo, decae 2 % por muestra
    env *= 0.98f;
    if (ac > env) env = ac;

    // 4. ¿prev1 fue pico?
    if (prev1 > prev2 && prev1 >= ac && prev1 > 0.5f * env &&
        (sample_idx - 1 - last_peak) > REFRACT_SMP) {
        uint32_t peak = sample_idx - 1;
        if (last_peak != 0 && finger_smp > SETTLE_SMP) {
            intervals[int_pos] = peak - last_peak;
            int_pos = (int_pos + 1) % N_AVG;
            if (int_cnt < N_AVG) int_cnt++;
        }
        beat_flag = 1;
        last_peak = peak;
    }
    prev2 = prev1;
    prev1 = ac;

    // 5. BPM con Media recortada
    if (int_cnt < N_AVG) return 0.0f;
    if (sample_idx - last_peak > STALE_SMP) return -2.0f;   // sin latidos recientes

    uint32_t s[N_AVG];
    memcpy(s, intervals, sizeof(s));
    for (uint8_t i = 1; i < N_AVG; i++) {
        uint32_t k = s[i];
        int8_t j = i - 1;
        while (j >= 0 && s[j] > k) { s[j + 1] = s[j]; j--; }
        s[j + 1] = k;
    }
    float mid = (float)(s[1] + s[2] + s[3]) / 3.0f;          // descarta s[0] y s[4]
    return 60.0f * FS_HZ / mid;
}

float spo2_update(uint32_t red, uint32_t ir) {
    // 1. Acumular muestra en ventana actual
    if (w_cnt == 0) {
        r_min = r_max = red;  i_min = i_max = ir;
        r_sum = i_sum = 0;
    }
    if (red < r_min) r_min = red;
    if (red > r_max) r_max = red;
    if (ir  < i_min) i_min = ir;
    if (ir  > i_max) i_max = ir;
    r_sum += red;  i_sum += ir;  w_cnt++;

    // 2. Latido nuevo: cerrar ventana y calcular R
    if (beat_flag) {
        beat_flag = 0;
        if (finger_smp > SETTLE_SMP &&
            w_cnt >= REFRACT_SMP && w_cnt <= STALE_SMP) {   // 30–187 BPM
            float r_ac = (float)(r_max - r_min);
            float i_ac = (float)(i_max - i_min);
            float r_dc = r_sum / w_cnt;
            float i_dc = i_sum / w_cnt;
            if (i_ac > 0.0f && r_dc > 0.0f) {
                ratios[rat_pos] = (r_ac / r_dc) / (i_ac / i_dc);
                rat_pos = (rat_pos + 1) % N_SPO2;
                if (rat_cnt < N_SPO2) rat_cnt++;
            }
        }
        w_cnt = 0;
    }

    // 3. Mediana de R
    if (rat_cnt < N_SPO2) return 0.0f;                     // midiendo
    float s[N_SPO2];
    memcpy(s, ratios, sizeof(s));
    for (uint8_t i = 1; i < N_SPO2; i++) {
        float k = s[i];
        int8_t j = i - 1;
        while (j >= 0 && s[j] > k) { s[j + 1] = s[j]; j--; }
        s[j + 1] = k;
    }
    float R = s[N_SPO2 / 2];

    // 4. R → SpO2
    if (R < 0.3f || R > 1.3f) return -1.0f;               // fuera de rango creíble
    float spo2 = 1.5958f * R * R - 34.6597f * R + 112.6899f;
    if (spo2 > 100.0f) spo2 = 100.0f;
    return spo2;
}

HAL_StatusTypeDef mlx_read_temp(uint8_t reg, float *t_c) {
    uint8_t d[3];                                   // LSB, MSB, PEC
    if (HAL_I2C_Mem_Read(&hi2c1, MLX_ADDR, reg,
                         I2C_MEMADD_SIZE_8BIT, d, 3, 10) != HAL_OK)
        return HAL_ERROR;

    uint16_t raw = (uint16_t)d[0] | ((uint16_t)d[1] << 8);
    if (raw & 0x8000) return HAL_ERROR;             // bit 15 = bandera de error

    *t_c = raw * 0.02f - 273.15f;                   // kelvin×50 → °C
    return HAL_OK;
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */
/* USER CODE BEGIN Boot_Mode_Sequence_0 */
#if defined(DUAL_CORE_BOOT_SYNC_SEQUENCE)
  int32_t timeout;
#endif /* DUAL_CORE_BOOT_SYNC_SEQUENCE */
/* USER CODE END Boot_Mode_Sequence_0 */

  /* MPU Configuration--------------------------------------------------------*/
  MPU_Config();

/* USER CODE BEGIN Boot_Mode_Sequence_1 */
#if defined(DUAL_CORE_BOOT_SYNC_SEQUENCE)
  /* Wait until CPU2 boots and enters in stop mode or timeout*/
  timeout = 0xFFFF;
  while((__HAL_RCC_GET_FLAG(RCC_FLAG_D2CKRDY) != RESET) && (timeout-- > 0));
  if ( timeout < 0 )
  {
  Error_Handler();
  }
#endif /* DUAL_CORE_BOOT_SYNC_SEQUENCE */
/* USER CODE END Boot_Mode_Sequence_1 */
  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();
/* USER CODE BEGIN Boot_Mode_Sequence_2 */
#if defined(DUAL_CORE_BOOT_SYNC_SEQUENCE)
/* When system initialization is finished, Cortex-M7 will release Cortex-M4 by means of
HSEM notification */
/*HW semaphore Clock enable*/
__HAL_RCC_HSEM_CLK_ENABLE();
/*Take HSEM */
HAL_HSEM_FastTake(HSEM_ID_0);
/*Release HSEM in order to notify the CPU2(CM4)*/
HAL_HSEM_Release(HSEM_ID_0,0);
/* wait until CPU2 wakes up from stop mode */
timeout = 0xFFFF;
while((__HAL_RCC_GET_FLAG(RCC_FLAG_D2CKRDY) == RESET) && (timeout-- > 0));
if ( timeout < 0 )
{
Error_Handler();
}
#endif /* DUAL_CORE_BOOT_SYNC_SEQUENCE */
/* USER CODE END Boot_Mode_Sequence_2 */

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_I2C2_Init();
  MX_USART3_UART_Init();
  MX_I2C1_Init();
  /* USER CODE BEGIN 2 */
  // Seccion MAX30102
  char buf[64];
  uint8_t v, p1, p2;

  if (max_init() != HAL_OK) {
      uart_print("Error en max_init\r\n");
  } else {
      max_read(REG_MODE_CONFIG, &v);
      snprintf(buf, sizeof(buf), "MODE = 0x%02X (esperado 0x03)\r\n", v);
      uart_print(buf);

      max_read(REG_SPO2_CONFIG, &v);
      snprintf(buf, sizeof(buf), "SPO2 = 0x%02X (esperado 0x27)\r\n", v);
      uart_print(buf);

      max_read(REG_FIFO_CONFIG, &v);
      snprintf(buf, sizeof(buf), "FIFO = 0x%02X (esperado 0x50)\r\n", v);
      uart_print(buf);

      max_read(REG_FIFO_WR_PTR, &p1);
      HAL_Delay(200);
      max_read(REG_FIFO_WR_PTR, &p2);
      snprintf(buf, sizeof(buf), "WR_PTR: %u -> %u\r\n", p1, p2);
      uart_print(buf);
  }
  // Seccion GY-906
  uint32_t red[32], ir[32];
  uint8_t n;
  uint32_t t_max = HAL_GetTick();     // última lectura de MAX
  uint32_t t_mlx = HAL_GetTick();     // última lectura de MLX
  uint32_t t_fs  = HAL_GetTick();     // inicio de ventana de fs
  uint32_t n_fs  = 0;                 // muestras en ventana
  uint8_t  mlx_ok = 0;                // 1 = última lectura MLX válida
  char     est[32];                   // texto de estado del pulso
  float ta, to;
  snprintf(buf, sizeof(buf), "I2C1 reposo: SCL=%d SDA=%d (esperado 1 1)\r\n",
           HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_8),
           HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_9));
  uart_print(buf);
  if (HAL_I2C_IsDeviceReady(&hi2c1, MLX_ADDR, 3, 10) != HAL_OK) {
      uart_print("MLX90614 no encontrado en I2C1\r\n");
  } else if (mlx_read_temp(MLX_REG_TA, &ta) == HAL_OK &&
             mlx_read_temp(MLX_REG_TOBJ1, &to) == HAL_OK) {
      snprintf(buf, sizeof(buf), "Ta = %d.%02d C  Tobj = %d.%02d C\r\n",
               (int)ta, (int)(ta * 100) % 100,
               (int)to, (int)(to * 100) % 100);
      uart_print(buf);
  } else {
      uart_print("MLX90614: error de lectura\r\n");
  }
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

	    /* USER CODE BEGIN 3 */
	    uint32_t now = HAL_GetTick();

	    // ---- Tarea 1: pulso cada 20 ms ----
	    if (now - t_max >= 20) {
	        t_max = now;
	        n = max_read_fifo(red, ir, 32);
	        n_fs += n;
	        for (uint8_t i = 0; i < n; i++) {
	            float bpm = hr_process(ir[i]);
	            float spo2 = spo2_update(red[i], ir[i]);
	            if ((sample_idx % PRINT_EVERY) != 0) continue;

	            if (bpm == -2.0f)   snprintf(est, sizeof(est), "Senal debil");
	            else if (bpm < 0)   snprintf(est, sizeof(est), "Sin dedo");
	            else if (bpm == 0)  snprintf(est, sizeof(est), "Midiendo...");
	            else if (spo2 > 0)  snprintf(est, sizeof(est), "BPM:%d SpO2:%d PI:%d",
	                                         (int)(bpm + 0.5f), (int)(spo2 + 0.5f),
	                                         (int)(10000.0f * env / dc));
	            else                snprintf(est, sizeof(est), "BPM:%d SpO2:-- PI:%d",
	                                         (int)(bpm + 0.5f), (int)(10000.0f * env / dc));
	            if (mlx_ok)
	                snprintf(buf, sizeof(buf), "%s | Ta:%d.%02d Tobj:%d.%02d C\r\n", est,
	                         (int)ta, (int)(ta * 100) % 100,
	                         (int)to, (int)(to * 100) % 100);
	            else
	                snprintf(buf, sizeof(buf), "%s | MLX: error\r\n", est);
	            uart_print(buf);
	        }
	    }

	    // ---- Tarea 2: temperatura cada 500 ms ----
	    if (now - t_mlx >= 500) {
	        t_mlx = now;
	        mlx_ok = (mlx_read_temp(MLX_REG_TA, &ta)    == HAL_OK &&
	                  mlx_read_temp(MLX_REG_TOBJ1, &to) == HAL_OK);
	    }

	    // ---- Tarea 3: frecuencia de muestreo real cada 10 s ----
	    if (now - t_fs >= 10000) {
	        uint32_t fs100 = n_fs * 100000UL / (now - t_fs);   // Hz × 100
	        snprintf(buf, sizeof(buf), "fs medida: %lu.%02lu Hz\r\n",
	                 fs100 / 100, fs100 % 100);
	        uart_print(buf);
	        n_fs = 0;
	        t_fs = now;
	    }
	  }                                     // ← cierra while, no borrar
	  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Supply configuration update enable
  */
  HAL_PWREx_ConfigSupply(PWR_DIRECT_SMPS_SUPPLY);

  /** Configure the main internal regulator output voltage
  */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE3);

  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_DIV1;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_NONE;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2
                              |RCC_CLOCKTYPE_D3PCLK1|RCC_CLOCKTYPE_D1PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
  RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV1;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_1) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief I2C1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_I2C1_Init(void)
{

  /* USER CODE BEGIN I2C1_Init 0 */

  /* USER CODE END I2C1_Init 0 */

  /* USER CODE BEGIN I2C1_Init 1 */

  /* USER CODE END I2C1_Init 1 */
  hi2c1.Instance = I2C1;
  hi2c1.Init.Timing = 0x00707CBB;
  hi2c1.Init.OwnAddress1 = 0;
  hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2 = 0;
  hi2c1.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
  hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Analogue filter
  */
  if (HAL_I2CEx_ConfigAnalogFilter(&hi2c1, I2C_ANALOGFILTER_ENABLE) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Digital filter
  */
  if (HAL_I2CEx_ConfigDigitalFilter(&hi2c1, 0) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2C1_Init 2 */

  /* USER CODE END I2C1_Init 2 */

}

/**
  * @brief I2C2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_I2C2_Init(void)
{

  /* USER CODE BEGIN I2C2_Init 0 */

  /* USER CODE END I2C2_Init 0 */

  /* USER CODE BEGIN I2C2_Init 1 */

  /* USER CODE END I2C2_Init 1 */
  hi2c2.Instance = I2C2;
  hi2c2.Init.Timing = 0x00707CBB;
  hi2c2.Init.OwnAddress1 = 0;
  hi2c2.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c2.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c2.Init.OwnAddress2 = 0;
  hi2c2.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
  hi2c2.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c2.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c2) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Analogue filter
  */
  if (HAL_I2CEx_ConfigAnalogFilter(&hi2c2, I2C_ANALOGFILTER_ENABLE) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Digital filter
  */
  if (HAL_I2CEx_ConfigDigitalFilter(&hi2c2, 0) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2C2_Init 2 */

  /* USER CODE END I2C2_Init 2 */

}

/**
  * @brief USART3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART3_UART_Init(void)
{

  /* USER CODE BEGIN USART3_Init 0 */

  /* USER CODE END USART3_Init 0 */

  /* USER CODE BEGIN USART3_Init 1 */

  /* USER CODE END USART3_Init 1 */
  huart3.Instance = USART3;
  huart3.Init.BaudRate = 115200;
  huart3.Init.WordLength = UART_WORDLENGTH_8B;
  huart3.Init.StopBits = UART_STOPBITS_1;
  huart3.Init.Parity = UART_PARITY_NONE;
  huart3.Init.Mode = UART_MODE_TX_RX;
  huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart3.Init.OverSampling = UART_OVERSAMPLING_16;
  huart3.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart3.Init.ClockPrescaler = UART_PRESCALER_DIV1;
  huart3.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart3) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetTxFifoThreshold(&huart3, UART_TXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetRxFifoThreshold(&huart3, UART_RXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_DisableFifoMode(&huart3) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART3_Init 2 */

  /* USER CODE END USART3_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

 /* MPU Configuration */

void MPU_Config(void)
{
  MPU_Region_InitTypeDef MPU_InitStruct = {0};

  /* Disables the MPU */
  HAL_MPU_Disable();

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.Number = MPU_REGION_NUMBER0;
  MPU_InitStruct.BaseAddress = 0x0;
  MPU_InitStruct.Size = MPU_REGION_SIZE_4GB;
  MPU_InitStruct.SubRegionDisable = 0x87;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_NO_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);
  /* Enables the MPU */
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
