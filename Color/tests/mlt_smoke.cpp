// SPDX-License-Identifier: GPL-3.0-or-later
// Only built with STUDIO_COLOR_MLT=ON against actual public MLT headers/libraries.
extern "C" {
#include <framework/mlt.h>
}
#include "studio_color/color.hpp"
#include <iostream>
#include <cstring>
#include <vector>
int main(int argc,char**argv){
    if(argc!=2)return 2;
    mlt_factory_init(argv[1]);auto profile=mlt_profile_init(nullptr);
    auto filter=mlt_factory_filter(profile,"studio.color",nullptr);
    if(!filter){std::cerr<<"Actual MLT failed to load studio.color\n";return 1;}
    auto props=MLT_FILTER_PROPERTIES(filter);
    mlt_properties_set_int(props,"studio_input_validated",1);
    mlt_properties_set(props,"studio_color_algorithm","sdr-primary-v1");
    mlt_properties_set_double(props,"exposure",.3);
    constexpr int w=65,h=253,bytes=w*h*4;
    auto *buffer=static_cast<uint8_t*>(mlt_pool_alloc(bytes));
    for(int i=0;i<bytes;++i)buffer[i]=static_cast<uint8_t>((i*31+15)%256);
    std::vector<uint8_t> reference(buffer,buffer+bytes);studio_color::Params p;p.exposure=.3;
    studio_color::Lut(p).process(reference.data(),reference.data(),w,h);
    auto frame=mlt_frame_init(nullptr);
    mlt_frame_set_image(frame,buffer,bytes,mlt_pool_release);
    mlt_properties_set_int(MLT_FRAME_PROPERTIES(frame),"width",w);
    mlt_properties_set_int(MLT_FRAME_PROPERTIES(frame),"height",h);
    mlt_properties_set_int(MLT_FRAME_PROPERTIES(frame),"format",mlt_image_rgba);
    mlt_filter_process(filter,frame);
    auto format=mlt_image_rgba;int width=w,height=h;uint8_t *actual=nullptr;
    const int error=mlt_frame_get_image(frame,&actual,&format,&width,&height,0);
    const bool ok=!error&&format==mlt_image_rgba&&width==w&&height==h&&actual&&std::memcmp(actual,reference.data(),bytes)==0;
    mlt_frame_close(frame);mlt_filter_close(filter);mlt_profile_close(profile);mlt_factory_close();
    std::cout<<(ok?"Actual MLT adapter: PASS\n":"Actual MLT adapter: FAIL\n");return ok?0:1;
}
