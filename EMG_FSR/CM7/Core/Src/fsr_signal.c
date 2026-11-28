/* Bloque de 50 ms de cada ADC -> voltaje medio por FSR -> R -> fuerza -> nivel.
 * Misma ley de potencia y mismos umbrales que el proyecto FSR original.
 */
#include "fsr_signal.h"
#include <math.h>
#include <string.h>

static void channel_stats(const uint16_t *b,uint32_t channels,float *mean_v,float *pp_v) {
 const float k=FSR_ADC_VREF_V/FSR_ADC_MAX_CODE;
 for(uint32_t ch=0;ch<channels;ch++) {
  uint32_t sum=0;uint16_t lo=0xFFFF,hi=0;
  for(uint32_t i=0;i<FSR_BLOCK_SCANS;i++) {
   uint16_t v=b[i*channels+ch];sum+=v;
   if(v<lo)lo=v;
   if(v>hi)hi=v;
  }
  mean_v[ch]=(float)sum/(float)FSR_BLOCK_SCANS*k;
  pp_v[ch]=(float)(hi-lo)*k;
 }
}

static FsrLevel level_of(float force_n) {
 if(force_n<FSR_FORCE_LIGHT_N)return FSR_NONE;
 if(force_n<FSR_FORCE_MED_N)return FSR_LIGHT;
 if(force_n<FSR_FORCE_HIGH_N)return FSR_MEDIUM;
 return FSR_STRONG;
}

static uint16_t to_mv(float v) {
 if(v<0.0f)return 0;
 if(v>65.535f)return 65535U;
 return (uint16_t)(v*1000.0f+0.5f);
}

void FsrSignal_Process(const uint16_t *adc2_block,const uint16_t *adc3_block,
 uint32_t block,FsrResult *out) {
 static const FsrMap map[FSR_COUNT]=FSR_MAP;
 float mean2[FSR_ADC2_CHANNELS],pp2[FSR_ADC2_CHANNELS];
 float mean3[FSR_ADC3_CHANNELS],pp3[FSR_ADC3_CHANNELS];
 channel_stats(adc2_block,FSR_ADC2_CHANNELS,mean2,pp2);
 channel_stats(adc3_block,FSR_ADC3_CHANNELS,mean3,pp3);
 float vcc=FSR_VCC_V;
#if FSR_USE_5V_MONITOR
 float measured=mean3[FSR_ADC3_VMON_INDEX]*FSR_VMON_GAIN;
 /* Si el monitor marca algo absurdo (desconectado) se usa el nominal. */
 if(measured>4.0f && measured<5.6f)vcc=measured;
#endif
 memset(out,0,sizeof(*out));
 out->block=block;out->vcc_mv=to_mv(vcc);
 float fmax=-1.0f;
 for(uint32_t i=0;i<FSR_COUNT;i++) {
  float v=map[i].adc==3U?mean3[map[i].index]:mean2[map[i].index];
  float pp=map[i].adc==3U?pp3[map[i].index]:pp2[map[i].index];
  float force=0.0f;
  out->pin_mv[i]=to_mv(v);out->pp_mv[i]=to_mv(pp);
  if(v<FSR_V_MIN) out->r_ohm[i]=-1;
  else {
   float r=vcc*FSR_R2_OHM/v-(FSR_R1_OHM+FSR_R2_OHM);
   if(r<FSR_R_MIN_OHM)r=FSR_R_MIN_OHM;
   force=FSR_F_REF_N*powf(FSR_R_REF_OHM/r,FSR_EXP_K);
   out->r_ohm[i]=(int32_t)(r+0.5f);
  }
  float mn=force*1000.0f;
  out->force_mn[i]=mn>4.0e9f?4000000000U:(uint32_t)(mn+0.5f);
  if(force>fmax){fmax=force;out->max_index=(uint8_t)i;}
 }
 out->max_level=level_of(fmax);
}
