/* SPDX-License-Identifier: GPL-3.0-or-later
 * Studio FX: bounded, deterministic SDR RGBA8888 effects for Frei0r.
 * No external image libraries, file access, threads, global RNG or GPU context.
 * One instance owns its caches. A host must serialize access to each instance,
 * as required by Frei0r. Separate instances may be rendered concurrently.
 */
#include "frei0r.h"
#include "sfx_contract.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#if defined(__GNUC__)
#define EXPORT __attribute__((visibility("default")))
#else
#define EXPORT
#endif
#define PI 3.14159265358979323846f

typedef struct { float r,g,b,a; } Pixel;
typedef struct {
 unsigned w,h; size_t pixels;
 double p[SFX_PARAM_COUNT];
 float *mapx, *mapy, *vignette; double cache_key[8]; int cache_valid;
 float *glow, *temp; unsigned gw,gh;
 uint32_t *alias_copy;
} Instance;

static float clampf(float x,float a,float b) { return x<a?a:(x>b?b:x); }
static float smooth(float a,float b,float x) {
 float u=clampf((x-a)/fmaxf(.00001f,b-a),0.f,1.f); return u*u*(3.f-2.f*u);
}
static uint32_t hash32(uint32_t x) {
 x ^= x>>16; x *= UINT32_C(0x7feb352d); x ^= x>>15;
 x *= UINT32_C(0x846ca68b); x ^= x>>16; return x;
}
static float noise(uint32_t x) { return (float)(hash32(x)&65535u)/32767.5f-1.f; }
static unsigned char byte(float x) { return (unsigned char)(clampf(x,0.f,255.f)+.5f); }
static Pixel load(const uint32_t *in,size_t i) {
 const unsigned char *p=(const unsigned char*)(in+i);
 Pixel c={(float)p[0],(float)p[1],(float)p[2],(float)p[3]}; return c;
}
static void store(uint32_t *out,size_t i,Pixel c) {
 unsigned char *p=(unsigned char*)(out+i);
 p[0]=byte(c.r); p[1]=byte(c.g); p[2]=byte(c.b); p[3]=byte(c.a);
}
static float luma(Pixel c) { return .2126f*c.r+.7152f*c.g+.0722f*c.b; }
/* Linear blend in premultiplied-alpha space; input/output stay straight RGBA.
 * This avoids opaque halos when mixing distorted semitransparent footage.
 */
static Pixel blend(Pixel a,Pixel b,float t) {
 if(t<=0.f) return a;
 if(t>=1.f) return b;
 if(a.a==b.a) {
  Pixel c={a.r+(b.r-a.r)*t,a.g+(b.g-a.g)*t,a.b+(b.b-a.b)*t,a.a};
  return c;
 }
 float aa=a.a*(1.f-t), ba=b.a*t, total=aa+ba;
 Pixel c={0,0,0,total};
 if(total>.00001f) {
  c.r=(a.r*aa+b.r*ba)/total; c.g=(a.g*aa+b.g*ba)/total; c.b=(a.b*aa+b.b*ba)/total;
 }
 return c;
}
static float mirror_coord(float v,unsigned size) {
 if(size<=1) return 0.f;
 float edge=(float)(size-1);
 if(v>=0.f && v<=edge) return v;
 v=fmodf(fabsf(v),2.f*edge); return v>edge?2.f*edge-v:v;
}
static Pixel sample(const Instance *s,const uint32_t *in,float x,float y) {
 x=mirror_coord(x,s->w); y=mirror_coord(y,s->h);
 unsigned x0=(unsigned)x,y0=(unsigned)y;
 unsigned x1=x0+1<s->w?x0+1:x0,y1=y0+1<s->h?y0+1:y0;
 float fx=x-(float)x0,fy=y-(float)y0;
 if(fx==0.f && fy==0.f) return load(in,(size_t)y0*s->w+x0);
 if(fy==0.f) return blend(load(in,(size_t)y0*s->w+x0),load(in,(size_t)y0*s->w+x1),fx);
 Pixel c[4]={load(in,(size_t)y0*s->w+x0),load(in,(size_t)y0*s->w+x1),
             load(in,(size_t)y1*s->w+x0),load(in,(size_t)y1*s->w+x1)};
 float wt[4]={(1-fx)*(1-fy),fx*(1-fy),(1-fx)*fy,fx*fy};
 Pixel out={0,0,0,0};
 if(c[0].a==255.f && c[1].a==255.f && c[2].a==255.f && c[3].a==255.f) {
  for(int k=0;k<4;k++){out.r+=c[k].r*wt[k];out.g+=c[k].g*wt[k];out.b+=c[k].b*wt[k];}
  out.a=255.f;return out;
 }
 for(int k=0;k<4;k++) {
  float a=c[k].a*wt[k]; out.a+=a;
  out.r+=c[k].r*a; out.g+=c[k].g*a; out.b+=c[k].b*a;
 }
 if(out.a>.00001f) {out.r/=out.a;out.g/=out.a;out.b/=out.a;}
 return out;
}
static int slot_of(const Instance *s) {
 int slot=(int)(s->p[0]*SFX_SLOT_SCALE+.5);
 return slot>=0 && slot<SFX_RECIPE_COUNT?slot:0;
}
static int is_geometry(int alg) {
 return alg==SFX_LENS || alg==SFX_DEFORM || alg==SFX_MIRROR || alg==SFX_KALEIDO;
}
/* Geometry only rebuilds on a relevant parameter change. Animation transforms
 * the precomputed map instead of recomputing trigonometry at every pixel.
 */
