/* SPDX-License-Identifier: GPL-3.0-or-later
 * Two small deterministic Frei0r filters. This file is compiled twice with
 * STUDIO_LENS or STUDIO_VINTAGE so every module exposes one Frei0r entry set.
 */
#include "frei0r.h"
#include "specialized_contract.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__)
#define EXPORT __attribute__((visibility("default")))
#else
#define EXPORT
#endif
#define PI 3.14159265358979323846f

#if defined(STUDIO_LENS)
#define PARAM_COUNT STUDIO_LENS_PARAM_COUNT
static const char *const names[PARAM_COUNT] = {"projection","strength","shape","radius","aspect","center_x","center_y","edge_darkness","edge_softness","overscan","edge_blur","sharpness_falloff","aberration","bloom","contrast","mix"};
static const double defaults[PARAM_COUNT] = {.5,.78,0,.82,.5,.5,.5,1,.36,.16,.28,.34,.12,.22,.46,1};
#elif defined(STUDIO_VINTAGE)
#define PARAM_COUNT STUDIO_VINTAGE_PARAM_COUNT
static const char *const names[PARAM_COUNT] = {"format","amount","chroma_bleed","luma_ring","hue_drift","interlace","scanlines","flicker","jitter","timebase","dropouts","grain","dust","bloom","vignette","seed","mix"};
static const double defaults[PARAM_COUNT] = {.25,.62,.46,.28,.22,.26,.30,.18,.24,.18,.12,.36,.20,.18,.24,.137,1};
#else
#error Define STUDIO_LENS or STUDIO_VINTAGE
#endif

typedef struct { float r,g,b,a; } Pixel;
typedef struct { unsigned w,h; size_t count; double p[PARAM_COUNT]; uint32_t *copy; } Instance;
static float clampf(float x,float lo,float hi){return x<lo?lo:x>hi?hi:x;}
static float smooth(float lo,float hi,float x){float t=clampf((x-lo)/fmaxf(.00001f,hi-lo),0,1);return t*t*(3-2*t);}
static uint32_t hash32(uint32_t x){x^=x>>16;x*=UINT32_C(0x7feb352d);x^=x>>15;x*=UINT32_C(0x846ca68b);return x^(x>>16);}
static float noise(uint32_t x){return (float)(hash32(x)&65535u)/32767.5f-1.f;}
static unsigned char byte(float x){return (unsigned char)(clampf(x,0,255)+.5f);}
static Pixel load(const uint32_t *in,size_t i){const unsigned char *p=(const unsigned char *)(in+i);Pixel c={p[0],p[1],p[2],p[3]};return c;}
static void store(uint32_t *out,size_t i,Pixel c){unsigned char *p=(unsigned char *)(out+i);p[0]=byte(c.r);p[1]=byte(c.g);p[2]=byte(c.b);p[3]=byte(c.a);}
static Pixel blend(Pixel a,Pixel b,float t){t=clampf(t,0,1);Pixel c={a.r+(b.r-a.r)*t,a.g+(b.g-a.g)*t,a.b+(b.b-a.b)*t,a.a};return c;}
static float mirror(float x,unsigned n){if(n<2)return 0;float edge=(float)(n-1);x=fmodf(fabsf(x),2*edge);return x>edge?2*edge-x:x;}
static Pixel sample(const Instance *s,const uint32_t *in,float x,float y){
 x=mirror(x,s->w);y=mirror(y,s->h);unsigned x0=(unsigned)x,y0=(unsigned)y,x1=x0+1<s->w?x0+1:x0,y1=y0+1<s->h?y0+1:y0;
 float fx=x-x0,fy=y-y0;Pixel a=load(in,(size_t)y0*s->w+x0),b=load(in,(size_t)y0*s->w+x1),c=load(in,(size_t)y1*s->w+x0),d=load(in,(size_t)y1*s->w+x1);
 Pixel top=blend(a,b,fx),bottom=blend(c,d,fx),result=blend(top,bottom,fy);result.a=a.a*(1-fx)*(1-fy)+b.a*fx*(1-fy)+c.a*(1-fx)*fy+d.a*fx*fy;return result;
}
static float luma(Pixel c){return .2126f*c.r+.7152f*c.g+.0722f*c.b;}

