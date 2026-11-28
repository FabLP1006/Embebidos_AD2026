/* MyoGrip EMG + FSR sobre FreeRTOS. Cinco tareas:
 *   EMG    (40): procesa cada muestra del ADC1 y arma ventanas de 50 ms.
 *   FSR    (32): procesa bloques de 50 ms de ADC2+ADC3 (fsr_task.c).
 *   Serial (24): una fila CSV por ventana EMG, con la fuerza del último bloque FSR.
 *   UI     (16): botón B1 y LEDs del EMG.
 * Las ISR solo encolan o notifican. No hay printf ni HAL_Delay en una ISR.
 */
#include "myogrip.h"
#include "emg_signal.h"
#include "fsr_task.h"
#include "vitals_task.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
/* cycles = DWT->CYCCNT al entrar la muestra (ISR); sirve para medir latencias (P-14). */
typedef struct {uint32_t sequence,cycles;uint16_t value;} Sample;
typedef struct {EmgResult signal;uint32_t sequence,lost,adc_errors,end_cycles;uint8_t simulation;} Report;
static QueueHandle_t samples,reports;
static volatile uint32_t sequence,lost,adc_errors,last_sample_tick;
static volatile uint32_t total_samples; /* diagnóstico: no se reinicia con s/r/c */
/* P-14: máximos del último periodo de diagnóstico, en ciclos de CPU. */
static volatile uint32_t lat_sample_max,lat_row_max;
static TaskHandle_t emg_handle,serial_handle,ui_handle;
static volatile uint32_t request; /* 1=recalibrar; 2=simulacion; 3=ADC */
static volatile uint8_t simulation=1,calibrated,active,rail;
static uint32_t lost_reports,uart_errors;
static void AcquireTask(void *argument);
static void SerialTask(void *argument);
static void UiTask(void *argument);

/* ---------- Inicialización (antes del planificador) ---------- */

static void config_error(const char *message) {
 char line[200];
 int n=snprintf(line,sizeof(line),"\r\n# ERROR CONFIG: %s\r\n",message);
 if(n<0)n=0;
 if(n>=(int)sizeof(line))n=sizeof(line)-1;
 for(;;) { /* LED rojo parpadeando = error de configuración del IOC */
  HAL_UART_Transmit(&huart3,(const uint8_t*)line,(uint16_t)n,100);
  for(int i=0;i<10;i++){HAL_GPIO_TogglePin(LED_RED_PORT,LED_RED_PIN);HAL_Delay(100);}
 }
}

/* El ST-LINK VCP está en PD8 (TX) / PD9 (RX), AF7. */
static int uart_on_vcp(void) {
 uint32_t moder=GPIOD->MODER,afr=GPIOD->AFR[1];
 return ((moder>>16)&3U)==2U && ((moder>>18)&3U)==2U && (afr&0xFU)==7U && ((afr>>4)&0xFU)==7U;
}

/* TIM6 se calcula aquí a partir de EMG_SAMPLE_HZ, así la frecuencia de muestreo
 * no depende de lo que quede en el IOC. TRGO = update dispara los tres ADC. */
static void config_sample_timer(void) {
 TIM_MasterConfigTypeDef master={0};
 uint32_t pclk1=HAL_RCC_GetPCLK1Freq();
 uint32_t timclk=((RCC->D2CFGR&RCC_D2CFGR_D2PPRE1)==RCC_D2CFGR_D2PPRE1_DIV1)?pclk1:2U*pclk1;
 htim6.Init.Prescaler=timclk/1000000U-1U;             /* 1 MHz */
 htim6.Init.Period=1000000U/EMG_SAMPLE_HZ-1U;         /* 2 kHz -> 499 */
 htim6.Init.CounterMode=TIM_COUNTERMODE_UP;
 htim6.Init.AutoReloadPreload=TIM_AUTORELOAD_PRELOAD_DISABLE;
 if(HAL_TIM_Base_Init(&htim6)!=HAL_OK)Error_Handler();
 master.MasterOutputTrigger=TIM_TRGO_UPDATE;master.MasterSlaveMode=TIM_MASTERSLAVEMODE_DISABLE;
 if(HAL_TIMEx_MasterConfigSynchronization(&htim6,&master)!=HAL_OK)Error_Handler();
}

