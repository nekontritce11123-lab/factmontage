// SPDX-License-Identifier: GPL-3.0-only
// Real MLT callback stack, not an ABI stub. Same production filter implementation.
#include "../mlt/filter_background.cpp"
#include <cassert>
#include <chrono>
#include <iostream>
#include <unistd.h>

static mlt_frame sourceFrame(int w,int h)
{
    auto frame=mlt_frame_init(nullptr); assert(frame);
    auto pixels=static_cast<uint8_t*>(mlt_pool_alloc(w*h*4));assert(pixels);
    for(int i=0;i<w*h;++i){pixels[i*4]=231;pixels[i*4+1]=20;pixels[i*4+2]=40;pixels[i*4+3]=255;}
    mlt_frame_set_image(frame,pixels,w*h*4,mlt_pool_release);
    auto props=MLT_FRAME_PROPERTIES(frame);
    mlt_properties_set_int(props,"width",w);mlt_properties_set_int(props,"height",h);
    mlt_properties_set_int(props,"format",mlt_image_rgba);
    // This is the exact MLT condition that turns a callback error into WHITE.
    mlt_properties_set_int(props,"test_audio",1);
    mlt_frame_set_position(frame,0);
    return frame;
}
int main()
{
    mlt_pool_init();
    auto profile=mlt_profile_init(nullptr);assert(profile);
    profile->width=64;profile->height=64;profile->frame_rate_num=25;profile->frame_rate_den=1;
    auto filter=static_cast<mlt_filter>(makeFilter(profile,mlt_service_filter_type,"studio.background",nullptr));assert(filter);
    mlt_service_set_profile(MLT_FILTER_SERVICE(filter),profile);
    auto props=MLT_FILTER_PROPERTIES(filter);
    // The factory assigns this when loading the filter in Kdenlive.
    mlt_properties_set(props,"_unique_id","studio-background-test");
    mlt_properties_set_int(props,"method",1);
    mlt_properties_set_double(props,"feather",0);
    mlt_properties_set_double(props,"refine",0);
    mlt_properties_set_double(props,"despill",0);
    const auto root=std::filesystem::temp_directory_path()/("studio-mlt-regression-"+std::to_string(getpid())+"-"
        +std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(root);
    auto path=(root/"маска с пробелами.sbg").string();
    mlt_properties_set(props,"_sbg_mask_path",path.c_str());
    mlt_properties_set(props,"source_sha256",std::string(64,'a').c_str());
    mlt_properties_set(props,"recipe_sha256",std::string(64,'b').c_str());
    {
        auto frame=sourceFrame(64,64);process(filter,frame);
        auto format=mlt_image_rgba;int w=64,h=64;uint8_t* pixels=nullptr;
        assert(mlt_frame_get_image(frame,&pixels,&format,&w,&h,1)==0);
        assert(pixels && format==mlt_image_rgba);
        assert(mlt_properties_get(MLT_FRAME_PROPERTIES(frame),"studio.background.error"));
        // Error must stay an opaque diagnostic, not white, transparent or original RGB.
        assert(pixels[0]==180 && pixels[1]==0 && pixels[2]==0 && pixels[3]==255);
        const int center=(32*64+32)*4;
        assert(pixels[center]==0 && pixels[center+1]==0 && pixels[center+2]==0 && pixels[center+3]==255);
        mlt_frame_close(frame);
    }
    sbg::CacheInfo info;info.width=64;info.height=64;info.frames=1;info.referenceW=64;info.referenceH=64;
    info.fpsNum=25;info.fpsDen=1;info.sourceSha256=std::string(64,'a');info.recipeSha256=std::string(64,'b');
    sbg::Mask mask(64,64);
    for(int y=0;y<64;++y)for(int x=32;x<64;++x)mask.alpha[y*64+x]=1;
    {sbg::CacheWriter writer(path,info);writer.append(mask);writer.finish();}
    for(int size:{64,32,16,64}) {
        auto frame=sourceFrame(size,size);process(filter,frame);
        auto format=mlt_image_rgba;int w=size,h=size;uint8_t* pixels=nullptr;
        assert(mlt_frame_get_image(frame,&pixels,&format,&w,&h,1)==0);
        assert(!mlt_properties_get(MLT_FRAME_PROPERTIES(frame),"studio.background.error"));
        const int foreground=(size/2*size+size*3/4)*4;
        const int background=(size/2*size+size/4)*4;
        assert(pixels[foreground]==231 && pixels[foreground+3]==255);
        assert(pixels[background+3]==0);
        // Alpha contract against a known green lower layer (not a GUI compositor test).
        const int alpha=pixels[background+3];
        assert((pixels[background+1]*alpha+255*(255-alpha))/255==255);
        mlt_frame_close(frame);
    }
    // A trimmed timeline entry uses an absolute source frame, while its mask
    // starts at sample zero. The runtime clip in must cancel that source offset.
    mlt_filter_set_in_and_out(filter,0,2000);
    mlt_properties_set_int(props,"_sbg_clip_in",1325);
    {
        auto frame=sourceFrame(64,64);
        mlt_frame_set_position(frame,1325);
        mlt_properties_set_int(MLT_FRAME_PROPERTIES(frame),"pos.studio-background-test",1325);
        process(filter,frame);
        auto format=mlt_image_rgba;int w=64,h=64;uint8_t* pixels=nullptr;
        assert(mlt_frame_get_image(frame,&pixels,&format,&w,&h,1)==0);
        assert(!mlt_properties_get(MLT_FRAME_PROPERTIES(frame),"studio.background.error"));
        assert(pixels[(32*64+48)*4]==231);
        assert(pixels[(32*64+16)*4+3]==0);
        mlt_frame_close(frame);
    }
    // Distinct samples prove motion/seek, rather than just a one-frame matte.
    const auto movingPath=(root/"moving.sbg").string();
    info.frames=3;
    {
        sbg::CacheWriter writer(movingPath,info);
        for(int sample=0;sample<3;++sample){
            sbg::Mask moving(64,64);
            for(int y=0;y<64;++y)for(int x=sample*16;x<sample*16+16;++x) moving.alpha[y*64+x]=1;
            writer.append(moving);
        }
        writer.finish();
    }
    mlt_properties_set(props,"_sbg_mask_path",movingPath.c_str());
    // A filter zone and a clip in are different clocks. Test both a whole-source
    // filter and a nonzero filter range, plus a trimmed/split mask offset.
    for(int filterIn:{0,1325})for(int offset:{0,1}) {
        mlt_filter_set_in_and_out(filter,filterIn,2000);
        mlt_properties_set_int(props,"_sbg_clip_in",1325+offset);
        mlt_properties_set_int(props,"sample_offset",offset);
        for(int sample:{2,1,2,0,1}) {
            if(sample<offset)continue;
            auto frame=sourceFrame(64,64);
            mlt_frame_set_position(frame,1325+sample);
            // Use the real dispatcher: it sets pos.<filter-id> before process().
            mlt_filter_process(filter,frame);
            // A downstream tractor may now replace the frame's timeline position.
            mlt_frame_set_position(frame,9000+sample);
            auto format=mlt_image_rgba;int w=64,h=64;uint8_t* pixels=nullptr;
            assert(mlt_frame_get_image(frame,&pixels,&format,&w,&h,1)==0);
            const auto failure=mlt_properties_get(MLT_FRAME_PROPERTIES(frame),"studio.background.error");
            if(failure)std::cerr<<"filterIn="<<filterIn<<" offset="<<offset<<" sample="<<sample<<": "<<failure<<'\n';
            assert(!failure);
            for(int band=0;band<3;++band) {
                const int alpha=pixels[(32*64+band*16+8)*4+3];
                assert(alpha==(band==sample?255:0));
            }
            mlt_frame_close(frame);
        }
    }
    mlt_filter_close(filter);mlt_profile_close(profile);mlt_pool_close();
    std::filesystem::remove_all(root);
    std::cout<<"Real MLT: missing matte stays diagnostic; known matte foreground/alpha and reduced preview PASS\n";
}
