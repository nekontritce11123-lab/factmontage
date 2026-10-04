/* Independent ABI smoke host for specialized Frei0r filters. */
#include "frei0r.h"
#include <dlfcn.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);return 2;}}while(0)
int main(int argc,char **argv){
 CHECK(argc==3);int expected=atoi(argv[2]);void *lib=dlopen(argv[1],RTLD_NOW);CHECK(lib);
 #define FN(type,name,args) type (*name) args=(type (*) args)dlsym(lib,#name);CHECK(name)
 FN(int,f0r_init,(void));FN(void,f0r_deinit,(void));FN(void,f0r_get_plugin_info,(f0r_plugin_info_t*));FN(void,f0r_get_param_info,(f0r_param_info_t*,int));FN(f0r_instance_t,f0r_construct,(unsigned,unsigned));FN(void,f0r_destruct,(f0r_instance_t));FN(void,f0r_set_param_value,(f0r_instance_t,f0r_param_t,int));FN(void,f0r_get_param_value,(f0r_instance_t,f0r_param_t,int));FN(void,f0r_update,(f0r_instance_t,double,const uint32_t*,uint32_t*));
 CHECK(f0r_init()==1);f0r_plugin_info_t info;f0r_get_plugin_info(&info);CHECK(info.num_params==expected);CHECK(info.color_model==F0R_COLOR_MODEL_RGBA8888);
 size_t n=96u*64u;uint32_t *in=malloc(n*4),*a=malloc(n*4),*b=malloc(n*4);CHECK(in&&a&&b);for(size_t i=0;i<n;i++)in[i]=(uint32_t)(i*UINT32_C(2654435761));
 f0r_instance_t inst=f0r_construct(96,64);CHECK(inst);for(int i=0;i<expected;i++){f0r_param_info_t param;f0r_get_param_info(&param,i);CHECK(param.name&&param.type==F0R_PARAM_DOUBLE);double v=(i+1.)/(expected+1.);f0r_set_param_value(inst,&v,i);double got=-1;f0r_get_param_value(inst,&got,i);CHECK(got==v);}
 f0r_update(inst,3.25,in,a);f0r_update(inst,3.25,in,b);CHECK(!memcmp(a,b,n*4));memcpy(b,in,n*4);f0r_update(inst,3.25,b,b);CHECK(!memcmp(a,b,n*4));for(size_t i=0;i<n;i++)CHECK(((unsigned char *)(a+i))[3]==((unsigned char *)(in+i))[3]);
 f0r_destruct(inst);f0r_deinit();dlclose(lib);free(in);free(a);free(b);puts("PASS specialized ABI/render");return 0;
}