static int prepare_map(Instance *s,int alg,int variant) {
 double key[8]={s->p[0],s->p[1],s->p[2],s->p[5],s->p[6],s->p[7],(double)s->w,(double)s->h};
 if(s->cache_valid && memcmp(key,s->cache_key,sizeof key)==0) return 1;
 if(!s->mapx) {
  s->mapx=malloc(s->pixels*sizeof(float)); s->mapy=malloc(s->pixels*sizeof(float));
  if(!s->mapx || !s->mapy) {free(s->mapx);free(s->mapy);s->mapx=s->mapy=NULL;return 0;}
 }
 float a=(float)s->p[1],size=(float)s->p[2],soft=(float)s->p[7];
 float cx=(float)s->p[5]*(s->w-1),cy=(float)s->p[6]*(s->h-1);
 float side=(float)(s->w<s->h?s->w:s->h);
 float radius=(.12f+size*.95f)*side;
 for(unsigned y=0;y<s->h;y++) for(unsigned x=0;x<s->w;x++) {
  float dx=((float)x-cx)/radius,dy=((float)y-cy)/radius;
  float rr=dx*dx+dy*dy, r=sqrtf(rr);
  float mask=1.f-smooth(1.f-soft*.85f,1.f,r);
  float nx=(float)x,ny=(float)y;
  if(alg==SFX_LENS) {
   float k=1.f;
   switch(variant) {
    case 0: k=1.f+a*.42f*(rr-.7f); nx=cx+dx*radius*k;ny=cy+dy*radius*k; break;
    case 1: case 6: k=1.f-a*.70f*mask; nx=cx+dx*radius*k;ny=cy+dy*radius*k; break;
    case 2: k=1.f+a*.90f*rr;nx=cx+dx*radius*k;ny=cy+dy*radius*k; break;
    case 3: k=1.f+a*.95f*mask;nx=cx+dx*radius*k;ny=cy+dy*radius*k; break;
    case 4: nx=cx+dx*radius*(1.f-a*.60f*mask);break;
    case 5: ny=cy+dy*radius*(1.f-a*.60f*mask);break;
   }
  } else if(alg==SFX_DEFORM) {
   switch(variant) {
    case 0: nx=cx+dx*radius*(1.f-.55f*a*mask);break;
    case 1: nx=cx+dx*radius*(1.f+.80f*a*mask);break;
    case 2: ny=cy+dy*radius*(1.f-.55f*a*mask);break;
    case 3: ny=cy+dy*radius*(1.f+.90f*a*mask);break;
    case 4: nx+=(sinf(dy*5.f)*.045f*side*a*mask);ny+=(sinf(dx*4.f)*.028f*side*a*mask);break;
    case 5: nx+=(sinf(dy*8.f)*.08f*side*a);ny+=(sinf(dx*6.f)*.035f*side*a);break;
    case 6: {float theta=2.8f*a*mask,cs=cosf(theta),sn=sinf(theta);
      nx=cx+(dx*cs-dy*sn)*radius;ny=cy+(dx*sn+dy*cs)*radius;break;}
   }
  } else if(alg==SFX_MIRROR) {
   float tx=(float)x,ty=(float)y;
   switch(variant) {
    case 0: tx=cx-fabsf((float)x-cx);break;
    case 1: tx=cx+fabsf((float)x-cx);break;
    case 2: ty=cy-fabsf((float)y-cy);break;
    case 3: ty=cy+fabsf((float)y-cy);break;
    case 4: {float xx=(float)x/s->w,yy=(float)y/s->h;
       if(xx>yy) {tx=yy*s->w;ty=xx*s->h;}break;}
   }
   nx=(float)x+(tx-(float)x)*a;ny=(float)y+(ty-(float)y)*a;
  } else if(alg==SFX_KALEIDO) {
   float theta=atan2f(dy,dx),sector=2.f*PI/(float)variant;
   theta=fmodf(theta+4.f*PI,sector);
   if(theta>sector*.5f) theta=sector-theta;
   float z=.55f+size;
   nx=cx+cosf(theta)*r*radius/z;ny=cy+sinf(theta)*r*radius/z;
   nx=(float)x+(nx-(float)x)*a;ny=(float)y+(ny-(float)y)*a;
  }
  size_t i=(size_t)y*s->w+x;s->mapx[i]=nx;s->mapy[i]=ny;
 }
 memcpy(s->cache_key,key,sizeof key);s->cache_valid=1;return 1;
}

