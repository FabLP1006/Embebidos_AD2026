/* Signos vitales por I2C sobre FreeRTOS. Viene del main.c del repo Embebidos_AD2026:
 * mismos registros, misma configuración del MAX30102 y mismo algoritmo
 * (pulse_signal.c). Lo que cambia:
 *   - El lazo con HAL_GetTick se volvió dos tareas con vTaskDelayUntil.
 *   - Las transferencias I2C van por interrupción y la tarea duerme mientras tanto.
 *   - El MLX90614 valida el PEC (CRC-8) de cada lectura.
 *   - Si un sensor no responde, la tarea lo reintenta cada 2 s (se puede conectar después).
 *   - No se imprime aquí: la tarea Serial pone los valores en el CSV. */
#include "vitals_task.h"
#include "pulse_signal.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

/* CubeMX define estos handles en main.c al activar I2C1/I2C2 en el IOC. Mientras
 * no existan, estas definiciones débiles (en cero) dejan compilar y el bus queda
 * como "sin configurar". */
__attribute__((weak)) I2C_HandleTypeDef hi2c1;
__attribute__((weak)) I2C_HandleTypeDef hi2c2;

/* ---------- Bus I2C: por interrupción con notificación, o bloqueante ---------- */

#define NOTIFY_OK 1UL
#define NOTIFY_ERROR 2UL

typedef struct {
 I2C_HandleTypeDef *h;
 I2C_TypeDef *instance;
 IRQn_Type ev,er;
 uint8_t scl,sda;               /* número de pin en GPIOB */
 volatile uint8_t mode;         /* BusMode */
 uint8_t idle;
 TaskHandle_t volatile waiter;  /* tarea esperando el fin de la transferencia */
 volatile uint32_t errors;
} Bus;

static Bus bus1={&hi2c1,I2C1,I2C1_EV_IRQn,I2C1_ER_IRQn,8U,9U,BUS_NOT_CONFIGURED,0xFFU,NULL,0U};
static Bus bus2={&hi2c2,I2C2,I2C2_EV_IRQn,I2C2_ER_IRQn,10U,11U,BUS_NOT_CONFIGURED,0xFFU,NULL,0U};

/* El pin está en función alterna 4 (I2C1/I2C2 en PB8-PB11) y en drenaje abierto. */
static int pin_is_i2c(uint32_t pin) {
 uint32_t moder=(GPIOB->MODER>>(pin*2U))&3U;
 uint32_t af=(GPIOB->AFR[pin>>3]>>((pin&7U)*4U))&0xFU;
 uint32_t od=(GPIOB->OTYPER>>pin)&1U;
 return moder==2U && af==4U && od==1U;
}

static void bus_setup(Bus *b) {
 if(b->h->Instance!=b->instance){b->mode=BUS_NOT_CONFIGURED;return;}
 if(pin_is_i2c(b->scl) && pin_is_i2c(b->sda))
  b->idle=(uint8_t)((((GPIOB->IDR>>b->scl)&1U)*10U)+((GPIOB->IDR>>b->sda)&1U)); /* 11 = ambos en alto */
 /* Por interrupción solo si el IOC activó event+error y su prioridad permite llamar a
  * FreeRTOS desde la ISR (número >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY). */
 b->mode=(NVIC_GetEnableIRQ(b->ev) && NVIC_GetEnableIRQ(b->er) &&
          NVIC_GetPriority(b->ev)>=configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY &&
          NVIC_GetPriority(b->er)>=configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY)?BUS_INTERRUPT:BUS_BLOCKING;
}

/* Bus trabado (p. ej. tiempo agotado): se reinicia el periférico con la misma
 * configuración del IOC. HAL_I2C_Init deja el filtro analógico activo y el
 * digital en 0, igual que el IOC. */
static void bus_recover(Bus *b) {
 b->waiter=NULL;
 (void)HAL_I2C_DeInit(b->h);
 (void)HAL_I2C_Init(b->h);
}

