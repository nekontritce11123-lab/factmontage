// SPDX-License-Identifier: GPL-3.0-only
// Compiles ONLY against the real MLT headers. No local ABI lookalikes.
#include <framework/mlt.h>
#include "studio_background/core.hpp"
#include "studio_background/cache.hpp"
#include <filesystem>
#include <memory>
#include <mutex>
#include <cstdio>
#include <cstring>
namespace {
struct ReaderState {
    std::mutex mutex;
    std::string path;
    std::unique_ptr<sbg::CacheReader> reader;
};
struct Snapshot {
    sbg::Params params;
    std::string asset,source,recipe,error;
    std::shared_ptr<ReaderState> state;
    int64_t sample=0;
    int rw=0,rh=0,fpsNum=0,fpsDen=0;
    bool staticImage=false;
};
std::string stringProp(mlt_properties p,const char* key){auto s=mlt_properties_get(p,key);return s?s:"";}
int getImage(mlt_frame frame,uint8_t** image,mlt_image_format* format,int* w,int* h,int){
    auto* s=static_cast<Snapshot*>(mlt_frame_pop_service(frame));
    *format=mlt_image_rgba;
    int error=mlt_frame_get_image(frame,image,format,w,h,1);
    if(error)return error;
    try{
        if(!s)throw std::runtime_error("Missing effect snapshot");
        if(!s->error.empty())throw std::runtime_error(s->error);
        auto fp=MLT_FRAME_PROPERTIES(frame);
        const int trc=mlt_properties_get_int(fp,"color_trc");
        if(trc==16||trc==18)throw std::runtime_error("HDR transfer is not supported; use an SDR project");
        if(*format!=mlt_image_rgba||!*image)throw std::runtime_error("Expected straight RGBA8888 from MLT");
        sbg::Image source(*w,*h);std::memcpy(source.rgba.data(),*image,source.rgba.size());
        sbg::Mask matte;
        if(s->params.method==1){
            if(s->sample<0)throw std::runtime_error("Invalid sample offset");
            std::lock_guard<std::mutex> lock(s->state->mutex);
            if(!s->state->reader||s->state->path!=s->asset){
                // The file must be immutable. Never replace it at the same path.
                auto r=std::make_unique<sbg::CacheReader>(s->asset);
                s->state->reader=std::move(r);s->state->path=s->asset;
            }
            s->state->reader->requireIdentity(s->source,s->recipe,s->fpsNum,s->fpsDen);
            const auto& meta=s->state->reader->info();
            if(s->rw!=int(meta.referenceW)||s->rh!=int(meta.referenceH))throw std::runtime_error("Project resolution changed; re-analyze");
            matte=s->state->reader->readSample(s->staticImage?meta.firstSample:uint64_t(s->sample));
        }
        auto out=sbg::render(source,s->params,s->params.method==1?&matte:nullptr,s->rw,s->rh);
        std::memcpy(*image,out.rgba.data(),out.rgba.size());
        return 0;
    }catch(const std::exception& e){
        // Fail CLOSED: missing matte must not expose a private background on export.
        // Also fail preflight in the host; some consumers ignore per-frame errors.
        if(*format==mlt_image_rgba&&*image&&*w>0&&*h>0&&uint64_t(*w)*(*h)<=sbg::MaxPixels){
            for(int y=0;y<*h;++y)for(int x=0;x<*w;++x){auto i=(std::size_t(y)*(*w)+x)*4;(*image)[i]=(y<12?180:0);(*image)[i+1]=0;(*image)[i+2]=0;(*image)[i+3]=255;}
        }
        mlt_properties_set(MLT_FRAME_PROPERTIES(frame),"studio.background.error",e.what());
        std::fprintf(stderr,"studio.background: %s\n",e.what());
        // mlt_frame_get_image replaces *any* nonzero callback result with its
        // test image (white when test_audio=1). Return the safe diagnostic image,
        // NOT the original source. The error property and host export preflight
        // remain authoritative: this is a displayed error, not a successful matte.
        return (*format==mlt_image_rgba&&*image&&*w>0&&*h>0
                &&uint64_t(*w)*(*h)<=sbg::MaxPixels) ? 0 : 1;
    }
}
mlt_frame process(mlt_filter filter,mlt_frame frame){
    auto* snapshot=new Snapshot;
    try{
        auto properties=MLT_FILTER_PROPERTIES(filter);
        auto ptr=static_cast<std::shared_ptr<ReaderState>*>(mlt_properties_get_data(properties,"_sbg.state",nullptr));
        if(!ptr)throw std::runtime_error("Uninitialized mask reader");snapshot->state=*ptr;
        // Capture controls now, not after downstream work or another GUI mutation.
        snapshot->params.forEach([&](const char* key,double fallback){
            const double value=mlt_properties_get(properties,key)?mlt_properties_get_double(properties,key):fallback;
            snapshot->params.set(key,value);
        });
        // MLT position is filter-relative, not source-relative. Restore the
        // filter in before converting source time to immutable mask samples.
        snapshot->sample=int64_t(mlt_filter_get_position(filter,frame))+int64_t(mlt_filter_get_in(filter))
            -mlt_properties_get_int64(properties,"_sbg_clip_in")
            +mlt_properties_get_int64(properties,"sample_offset");
        snapshot->source=stringProp(properties,"source_sha256");snapshot->recipe=stringProp(properties,"recipe_sha256");
        snapshot->asset=stringProp(properties,"_sbg_mask_path");
        snapshot->staticImage=mlt_properties_get_int(properties,"static_image")!=0;
        auto profile=mlt_service_profile(MLT_FILTER_SERVICE(filter));
        if(!profile)throw std::runtime_error("Missing MLT project profile");
        snapshot->rw=profile->width;snapshot->rh=profile->height;snapshot->fpsNum=profile->frame_rate_num;snapshot->fpsDen=profile->frame_rate_den;
        if(snapshot->params.method==1){
            std::filesystem::path path(snapshot->asset);
            if(path.empty()||!path.is_absolute())throw std::runtime_error("Host must resolve mask_asset into _sbg_mask_path for runtime");
        }
    }catch(const std::exception& e){snapshot->error=e.what();}
    // Frame owns the snapshot even when nobody requests its image.
    auto owned=mlt_frame_unique_properties(frame,MLT_FILTER_SERVICE(filter));
    mlt_properties_set_data(owned,"sbg.snapshot",snapshot,0,[](void* p){delete static_cast<Snapshot*>(p);},nullptr);
    mlt_frame_push_service(frame,snapshot);mlt_frame_push_get_image(frame,getImage);return frame;
}
void* makeFilter(mlt_profile,mlt_service_type,const char*,const void*){
    auto f=mlt_filter_new();if(!f)return nullptr;
    try{
        auto p=MLT_FILTER_PROPERTIES(f);
        auto state=new std::shared_ptr<ReaderState>(std::make_shared<ReaderState>());
        mlt_properties_set_data(p,"_sbg.state",state,0,[](void* v){delete static_cast<std::shared_ptr<ReaderState>*>(v);},nullptr);
        sbg::Params{}.forEach([&](const char* key,double value){mlt_properties_set_double(p,key,value);});
        mlt_properties_set_int64(p,"sample_offset",0);f->process=process;return f;
    }catch(...){mlt_filter_close(f);return nullptr;}
}
mlt_properties metadata(mlt_service_type,const char*,void*){
    auto p=mlt_properties_new();mlt_properties_set(p,"title","Studio Background (experimental)");
    mlt_properties_set(p,"identifier","studio.background");mlt_properties_set(p,"description","SDR chroma key / precomputed human matte / background blur");
    mlt_properties_set(p,"version","1.0.0");mlt_properties_set(p,"license","GPL-3.0-only");return p;
}
}
extern "C" MLT_REPOSITORY {
    MLT_REGISTER(mlt_service_filter_type,"studio.background",makeFilter);
    MLT_REGISTER_METADATA(mlt_service_filter_type,"studio.background",metadata,nullptr);
}
