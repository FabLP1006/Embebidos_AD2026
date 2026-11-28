#ifndef MYOGRIP_H
#define MYOGRIP_H
/* MyoGrip EMG + 4 FSR sobre FreeRTOS (CM7).
 * El IOC genera los periféricos; este módulo solo los usa.
 */
#include "main.h"

extern ADC_HandleTypeDef hadc1;   /* EMG: PA3 = ADC1_INP15, por interrupción */
extern ADC_HandleTypeDef hadc2;   /* FSR3: PB1 = ADC2_INP5, DMA1 circular */
extern ADC_HandleTypeDef hadc3;   /* FSR1, FSR2, FSR4: PC0, PC3_C, PC2_C, BDMA circular */
extern TIM_HandleTypeDef htim6;   /* disparo común de los tres ADC */
extern UART_HandleTypeDef huart3; /* ST-LINK VCP: PD8/PD9 */

/* LD1=PB0 verde, LD2=PE1 amarillo, LD3=PB14 rojo, B1=PC13 */
#define LED_GREEN_PORT GPIOB
#define LED_GREEN_PIN GPIO_PIN_0
#define LED_YELLOW_PORT GPIOE
#define LED_YELLOW_PIN GPIO_PIN_1
#define LED_RED_PORT GPIOB
#define LED_RED_PIN GPIO_PIN_14
#define BUTTON_PORT GPIOC
#define BUTTON_PIN GPIO_PIN_13

/* P-14, salidas para osciloscopio en el Zio CN7 (GND en el pin 8 del mismo conector).
 * D13 = PA5 (pin 10): cambia en cada muestra -> onda cuadrada de fs/2 = 1.000 kHz.
 * D12 = PA6 (pin 12): sube con la última muestra de cada ventana y baja al terminar
 * de enviar su fila CSV -> pulso de 20 Hz; su ancho es la latencia ventana -> PC.
 * Se configuran en código; no están en el IOC. */
#define P14_SAMPLE_PORT GPIOA
#define P14_SAMPLE_PIN GPIO_PIN_5
#define P14_ROW_PORT GPIOA
#define P14_ROW_PIN GPIO_PIN_6

/* Prioridades (escala CMSIS-RTOS2, configMAX_PRIORITIES = 56).
 * El EMG manda; el FSR va enseguida; serial y UI al final. */
#define PRIO_EMG 40U    /* osPriorityHigh */
#define PRIO_FSR 32U    /* osPriorityAboveNormal */
#define PRIO_SERIAL 24U /* osPriorityNormal */
#define PRIO_VITALS 20U /* pulso (I2C2) y temperatura (I2C1); por debajo de Serial */
#define PRIO_UI 16U     /* osPriorityBelowNormal */

/* Antes del planificador: fija TIM6 a EMG_SAMPLE_HZ, revisa el IOC,
 * calibra los tres ADC, arma los DMA del FSR y revisa I2C1/I2C2 (vitals_task.c). */
void MyoGrip_Init(void);
void MyoGrip_CreateTasks(void);
void MyoGrip_TimerISR(void);
void MyoGrip_AdcISR(uint16_t sample);
/* Muestras EMG desde el arranque (no se reinicia con s/r/c). Para diagnóstico. */
uint32_t MyoGrip_SampleCount(void);
#endif