static HAL_StatusTypeDef bus_mem(Bus *b,uint16_t addr,uint8_t reg,uint8_t *data,uint16_t len,
                                 uint8_t write,uint32_t timeout_ms) {
 HAL_StatusTypeDef st;
 if(b->mode==BUS_NOT_CONFIGURED)return HAL_ERROR;
 if(b->mode==BUS_BLOCKING) {
  st=write?HAL_I2C_Mem_Write(b->h,addr,reg,I2C_MEMADD_SIZE_8BIT,data,len,timeout_ms)
          :HAL_I2C_Mem_Read(b->h,addr,reg,I2C_MEMADD_SIZE_8BIT,data,len,timeout_ms);
 } else {
  uint32_t result=0;
  (void)ulTaskNotifyValueClear(NULL,0xFFFFFFFFUL);
  (void)xTaskNotifyStateClear(NULL);
  b->waiter=xTaskGetCurrentTaskHandle();
  st=write?HAL_I2C_Mem_Write_IT(b->h,addr,reg,I2C_MEMADD_SIZE_8BIT,data,len)
          :HAL_I2C_Mem_Read_IT(b->h,addr,reg,I2C_MEMADD_SIZE_8BIT,data,len);
  if(st==HAL_OK) {
   if(xTaskNotifyWait(0,0xFFFFFFFFUL,&result,pdMS_TO_TICKS(timeout_ms))!=pdPASS)st=HAL_TIMEOUT;
   else if(result!=NOTIFY_OK)st=HAL_ERROR;
  }
  b->waiter=NULL;
 }
 if(st!=HAL_OK) {
  b->errors++;
  /* Un NACK (sensor ausente) deja el periférico listo; solo se reinicia si quedó trabado. */
  if(st==HAL_TIMEOUT || HAL_I2C_GetState(b->h)!=HAL_I2C_STATE_READY)bus_recover(b);
 }
 return st;
}

static void bus_notify(I2C_HandleTypeDef *h,uint32_t value) {
 Bus *b=(h==&hi2c1)?&bus1:(h==&hi2c2)?&bus2:NULL;
 TaskHandle_t t=b?b->waiter:NULL;
 if(t==NULL)return;
 BaseType_t wake=pdFALSE;
 xTaskNotifyFromISR(t,value,eSetValueWithOverwrite,&wake);
 portYIELD_FROM_ISR(wake);
}
void HAL_I2C_MemRxCpltCallback(I2C_HandleTypeDef *h){bus_notify(h,NOTIFY_OK);}
void HAL_I2C_MemTxCpltCallback(I2C_HandleTypeDef *h){bus_notify(h,NOTIFY_OK);}
void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *h){bus_notify(h,NOTIFY_ERROR);}
void HAL_I2C_AbortCpltCallback(I2C_HandleTypeDef *h){bus_notify(h,NOTIFY_ERROR);}

/* ---------- Resultado compartido con la tarea Serial ---------- */

static VitalsResult shared;
static TaskHandle_t pulse_task,temp_task;
static PpgWindow ppg={UINT32_MAX,0U,UINT32_MAX,0U,0U};

/* ---------- MAX30102 en I2C2 ---------- */

#define MAX_ADDR (0x57U<<1)   /* 0xAE: HAL pide la dirección de 8 bits */
#define MAX_REG_INT_STATUS_1 0x00U
#define MAX_REG_FIFO_WR_PTR 0x04U
#define MAX_REG_OVF_COUNTER 0x05U
#define MAX_REG_FIFO_RD_PTR 0x06U
#define MAX_REG_FIFO_DATA 0x07U
#define MAX_REG_FIFO_CONFIG 0x08U
#define MAX_REG_MODE_CONFIG 0x09U
#define MAX_REG_SPO2_CONFIG 0x0AU
#define MAX_REG_LED1_PA 0x0CU   /* LED rojo */
#define MAX_REG_LED2_PA 0x0DU   /* LED infrarrojo */
#define MAX_REG_PART_ID 0xFFU
#define MAX_FIFO_DEPTH 32U
#define PULSE_PERIOD_MS 100U    /* 25 muestras/s -> ~2.5 muestras por lectura; la FIFO aguanta 1.28 s */

static PulseState pulse;

static HAL_StatusTypeDef max_write(uint8_t reg,uint8_t v){return bus_mem(&bus2,MAX_ADDR,reg,&v,1,1,20);}
static HAL_StatusTypeDef max_read(uint8_t reg,uint8_t *v,uint16_t n){return bus_mem(&bus2,MAX_ADDR,reg,v,n,0,50);}

/* Misma secuencia que max_init() del original:
 *   1. reset y esperar a que termine;
 *   2. limpiar la bandera de encendido;
 *   3. vaciar la FIFO;
 *   4. configurar: FIFO con promedio de 4 y rollover (0x50); SpO2 a 4096 nA,
 *      100 sps y 411 µs (0x27); LEDs de ~7 mA (0x24); modo SpO2 (0x03).
 * Devuelve 1 si el sensor respondió y quedó en modo SpO2. */