EXPORT int f0r_init(void){return 1;}
EXPORT void f0r_deinit(void){}
EXPORT void f0r_get_plugin_info(f0r_plugin_info_t *info){
 if(!info)return;
#if defined(STUDIO_LENS)
 *info=(f0r_plugin_info_t){"Studio Lens","Studio FX contributors",F0R_PLUGIN_TYPE_FILTER,F0R_COLOR_MODEL_RGBA8888,FREI0R_MAJOR_VERSION,0,1,PARAM_COUNT,"Aspect-correct fisheye and peephole with a dark optical aperture."};
#else
 *info=(f0r_plugin_info_t){"Studio Vintage","Studio FX contributors",F0R_PLUGIN_TYPE_FILTER,F0R_COLOR_MODEL_RGBA8888,FREI0R_MAJOR_VERSION,0,1,PARAM_COUNT,"Seek-stable old camera, tape and CRT signal treatment."};
#endif
}
EXPORT void f0r_get_param_info(f0r_param_info_t *info,int i){if(info)*info=(f0r_param_info_t){i>=0&&i<PARAM_COUNT?names[i]:"invalid",F0R_PARAM_DOUBLE,"Normalized stable ABI parameter [0,1]."};}
EXPORT f0r_instance_t f0r_construct(unsigned w,unsigned h){if(!w||!h||w>8192||h>8192||(size_t)w*h>UINT32_C(16777216))return NULL;Instance *s=calloc(1,sizeof *s);if(!s)return NULL;s->w=w;s->h=h;s->count=(size_t)w*h;memcpy(s->p,defaults,sizeof defaults);return s;}
EXPORT void f0r_destruct(f0r_instance_t instance){Instance *s=instance;if(s){free(s->copy);free(s);}}
EXPORT void f0r_set_param_value(f0r_instance_t instance,f0r_param_t value,int i){Instance *s=instance;if(!s||!value||i<0||i>=PARAM_COUNT)return;double v=*(double *)value;if(!isfinite(v))v=defaults[i];s->p[i]=v<0?0:v>1?1:v;}
EXPORT void f0r_get_param_value(f0r_instance_t instance,f0r_param_t value,int i){Instance *s=instance;if(s&&value&&i>=0&&i<PARAM_COUNT)*(double *)value=s->p[i];}