/* Dedicated simple kernels keep cheap effects cheap. No shader framework.
 * Static vignette mask: one parameter-time pass, then streaming RGB multiply.
 */
static int render_vignette(Instance *s,const uint32_t *in,uint32_t *out,int variant) {
 double key[8]={s->p[0],s->p[1],s->p[2],s->p[5],s->p[6],s->p[7],(double)s->w,(double)s->h};
 if(!s->vignette) {s->vignette=malloc(s->pixels*sizeof(float));if(!s->vignette)return 0;}
 if(!s->cache_valid || memcmp(key,s->cache_key,sizeof key)!=0) {
  float side=(float)(s->w<s->h?s->w:s->h),cx=(float)s->p[5]*(s->w-1),cy=(float)s->p[6]*(s->h-1);
  float start=.08f+(float)s->p[2]*.98f,end=start+.1f+(float)s->p[7]*.95f;
  for(unsigned y=0;y<s->h;y++)for(unsigned x=0;x<s->w;x++){
   float dx=((float)x-cx)/(s->w*.5f),dy=((float)y-cy)/(s->h*.5f);
   if(variant==1){dx=((float)x-cx)/(side*.5f);dy=((float)y-cy)/(side*.5f);}
   if(variant==2)dx*=1.55f;
   s->vignette[(size_t)y*s->w+x]=smooth(start,end,sqrtf(dx*dx+dy*dy))*(float)s->p[1];
  }
  memcpy(s->cache_key,key,sizeof key);s->cache_valid=1;
 }
 float target[3]={0,0,0};if(variant==3){target[0]=245;target[1]=242;target[2]=233;}
 if(variant==4){target[0]=166;target[1]=91;target[2]=39;}
 const unsigned char *src=(const unsigned char*)in;unsigned char *dst=(unsigned char*)out;
 float mix=(float)s->p[4];
 for(size_t i=0;i<s->pixels;i++) {
  float t=s->vignette[i]*mix;
  for(int k=0;k<3;k++){float c=src[i*4+k];dst[i*4+k]=(unsigned char)(c+(target[k]-c)*t+.5f);}
  dst[i*4+3]=src[i*4+3];
 }
 return 1;
}
static void render_crt(Instance *s,const uint32_t *in,uint32_t *out,int variant) {
 const unsigned char *src=(const unsigned char*)in;unsigned char *dst=(unsigned char*)out;
 float a=(float)s->p[1],mix=(float)s->p[4];
 float period=fmaxf(1.f,(float)s->h/(180.f+420.f*(1.f-(float)s->p[2])));
 for(unsigned y=0;y<s->h;y++){
  float scan=.94f+.06f*sinf(2.f*PI*(float)y/period);
  for(unsigned x=0;x<s->w;x++){
   size_t i=((size_t)y*s->w+x)*4;
   float fr=scan*(x%3==0?.95f:1.f)*(variant==1?1.06f:1.f);
   float fg=scan,fb=scan*(x%3==2?.95f:1.f)*(variant==1?.88f:1.f);
   float r=clampf(src[i]*(1.f+a*(fr-1.f)),0,255),g=clampf(src[i+1]*(1.f+a*(fg-1.f)),0,255),b=clampf(src[i+2]*(1.f+a*(fb-1.f)),0,255);
   dst[i]=(unsigned char)(src[i]+(r-src[i])*mix+.5f);
   dst[i+1]=(unsigned char)(src[i+1]+(g-src[i+1])*mix+.5f);
   dst[i+2]=(unsigned char)(src[i+2]+(b-src[i+2])*mix+.5f);dst[i+3]=src[i+3];
  }
 }
}
static void render_centered_mirror(Instance *s,const uint32_t *in,uint32_t *out,int variant){
 for(unsigned y=0;y<s->h;y++)for(unsigned x=0;x<s->w;x++){
  unsigned xx=x,yy=y;
  if(variant==0 && x>s->w/2-((s->w%2)?0u:1u))xx=s->w-1-x;
  if(variant==1 && x<s->w/2)xx=s->w-1-x;
  if(variant==2 && y>s->h/2-((s->h%2)?0u:1u))yy=s->h-1-y;
  if(variant==3 && y<s->h/2)yy=s->h-1-y;
  out[(size_t)y*s->w+x]=in[(size_t)yy*s->w+xx];
 }
}
static float neighbor_luma(const Instance *s,const uint32_t *in,int x,int y,float center){
 int w=(int)s->w,h=(int)s->h;
 if(w==1)x=0;else {x=abs(x);if(x>=w)x=2*(w-1)-x;}
 if(h==1)y=0;else {y=abs(y);if(y>=h)y=2*(h-1)-y;}
 /* Radius is bounded by image size in this kernel. */
 if(x<0)x=0;
 if(y<0)y=0;
 const unsigned char *p=(const unsigned char*)(in+(size_t)y*s->w+(unsigned)x);
 return p[3]?(.2126f*p[0]+.7152f*p[1]+.0722f*p[2]):center;
}

