#ifndef FSR_SIGNAL_H
#define FSR_SIGNAL_H
/* Procesamiento de los 4 FSR 402. No usa HAL ni FreeRTOS, igual que emg_signal.
 *
 * Circuito por sensor (alimentado a 5 V; el pin nunca pasa de 3.18 V):
 *   5V --[ FSR 402 ]--+--[ R1 4.7k ]--+-- pin ADC
 *                                     |
 *                                  [ R2 8.2k ]
 *                                     |
 *                                    GND
 *   V_pin = VCC * R2 / (R_FSR + R1 + R2)  ->  R_FSR = VCC * R2 / V_pin - (R1 + R2)
 *
 * Canales (deben coincidir con los Rank del IOC):
 *   FSR1  PC0   (A1)  ADC3 Rank 1  IN10
 *   FSR2  PC3_C (A2)  ADC3 Rank 2  IN1
 *   FSR3  PB1   (A3)  ADC2 Rank 1  IN5
 *   FSR4  PC2_C (A4)  ADC3 Rank 3  IN0
 *   5V    PF10  (A5)  ADC3 Rank 4  IN6  (opcional, divisor 10k/10k)
 */
#include <stdint.h>

#define FSR_COUNT 4U
/* 100 disparos de TIM6 = 50 ms a 2 kHz: un bloque FSR por ventana EMG. */
#define FSR_BLOCK_SCANS 100U

/* 1 = medir el riel de 5 V en PF10 y usarlo en el cálculo de R.
 * Requiere ADC3 con 4 conversiones (Rank 4 = IN6) en el IOC. */
#define FSR_USE_5V_MONITOR 0
#define FSR_ADC2_CHANNELS 1U
#if FSR_USE_5V_MONITOR
#define FSR_ADC3_CHANNELS 4U
#define FSR_ADC3_VMON_INDEX 3U
#else
#define FSR_ADC3_CHANNELS 3U
#endif

/* Dónde está cada FSR: ADC (2 o 3) y posición dentro de su secuencia (Rank-1). */
typedef struct {uint8_t adc; uint8_t index;} FsrMap;
#define FSR_MAP {{3U,0U},{3U,1U},{2U,0U},{3U,2U}}

#define FSR_ADC_VREF_V 3.3f
#define FSR_ADC_MAX_CODE 65535.0f /* ADC2 y ADC3 a 16 bits */

#define FSR_VCC_V 5.0f
#define FSR_R1_OHM 4700.0f
#define FSR_R2_OHM 8200.0f
#define FSR_V_MIN 0.05f      /* < 50 mV en el pin: sin contacto */
#define FSR_R_MIN_OHM 10.0f  /* evita dividir entre 0 si el FSR queda en corto */
#define FSR_VMON_GAIN 2.0f   /* divisor 10k/10k del monitor */

/* Modelo aproximado del FSR 402 (Interlink): ~30 kΩ a 0.2 N, ~6 kΩ a 1 N, ~1 kΩ a 10 N.
 * F = F_REF * (R_REF / R)^K. Es solo referencia (±25 % entre piezas); calibrar con pesos. */
#define FSR_R_REF_OHM 6000.0f
#define FSR_F_REF_N 1.0f
#define FSR_EXP_K 1.28f

/* Niveles con la fuerza máxima de los 4 sensores. */
#define FSR_FORCE_LIGHT_N 0.2f
#define FSR_FORCE_MED_N 2.0f
#define FSR_FORCE_HIGH_N 10.0f
typedef enum {FSR_NONE=0, FSR_LIGHT=1, FSR_MEDIUM=2, FSR_STRONG=3} FsrLevel;

typedef struct {
 uint32_t block;               /* bloque de 50 ms desde el arranque */
 uint16_t pin_mv[FSR_COUNT];   /* voltaje medio en el pin */
 uint16_t pp_mv[FSR_COUNT];    /* pico a pico dentro del bloque */
 int32_t r_ohm[FSR_COUNT];     /* -1 = sin contacto */
 uint32_t force_mn[FSR_COUNT]; /* fuerza estimada en mN */
 uint16_t vcc_mv;              /* 5 V usado en el cálculo (medido o nominal) */
 uint8_t max_index;            /* sensor con mayor fuerza (0..3) */
 FsrLevel max_level;
} FsrResult;

/* adc2_block: FSR_BLOCK_SCANS x FSR_ADC2_CHANNELS muestras intercaladas.
 * adc3_block: FSR_BLOCK_SCANS x FSR_ADC3_CHANNELS muestras intercaladas. */
void FsrSignal_Process(const uint16_t *adc2_block,const uint16_t *adc3_block,
 uint32_t block,FsrResult *out);
#endif
