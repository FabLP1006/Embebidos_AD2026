#ifndef PULSE_SIGNAL_H
#define PULSE_SIGNAL_H
/* Pulso y SpO2 a partir de las muestras rojo/IR del MAX30102.
 * Mismo algoritmo que el main.c del repo Embebidos_AD2026 (detección de picos con
 * envolvente, media recortada de 5 intervalos, mediana de 5 cocientes R), pasado a
 * una estructura de estado para usarlo desde una tarea y probarlo en la PC.
 * Sin HAL ni FreeRTOS. */
#include <stdint.h>

#define PULSE_FS_HZ 25U          /* 100 muestras/s del sensor con promedio de 4 */
#define PULSE_FINGER_THR 70000U  /* empírico: sin dedo ~40-51k, con dedo ~110k */
#define PULSE_REFRACT_SMP 8U     /* 320 ms -> máximo ~187 BPM */
#define PULSE_N_AVG 5U           /* intervalos para la media recortada */
#define PULSE_SETTLE_SMP (2U*PULSE_FS_HZ) /* 2 s de asentamiento tras poner el dedo */
#define PULSE_STALE_SMP (2U*PULSE_FS_HZ)  /* 2 s sin latido = señal débil */
#define PULSE_N_SPO2 5U

/* Estado para la columna pulso_estado del CSV. */
typedef enum {PULSE_NO_SENSOR=0,PULSE_NO_FINGER=1,PULSE_MEASURING=2,PULSE_WEAK=3,PULSE_OK=4} PulseStatus;

typedef struct {
 float dc,ac,prev1,prev2,env;
 uint32_t sample_idx,last_peak,intervals[PULSE_N_AVG];
 uint8_t int_pos,int_cnt,beat_flag;
 uint32_t finger_smp;
 uint32_t r_min,r_max,i_min,i_max,w_cnt;
 float r_sum,i_sum;
 float ratios[PULSE_N_SPO2];
 uint8_t rat_pos,rat_cnt;
 float bpm;   /* >0 BPM; 0 midiendo; -1 sin dedo; -2 señal débil */
 float spo2;  /* >0 %; 0 midiendo; -1 fuera de rango */
} PulseState;

void Pulse_Reset(PulseState *s);
/* Una muestra (rojo, IR) de 18 bits. Actualiza s->bpm y s->spo2. */
void Pulse_Push(PulseState *s,uint32_t red,uint32_t ir);
PulseStatus Pulse_Status(const PulseState *s);
/* Valores enteros para el CSV (0 si no hay dato válido). */
uint16_t Pulse_Bpm(const PulseState *s);
uint16_t Pulse_Spo2(const PulseState *s);
uint16_t Pulse_PiX100(const PulseState *s); /* índice de perfusión en centésimas de % */
#endif
