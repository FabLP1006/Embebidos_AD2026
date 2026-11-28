#ifndef FSR_TASK_H
#define FSR_TASK_H
/* Adquisición de los FSR: ADC2 (DMA1) + ADC3 (BDMA), ambos en circular y
 * disparados por el mismo TIM6 que el EMG. Cada mitad de buffer = 1 bloque de 50 ms.
 * Las ISR solo notifican; el cálculo se hace en la tarea FSR. */
#include "fsr_signal.h"
#include "stm32h7xx_hal.h"

/* Antes del planificador: revisa el IOC, calibra ADC2/ADC3 y arma los DMA.
 * Devuelve NULL si todo está bien o un texto con el problema. */
const char *Fsr_Init(void);
void Fsr_CreateTask(uint32_t priority);
/* Llamadas desde los callbacks de HAL (contexto de interrupción). */
void Fsr_AdcBlockISR(ADC_HandleTypeDef *h,uint8_t half);
void Fsr_AdcErrorISR(ADC_HandleTypeDef *h);
/* Último bloque procesado (buzón de 1 elemento). Devuelve 1 si ya hay datos. */
int Fsr_GetLatest(FsrResult *r);
/* Bloques perdidos + desincronizados + errores de ADC/DMA. */
uint32_t Fsr_Errors(void);
/* Diagnóstico: eventos DMA recibidos desde el arranque (mitad / buffer completo)
 * y bloques procesados. En 1 s deberían sumar 10+10 por ADC y 20 bloques.
 * phase[]: muestras EMG de retraso del último evento (0 = a tiempo), en el
 * orden ADC2 mitad, ADC2 completo, ADC3 mitad, ADC3 completo. */
typedef struct {uint32_t adc2_half,adc2_full,adc3_half,adc3_full,blocks,phase[4],stack_free;} FsrDiag;
void Fsr_GetDiag(FsrDiag *d);
#endif