/* P-14: pines de osciloscopio y contador de ciclos (DWT). En el M7 el DWT
 * se desbloquea con LAR antes de habilitar CYCCNT. */
static void p14_init(void) {
 GPIO_InitTypeDef g={0};
 __HAL_RCC_GPIOA_CLK_ENABLE();
 g.Pin=P14_SAMPLE_PIN|P14_ROW_PIN;g.Mode=GPIO_MODE_OUTPUT_PP;g.Pull=GPIO_NOPULL;g.Speed=GPIO_SPEED_FREQ_LOW;
 HAL_GPIO_WritePin(GPIOA,P14_SAMPLE_PIN|P14_ROW_PIN,GPIO_PIN_RESET);
 HAL_GPIO_Init(GPIOA,&g);
 CoreDebug->DEMCR|=CoreDebug_DEMCR_TRCENA_Msk;
 DWT->LAR=0xC5ACCE55U;
 DWT->CYCCNT=0;DWT->CTRL|=DWT_CTRL_CYCCNTENA_Msk;
}

void MyoGrip_Init(void) {
 p14_init();
 if(!uart_on_vcp())config_error("USART3 debe ir en PD8/PD9 (ST-LINK VCP)");
 if(hadc1.Init.Resolution!=ADC_RESOLUTION_12B || hadc1.Init.NbrOfConversion!=1U ||
    hadc1.Init.ConversionDataManagement!=ADC_CONVERSIONDATA_DR ||
    hadc1.Init.ExternalTrigConv!=ADC_EXTERNALTRIG_T6_TRGO)
  config_error("ADC1 debe ser IN15, 12 bits, 1 conversion, datos en DR, disparo TIM6 TRGO");
 config_sample_timer();
 if(HAL_ADCEx_Calibration_Start(&hadc1,ADC_CALIB_OFFSET_LINEARITY,ADC_SINGLE_ENDED)!=HAL_OK)
  config_error("fallo la calibracion de ADC1");
 const char *error=Fsr_Init();
 if(error)config_error(error);
 /* Los sensores I2C son independientes del EMG/FSR: si faltan en el IOC o no
  * responden, no se detiene nada; se reporta en el CSV y en "# vit". */
 Vitals_Init();
}

/* ---------- Interrupciones ---------- */

static void push_from_isr(uint16_t value) {
 BaseType_t wake=pdFALSE;Sample s={.sequence=sequence++,.cycles=DWT->CYCCNT,.value=value};
 total_samples++;last_sample_tick=HAL_GetTick();
 HAL_GPIO_TogglePin(P14_SAMPLE_PORT,P14_SAMPLE_PIN);
 if(s.sequence%EMG_WINDOW_SAMPLES==EMG_WINDOW_SAMPLES-1U)
  P14_ROW_PORT->BSRR=P14_ROW_PIN; /* última muestra de la ventana */
 if(xQueueSendFromISR(samples,&s,&wake)!=pdPASS)lost++;
 portYIELD_FROM_ISR(wake);
}
void MyoGrip_TimerISR(void) {
 if(!simulation)return;
 /* Senal sintetica tipo envolvente. 3 s reposo, 3 s activacion. */
 uint32_t n=sequence,phase=n%100U;
 uint32_t triangle=phase<50?phase:100-phase;
 uint16_t value=(uint16_t)(100U+triangle/10U);
 if((n/6000U)%2U)value=(uint16_t)(value+800U+triangle*2U);
 push_from_isr(value);
}
void MyoGrip_AdcISR(uint16_t value){if(!simulation)push_from_isr(value);}
uint32_t MyoGrip_SampleCount(void){return total_samples;}