static int max_init(void) {
 static const uint8_t config[][2]={
  {MAX_REG_FIFO_WR_PTR,0x00},{MAX_REG_OVF_COUNTER,0x00},{MAX_REG_FIFO_RD_PTR,0x00},
  {MAX_REG_FIFO_CONFIG,0x50},{MAX_REG_SPO2_CONFIG,0x27},
  {MAX_REG_LED1_PA,0x24},{MAX_REG_LED2_PA,0x24},{MAX_REG_MODE_CONFIG,0x03}};
 uint8_t v=0;
 if(max_read(MAX_REG_PART_ID,&v,1)!=HAL_OK)return 0;
 taskENTER_CRITICAL();shared.part_id=v;taskEXIT_CRITICAL();
 if(max_write(MAX_REG_MODE_CONFIG,0x40)!=HAL_OK)return 0;   /* reset */
 for(uint32_t i=0;;i++) {
  vTaskDelay(pdMS_TO_TICKS(5));
  if(max_read(MAX_REG_MODE_CONFIG,&v,1)!=HAL_OK)return 0;
  if((v&0x40U)==0U)break;
  if(i>=20U)return 0;                                       /* 100 ms, como el original */
 }
 (void)max_read(MAX_REG_INT_STATUS_1,&v,1);
 for(uint32_t i=0;i<sizeof(config)/sizeof(config[0]);i++)
  if(max_write(config[i][0],config[i][1])!=HAL_OK)return 0;
 return max_read(MAX_REG_MODE_CONFIG,&v,1)==HAL_OK && v==0x03U;
}

/* max_read_fifo del original. Devuelve las muestras leídas, o -1 si falló el I2C. */
static int max_read_fifo(uint32_t *red,uint32_t *ir,uint8_t max_n) {
 static uint8_t raw[MAX_FIFO_DEPTH*6U];
 uint8_t wr,rd,n;
 if(max_read(MAX_REG_FIFO_WR_PTR,&wr,1)!=HAL_OK)return -1;
 if(max_read(MAX_REG_FIFO_RD_PTR,&rd,1)!=HAL_OK)return -1;
 n=(uint8_t)((wr-rd)&0x1FU);
 if(n==0U)return 0;
 if(n>max_n)n=max_n;
 if(max_read(MAX_REG_FIFO_DATA,raw,(uint16_t)(n*6U))!=HAL_OK)return -1;
 for(uint8_t i=0;i<n;i++) {
  const uint8_t *s=&raw[i*6U];
  red[i]=(((uint32_t)s[0]<<16)|((uint32_t)s[1]<<8)|s[2])&0x3FFFFU;
  ir[i]=(((uint32_t)s[3]<<16)|((uint32_t)s[4]<<8)|s[5])&0x3FFFFU;
 }
 return n;
}

static void publish_pulse(uint8_t status) {
 taskENTER_CRITICAL();
 shared.pulse_status=status;
 shared.bpm=status==PULSE_NO_SENSOR?0U:Pulse_Bpm(&pulse);
 shared.spo2=status==PULSE_NO_SENSOR?0U:Pulse_Spo2(&pulse);
 shared.pi_x100=status==PULSE_NO_SENSOR?0U:Pulse_PiX100(&pulse);
 taskEXIT_CRITICAL();
}

static void PulseTask(void *argument) {
 (void)argument;
 static uint32_t red[MAX_FIFO_DEPTH],ir[MAX_FIFO_DEPTH];
 uint8_t ready=0,fails=0;
 TickType_t last=xTaskGetTickCount();
 for(;;) {
  if(!ready) {
   if(bus2.mode!=BUS_NOT_CONFIGURED && max_init()){ready=1;fails=0;Pulse_Reset(&pulse);}
   else {publish_pulse(PULSE_NO_SENSOR);vTaskDelay(pdMS_TO_TICKS(2000));last=xTaskGetTickCount();continue;}
  }
  int n=max_read_fifo(red,ir,MAX_FIFO_DEPTH);
  if(n<0) {
   if(++fails>=3U)ready=0;   /* tres fallos seguidos: se da por desconectado y se reintenta */
  } else {
   fails=0;
   uint32_t ir_lo=UINT32_MAX,ir_hi=0U,red_lo=UINT32_MAX,red_hi=0U;
   for(int i=0;i<n;i++) {
    Pulse_Push(&pulse,red[i],ir[i]);
    if(ir[i]<ir_lo)ir_lo=ir[i];
    if(ir[i]>ir_hi)ir_hi=ir[i];
    if(red[i]<red_lo)red_lo=red[i];
    if(red[i]>red_hi)red_hi=red[i];
   }
   taskENTER_CRITICAL();
   shared.pulse_samples+=(uint32_t)n;
   if(n>0) {
    if(ir_lo<ppg.ir_min)ppg.ir_min=ir_lo;
    if(ir_hi>ppg.ir_max)ppg.ir_max=ir_hi;
    if(red_lo<ppg.red_min)ppg.red_min=red_lo;
    if(red_hi>ppg.red_max)ppg.red_max=red_hi;
    ppg.count+=(uint32_t)n;
   }
   taskEXIT_CRITICAL();
   publish_pulse((uint8_t)Pulse_Status(&pulse));
  }
  vTaskDelayUntil(&last,pdMS_TO_TICKS(PULSE_PERIOD_MS));
 }
}

/* ---------- MLX90614 (GY-906) en I2C1 ---------- */

