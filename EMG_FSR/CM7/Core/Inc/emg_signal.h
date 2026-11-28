#ifndef EMG_SIGNAL_H
#define EMG_SIGNAL_H
#include <stdint.h>
#define EMG_SAMPLE_HZ 2000U
#define EMG_WINDOW_SAMPLES 100U /* 50 ms, 20 reportes/s */
#define EMG_CALIBRATION_SAMPLES 4000U /* 2 s */
#define EMG_VREF_MV 3300U /* Valor nominal; ajustar tras medir VREF+. */
typedef struct {
 float baseline, noise, threshold;
 double cal_sum, cal_square;
 uint32_t cal_count, count, sum;
 uint64_t square;
 uint16_t minimum, maximum, last;
 uint32_t rails;
 float smooth;
 uint8_t ready, active;
} EmgSignal;
typedef struct {
 uint16_t last, mean, rms, rms_ac, delta_rms, smooth;
 uint16_t baseline, threshold, minimum, maximum;
 uint8_t calibrated, active;
 uint16_t rail_samples;
} EmgResult;
void EmgSignal_Reset(EmgSignal *s);
int EmgSignal_Push(EmgSignal *s, uint16_t adc, EmgResult *out);
#endif
