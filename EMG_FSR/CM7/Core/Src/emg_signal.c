/* Estadisticas de la salida SIG. No se asume que SIG sea EMG crudo.
 * RMS_AC quita la media de cada ventana; DELTA_RMS usa la media de reposo.
 * El umbral es preliminar, no diagnostica fatiga ni una enfermedad.
 */
#include "emg_signal.h"
#include <math.h>
#include <string.h>
static uint16_t rounded(float x) {return (uint16_t)(x+0.5f);}
void EmgSignal_Reset(EmgSignal *s) {
 memset(s,0,sizeof(*s)); s->minimum=4095; s->threshold=20;
}
int EmgSignal_Push(EmgSignal *s,uint16_t x,EmgResult *r) {
 if(s->cal_count==0 && s->count==0) s->smooth=x;
 /* EMA: alpha=0.02469, tau cercano a 20 ms con fs=2 kHz. */
 s->smooth+=0.02469f*((float)x-s->smooth);
 if(!s->ready) {
  s->cal_sum+=x; s->cal_square+=(double)x*x; s->cal_count++;
  if(s->cal_count==EMG_CALIBRATION_SAMPLES) {
   s->baseline=(float)(s->cal_sum/s->cal_count);
   double variance=s->cal_square/s->cal_count-(double)s->baseline*s->baseline;
   s->noise=sqrtf((float)(variance>0?variance:0));
   s->threshold=fmaxf(20.0f,3.0f*s->noise); s->ready=1;
  }
 }
 s->count++; s->sum+=x; s->square+=(uint64_t)x*x; s->last=x;
 if(x<s->minimum)s->minimum=x;
 if(x>s->maximum)s->maximum=x;
 if(x<=5 || x>=4090)s->rails++;
 if(s->count<EMG_WINDOW_SAMPLES)return 0;
 float mean=(float)s->sum/s->count;
 float sq=(float)((double)s->square/s->count);
 float ac=sqrtf(fmaxf(0,sq-mean*mean));
 float delta=sqrtf(fmaxf(0,sq-2*s->baseline*mean+s->baseline*s->baseline));
 if(!s->ready)s->active=0;
 else if(delta>s->threshold*1.15f)s->active=1;
 else if(delta<s->threshold*0.85f)s->active=0;
 *r=(EmgResult){.last=s->last,.mean=rounded(mean),.rms=rounded(sqrtf(sq)),
  .rms_ac=rounded(ac),.delta_rms=rounded(delta),.smooth=rounded(s->smooth),
  .baseline=rounded(s->baseline),.threshold=rounded(s->threshold),
  .minimum=s->minimum,.maximum=s->maximum,.calibrated=s->ready,
  .active=s->active,.rail_samples=(uint16_t)s->rails};
 s->count=0; s->sum=0; s->square=0; s->minimum=4095; s->maximum=0;s->rails=0;
 return 1;
}
