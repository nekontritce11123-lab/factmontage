/* Independent C host using Frei0r API. Does not include or link engine source. */
#include "frei0r.h"
#include "sfx_contract.h"
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if(!(x)) {fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);exit(2);} checks++; } while(0)
static unsigned checks=0;
static uint32_t rng=12;
static uint32_t random32(void){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
int main(int argc,char **argv) {
 CHECK(argc==2);void *lib=dlopen(argv[1],RTLD_NOW);if(!lib){fprintf(stderr,"%s\n",dlerror());return 2;}
 #define FN(type,name,args) type (*name) args = (type (*) args)dlsym(lib,#name);CHECK(name)
 FN(int,f0r_init,(void));FN(void,f0r_deinit,(void));FN(void,f0r_get_plugin_info,(f0r_plugin_info_t*));
 FN(void,f0r_get_param_info,(f0r_param_info_t*,int));FN(f0r_instance_t,f0r_construct,(unsigned,unsigned));
 FN(void,f0r_destruct,(f0r_instance_t));FN(void,f0r_set_param_value,(f0r_instance_t,f0r_param_t,int));
 FN(void,f0r_get_param_value,(f0r_instance_t,f0r_param_t,int));
 FN(void,f0r_update,(f0r_instance_t,double,const uint32_t*,uint32_t*));
 FN(void,f0r_update2,(f0r_instance_t,double,const uint32_t*,const uint32_t*,const uint32_t*,uint32_t*));
 CHECK(sizeof(f0r_param_color_t)==12);CHECK(sizeof(f0r_param_double)==8);CHECK(f0r_init()==1);
 f0r_plugin_info_t info;f0r_get_plugin_info(&info);CHECK(info.num_params==11);CHECK(info.color_model==F0R_COLOR_MODEL_RGBA8888);CHECK(info.plugin_type==0);
 CHECK(!f0r_construct(0,16));CHECK(!f0r_construct(16,0));CHECK(!f0r_construct(UINT32_MAX,UINT32_MAX));
 const unsigned widths[]={8,64,128,24,320},heights[]={8,48,72,128,180};
 for(unsigned z=0;z<5;z++) {
  unsigned w=widths[z],h=heights[z];size_t n=(size_t)w*h;
  uint32_t *in=malloc(n*4),*a=malloc(n*4),*b=malloc(n*4),*copy=malloc(n*4);CHECK(in&&a&&b&&copy);
  for(size_t i=0;i<n;i++)in[i]=random32();
  memcpy(copy,in,n*4);
  f0r_instance_t inst=f0r_construct(w,h);CHECK(inst);
  for(int slot=0;slot<80;slot++) {
   for(int p=0;p<SFX_PARAM_COUNT;p++){double value=sfx_recipes[slot].defaults[p];f0r_set_param_value(inst,&value,p);double got=-1;f0r_get_param_value(inst,&got,p);CHECK(got==value);}
   f0r_update(inst,2.375,in,a);f0r_update(inst,7.9,in,b);f0r_update(inst,2.375,in,b);
   CHECK(!memcmp(a,b,n*4));CHECK(!memcmp(in,copy,n*4));
   for(int p=0;p<SFX_PARAM_COUNT;p++){double got=-1;f0r_get_param_value(inst,&got,p);CHECK(got==sfx_recipes[slot].defaults[p]);}
   memcpy(b,in,n*4);f0r_update(inst,2.375,b,b);CHECK(!memcmp(a,b,n*4));
   f0r_update2(inst,2.375,in,NULL,NULL,b);CHECK(!memcmp(a,b,n*4));
   double zero=0;f0r_set_param_value(inst,&zero,4);f0r_update(inst,2,in,b);CHECK(!memcmp(in,b,n*4));
   double one=1;f0r_set_param_value(inst,&one,4);f0r_set_param_value(inst,&zero,1);f0r_update(inst,2,in,b);CHECK(!memcmp(in,b,n*4));
   for(int p=0;p<SFX_PARAM_COUNT;p++){double value=(p%2)?INFINITY:NAN;f0r_set_param_value(inst,&value,p);double got=-1;f0r_get_param_value(inst,&got,p);CHECK(isfinite(got)&&got>=0&&got<=1);}
   f0r_update(inst,NAN,in,b);f0r_update(inst,-INFINITY,in,b);
   for(int boundary=0;boundary<2;boundary++) {
    for(int p=1;p<SFX_PARAM_COUNT;p++){double val=(double)boundary;f0r_set_param_value(inst,&val,p);}
    double sel=slot/127.;f0r_set_param_value(inst,&sel,0);f0r_update(inst,1.e300,in,b);
   }
  }
  f0r_destruct(inst);free(in);free(a);free(b);free(copy);
 }
 {
  const unsigned w=64,h=48;const size_t n=(size_t)w*h;
  uint32_t *in=malloc(n*4),*a=malloc(n*4),*b=malloc(n*4);CHECK(in&&a&&b);
  for(size_t i=0;i<n;i++)in[i]=random32();
  f0r_instance_t inst=f0r_construct(w,h);CHECK(inst);
  for(int p=0;p<SFX_PARAM_COUNT;p++){double value=sfx_recipes[10].defaults[p];f0r_set_param_value(inst,&value,p);}
  f0r_update(inst,5.,in,a);
  double offset=5./86400.;f0r_set_param_value(inst,&offset,10);f0r_update(inst,0.,in,b);
  CHECK(!memcmp(a,b,n*4));
  f0r_destruct(inst);free(in);free(a);free(b);
 }
 for(int i=0;i<SFX_PARAM_COUNT;i++){f0r_param_info_t p;f0r_get_param_info(&p,i);CHECK(p.name&&p.type==F0R_PARAM_DOUBLE);}
 f0r_deinit();dlclose(lib);printf("PASS: %u ABI/render assertions; 80 recipes x 5 resolutions; alias, seek, zero, boundaries, NaN/Inf\n",checks);return 0;
}