/* Reduced-resolution glow, two O(N) sliding-window passes, no frame history. */
static int prepare_glow(Instance *s,const uint32_t *in,int variant,float size) {
 unsigned w=(s->w+3)/4,h=(s->h+3)/4;
 size_t count=(size_t)w*h*3;
 if(!s->glow) {
  s->glow=calloc(count,sizeof(float));s->temp=calloc(count,sizeof(float));
  if(!s->glow || !s->temp) {free(s->glow);free(s->temp);s->glow=s->temp=NULL;return 0;}
  s->gw=w;s->gh=h;
 }
 for(unsigned y=0;y<h;y++) for(unsigned x=0;x<w;x++) {
  float v[3]={0,0,0};int n=0;
  for(unsigned yy=y*4;yy<s->h && yy<y*4+4;yy++)
   for(unsigned xx=x*4;xx<s->w && xx<x*4+4;xx++) {
    Pixel c=load(in,(size_t)yy*s->w+xx);
    float gate=variant==1?.7f:smooth(100.f,230.f,luma(c));
    float fac=gate*c.a/(255.f*255.f);
    v[0]+=c.r*fac;v[1]+=c.g*fac;v[2]+=c.b*fac;n++;
   }
  size_t off=((size_t)y*w+x)*3;
  for(int c=0;c<3;c++)s->glow[off+c]=v[c]/n;
 }
 int radius=1+(int)((float)(w<h?w:h)*(.008f+.075f*size));
 int rx=variant==5?1:radius,ry=variant==4?1:radius;
 if(variant==4)rx*=3;
 if(variant==5)ry*=3;
 for(unsigned y=0;y<h;y++) for(int c=0;c<3;c++) {
  float sum=0;
  for(int k=-rx;k<=rx;k++) {unsigned xx=k<0?0:(unsigned)k>=w?w-1:(unsigned)k;sum+=s->glow[((size_t)y*w+xx)*3+c];}
  for(unsigned x=0;x<w;x++) {
   s->temp[((size_t)y*w+x)*3+c]=sum/(2*rx+1);
   int lo=(int)x-rx,hi=(int)x+rx+1;
   lo=lo<0?0:lo;hi=hi>=(int)w?(int)w-1:hi;
   sum+=s->glow[((size_t)y*w+(unsigned)hi)*3+c]-s->glow[((size_t)y*w+(unsigned)lo)*3+c];
  }
 }
 for(unsigned x=0;x<w;x++) for(int c=0;c<3;c++) {
  float sum=0;
  for(int k=-ry;k<=ry;k++) {unsigned yy=k<0?0:(unsigned)k>=h?h-1:(unsigned)k;sum+=s->temp[((size_t)yy*w+x)*3+c];}
  for(unsigned y=0;y<h;y++) {
   s->glow[((size_t)y*w+x)*3+c]=sum/(2*ry+1);
   int lo=(int)y-ry,hi=(int)y+ry+1;
   lo=lo<0?0:lo;hi=hi>=(int)h?(int)h-1:hi;
   sum+=s->temp[((size_t)(unsigned)hi*w+x)*3+c]-s->temp[((size_t)(unsigned)lo*w+x)*3+c];
  }
 }
 return 1;
}
static float glow_value(const Instance *s,unsigned x,unsigned y,int channel) {
 float gx=((float)x+.5f)*.25f-.5f,gy=((float)y+.5f)*.25f-.5f;
 gx=clampf(gx,0.f,(float)(s->gw-1));gy=clampf(gy,0.f,(float)(s->gh-1));
 unsigned x0=(unsigned)gx,y0=(unsigned)gy,x1=x0+1<s->gw?x0+1:x0,y1=y0+1<s->gh?y0+1:y0;
 float fx=gx-x0,fy=gy-y0;
 return (s->glow[((size_t)y0*s->gw+x0)*3+channel]*(1-fx)+s->glow[((size_t)y0*s->gw+x1)*3+channel]*fx)*(1-fy)
       +(s->glow[((size_t)y1*s->gw+x0)*3+channel]*(1-fx)+s->glow[((size_t)y1*s->gw+x1)*3+channel]*fx)*fy;
}

