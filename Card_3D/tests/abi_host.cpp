// Compile against the official/system Frei0r header, NOT src/frei0r_minimal.h.
#include <frei0r.h>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

static_assert(sizeof(f0r_param_color_t)==12, "Official color ABI changed");
static_assert(offsetof(f0r_param_color_t,b)==8, "Official color offsets changed");

int main(int argc,char** argv){
    assert(argc==2);
#ifdef _WIN32
    auto library=LoadLibraryA(argv[1]);assert(library);
    auto symbol=[&](const char* name){auto p=GetProcAddress(library,name);assert(p);return p;};
#else
    auto library=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
    if(!library){std::fprintf(stderr,"%s\n",dlerror());return 1;}
    auto symbol=[&](const char* name){auto p=dlsym(library,name);assert(p);return p;};
#endif
    auto init=reinterpret_cast<decltype(&f0r_init)>(symbol("f0r_init"));
    auto deinit=reinterpret_cast<decltype(&f0r_deinit)>(symbol("f0r_deinit"));
    auto info=reinterpret_cast<decltype(&f0r_get_plugin_info)>(symbol("f0r_get_plugin_info"));
    auto param=reinterpret_cast<decltype(&f0r_get_param_info)>(symbol("f0r_get_param_info"));
    auto construct=reinterpret_cast<decltype(&f0r_construct)>(symbol("f0r_construct"));
    auto destruct=reinterpret_cast<decltype(&f0r_destruct)>(symbol("f0r_destruct"));
    auto get=reinterpret_cast<decltype(&f0r_get_param_value)>(symbol("f0r_get_param_value"));
    auto set=reinterpret_cast<decltype(&f0r_set_param_value)>(symbol("f0r_set_param_value"));
    auto update=reinterpret_cast<decltype(&f0r_update)>(symbol("f0r_update"));
    assert(init());f0r_plugin_info_t metadata{};info(&metadata);
    assert(metadata.num_params==18&&metadata.plugin_type==F0R_PLUGIN_TYPE_FILTER);
    assert(metadata.color_model==F0R_COLOR_MODEL_RGBA8888&&metadata.frei0r_version==1);
    assert(std::strcmp(metadata.name,"FactMontage Cards")==0);
    auto instance=construct(64,64);assert(instance);
    const char* names[]={"STYLE","ENTRANCE","CORNER","PLACEMENT","SIZE","DURATION","MOTION","SOURCE","ROUNDING","BACKGROUND","SHADOW","POSITION_MODE","X","Y","EXIT_ENABLED","EXIT_ANIMATION","EXIT_DURATION","EXIT_AT"};
    const double defaults[]={0,.25,1./3,1./6,.68,.34,.5,0,.2,0,.35,0,0,.5,1,0,.34,0};
    for(int n=0;n<18;n++){
        f0r_param_info_t description{};param(&description,n);
        assert(description.type==F0R_PARAM_DOUBLE&&std::strcmp(description.name,names[n])==0);
        struct Guard { uint64_t before; double value; uint64_t after; } out{0x12345678,0,0x87654321};
        get(instance,&out.value,n);
        assert(out.before==0x12345678&&out.after==0x87654321);
        assert(std::abs(out.value-defaults[n])<1e-12);
        set(instance,&out.value,n);
    }
    std::vector<uint32_t> input(64*64,0xff2040e0),output(64*64+8,0x12345678);
    update(instance,0,input.data(),output.data()+4);
    for(int i=4;i<64*64+4;i++)assert(output[i]==0);
    update(instance,2,input.data(),output.data()+4);
    bool visible=false;for(int i=4;i<64*64+4;i++)visible|=output[i]!=0;
    assert(visible);
    for(int i=0;i<4;i++)assert(output[i]==0x12345678&&output[64*64+4+i]==0x12345678);
    destruct(instance);deinit();
#ifdef _WIN32
    FreeLibrary(library);
#else
    dlclose(library);
#endif
    std::puts("Official Frei0r header: metadata, 18 parameters, guarded values and render PASS");
}
