/* Tarea FSR. Los dos DMA corren en circular y nunca se detienen; TIM6 marca el
 * paso. Como ADC2 y ADC3 reciben el mismo disparo, la mitad k de un buffer
 * corresponde a la mitad k del otro. La tarea las empareja y procesa cada bloque.
 */
#include "fsr_task.h"
#include "myogrip.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#define ADC2_HALF (FSR_BLOCK_SCANS*FSR_ADC2_CHANNELS)
#define ADC3_HALF (FSR_BLOCK_SCANS*FSR_ADC3_CHANNELS)

/* DMA1 no puede escribir en DTCM. El linker generado deja .bss en RAM_D1 (AXI SRAM). */
static uint16_t adc2_buffer[2U*ADC2_HALF] __attribute__((aligned(32)));
/* El BDMA solo ve SRAM4 (0x38000000): sección .ram_d3 de STM32H755ZITX_FLASH.ld. */
static uint16_t adc3_buffer[2U*ADC3_HALF] __attribute__((section(".ram_d3"),aligned(32)));

#define BIT_ADC2_H0 (1UL<<0)
#define BIT_ADC2_H1 (1UL<<1)
#define BIT_ADC3_H0 (1UL<<2)
#define BIT_ADC3_H1 (1UL<<3)

static TaskHandle_t fsr_task;
static QueueHandle_t latest;
static volatile uint32_t lost,desync,adc_errors;
static volatile uint32_t events[4],phase[4],blocks; /* ADC2 mitad/completo, ADC3 mitad/completo */
extern DMA_HandleTypeDef hdma_adc2;

static void FsrTask(void *argument);

const char *Fsr_Init(void) {
 if(hadc2.Init.NbrOfConversion!=FSR_ADC2_CHANNELS || hadc2.Init.Resolution!=ADC_RESOLUTION_16B ||
    hadc2.Init.ConversionDataManagement!=ADC_CONVERSIONDATA_DMA_CIRCULAR ||
    hadc2.Init.ExternalTrigConv!=ADC_EXTERNALTRIG_T6_TRGO)
  return "ADC2 debe ser IN5, 16 bits, 1 conversion, DMA circular, disparo TIM6 TRGO";
 if(hadc3.Init.NbrOfConversion!=FSR_ADC3_CHANNELS || hadc3.Init.Resolution!=ADC_RESOLUTION_16B ||
    (FSR_ADC3_CHANNELS>1U && hadc3.Init.ScanConvMode!=ADC_SCAN_ENABLE) ||
    hadc3.Init.ConversionDataManagement!=ADC_CONVERSIONDATA_DMA_CIRCULAR ||
    hadc3.Init.ExternalTrigConv!=ADC_EXTERNALTRIG_T6_TRGO)
  return "ADC3 debe tener scan con FSR_ADC3_CHANNELS conversiones, 16 bits, DMA circular, disparo TIM6 TRGO";
 if(HAL_ADCEx_Calibration_Start(&hadc2,ADC_CALIB_OFFSET_LINEARITY,ADC_SINGLE_ENDED)!=HAL_OK)
  return "fallo la calibracion de ADC2";
 if(HAL_ADCEx_Calibration_Start(&hadc3,ADC_CALIB_OFFSET_LINEARITY,ADC_SINGLE_ENDED)!=HAL_OK)
  return "fallo la calibracion de ADC3";
 /* DMA1 en modo directo. Con FIFO (umbral lleno = 8 muestras) y mitades de 100,
  * las últimas 4 muestras de la mitad 0 esperan 2 ms en la FIFO: la bandera de
  * media transferencia llega tarde y esa fila CSV repite el bloque anterior.
  * El BDMA de ADC3 no tiene FIFO. */
 if(hdma_adc2.Init.FIFOMode!=DMA_FIFOMODE_DISABLE) {
  hdma_adc2.Init.FIFOMode=DMA_FIFOMODE_DISABLE;
  if(HAL_DMA_Init(&hdma_adc2)!=HAL_OK)return "no se pudo poner el DMA de ADC2 en modo directo";
 }
 /* Quedan armados esperando a TIM6, que arranca la tarea EMG. */
 if(HAL_ADC_Start_DMA(&hadc2,(uint32_t*)adc2_buffer,2U*ADC2_HALF)!=HAL_OK)
  return "no arranco el DMA de ADC2";
 if(HAL_ADC_Start_DMA(&hadc3,(uint32_t*)adc3_buffer,2U*ADC3_HALF)!=HAL_OK)
  return "no arranco el BDMA de ADC3";
 return NULL;
}