EXPORT int f0r_init(void) { return 1; }
EXPORT void f0r_deinit(void) {}
EXPORT void f0r_get_plugin_info(f0r_plugin_info_t *info) {
 if(!info)return;
 *info=(f0r_plugin_info_t){"FactMontage Effects","Studio FX contributors",F0R_PLUGIN_TYPE_FILTER,
 F0R_COLOR_MODEL_RGBA8888,FREI0R_MAJOR_VERSION,0,2,SFX_PARAM_COUNT,
 "80 deterministic SDR effects; bounded controls; no neural networks or GPU dependencies."};
}
EXPORT void f0r_get_param_info(f0r_param_info_t *info,int i) {
 if(!info)return;
 *info=(f0r_param_info_t){i>=0&&i<SFX_PARAM_COUNT?sfx_names[i]:"invalid",F0R_PARAM_DOUBLE,"Normalized stable ABI parameter [0,1]."};
}
EXPORT f0r_instance_t f0r_construct(unsigned w,unsigned h) {
 if(!w||!h||w>8192||h>8192||(size_t)w*h>UINT32_C(16777216))return NULL;
 Instance *s=calloc(1,sizeof(*s));if(!s)return NULL;
 s->w=w;s->h=h;s->pixels=(size_t)w*h;
 memcpy(s->p,sfx_recipes[0].defaults,sizeof s->p);return s;
}
EXPORT void f0r_destruct(f0r_instance_t instance) {
 Instance *s=instance;if(!s)return;
 free(s->mapx);free(s->mapy);free(s->vignette);free(s->glow);free(s->temp);free(s->alias_copy);free(s);
}
EXPORT void f0r_set_param_value(f0r_instance_t instance,f0r_param_t param,int i) {
 Instance *s=instance;if(!s||!param||i<0||i>=SFX_PARAM_COUNT)return;
 double v=*(const double*)param;
 if(!isfinite(v))v=sfx_recipes[slot_of(s)].defaults[i];
 int previous=slot_of(s);
 s->p[i]=v<0?0:v>1?1:v;
 if(i==0 && previous!=slot_of(s)) {
  int alg=sfx_recipes[slot_of(s)].algorithm;
  s->cache_valid=0;
  if(!is_geometry(alg)){free(s->mapx);free(s->mapy);s->mapx=s->mapy=NULL;}
  if(alg!=SFX_VIGNETTE){free(s->vignette);s->vignette=NULL;}
  if(alg!=SFX_GLOW){free(s->glow);free(s->temp);s->glow=s->temp=NULL;s->gw=s->gh=0;}
 }
}
EXPORT void f0r_get_param_value(f0r_instance_t instance,f0r_param_t param,int i) {
 Instance *s=instance;if(!s||!param||i<0||i>=SFX_PARAM_COUNT)return;
 *(double*)param=s->p[i];
}
EXPORT void f0r_update(f0r_instance_t instance,double time,const uint32_t *in,uint32_t *out) {
 Instance *s=instance;if(!s||!in||!out)return;
 if(s->p[1]<=0 || s->p[4]<=0) {if(in!=out)memcpy(out,in,s->pixels*4);return;}
 if(in==out) {
  if(!s->alias_copy)s->alias_copy=malloc(s->pixels*4);
  if(!s->alias_copy)return;
  memcpy(s->alias_copy,in,s->pixels*4);in=s->alias_copy;
 }
 int slot=slot_of(s),alg=sfx_recipes[slot].algorithm,v=sfx_recipes[slot].variant;
 float a=(float)s->p[1],size=(float)s->p[2],mix=(float)s->p[4],soft=(float)s->p[7];
 float side=(float)(s->w<s->h?s->w:s->h),cx=(float)s->p[5]*(s->w-1),cy=(float)s->p[6]*(s->h-1);
 float speed=fmaxf(.25f,2.f*(float)s->p[3]);
 /* Bound externally supplied time before converting to integer noise counters. */
 double seconds=isfinite(time)?fmod(time,1000000.):0.;
 seconds+=s->p[10]*86400.;
 double t=s->p[8]>=.5?seconds*(double)speed:0.;
 uint32_t tick=(uint32_t)(int64_t)floor(t*24.0);
 uint32_t seed=hash32((uint32_t)(s->p[9]*16777215.0));
 float pulse=.70f+.30f*sinf((float)(t*.8)),sn=sinf((float)(t*.18)),cs=cosf((float)(t*.18));
 if(alg==SFX_VIGNETTE) {if(!render_vignette(s,in,out,v))memcpy(out,in,s->pixels*4);return;}
 if(alg==SFX_TV && v<2) {render_crt(s,in,out,v);return;}
 if(alg==SFX_MIRROR && v<4 && s->p[1]==1. && s->p[4]==1. && s->p[5]==.5 && s->p[6]==.5) {
  render_centered_mirror(s,in,out,v);return;
 }
 if(is_geometry(alg) && !prepare_map(s,alg,v)) {memcpy(out,in,s->pixels*4);return;}
 if(alg==SFX_GLOW && !prepare_glow(s,in,v,size)){memcpy(out,in,s->pixels*4);return;}
 int cell=1+(int)(side*(.001f+size*size*.07f));
 if(alg==SFX_PIXEL && cell<2)cell=2;
 float scanperiod=fmaxf(1.f,(float)s->h/(180.f+420.f*(1.f-size)));
 static const int bayer[16]={0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5};
 for(unsigned y=0;y<s->h;y++) {
  uint32_t rowhash=hash32((y/(1u+(unsigned)(size*side*.015f)))^seed^hash32(tick));
  float rownoise=noise(rowhash);
  float scan=.94f+.06f*sinf(2.f*PI*(float)y/scanperiod);
  float jitter=0.f;
  if(alg==SFX_TV && v>=2)jitter=rownoise*side*.0025f*a*(v==3?3.f:1.f);
  if(alg==SFX_TV && v==5)jitter+=sinf((float)y/s->h*8.f+(float)t)*side*.028f*a;
  float filmx=alg==SFX_GRAIN && (v==2||v==3)?noise(seed^tick)*a*side*.002f:0;
  float filmy=alg==SFX_GRAIN && (v==2||v==3)?noise(seed^tick^1717u)*a*side*.001f:0;
  for(unsigned x=0;x<s->w;x++) {
   size_t i=(size_t)y*s->w+x;Pixel original=load(in,i),c=original;
   float xx=(float)x,yy=(float)y;
   if(is_geometry(alg)) {
    float mx=s->mapx[i],my=s->mapy[i];
    if(s->p[8]>=.5) {
     if(alg==SFX_LENS&&v==6) {mx=xx+(mx-xx)*pulse;my=yy+(my-yy)*pulse;}
     if(alg==SFX_DEFORM&&(v==4||v==5)) {
      mx=xx+(mx-xx)*sinf((float)(t*1.1)+.6f);my=yy+(my-yy)*cosf((float)(t*.83));
     }
     if(alg==SFX_KALEIDO) {float dx=mx-cx,dy=my-cy;mx=cx+dx*cs-dy*sn;my=cy+dx*sn+dy*cs;}
    }
    c=sample(s,in,mx,my);
    if(alg==SFX_LENS && v==2) {
     float dx=(xx-cx)/(side*.52f),dy=(yy-cy)/(side*.52f);
     float mask=1.f-smooth(.86f,1.0f,sqrtf(dx*dx+dy*dy));
     mask=1.f-a*(1.f-mask);
     c.r*=mask;c.g*=mask;c.b*=mask; /* black housing; not transparent */
    }
   } else if(alg==SFX_VIGNETTE) {
    float dx=(xx-cx)/(s->w*.5f),dy=(yy-cy)/(s->h*.5f);
    if(v==1){dx=(xx-cx)/(side*.5f);dy=(yy-cy)/(side*.5f);}
    if(v==2)dx*=1.55f;
    float rr=sqrtf(dx*dx+dy*dy),start=.08f+size*.98f;
    float wgt=smooth(start,start+.10f+soft*.95f,rr)*a;
    Pixel edge={0,0,0,c.a};
    if(v==3)edge=(Pixel){245,242,233,c.a};
    if(v==4)edge=(Pixel){166,91,39,c.a};
    c=blend(c,edge,wgt);
   } else if(alg==SFX_TV) {
    Pixel base=sample(s,in,xx+jitter,yy);
    c=base;
    float sh=side*(.001f+.006f*size)*(v==3?1.6f:1.f);
    Pixel left=sample(s,in,xx+jitter-sh,yy),right=sample(s,in,xx+jitter+sh,yy);
    float lum=luma(base),n=noise(seed^(uint32_t)i^hash32(tick))*((v==3)?20.f:7.f);
    if(v==0||v==1) {
     float grid=(x%3==0?.95f:1.f);
     c.r*=scan*grid;c.g*=scan;c.b*=scan*(x%3==2?.95f:1.f);
    } else {
     c.r=.50f*lum+.50f*left.r;c.g=.50f*lum+.50f*base.g;c.b=.50f*lum+.50f*right.b;
     c.r=c.r*scan+n;c.g=c.g*scan+n;c.b=c.b*scan+n;
    }
    if(v==1) {c.r*=1.06f;c.b*=.88f;}
    if(v==3 && (rowhash%97u)<3u){c.r+=25.f*rownoise;c.g+=25.f*rownoise;c.b+=25.f*rownoise;}
    if(v==4) {float l=luma(c);c.r=c.g=c.b=l;}
    if(v==6) {float l=luma(c);c.r=l*.64f;c.g=l*.95f;c.b=l*.67f;}
    if(v==7) {c.r=roundf(c.r/18.f)*18.f;c.g=roundf(c.g/18.f)*18.f;c.b=roundf(c.b/22.f)*22.f;}
    c.a=base.a;c=blend(original,c,a);
   } else if(alg==SFX_RGB) {
    float shift=side*(.001f+size*.015f)*a;
    float wave=v==2?sinf((float)t+yy/s->h*7.f):1.f;
    float dx=v==0?((xx-cx)/s->w)*shift:shift*wave;
    float dy=v==0?((yy-cy)/s->h)*shift:shift*.13f*wave;
    Pixel left=sample(s,in,xx-dx,yy-dy),right=sample(s,in,xx+dx,yy+dy);
    /* Colour fringing does not manufacture coverage outside the source alpha. */
    c.r=left.a>0?left.r:original.r;c.b=right.a>0?right.b:original.b;
   } else if(alg==SFX_GLITCH) {
    float gate=(rowhash%100u)<(unsigned)(4.f+22.f*a)?1.f:0.f;
    if(v==4)gate*=((hash32(tick/12u+seed)%9u)==0u?1.f:0.f);
    float shift=side*(.007f+.10f*size)*rownoise*a*gate;
    if(v==0)shift=roundf(shift/(float)cell)*(float)cell;
    c=sample(s,in,xx+shift,yy);
    if(v==2 && gate>0) {c.r=255-c.r;c.b=255-c.b;c=blend(original,c,a*.55f);}
    if(v==3 && gate>0) {
     float bx=floorf((xx+shift)/cell)*cell,by=floorf(yy/cell)*cell;
     c=sample(s,in,bx,by);c.r=roundf(c.r/32.f)*32.f;c.g=roundf(c.g/32.f)*32.f;c.b=roundf(c.b/32.f)*32.f;
    }
   } else if(alg==SFX_GRAIN) {
    if(filmx||filmy)c=sample(s,in,xx+filmx,yy+filmy);
    unsigned graincell=1u+(unsigned)(size*side*.006f);
    uint32_t nkey=(x/graincell)+UINT32_C(65537)*(y/graincell);
    float n=noise(nkey^hash32(tick)^seed)*a*25.f;
    float mid=1.f-fabsf(luma(c)/127.5f-1.f)*.60f;
    c.r+=n*mid;c.g+=n*mid;c.b+=n*mid;
    if(v==1||v==3||v==4) {
     Pixel toned=c;float l=luma(c);
     toned.r=l*.32f+c.r*.68f+10.f;toned.g=l*.22f+c.g*.76f+3.f;toned.b=l*.35f+c.b*.55f+8.f;
     c=blend(c,toned,a*.6f);
    }
    if(v==3) {float f=1.f+.03f*a*sinf((float)(t*2.4));c.r*=f;c.g*=f;c.b*=f;}
    if(v==4) {Pixel faded={20.f+c.r*.85f,19.f+c.g*.83f,25.f+c.b*.77f,c.a};c=blend(c,faded,a);}
   } else if(alg==SFX_DUST) {
    unsigned block=8u+(unsigned)(side*.045f);unsigned gx=x/block,gy=y/block;
    uint32_t h=hash32(gx+UINT32_C(4099)*gy+seed+hash32(tick/3u));
    float dx=(float)(x%block)/(float)block-(float)(h&255u)/255.f;
    float dy=(float)(y%block)/(float)block-(float)((h>>8)&255u)/255.f;
    float radius=.04f+.12f*size;
    float dust=(h%31u)==0u?1.f-smooth(radius*.45f,radius,sqrtf(dx*dx+dy*dy)):0.f;
    if(v==1) {
     uint32_t line=hash32((uint32_t)((float)x/fmaxf(1.f,side*.0015f))+seed+hash32(tick/8u));
     dust=(line%311u)<2u?.75f:0.f;
    }
    Pixel speck={225,216,193,c.a};c=blend(c,speck,dust*a);
   } else if(alg==SFX_GLOW) {
    float r=glow_value(s,x,y,0),g=glow_value(s,x,y,1),b=glow_value(s,x,y,2);
    if(v==2){r*=1.25f;g*=.83f;b*=.42f;}
    if(v==3){r*=1.30f;g*=.40f;b*=1.30f;}
    /* Screen blend only uses available headroom; highlights stay bounded. */
    c.r+=(255.f-c.r)*clampf(r*a*1.6f,0,1);
    c.g+=(255.f-c.g)*clampf(g*a*1.6f,0,1);
    c.b+=(255.f-c.b)*clampf(b*a*1.6f,0,1);
   } else if(alg==SFX_LEAK) {
    float center=cx+side*.14f*sinf((float)(t*.6));
    float dx=(xx-center)/(side*(.20f+size*.8f)),dy=(yy-cy)/(side*1.1f);
    float mask=(1.f-smooth(.1f,1.f+soft,dx*dx+dy*dy))*a*.60f;
    float lr=1.f,lg=.38f,lb=.08f;
    if(v==1){lr=.55f+.45f*sinf(dx*3.f);lg=.5f+.5f*sinf(dx*3.f+2.f);lb=.5f+.5f*sinf(dx*3.f+4.f);}
    c.r+=(255-c.r)*mask*lr;c.g+=(255-c.g)*mask*lg;c.b+=(255-c.b)*mask*lb;
   } else if(alg==SFX_PIXEL) {
    unsigned bx=(x/(unsigned)cell)*(unsigned)cell+(unsigned)cell/2;
    unsigned by=(y/(unsigned)cell)*(unsigned)cell+(unsigned)cell/2;
    bx=bx<s->w?bx:s->w-1;by=by<s->h?by:s->h-1;
    c=load(in,(size_t)by*s->w+bx);
    if(v==1 && cell>2 && (x%(unsigned)cell==0||y%(unsigned)cell==0)){c.r*=.60f;c.g*=.60f;c.b*=.60f;}
    c=blend(original,c,a);
   } else if(alg==SFX_PALETTE) {
    if(v==0) {
     static const float pal[4][3]={{23,41,28},{63,82,45},{135,155,79},{213,225,151}};
     int k=(int)clampf(luma(c)/64.f,0,3);c.r=pal[k][0];c.g=pal[k][1];c.b=pal[k][2];
    } else {
     float levels=2.f+floorf((1.f-size)*6.f),step=255.f/levels;
     float d=v==1?((float)bayer[(y%4)*4+x%4]/16.f-.5f)*step:0;
     c.r=roundf((c.r+d)/step)*step;c.g=roundf((c.g+d)/step)*step;c.b=roundf((c.b+d)/step)*step;
    }
    c=blend(original,c,a);
   } else if(alg==SFX_HALFTONE) {
    unsigned period=3u+(unsigned)(side*(.003f+.018f*size));
    float dx=((float)(x%period)+.5f)/period-.5f,dy=((float)(y%period)+.5f)/period-.5f;
    float dist=sqrtf(dx*dx+dy*dy),l=luma(c)/255.f;
    if(v==0) {float ink=1.f-smooth(.52f*sqrtf(1.f-l)-.08f,.52f*sqrtf(1.f-l)+.04f,dist);c.r=c.g=c.b=248.f*(1.f-ink);}
    else {c.r=255.f*smooth(.53f*sqrtf(1.f-c.r/255.f)-.06f,.53f*sqrtf(1.f-c.r/255.f)+.05f,dist);
          c.g=255.f*smooth(.53f*sqrtf(1.f-c.g/255.f)-.06f,.53f*sqrtf(1.f-c.g/255.f)+.05f,dist);
          c.b=255.f*smooth(.53f*sqrtf(1.f-c.b/255.f)-.06f,.53f*sqrtf(1.f-c.b/255.f)+.05f,dist);}
    c=blend(original,c,a);
   } else if(alg==SFX_EDGE||alg==SFX_EMBOSS) {
    int rad=1+(int)(side*.003f*size);
    float l=luma(c);
    float ll=neighbor_luma(s,in,(int)x-rad,(int)y,l),lr=neighbor_luma(s,in,(int)x+rad,(int)y,l);
    float lu=neighbor_luma(s,in,(int)x,(int)y-rad,l),ld=neighbor_luma(s,in,(int)x,(int)y+rad,l);
    float edge=clampf((fabsf(lr-ll)+fabsf(ld-lu))*1.7f,0,255);
    if(alg==SFX_EMBOSS) {
     float value=128.f+(lr-ll+ld-lu)*1.3f;
     if(v==1)value=255.f-edge*.8f-(sinf((xx+yy)*PI/3.f)>0?l*.13f:0.f);
     c.r=c.g=c.b=value;
    } else switch(v) {
     case 0:c.r=c.g=c.b=255.f-edge*.90f;break;
     case 1:c.r=c.g=c.b=235.f-edge*1.45f+noise(seed^(uint32_t)i)*12.f;break;
     case 2: {
      float e=1.f-smooth(35,110,edge);
      c.r=roundf(c.r/51.f)*51.f*e;c.g=roundf(c.g/51.f)*51.f*e;c.b=roundf(c.b/51.f)*51.f*e;break;}
     case 3:c.r=c.g=c.b=edge>45.f?10.f:245.f;break;
     case 4:c.r=c.g=c.b=l>(70.f+size*130.f)?245.f:15.f;break;
     case 5:c.r=edge*(.30f+c.r/255.f);c.g=edge*(.20f+c.g/255.f);c.b=edge*(.45f+c.b/255.f);break;
    }
    c=blend(original,c,a);
   }
   c.r=clampf(c.r,0,255);c.g=clampf(c.g,0,255);c.b=clampf(c.b,0,255);
   c=blend(original,c,mix);store(out,i,c);
  }
 }
}
EXPORT void f0r_update2(f0r_instance_t instance,double time,const uint32_t *inframe1,
 const uint32_t *inframe2,const uint32_t *inframe3,uint32_t *outframe) {
 (void)inframe2;(void)inframe3;f0r_update(instance,time,inframe1,outframe);
}
