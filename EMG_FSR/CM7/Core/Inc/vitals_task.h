#ifndef VITALS_TASK_H
#define VITALS_TASK_H
/* Signos vitales por I2C (código del repo Embebidos_AD2026 pasado a FreeRTOS):
 *   I2C2 (PB10 SCL / PB11 SDA): MAX30102, pulso, SpO2 e índice de perfusión.
 *   I2C1 (PB8 SCL / PB9 SDA):   MLX90614 (GY-906), temperatura ambiente y de objeto.
 * Una tarea por bus. Las transferencias van por interrupción si el IOC las tiene
 * activas (la tarea duerme mientras tanto); si no, en modo bloqueante.
 * Solo la tarea Serial escribe en la UART: aquí no se imprime nada. */
#include "stm32h7xx_hal.h"

/* Estado de cada bus, para la línea "# vit". */
typedef enum {BUS_NOT_CONFIGURED=0,BUS_BLOCKING=1,BUS_INTERRUPT=2} BusMode;
/* temp_estado del CSV */
typedef enum {TEMP_NO_SENSOR=0,TEMP_OK=1,TEMP_READ_ERROR=2} TempStatus;

typedef struct {
 /* Columnas del CSV */
 uint8_t pulse_status;        /* PulseStatus: 0 sin sensor, 1 sin dedo, 2 midiendo, 3 débil, 4 ok */
 uint16_t bpm,spo2,pi_x100;
 uint8_t temp_status;         /* TempStatus */
 int32_t t_amb_cc,t_obj_cc;   /* centésimas de °C (0 si no hay dato) */
 uint32_t i2c_errors;         /* acumulado de los dos buses */
 /* Diagnóstico */
 uint8_t bus1_mode,bus2_mode; /* BusMode */
 uint8_t bus1_idle,bus2_idle; /* SCL y SDA en reposo al arrancar: 11 = bien (hay pull-up);
                                 0xFF = no medido (bus sin configurar o en otros pines) */
 uint8_t part_id;             /* MAX30102: 0x15 */
 uint32_t pulse_samples,temp_reads,pec_errors;
 uint32_t stack_pulse,stack_temp; /* pila libre mínima, en palabras */
} VitalsResult;

/* Valores crudos del MAX30102 (18 bits, 0..262143) desde la lectura anterior.
 * Sirve para ver el nivel de DC, si hay pulso (max - min) y si satura (262143). */
#define PPG_FULL_SCALE 262143U
typedef struct {uint32_t ir_min,ir_max,red_min,red_max,count;} PpgWindow;

/* Antes del planificador: revisa handles, pines y NVIC de I2C1/I2C2. No hace
 * transferencias (los sensores se inicializan en sus tareas). */
void Vitals_Init(void);
void Vitals_CreateTasks(uint32_t priority);
void Vitals_GetLatest(VitalsResult *out);
/* Devuelve el mínimo y máximo de rojo e IR desde la llamada anterior y reinicia la ventana. */
void Vitals_TakePpgWindow(PpgWindow *out);
#endif