void Fsr_CreateTask(uint32_t priority) {
 latest=xQueueCreate(1,sizeof(FsrResult));
 configASSERT(latest);
 configASSERT(xTaskCreate(FsrTask,"FSR",512,NULL,priority,&fsr_task)==pdPASS);
}

void Fsr_AdcBlockISR(ADC_HandleTypeDef *h,uint8_t half) {
 uint32_t bit;
 if(h->Instance==ADC2)bit=half?BIT_ADC2_H1:BIT_ADC2_H0;
 else if(h->Instance==ADC3)bit=half?BIT_ADC3_H1:BIT_ADC3_H0;
 else return;
 uint32_t k=(h->Instance==ADC2?0U:2U)+(half?1U:0U);
 events[k]++;phase[k]=MyoGrip_SampleCount()%FSR_BLOCK_SCANS; /* 0 = llegó a tiempo */
 if(fsr_task==NULL)return;
 BaseType_t wake=pdFALSE;
 xTaskNotifyFromISR(fsr_task,bit,eSetBits,&wake);
 portYIELD_FROM_ISR(wake);
}

void Fsr_AdcErrorISR(ADC_HandleTypeDef *h) {
 if(h->Instance==ADC2 || h->Instance==ADC3)adc_errors++;
}

int Fsr_GetLatest(FsrResult *r) {
 return latest!=NULL && xQueuePeek(latest,r,0)==pdPASS;
}

uint32_t Fsr_Errors(void) {return lost+desync+adc_errors;}

void Fsr_GetDiag(FsrDiag *d) {
 d->adc2_half=events[0];d->adc2_full=events[1];
 d->adc3_half=events[2];d->adc3_full=events[3];d->blocks=blocks;
 for(int i=0;i<4;i++)d->phase[i]=phase[i];
 d->stack_free=fsr_task?(uint32_t)uxTaskGetStackHighWaterMark(fsr_task):0U; /* palabras */
}

/* Registra qué mitad entregó un ADC. Si llegan las dos mitades juntas, la
 * tarea se atrasó más de 50 ms: el bloque se descarta. */
static void take_half(uint32_t bits,uint32_t b0,uint32_t b1,int8_t *pending) {
 uint32_t got=bits&(b0|b1);
 if(got==0)return;
 if(got==(b0|b1)){lost++;*pending=-1;return;}
 if(*pending>=0)lost++; /* el otro ADC no entregó su mitad a tiempo */
 *pending=(got==b1)?1:0;
}

static void FsrTask(void *argument) {
 (void)argument;
 int8_t half2=-1,half3=-1;FsrResult result;
 for(;;) {
  uint32_t bits=0;
  /* Sin disparos (TIM6 detenido al recalibrar) solo se vuelve a esperar. */
  if(xTaskNotifyWait(0,0xFFFFFFFFUL,&bits,pdMS_TO_TICKS(200))!=pdPASS)continue;
  take_half(bits,BIT_ADC2_H0,BIT_ADC2_H1,&half2);
  take_half(bits,BIT_ADC3_H0,BIT_ADC3_H1,&half3);
  if(half2<0 || half3<0)continue;
  if(half2==half3) {
   FsrSignal_Process(&adc2_buffer[(uint32_t)half2*ADC2_HALF],
    &adc3_buffer[(uint32_t)half3*ADC3_HALF],blocks,&result);
   blocks++;
   xQueueOverwrite(latest,&result);
  }
  else desync++;
  half2=-1;half3=-1;
 }
}
