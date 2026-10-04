#define CARD3D_TEST_API
#include "../src/card_studio.cpp"
#include <cstdio>
#include <cassert>
#include <cstddef>
int main(){
    static_assert(sizeof(f0r_param_color_t)==12);
    static_assert(offsetof(f0r_param_color_t,b)==8);
    uint32_t rnd=17561;int frames=0;
    for(int batch=0;batch<12;batch++){
        unsigned w=31+batch*7,h=17+batch*5;
        auto* s=f0r_construct(w,h);if(!s)return 1;
        std::vector<uint32_t> input(w*h,0xff80a0e0),guard(w*h+16,0x12345678);
        for(int n=0;n<30;n++){
            for(int k=0;k<PUBLIC_PARAMS;k++){
                rnd=hash32(rnd);double v=(double)rnd/4294967295.;f0r_set_param_value(s,&v,k);
            }
            // Exercise the private renderer controls as well as public presets.
            if(n%2==0)for(int k=0;k<PARAMS;k++)if(SPECS[k].type==F0R_PARAM_DOUBLE){
                rnd=hash32(rnd);double v=(double)rnd/4294967295.;card3d_test_set(s,&v,k);
            }
            for(int k:{FRAME_COLOR,SIDE_COLOR}){
                // A 12-byte stack object makes an erroneous 24-byte read visible to ASan.
                f0r_param_color_t color{.25f,.5f,.75f};card3d_test_set(s,&color,k);
                struct Guard { uint32_t before; f0r_param_color_t color; uint32_t after; } result{0x12345678,{},0x87654321};
                card3d_test_get(s,&result.color,k);
                assert(result.before==0x12345678&&result.after==0x87654321);
                assert(result.color.r==.25f&&result.color.g==.5f&&result.color.b==.75f);
            }
            if(n%7==0)std::fill(input.begin(),input.end(),0);
            else std::fill(input.begin(),input.end(),0xff80a0e0);
            f0r_update(s,n*.13,input.data(),guard.data()+8);
            for(int z=0;z<8;z++)if(guard[z]!=0x12345678||guard[w*h+8+z]!=0x12345678)return 2;
            f0r_update(s,n*.13,input.data(),input.data());frames+=2;
        }
        f0r_update(s,2,nullptr,guard.data()+8);frames++;
        f0r_set_param_value(s,nullptr,-1);f0r_destruct(s);
    }
    printf("%d update calls; parameter/color/frame guards: PASS (sanitizers depend on build flags)\n",frames);
}
