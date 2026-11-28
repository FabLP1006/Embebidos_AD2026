/* Algoritmo de pulso y SpO2 del repo Embebidos_AD2026 (hr_process + spo2_update),
 * sin cambios de lógica: solo se movieron las variables globales a PulseState. */
#include "pulse_signal.h"
#include <string.h>

void Pulse_Reset(PulseState *s) {
 memset(s,0,sizeof(*s));
 s->bpm=-1.0f;
}

static void insertion_sort_u32(uint32_t *v,uint32_t n) {
 for(uint32_t i=1;i<n;i++) {
  uint32_t k=v[i];int32_t j=(int32_t)i-1;
  while(j>=0 && v[j]>k){v[j+1]=v[j];j--;}
  v[j+1]=k;
 }
}

static void insertion_sort_f(float *v,uint32_t n) {
 for(uint32_t i=1;i<n;i++) {
  float k=v[i];int32_t j=(int32_t)i-1;
  while(j>=0 && v[j]>k){v[j+1]=v[j];j--;}
  v[j+1]=k;
 }
}

/* hr_process del original. */
static float hr_process(PulseState *s,uint32_t ir_raw) {
 float x=(float)ir_raw;
 s->sample_idx++;
 /* 1. Sin dedo: reiniciar estado */
 if(ir_raw<PULSE_FINGER_THR) {
  s->dc=x;s->ac=0;s->env=0;s->prev1=s->prev2=0;
  s->last_peak=0;s->int_pos=0;s->int_cnt=0;s->finger_smp=0;
  s->w_cnt=0;s->rat_pos=0;s->rat_cnt=0;s->beat_flag=0;
  return -1.0f;
 }
 s->finger_smp++;
 /* 2. Quitar DC e invertir */
 s->dc+=(x-s->dc)/16.0f;
 s->ac=s->dc-x;
 /* 3. Envolvente: sube instantáneo, decae 2 % por muestra */
 s->env*=0.98f;
 if(s->ac>s->env)s->env=s->ac;
 /* 4. ¿prev1 fue pico? */
 if(s->prev1>s->prev2 && s->prev1>=s->ac && s->prev1>0.5f*s->env &&
    (s->sample_idx-1U-s->last_peak)>PULSE_REFRACT_SMP) {
  uint32_t peak=s->sample_idx-1U;
  if(s->last_peak!=0 && s->finger_smp>PULSE_SETTLE_SMP) {
   s->intervals[s->int_pos]=peak-s->last_peak;
   s->int_pos=(uint8_t)((s->int_pos+1U)%PULSE_N_AVG);
   if(s->int_cnt<PULSE_N_AVG)s->int_cnt++;
  }
  s->beat_flag=1;
  s->last_peak=peak;
 }
 s->prev2=s->prev1;
 s->prev1=s->ac;
 /* 5. BPM con media recortada */
 if(s->int_cnt<PULSE_N_AVG)return 0.0f;
 if(s->sample_idx-s->last_peak>PULSE_STALE_SMP)return -2.0f; /* sin latidos recientes */
 uint32_t v[PULSE_N_AVG];
 memcpy(v,s->intervals,sizeof(v));
 insertion_sort_u32(v,PULSE_N_AVG);
 float mid=(float)(v[1]+v[2]+v[3])/3.0f; /* descarta el menor y el mayor */
 return 60.0f*(float)PULSE_FS_HZ/mid;
}

/* spo2_update del original. */
static float spo2_update(PulseState *s,uint32_t red,uint32_t ir) {
 /* 1. Acumular muestra en la ventana actual */
 if(s->w_cnt==0) {
  s->r_min=s->r_max=red;s->i_min=s->i_max=ir;
  s->r_sum=s->i_sum=0;
 }
 if(red<s->r_min)s->r_min=red;
 if(red>s->r_max)s->r_max=red;
 if(ir<s->i_min)s->i_min=ir;
 if(ir>s->i_max)s->i_max=ir;
 s->r_sum+=(float)red;s->i_sum+=(float)ir;s->w_cnt++;
 /* 2. Latido nuevo: cerrar ventana y calcular R */
 if(s->beat_flag) {
  s->beat_flag=0;
  if(s->finger_smp>PULSE_SETTLE_SMP && s->w_cnt>=PULSE_REFRACT_SMP && s->w_cnt<=PULSE_STALE_SMP) {
   float r_ac=(float)(s->r_max-s->r_min),i_ac=(float)(s->i_max-s->i_min);
   float r_dc=s->r_sum/(float)s->w_cnt,i_dc=s->i_sum/(float)s->w_cnt;
   if(i_ac>0.0f && r_dc>0.0f) {
    s->ratios[s->rat_pos]=(r_ac/r_dc)/(i_ac/i_dc);
    s->rat_pos=(uint8_t)((s->rat_pos+1U)%PULSE_N_SPO2);
    if(s->rat_cnt<PULSE_N_SPO2)s->rat_cnt++;
   }
  }
  s->w_cnt=0;
 }
 /* 3. Mediana de R */
 if(s->rat_cnt<PULSE_N_SPO2)return 0.0f;
 float v[PULSE_N_SPO2];
 memcpy(v,s->ratios,sizeof(v));
 insertion_sort_f(v,PULSE_N_SPO2);
 float r=v[PULSE_N_SPO2/2U];
 /* 4. R -> SpO2 */
 if(r<0.3f || r>1.3f)return -1.0f;
 float spo2=1.5958f*r*r-34.6597f*r+112.6899f;
 return spo2>100.0f?100.0f:spo2;
}

void Pulse_Push(PulseState *s,uint32_t red,uint32_t ir) {
 s->bpm=hr_process(s,ir);
 s->spo2=spo2_update(s,red,ir);
}

PulseStatus Pulse_Status(const PulseState *s) {
 if(s->bpm==-1.0f)return PULSE_NO_FINGER;
 if(s->bpm==-2.0f)return PULSE_WEAK;
 if(s->bpm<=0.0f)return PULSE_MEASURING;
 return PULSE_OK;
}

uint16_t Pulse_Bpm(const PulseState *s){return s->bpm>0.0f?(uint16_t)(s->bpm+0.5f):0U;}
uint16_t Pulse_Spo2(const PulseState *s){return (s->bpm>0.0f && s->spo2>0.0f)?(uint16_t)(s->spo2+0.5f):0U;}
uint16_t Pulse_PiX100(const PulseState *s) {
 if(s->bpm==-1.0f || s->dc<=0.0f || s->env<=0.0f)return 0U;
 float pi=10000.0f*s->env/s->dc;
 return pi>65535.0f?65535U:(uint16_t)(pi+0.5f);
}