/* Callbacks de HAL compartidos por los tres ADC: siempre se filtra por Instance.
 * ADC1 (EMG) entrega una muestra por interrupción; ADC2/ADC3 entregan medio o
 * todo el buffer DMA. */
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *h) {
 if(h->Instance==ADC1)MyoGrip_AdcISR((uint16_t)HAL_ADC_GetValue(h));
 else Fsr_AdcBlockISR(h,1U);
}
void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *h){Fsr_AdcBlockISR(h,0U);}
void HAL_ADC_ErrorCallback(ADC_HandleTypeDef *h) {
 if(h->Instance==ADC1)adc_errors++;
 else Fsr_AdcErrorISR(h);
}
/* TIM6 no usa HAL_TIM_PeriodElapsedCallback: MyoGrip_TimerISR() se llama desde
 * TIM6_DAC_IRQHandler (stm32h7xx_it.c, USER CODE). Ese callback queda libre para
 * el timebase de HAL (TIM7), que CubeMX genera en main.c. */

/* ---------- Tareas ---------- */

void MyoGrip_CreateTasks(void) {
 samples=xQueueCreate(512,sizeof(Sample));reports=xQueueCreate(8,sizeof(Report));
 configASSERT(samples && reports);
 configASSERT(xTaskCreate(AcquireTask,"EMG",768,NULL,PRIO_EMG,&emg_handle)==pdPASS);
 Fsr_CreateTask(PRIO_FSR);
 Vitals_CreateTasks(PRIO_VITALS);
 configASSERT(xTaskCreate(SerialTask,"Serial",1024,NULL,PRIO_SERIAL,&serial_handle)==pdPASS);
 configASSERT(xTaskCreate(UiTask,"UI",384,NULL,PRIO_UI,&ui_handle)==pdPASS);
}
static void restart_source(EmgSignal *state,uint8_t use_simulation) {
 HAL_TIM_Base_Stop_IT(&htim6);
 HAL_ADC_Stop_IT(&hadc1);
 /* Detener fuentes y limpiar interrupciones pendientes antes de reiniciar colas.
  * Con TIM6 detenido, ADC2/ADC3 solo se pausan: sus DMA siguen armados. */
 HAL_NVIC_DisableIRQ(ADC_IRQn);HAL_NVIC_DisableIRQ(TIM6_DAC_IRQn);
 HAL_NVIC_ClearPendingIRQ(ADC_IRQn);HAL_NVIC_ClearPendingIRQ(TIM6_DAC_IRQn);
 xQueueReset(samples);xQueueReset(reports);
 simulation=use_simulation;sequence=0;lost=0;adc_errors=0;
 calibrated=0;active=0;rail=0;last_sample_tick=HAL_GetTick();
 EmgSignal_Reset(state);
 __HAL_TIM_SET_COUNTER(&htim6,0);__HAL_TIM_CLEAR_FLAG(&htim6,TIM_FLAG_UPDATE);
 HAL_NVIC_EnableIRQ(ADC_IRQn);HAL_NVIC_EnableIRQ(TIM6_DAC_IRQn);
 if(!simulation && HAL_ADC_Start_IT(&hadc1)!=HAL_OK)Error_Handler();
 /* La interrupción de TIM6 solo hace falta para la señal simulada. */
 if(simulation) {if(HAL_TIM_Base_Start_IT(&htim6)!=HAL_OK)Error_Handler();}
 else if(HAL_TIM_Base_Start(&htim6)!=HAL_OK)Error_Handler();
}
static void AcquireTask(void *argument) {
 (void)argument;EmgSignal state;Sample s;uint32_t previous=0;uint8_t have_previous=0;
 restart_source(&state,1);
 for(;;) {
  uint32_t command;
  taskENTER_CRITICAL();command=request;request=0;taskEXIT_CRITICAL();
  if(command) {restart_source(&state,command==2?1:command==3?0:simulation);have_previous=0;}
  if(xQueueReceive(samples,&s,pdMS_TO_TICKS(20))!=pdPASS)continue;
  uint32_t wait=DWT->CYCCNT-s.cycles; /* ISR -> tarea EMG */
  if(wait>lat_sample_max)lat_sample_max=wait;
  /* Una discontinuidad invalida las estadisticas de la ventana y el reposo. */
  if(have_previous && s.sequence!=previous+1)EmgSignal_Reset(&state);
  previous=s.sequence;have_previous=1;
  Report report={0};
  if(EmgSignal_Push(&state,s.value,&report.signal)) {
   calibrated=report.signal.calibrated;active=report.signal.active;
   rail=report.signal.rail_samples>0;
   report.sequence=s.sequence;report.end_cycles=s.cycles;report.simulation=simulation;
   report.lost=lost;report.adc_errors=adc_errors;
   if(xQueueSend(reports,&report,0)!=pdPASS)lost_reports++;
  }
 }
}
static void send_text(const char *text) {
 if(HAL_UART_Transmit(&huart3,(const uint8_t*)text,(uint16_t)strlen(text),100)!=HAL_OK)uart_errors++;
}
static void help(void) {
 send_text("\r\n# MyoGrip EMG + FSR + pulso + temperatura NUCLEO-H755ZI-Q - inicio SIM, sin sensor\r\n"
 "# s=SIM; r=ADC real; c=recalibrar 2s; h=ayuda\r\n"
 "# B1 tambien recalibra. ADC real: verificar SIG antes de conectar.\r\n"
 "# EMG PA3 (A0); FSR1 PC0 (A1), FSR2 PC3_C (A2), FSR3 PB1 (A3), FSR4 PC2_C (A4)\r\n"
 "# MAX30102 en I2C2 (PB10 SCL, PB11 SDA); MLX90614 en I2C1 (PB8 SCL, PB9 SDA)\r\n"
 "# fs nominal=2000 Hz; ventana=100; salida=20 Hz; VREF nominal=3300mV\r\n"
 "# delta_rms respecto al reposo; activo es un umbral experimental\r\n"
 "# f1..f4 en mN (modelo aproximado del FSR 402); fsr_nivel 0-3 con el maximo\r\n"
 "# pulso_estado 0 sin sensor,1 sin dedo,2 midiendo,3 debil,4 ok; spo2 en %; pi_x100 = PI en centesimas de %\r\n"
 "# temp_estado 0 sin sensor,1 ok,2 error; t_amb_cC y t_obj_cC en centesimas de grado C\r\n"
 "modo,muestras,tiempo_ms,adc,mV,media,rms,rms_ac,delta_rms,suavizado,base,umbral,calibrado,activo,riel,perdidas,errores_adc,reportes_perdidos,errores_uart,"
 "f1_mN,f2_mN,f3_mN,f4_mN,fsr_nivel,errores_fsr,"
 "pulso_estado,bpm,spo2,pi_x100,temp_estado,t_amb_cC,t_obj_cC,errores_i2c\r\n");
}
static void SerialTask(void *argument) {
 (void)argument;Report r;char line[320];uint32_t notice=HAL_GetTick(),diag=notice;help();
 /* "# vit" va desfasada 2.5 s de "# diag"/"# p14" para no juntar ~35 ms de UART. */
 uint32_t vit=notice-2500U,vit_samples=0;
 /* La primera "# vit" sale a los 2.5 s y solo toma la referencia: su ventana no mide
  * 5 s y el MAX30102 apenas se estaba configurando (27/nov/2026 dio 11.76 en vez de 25). */
 bool vit_first=true;
 snprintf(line,sizeof(line),"# reloj sysclk=%lu pclk1=%lu tim6 psc=%lu arr=%lu\r\n",
  (unsigned long)HAL_RCC_GetSysClockFreq(),(unsigned long)HAL_RCC_GetPCLK1Freq(),
  (unsigned long)TIM6->PSC,(unsigned long)TIM6->ARR);
 send_text(line);
 for(;;) {
  uint8_t command;
  if(HAL_UART_Receive(&huart3,&command,1,0)==HAL_OK) {
   if(command=='s'||command=='S')request=2;
   else if(command=='r'||command=='R')request=3;
   else if(command=='c'||command=='C')request=1;
   else if(command=='h'||command=='H')help();
  }
  if(xQueueReceive(reports,&r,pdMS_TO_TICKS(10))==pdPASS) {
   /* La ventana EMG termina en el mismo disparo que el bloque FSR, pero ADC2/ADC3
    * acaban ~0.1 ms después. Con 1-2 ms de espera la fila lleva siempre el bloque
    * de su misma ventana y no uno que a veces sí y a veces no. */
   vTaskDelay(pdMS_TO_TICKS(2));
   FsrResult f;
   if(!Fsr_GetLatest(&f))memset(&f,0,sizeof(f));
   VitalsResult v;Vitals_GetLatest(&v);
   uint32_t n=r.sequence+1U;
   unsigned mv=((uint32_t)r.signal.last*EMG_VREF_MV+2047)/4095;
   int length=snprintf(line,sizeof(line),
    "%s,%lu,%lu,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%u,%lu,"
    "%u,%u,%u,%u,%u,%ld,%ld,%lu\r\n",
    r.simulation?"SIM":"ADC",(unsigned long)n,(unsigned long)((uint64_t)n*1000U/EMG_SAMPLE_HZ),
    r.signal.last,mv,r.signal.mean,r.signal.rms,r.signal.rms_ac,r.signal.delta_rms,
    r.signal.smooth,r.signal.baseline,r.signal.threshold,r.signal.calibrated,r.signal.active,
    r.signal.rail_samples,(unsigned long)r.lost,(unsigned long)r.adc_errors,
    (unsigned long)lost_reports,(unsigned long)uart_errors,
    (unsigned long)f.force_mn[0],(unsigned long)f.force_mn[1],
    (unsigned long)f.force_mn[2],(unsigned long)f.force_mn[3],
    (unsigned)f.max_level,(unsigned long)Fsr_Errors(),
    (unsigned)v.pulse_status,(unsigned)v.bpm,(unsigned)v.spo2,(unsigned)v.pi_x100,
    (unsigned)v.temp_status,(long)v.t_amb_cc,(long)v.t_obj_cc,(unsigned long)v.i2c_errors);
   if(length>0 && length<(int)sizeof(line))send_text(line);
   P14_ROW_PORT->BSRR=(uint32_t)P14_ROW_PIN<<16U; /* fila enviada: baja D12 */
   uint32_t latency=DWT->CYCCNT-r.end_cycles;   /* última muestra -> fila enviada */
   if(latency>lat_row_max)lat_row_max=latency;
  }
  if(HAL_GetTick()-notice>=1000) {
   notice=HAL_GetTick();
   if(HAL_GetTick()-last_sample_tick>1000)send_text("# ERROR: no llegan muestras; revisar ADC/reiniciar con c\r\n");
  }
  /* Diagnóstico temporal: contadores acumulados. Entre dos líneas (5 s) deberían
   * subir 10000 muestras, 100 bloques y 50+50 eventos por ADC. */
  if(HAL_GetTick()-diag>=5000) {
   FsrDiag d;Fsr_GetDiag(&d);diag=HAL_GetTick();
   snprintf(line,sizeof(line),
    "# diag ms=%lu muestras=%lu bloques_fsr=%lu adc2_mitad=%lu adc2_completo=%lu adc3_mitad=%lu adc3_completo=%lu retraso=%lu,%lu,%lu,%lu\r\n",
    (unsigned long)diag,(unsigned long)total_samples,(unsigned long)d.blocks,
    (unsigned long)d.adc2_half,(unsigned long)d.adc2_full,
    (unsigned long)d.adc3_half,(unsigned long)d.adc3_full,
    (unsigned long)d.phase[0],(unsigned long)d.phase[1],
    (unsigned long)d.phase[2],(unsigned long)d.phase[3]);
   send_text(line);
   /* P-14: latencias máximas de los últimos 5 s (en µs) y mínimos de memoria
    * libre desde el arranque (pila en palabras de 4 bytes, heap en bytes). */
   uint32_t cpu_mhz=SystemCoreClock/1000000U,ls,lr;
   taskENTER_CRITICAL();ls=lat_sample_max;lr=lat_row_max;lat_sample_max=0;lat_row_max=0;taskEXIT_CRITICAL();
   snprintf(line,sizeof(line),
    "# p14 lat_muestra_max_us=%lu lat_fila_max_us=%lu pila_libre_emg=%lu fsr=%lu serial=%lu ui=%lu heap_libre_min=%lu\r\n",
    (unsigned long)(ls/cpu_mhz),(unsigned long)(lr/cpu_mhz),
    (unsigned long)uxTaskGetStackHighWaterMark(emg_handle),(unsigned long)d.stack_free,
    (unsigned long)uxTaskGetStackHighWaterMark(serial_handle),
    (unsigned long)uxTaskGetStackHighWaterMark(ui_handle),
    (unsigned long)xPortGetMinimumEverFreeHeapSize());
   send_text(line);
  }
  /* Sensores I2C: modo de cada bus, nivel en reposo de SCL/SDA (11 = bien),
   * PART_ID del MAX30102 (0x15), su frecuencia real (nominal 25 Hz), errores y pila. */
  if(HAL_GetTick()-vit>=5000) {
   static const char *const mode_name[]={"sin_cfg","bloqueante","interrupcion"};
   VitalsResult v;Vitals_GetLatest(&v);
   uint32_t now=HAL_GetTick(),elapsed=now-vit;
   uint32_t fs100=(uint32_t)((uint64_t)(v.pulse_samples-vit_samples)*100000U/(elapsed?elapsed:1U));
   char fs[16];
   if(vit_first)strcpy(fs,"---");
   else snprintf(fs,sizeof(fs),"%lu.%02lu",(unsigned long)(fs100/100U),(unsigned long)(fs100%100U));
   vit=now;vit_samples=v.pulse_samples;vit_first=false;
   /* Crudos del MAX30102 en los últimos 5 s: nivel, pulso (max-min) y saturación (262143). */
   PpgWindow w;Vitals_TakePpgWindow(&w);
   char ppg[64];
   if(w.count==0U)strcpy(ppg,"ir=- rojo=-");
   else snprintf(ppg,sizeof(ppg),"ir=%lu-%lu rojo=%lu-%lu",(unsigned long)w.ir_min,
                 (unsigned long)w.ir_max,(unsigned long)w.red_min,(unsigned long)w.red_max);
   snprintf(line,sizeof(line),
    "# vit i2c1=%s reposo=%02u i2c2=%s reposo=%02u max30102_id=0x%02X fs_pulso=%s %s "
    "lecturas_temp=%lu errores_pec=%lu errores_i2c=%lu pila_libre_pulso=%lu temp=%lu\r\n",
    mode_name[v.bus1_mode<3U?v.bus1_mode:0U],(unsigned)(v.bus1_idle==0xFFU?99U:v.bus1_idle),
    mode_name[v.bus2_mode<3U?v.bus2_mode:0U],(unsigned)(v.bus2_idle==0xFFU?99U:v.bus2_idle),
    (unsigned)v.part_id,fs,ppg,
    (unsigned long)v.temp_reads,(unsigned long)v.pec_errors,(unsigned long)v.i2c_errors,
    (unsigned long)v.stack_pulse,(unsigned long)v.stack_temp);
   send_text(line);
  }
 }
}
static void UiTask(void *argument) {
 (void)argument;TickType_t last=xTaskGetTickCount();
 GPIO_PinState stable=HAL_GPIO_ReadPin(BUTTON_PORT,BUTTON_PIN),candidate=stable;
 uint32_t changed=HAL_GetTick();
 for(;;) {
  GPIO_PinState now=HAL_GPIO_ReadPin(BUTTON_PORT,BUTTON_PIN);uint32_t tick=HAL_GetTick();
  if(now!=candidate){candidate=now;changed=tick;}
  if(candidate!=stable && tick-changed>=50){stable=candidate;if(stable==GPIO_PIN_SET)request=1;}
  uint8_t fault=(uint8_t)(rail || lost || adc_errors || tick-last_sample_tick>1000);
  HAL_GPIO_WritePin(LED_GREEN_PORT,LED_GREEN_PIN,calibrated&&!active&&!fault?GPIO_PIN_SET:GPIO_PIN_RESET);
  HAL_GPIO_WritePin(LED_YELLOW_PORT,LED_YELLOW_PIN,(!calibrated?((tick/200)%2):active)?GPIO_PIN_SET:GPIO_PIN_RESET);
  HAL_GPIO_WritePin(LED_RED_PORT,LED_RED_PIN,fault?GPIO_PIN_SET:GPIO_PIN_RESET);
  vTaskDelayUntil(&last,pdMS_TO_TICKS(10));
 }
}

/* ---------- Ganchos de FreeRTOS (activados en FreeRTOSConfig.h, USER CODE Defines) ---------- */
void vApplicationMallocFailedHook(void){Error_Handler();}
void vApplicationStackOverflowHook(TaskHandle_t task,char *name){(void)task;(void)name;Error_Handler();}