#if defined(STUDIO_LENS)
static void render(Instance *s,double time,const uint32_t *in,uint32_t *out){
 (void)time;float strength=(float)s->p[1],radius=.35f+.65f*(float)s->p[3],cx=(float)s->p[5]*(s->w-1),cy=(float)s->p[6]*(s->h-1);
 float aspect=.55f+.9f*(float)s->p[4],soft=.015f+.35f*(float)s->p[8],dark=(float)s->p[7],overscan=1.f+.35f*(float)s->p[9];
 float blur=(float)s->p[10],fall=(float)s->p[11],ab=(float)s->p[12],bloom=(float)s->p[13],contrast=.7f+1.3f*(float)s->p[14],mix=(float)s->p[15];
 int projection=(int)floor(s->p[0]*2.999),shape=(int)floor(s->p[2]*2.999);float side=(float)(s->w<s->h?s->w:s->h);
 for(unsigned y=0;y<s->h;y++)for(unsigned x=0;x<s->w;x++){
  size_t i=(size_t)y*s->w+x;Pixel original=load(in,i);float dx=((float)x-cx)/(side*.5f*radius),dy=((float)y-cy)/(side*.5f*radius*aspect);
  float aperture=shape==2?fmaxf(fabsf(dx),fabsf(dy)):sqrtf(dx*dx+dy*dy);if(shape==1)aperture=sqrtf(dx*dx+dy*dy*.72f);
  float r=sqrtf(dx*dx+dy*dy),theta=atan2f(dy,dx),mapped=r;
  if(projection==0)mapped=r*(1.f+.28f*strength*r*r);
  else if(projection==1)mapped=tanf(fminf(r,1.35f)*(.55f+.25f*strength))/(.55f+.25f*strength);
  else mapped=sinf(fminf(r,1.45f)*(1.f+.18f*strength))/(1.f+.18f*strength);
  mapped*=overscan;float sx=cx+cosf(theta)*mapped*side*.5f*radius,sy=cy+sinf(theta)*mapped*side*.5f*radius*aspect;
  float edge=smooth(.58f,1.f,r),blurPx=blur*edge*side*.012f;Pixel c=sample(s,in,sx,sy);
  if(blurPx>.2f){Pixel a=sample(s,in,sx+blurPx,sy),b=sample(s,in,sx-blurPx,sy),d=sample(s,in,sx,sy+blurPx),e=sample(s,in,sx,sy-blurPx);c.r=(c.r+a.r+b.r+d.r+e.r)/5;c.g=(c.g+a.g+b.g+d.g+e.g)/5;c.b=(c.b+a.b+b.b+d.b+e.b)/5;}
  float fringe=ab*edge*side*.006f;if(fringe>.05f){Pixel red=sample(s,in,sx+cosf(theta)*fringe,sy+sinf(theta)*fringe),blue=sample(s,in,sx-cosf(theta)*fringe,sy-sinf(theta)*fringe);c.r=red.r;c.b=blue.b;}
  float lum=luma(c);c.r+=(255-c.r)*bloom*smooth(155,245,lum)*.35f;c.g+=(255-c.g)*bloom*smooth(155,245,lum)*.35f;c.b+=(255-c.b)*bloom*smooth(155,245,lum)*.35f;
  float sharp=1.f-fall*edge*.32f;c.r=127.5f+(c.r-127.5f)*contrast*sharp;c.g=127.5f+(c.g-127.5f)*contrast*sharp;c.b=127.5f+(c.b-127.5f)*contrast*sharp;
  float outside=smooth(1.f-soft,1.f+soft,aperture);Pixel black={0,0,0,original.a};c=blend(c,black,outside*dark);c.a=original.a;store(out,i,blend(original,c,mix));
 }
}
#else
static void render(Instance *s,double time,const uint32_t *in,uint32_t *out){
 double seconds=isfinite(time)?fmod(time,1000000.):0;uint32_t tick=(uint32_t)floor(seconds*24.0),seed=hash32((uint32_t)(s->p[15]*UINT32_C(16777215)));
 float amount=(float)s->p[1],bleed=(float)s->p[2],ring=(float)s->p[3],hue=(float)s->p[4],interlace=(float)s->p[5],scan=(float)s->p[6],flicker=(float)s->p[7];
 float jitter=(float)s->p[8],timebase=(float)s->p[9],drop=(float)s->p[10],grain=(float)s->p[11],dust=(float)s->p[12],bloom=(float)s->p[13],vignette=(float)s->p[14],mix=(float)s->p[16];
 int format=(int)floor(s->p[0]*4.999);float side=(float)(s->w<s->h?s->w:s->h),frameFlicker=1.f+flicker*.07f*sinf((float)seconds*11.7f+noise(seed));
 for(unsigned y=0;y<s->h;y++){
  uint32_t rowHash=hash32(seed^tick^(y*UINT32_C(2654435761)));float rowShift=noise(rowHash)*jitter*side*.012f+timebase*side*.009f*sinf((float)y*.035f+(float)seconds*6.f);
  for(unsigned x=0;x<s->w;x++){
   size_t i=(size_t)y*s->w+x;Pixel original=load(in,i),c=sample(s,in,(float)x+rowShift,(float)y);float spread=bleed*side*.01f;
   Pixel left=sample(s,in,(float)x+rowShift-spread,(float)y),right=sample(s,in,(float)x+rowShift+spread,(float)y);float center=luma(c),near=luma(sample(s,in,(float)x+rowShift+1,(float)y));
   c.r=left.r;c.b=right.b;c.r+=(center-near)*ring*.8f;c.g+=(near-center)*ring*.25f;
   float phase=hue*.12f*sinf((float)seconds*.83f+2.f);float rr=c.r;c.r+=phase*(c.g-c.b);c.b+=phase*(c.g-rr);
   float lines=1.f-scan*.16f*(.5f+.5f*sinf((float)y*PI));if(((y+tick)&1u)!=0u)lines*=1.f-interlace*.10f;c.r*=lines*frameFlicker;c.g*=lines*frameFlicker;c.b*=lines*frameFlicker;
   float n=noise(seed^(uint32_t)i^hash32(tick))*grain*24.f;c.r+=n;c.g+=n;c.b+=n;
   if((rowHash%1000u)<(unsigned)(drop*32.f)&&x>(rowHash%s->w)){c.r*=.35f;c.g*=.35f;c.b*=.35f;}
   uint32_t speck=hash32(seed^(uint32_t)i^hash32(tick/3u));if((speck%20000u)<(unsigned)(dust*18.f)){c.r=c.g=c.b=(speck&1u)?235.f:20.f;}
   float lum=luma(c),glow=bloom*smooth(150,245,lum)*.28f;c.r+=(255-c.r)*glow;c.g+=(255-c.g)*glow;c.b+=(255-c.b)*glow;
   float nx=(2.f*x)/(s->w?s->w:1)-1.f,ny=(2.f*y)/(s->h?s->h:1)-1.f,vig=1.f-vignette*.55f*smooth(.35f,1.35f,nx*nx+ny*ny);c.r*=vig;c.g*=vig;c.b*=vig;
   if(format==1){c.r*=1.05f;c.b*=.92f;}else if(format==2){float l=luma(c);c.r=.72f*c.r+.28f*l+10;c.g=.78f*c.g+.22f*l+4;c.b=.62f*c.b+.38f*l;}else if(format==3){c.r*=.95f;c.g*=1.02f;}else if(format==4){c.r=roundf(c.r/12)*12;c.g=roundf(c.g/12)*12;c.b=roundf(c.b/12)*12;}
   c.a=original.a;store(out,i,blend(original,c,amount*mix));
  }
 }
}
#endif

EXPORT void f0r_update(f0r_instance_t instance,double time,const uint32_t *in,uint32_t *out){Instance *s=instance;if(!s||!in||!out)return;if(in==out){if(!s->copy)s->copy=malloc(s->count*4);if(!s->copy)return;memcpy(s->copy,in,s->count*4);in=s->copy;}render(s,time,in,out);}
EXPORT void f0r_update2(f0r_instance_t instance,double time,const uint32_t *in,const uint32_t *unused2,const uint32_t *unused3,uint32_t *out){(void)unused2;(void)unused3;f0r_update(instance,time,in,out);}
