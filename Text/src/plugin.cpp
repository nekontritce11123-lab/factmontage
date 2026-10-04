// SPDX-License-Identifier: MIT
// Stable frei0r C ABI. No Qt, Python, font engine or GPU dependency in the plugin.
#include "engine.hpp"
#include <chrono>
#include <cstdio>
#include <mutex>
#include <map>
#include <new>
#include <sys/stat.h>
#if defined(__GNUC__)
#define API extern "C" __attribute__((visibility("default")))
#else
#define API extern "C"
#endif
struct f0r_plugin_info_t {const char*name;const char*author;int plugin_type,color_model,frei0r_version,major_version,minor_version,num_params;const char*explanation;};
struct f0r_param_info_t {const char*name;int type;const char*explanation;};
struct Instance {unsigned w,h;sunimo::Renderer renderer;std::string path,error;double duration=0,offset=.5;std::mutex mutex;std::chrono::steady_clock::time_point check{};int64_t stamp=0;Instance(unsigned x,unsigned y):w(x),h(y){}};
// MLT uses one instance per render worker. Immutable glyph/blur textures are shared.
// Weak references ensure closed projects release scene memory without an LRU leak.
static std::shared_ptr<sunimo::Scene> cached_scene(const std::string&path,int64_t stamp){
    static std::mutex mutex;
    static std::map<std::pair<std::string,int64_t>,std::weak_ptr<sunimo::Scene>> scenes;
    std::lock_guard<std::mutex> lock(mutex);
    const auto key=std::make_pair(path,stamp);
    auto it=scenes.find(key);
    if(it!=scenes.end())if(auto scene=it->second.lock())return scene;
    if(scenes.size()>64){
        for(auto p=scenes.begin();p!=scenes.end();){
            if(p->second.expired())p=scenes.erase(p);else ++p;
        }
    }
    auto scene=sunimo::Scene::file(path);scenes[key]=scene;return scene;
}
static void error(Instance*i,const std::string&e){if(i->error!=e)std::fprintf(stderr,"FactMontage Text: %s\n",e.c_str());i->error=e;}
API int f0r_init(){return 1;}
API void f0r_deinit(){}
API void f0r_get_plugin_info(f0r_plugin_info_t*i){if(i)*i={"FactMontage Text","SUNIMO",0,1,1,0,2,3,"100 transitions + 100 matching life profiles. Create a .stxt in the local editor; deterministic CPU rendering."};}
API void f0r_get_param_info(f0r_param_info_t*i,int p){if(!i)return;static const f0r_param_info_t infos[]={{"Scene",4,"Absolute path to a .stxt scene"},{"Duration",1,"0 uses scene duration; otherwise value times 120 seconds"},{"Time offset",1,"0.5 = zero; range -60 to +60 seconds"}};*i=infos[std::max(0,std::min(2,p))];}
API void* f0r_construct(unsigned w,unsigned h){try{if(w<1||h<1||w>7680||h>7680||uint64_t(w)*h>33554432)return nullptr;return new Instance(w,h);}catch(...){return nullptr;}}
API void f0r_destruct(void*p){delete static_cast<Instance*>(p);}
API void f0r_set_param_value(void*p,void*v,int n){if(!p||!v)return;auto*i=static_cast<Instance*>(p);try{std::lock_guard<std::mutex>l(i->mutex);
    if(n==0){const char*s=*static_cast<char**>(v);std::string path=s?s:"";if(path.size()>8192)throw std::runtime_error("Scene path too long");if(i->path!=path){i->path=path;i->check={};i->stamp=0;i->renderer.set(nullptr);}}
    else {double d=*static_cast<double*>(v);if(!std::isfinite(d))throw std::runtime_error("Non-finite host parameter");if(n==1)i->duration=sunimo::clamp(d);if(n==2)i->offset=sunimo::clamp(d);}
}catch(const std::exception&e){error(i,e.what());}catch(...){error(i,"Parameter update failed");}}
API void f0r_get_param_value(void*p,void*v,int n){if(!p||!v)return;auto*i=static_cast<Instance*>(p);std::lock_guard<std::mutex>l(i->mutex);if(n==0)*static_cast<const char**>(v)=i->path.c_str();if(n==1)*static_cast<double*>(v)=i->duration;if(n==2)*static_cast<double*>(v)=i->offset;}
API void f0r_update(void*p,double time,const uint32_t*in,uint32_t*out){if(!p||!out)return;auto*i=static_cast<Instance*>(p);std::lock_guard<std::mutex>l(i->mutex);try{
    auto now=std::chrono::steady_clock::now();if(!i->path.empty()&&now-i->check>std::chrono::milliseconds(500)){
        i->check=now;struct stat st{};if(stat(i->path.c_str(),&st))throw std::runtime_error("Scene file unavailable: "+i->path);
        int64_t stamp=int64_t(st.st_mtim.tv_sec)*1000000000LL+st.st_mtim.tv_nsec;
        if(stamp!=i->stamp||!i->renderer.scene){auto s=cached_scene(i->path,stamp);i->renderer.set(std::move(s));i->stamp=stamp;i->error.clear();}
    }
    if(!std::isfinite(time))throw std::runtime_error("Non-finite host time");
    i->renderer.render(time+(i->offset-.5)*120,i->w,i->h,reinterpret_cast<const uint8_t*>(in),reinterpret_cast<uint8_t*>(out),i->duration*120);
}catch(const std::exception&e){error(i,e.what());if(in&&in!=out)std::memcpy(out,in,size_t(i->w)*i->h*4);else if(!in)std::memset(out,0,size_t(i->w)*i->h*4);}catch(...){error(i,"Frame rendering failed");if(in&&in!=out)std::memcpy(out,in,size_t(i->w)*i->h*4);else if(!in)std::memset(out,0,size_t(i->w)*i->h*4);}}
// Small C API for embedders, tests and the local preview. Same renderer, no browser simulation.
API void* smt_create(){try{return new Instance(1,1);}catch(...){return nullptr;}}
API void smt_destroy(void*p){f0r_destruct(p);}
API int smt_load(void*p,const void*bytes,size_t n){if(!p||(!bytes&&n))return 0;auto*i=static_cast<Instance*>(p);try{auto s=sunimo::Scene::load(bytes,n);std::lock_guard<std::mutex>l(i->mutex);i->renderer.set(std::move(s));i->error.clear();return 1;}catch(const std::exception&e){error(i,e.what());return 0;}catch(...){error(i,"Scene load failed");return 0;}}
API int smt_render(void*p,double t,unsigned w,unsigned h,const void*in,void*out){if(!p||!out)return 0;auto*i=static_cast<Instance*>(p);try{std::lock_guard<std::mutex>l(i->mutex);if(!std::isfinite(t))throw std::runtime_error("Non-finite time");i->renderer.render(t,w,h,static_cast<const uint8_t*>(in),static_cast<uint8_t*>(out));return 1;}catch(const std::exception&e){error(i,e.what());return 0;}catch(...){error(i,"Render failed");return 0;}}
API const char* smt_last_error(void*p){return p?static_cast<Instance*>(p)->error.c_str():"Null instance";}