#define MLX_ADDR (0x5AU<<1)   /* 0xB4 */
#define MLX_REG_TA 0x06U      /* temperatura ambiente */
#define MLX_REG_TOBJ1 0x07U   /* temperatura de objeto */
#define TEMP_PERIOD_MS 500U

/* PEC de SMBus: CRC-8, polinomio x^8+x^2+x+1 (0x07), valor inicial 0. */
static uint8_t crc8(const uint8_t *d,uint32_t n) {
 uint8_t c=0;
 for(uint32_t i=0;i<n;i++) {
  c^=d[i];
  for(int b=0;b<8;b++)c=(uint8_t)((c&0x80U)?(((uint32_t)c<<1)^0x07U):((uint32_t)c<<1));
 }
 return c;
}

/* Lee un registro de RAM en centésimas de °C. 0 = bien; 1 = error de I2C;
 * 2 = PEC incorrecto o bandera de error del sensor (bit 15). */
static int mlx_read(uint8_t reg,int32_t *cc) {
 uint8_t d[3];                                   /* LSB, MSB, PEC */
 if(bus_mem(&bus1,MLX_ADDR,reg,d,3,0,20)!=HAL_OK)return 1;
 const uint8_t frame[5]={(uint8_t)MLX_ADDR,reg,(uint8_t)(MLX_ADDR|1U),d[0],d[1]};
 if(crc8(frame,5)!=d[2])return 2;
 uint16_t raw=(uint16_t)(d[0]|((uint16_t)d[1]<<8));
 if(raw&0x8000U)return 2;
 *cc=(int32_t)raw*2-27315;                       /* raw·0.02 K -> °C, en centésimas */
 return 0;
}

static void TempTask(void *argument) {
 (void)argument;
 uint8_t i2c_fails=0;
 TickType_t last=xTaskGetTickCount();
 for(;;) {
  if(bus1.mode==BUS_NOT_CONFIGURED) {
   taskENTER_CRITICAL();shared.temp_status=TEMP_NO_SENSOR;taskEXIT_CRITICAL();
   vTaskDelay(pdMS_TO_TICKS(2000));last=xTaskGetTickCount();continue;
  }
  int32_t ta=0,to=0;
  int e=mlx_read(MLX_REG_TA,&ta);
  if(e==0)e=mlx_read(MLX_REG_TOBJ1,&to);
  taskENTER_CRITICAL();
  if(e==0) {
   i2c_fails=0;shared.temp_status=TEMP_OK;shared.t_amb_cc=ta;shared.t_obj_cc=to;shared.temp_reads++;
  } else {
   if(e==1){if(i2c_fails<255U)i2c_fails++;} else shared.pec_errors++;
   /* Varios fallos de I2C seguidos = sensor ausente; uno suelto o un PEC malo = error de lectura. */
   shared.temp_status=(e==1 && i2c_fails>=3U)?TEMP_NO_SENSOR:TEMP_READ_ERROR;
   shared.t_amb_cc=0;shared.t_obj_cc=0;
  }
  taskEXIT_CRITICAL();
  vTaskDelayUntil(&last,pdMS_TO_TICKS(TEMP_PERIOD_MS));
 }
}

/* ---------- Interfaz ---------- */

void Vitals_Init(void) {
 bus_setup(&bus1);bus_setup(&bus2);
 memset(&shared,0,sizeof(shared));
 shared.pulse_status=PULSE_NO_SENSOR;shared.temp_status=TEMP_NO_SENSOR;
 shared.bus1_mode=bus1.mode;shared.bus2_mode=bus2.mode;
 shared.bus1_idle=bus1.idle;shared.bus2_idle=bus2.idle;
}

void Vitals_CreateTasks(uint32_t priority) {
 configASSERT(xTaskCreate(PulseTask,"Pulso",512,NULL,priority,&pulse_task)==pdPASS);
 configASSERT(xTaskCreate(TempTask,"Temp",384,NULL,priority,&temp_task)==pdPASS);
}

void Vitals_GetLatest(VitalsResult *out) {
 taskENTER_CRITICAL();
 *out=shared;
 out->i2c_errors=bus1.errors+bus2.errors;
 taskEXIT_CRITICAL();
 out->stack_pulse=pulse_task?(uint32_t)uxTaskGetStackHighWaterMark(pulse_task):0U;
 out->stack_temp=temp_task?(uint32_t)uxTaskGetStackHighWaterMark(temp_task):0U;
}

void Vitals_TakePpgWindow(PpgWindow *out) {
 taskENTER_CRITICAL();
 *out=ppg;
 ppg.ir_min=UINT32_MAX;ppg.ir_max=0U;ppg.red_min=UINT32_MAX;ppg.red_max=0U;ppg.count=0U;
 taskEXIT_CRITICAL();
}
