// SPDX-License-Identifier: MIT
// SUNIMO Motion 0.2.0 — standalone native renderer / companion.
#include "transition.hpp"
#include <cstring>
#include <stdexcept>
#if defined(__SSE2__) && !defined(TRANSITIONS_SCALAR_REFERENCE)
#include <emmintrin.h>
#endif
namespace sunimo {
namespace {
struct Pixel {float r=0,g=0,b=0,a=0;}; // premultiplied, all channels 0..255
inline Pixel operator+(Pixel a,Pixel b){return{a.r+b.r,a.g+b.g,a.b+b.b,a.a+b.a};}
inline Pixel operator*(Pixel a,float s){return{a.r*s,a.g*s,a.b*s,a.a*s};}
inline float reflect(float v,int n){if(n<=1)return 0;float edge=float(n-1);if(v>=0&&v<=edge)return v;v=std::fmod(v,2*edge);if(v<0)v+=2*edge;return v>edge?2*edge-v:v;}
inline Pixel read(const uint8_t*p,bool premul){float a=p[3];float f=premul?1.f:a*(1.f/255.f);return{p[0]*f,p[1]*f,p[2]*f,a};}
inline Pixel sample(const uint8_t*buf,int w,int h,float x,float y,bool premul=false,bool bounded=false){if(!buf)return{};x=bounded?std::clamp(x,0.f,float(w-1)):reflect(x,w);y=bounded?std::clamp(y,0.f,float(h-1)):reflect(y,h);int ix=int(x),iy=int(y),jx=std::min(ix+1,w-1),jy=std::min(iy+1,h-1);float fx=x-ix,fy=y-iy;const uint8_t*p=buf+(size_t(iy)*w+ix)*4;Pixel a=read(p,premul),b=read(buf+(size_t(iy)*w+jx)*4,premul),c=read(buf+(size_t(jy)*w+ix)*4,premul),d=read(buf+(size_t(jy)*w+jx)*4,premul);return (a*(1-fx)+b*fx)*(1-fy)+(c*(1-fx)+d*fx)*fy;}
inline uint8_t byte(float f){return uint8_t(std::clamp(f+0.5f,0.f,255.f));}
inline void store(uint8_t*p,Pixel v){if(v.a<.5f){p[0]=p[1]=p[2]=p[3]=0;return;}float f=255/v.a;p[0]=byte(v.r*f);p[1]=byte(v.g*f);p[2]=byte(v.b*f);p[3]=byte(v.a);}
struct Geometry {float scale=1,angle=0,tx=0,ty=0,shear=0;float cs=1,sn=0;void ready(){cs=std::cos(angle)/scale;sn=std::sin(angle)/scale;}void map(float x,float y,float cx,float cy,float&u,float&v)const{x-=cx+tx;y-=cy+ty;u=cx+cs*x+sn*y-shear*y;v=cy-sn*x+cs*y;}};
struct State {Geometry a,b;double e,p,fade;};
State state(double p,int w,int h,const TransitionConfig&c){State s;s.p=clamp(p);s.e=ease(p);s.fade=window(p,.14,.86);double e=s.e,env=bell(p),d=.08+.27*c.strength;double dx=c.direction==0?1:c.direction==1?-1:0,dy=c.direction==2?1:c.direction==3?-1:0;
switch(c.style){
case 0:s.a.scale=std::exp(-d*e);s.b.scale=std::exp(d*(1-e));break;
case 1:s.a.scale=std::exp(d*e);s.b.scale=std::exp(-d*(1-e));break;
case 2:s.a.tx=-dx*w*e;s.a.ty=-dy*h*e;s.b.tx=dx*w*(1-e);s.b.ty=dy*h*(1-e);s.fade=window(p,.35,.65);s.a.scale=s.b.scale=1+.04*env;break;
case 3:s.a.tx=-dx*w*e;s.a.ty=-dy*h*e;s.b.tx=dx*w*(1-e);s.b.ty=dy*h*(1-e);break;
case 4:s.a.scale=1+.025*e;s.b.scale=1+.025*(1-e);s.fade=e;break;
case 5:s.a.scale=1+.025*env;s.b.scale=1+.04*(1-e);break;
case 6:s.a.angle=(.025+.10*c.strength)*e;s.b.angle=-(.025+.10*c.strength)*(1-e);s.a.scale=s.b.scale=1+.12*env;break;
case 7:s.a.tx=-dx*w*d*.15*e;s.a.ty=-dy*h*d*.15*e;s.b.tx=dx*w*d*.15*(1-e);s.b.ty=dy*h*d*.15*(1-e);s.a.scale=1+.025*env;s.b.scale=1+.025*env;break;
case 8:s.b.scale=1+d*(1-e);break;
case 9:s.a.scale=1+.04*env;s.b.scale=1+.07*(1-e);break;
case 10:s.a.shear=(.04+.12*c.strength)*e;s.b.shear=-(.04+.12*c.strength)*(1-e);s.a.scale=1+.12*e;s.b.scale=1+.12*(1-e);break;
case 11:s.b.tx=dx*w*.025*(1-e);s.b.ty=dy*h*.025*(1-e);break;
}s.a.ready();s.b.ready();return s;}
double mask(float x,float y,int w,int h,const State&s,const TransitionConfig&c){double n=(c.direction<2?x/std::max(1,w-1):y/std::max(1,h-1));if(c.direction==1||c.direction==3)n=1-n;double feather=.005+.09*c.softness;switch(c.style){case 2:case 3:return window(n,1-s.e-feather,1-s.e+feather);case 5:{double u=(x/std::max(1,w-1)+y/std::max(1,h-1))*.5;if(c.direction==1||c.direction==3)u=1-u;return window(s.e,u-feather,u+feather);}case 8:{double nx=(x/(w-1.0)-c.cx)*w/std::max(w,h),ny=(y/(h-1.0)-c.cy)*h/std::max(w,h);double far=std::hypot(std::max(c.cx,1-c.cx)*w,std::max(c.cy,1-c.cy)*h)/std::max(w,h);double dist=std::hypot(nx,ny)/std::max(.01,far);return window(s.e,dist-feather,dist+feather);}case 9:{double v=std::abs(n-.5)*2;return window(s.e,v-feather,v+feather);}case 11:return window(s.e,n-feather,n+feather);default:return s.fade;}}
void blur_rgba(std::vector<uint8_t>&buf,int w,int h,int radius,std::vector<uint8_t>&scratch){scratch.resize(buf.size());int d=radius*2+1;for(int pass=0;pass<3;pass++){for(int y=0;y<h;y++){int sum[4]={};for(int k=-radius;k<=radius;k++){const uint8_t*p=&buf[(size_t(y)*w+std::clamp(k,0,w-1))*4];for(int c=0;c<4;c++)sum[c]+=p[c];}for(int x=0;x<w;x++){size_t k=(size_t(y)*w+x)*4;for(int c=0;c<4;c++)scratch[k+c]=uint8_t((sum[c]+d/2)/d);const uint8_t*a=&buf[(size_t(y)*w+std::clamp(x-radius,0,w-1))*4],*b=&buf[(size_t(y)*w+std::clamp(x+radius+1,0,w-1))*4];for(int c=0;c<4;c++)sum[c]+=int(b[c])-a[c];}}for(int x=0;x<w;x++){int sum[4]={};for(int k=-radius;k<=radius;k++){const uint8_t*p=&scratch[(size_t(std::clamp(k,0,h-1))*w+x)*4];for(int c=0;c<4;c++)sum[c]+=p[c];}for(int y=0;y<h;y++){size_t k=(size_t(y)*w+x)*4;for(int c=0;c<4;c++)buf[k+c]=uint8_t((sum[c]+d/2)/d);const uint8_t*a=&scratch[(size_t(std::clamp(y-radius,0,h-1))*w+x)*4],*b=&scratch[(size_t(std::clamp(y+radius+1,0,h-1))*w+x)*4];for(int c=0;c<4;c++)sum[c]+=int(b[c])-a[c];}}}}
void downsample(const uint8_t*in,int w,int h,std::vector<uint8_t>&out,int lw,int lh){out.resize(size_t(lw)*lh*4);for(int y=0;y<lh;y++)for(int x=0;x<lw;x++){Pixel p=sample(in,w,h,(x+.5f)*w/lw-.5f,(y+.5f)*h/lh-.5f);size_t k=(size_t(y)*lw+x)*4;out[k]=byte(p.r);out[k+1]=byte(p.g);out[k+2]=byte(p.b);out[k+3]=byte(p.a);}}
#if defined(__SSE2__) && !defined(TRANSITIONS_SCALAR_REFERENCE)
// The common SDR-video path uses exact opacity detection, cached 1-D maps and
// SSE2 fixed-point bilinear interpolation. No AVX or -march=native requirement.
// Transparent inputs keep the general premultiplied-alpha reference path.
static bool opaque(const uint8_t* pixels, size_t bytes) {
    if (!pixels) return false;
    const __m128i alpha = _mm_set1_epi32(int(0xff000000u));
    size_t i = 0;
    for (; i + 16 <= bytes; i += 16) {
        __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(pixels + i));
        if (_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_and_si128(v, alpha), alpha)) != 65535)
            return false;
    }
    for (; i < bytes; i += 4) if (pixels[i + 3] != 255) return false;
    return true;
}
static void axis_map(int* dst, int count, int extent, float scale, float offset,
                     int stride, bool bounded = false) {
    for (int i = 0; i < count; ++i) {
        float p = bounded ? std::clamp(scale * i + offset, 0.f, float(extent - 1)) : reflect(scale * i + offset, extent);
        int a = int(p);
        dst[i * 3] = a * stride;
        dst[i * 3 + 1] = std::min(a + 1, extent - 1) * stride;
        dst[i * 3 + 2] = std::clamp(int((p - a) * 256 + .5f), 0, 256);
    }
}
static inline __m128i load_pixel(const uint8_t* p) {
    uint32_t v; std::memcpy(&v, p, sizeof(v));
    return _mm_unpacklo_epi8(_mm_cvtsi32_si128(int(v)), _mm_setzero_si128());
}
static inline __m128i interpolate(__m128i a, __m128i b, int amount) {
    auto wa = _mm_set1_epi16(short(256 - amount));
    auto wb = _mm_set1_epi16(short(amount));
    auto sum = _mm_add_epi16(_mm_mullo_epi16(a, wa), _mm_mullo_epi16(b, wb));
    return _mm_srli_epi16(_mm_add_epi16(sum, _mm_set1_epi16(128)), 8);
}
static inline __m128i mapped_sample(const uint8_t* p, const int* x, const int* y) {
    auto a = interpolate(load_pixel(p + y[0] + x[0]), load_pixel(p + y[0] + x[1]), x[2]);
    auto b = interpolate(load_pixel(p + y[1] + x[0]), load_pixel(p + y[1] + x[1]), x[2]);
    return interpolate(a, b, y[2]);
}
static bool fast_opaque(const uint8_t* a, const uint8_t* b, uint8_t* out,
                        int w, int h, const State& s, const TransitionConfig& c,
                        const std::vector<uint8_t>& lowA, const std::vector<uint8_t>& lowB,
                        int lw, int lh, double blur, std::vector<int>& maps,
                        std::vector<int>& masks) {
    if (!(c.style == 0 || c.style == 1 || c.style == 2 || c.style == 3 ||
          c.style == 4 || c.style == 7 || c.style == 11)) return false;
    if (!opaque(a, size_t(w)*h*4) || !opaque(b, size_t(w)*h*4)) return false;
    const bool hasBlur = blur > .01;
    maps.resize(size_t(w+h)* (hasBlur ? 12 : 6));
    int* ax = maps.data(); int* ay = ax+w*3; int* bx = ay+h*3; int* by = bx+w*3;
    float cx = float(c.cx*(w-1)), cy = float(c.cy*(h-1));
    float aox = cx - s.a.cs*(cx+s.a.tx), aoy = cy-s.a.cs*(cy+s.a.ty);
    float box = cx - s.b.cs*(cx+s.b.tx), boy = cy-s.b.cs*(cy+s.b.ty);
    const bool bounded = c.style == 2 || c.style == 3;
    axis_map(ax, w, w, s.a.cs, aox, 4, bounded); axis_map(ay, h, h, s.a.cs, aoy, w*4, bounded);
    axis_map(bx, w, w, s.b.cs, box, 4, bounded); axis_map(by, h, h, s.b.cs, boy, w*4, bounded);
    int *lax = nullptr, *lay = nullptr, *lbx = nullptr, *lby = nullptr;
    if (hasBlur) {
        lax=by+h*3; lay=lax+w*3; lbx=lay+h*3; lby=lbx+w*3;
        float sx=lw/float(w), sy=lh/float(h);
        axis_map(lax,w,lw,s.a.cs*sx,(aox+.5f)*sx-.5f,4);
        axis_map(lay,h,lh,s.a.cs*sy,(aoy+.5f)*sy-.5f,lw*4);
        axis_map(lbx,w,lw,s.b.cs*sx,(box+.5f)*sx-.5f,4);
        axis_map(lby,h,lh,s.b.cs*sy,(boy+.5f)*sy-.5f,lw*4);
    }
    const int mb = std::clamp(int(blur*256+.5),0,256);
    const bool spatial = c.style==2 || c.style==3 || c.style==11;
    const int common = std::clamp(int(s.fade*256+.5),0,256);
    if (spatial) {
        int count=c.direction<2?w:h; masks.resize(count);
        for(int i=0;i<count;i++) masks[i]=std::clamp(int(mask(c.direction<2?i:0,
            c.direction<2?0:i,w,h,s,c)*256+.5),0,256);
    }
    for(int y=0;y<h;y++) for(int x=0;x<w;x++) {
        int m=spatial?masks[c.direction<2?x:y]:common;
        __m128i pa=_mm_setzero_si128(),pb=pa;
        if(m<256) {
            pa=mapped_sample(a,ax+x*3,ay+y*3);
            if(hasBlur) pa=interpolate(pa,mapped_sample(lowA.data(),lax+x*3,lay+y*3),mb);
        }
        if(m>0) {
            pb=mapped_sample(b,bx+x*3,by+y*3);
            if(hasBlur) pb=interpolate(pb,mapped_sample(lowB.data(),lbx+x*3,lby+y*3),mb);
        }
        auto value=interpolate(pa,pb,m);
        uint32_t pixel=uint32_t(_mm_cvtsi128_si32(_mm_packus_epi16(value,_mm_setzero_si128())));
        std::memcpy(out+(size_t(y)*w+x)*4,&pixel,4);
    }
    return true;
}
#endif

}
void TransitionRenderer::render(int w,int h,const uint8_t*a,const uint8_t*b,uint8_t*out,double p,const TransitionConfig&c){if(!out||w<2||h<2||w>8192||h>8192||size_t(w)*h>33554432)throw std::runtime_error("Недопустимый размер кадра");size_t bytes=size_t(w)*h*4;p=clamp(p);if(p==0){if(a)std::memmove(out,a,bytes);else std::memset(out,0,bytes);return;}if(p==1){if(b)std::memmove(out,b,bytes);else std::memset(out,0,bytes);return;}if(out==a){copyA.assign(a,a+bytes);a=copyA.data();}if(out==b){copyB.assign(b,b+bytes);b=copyB.data();}
const float cx=float(c.cx*(w-1)),cy=float(c.cy*(h-1));int samples=(c.quality&&c.style==2&&c.softness>.05&&bell(p)>.03)?3:1;State states[3];for(int k=0;k<samples;k++)states[k]=state(p+(samples==1?0:(k-1)*.014*c.softness*bell(p)),w,h,c);
int lw=std::max(2,w/4),lh=std::max(2,h/4);double blurAmount=(c.style==4?bell(p)*(.35+.65*c.softness):0);if(blurAmount>.01){downsample(a,w,h,blurA,lw,lh);downsample(b,w,h,blurB,lw,lh);int r=std::max(1,int((1+4*c.softness)*w/1280.));blur_rgba(blurA,lw,lh,r,work);blur_rgba(blurB,lw,lh,r,work);}
#if defined(__SSE2__) && !defined(TRANSITIONS_SCALAR_REFERENCE)
if(samples==1 && fast_opaque(a,b,out,w,h,states[0],c,blurA,blurB,lw,lh,blurAmount,mapCache,maskCache))return;
#endif
for(int y=0;y<h;y++){for(int x=0;x<w;x++){Pixel result;for(int k=0;k<samples;k++){const auto&s=states[k];float au,av,bu,bv;s.a.map(float(x),float(y),cx,cy,au,av);s.b.map(float(x),float(y),cx,cy,bu,bv);if(c.style==9){double direction=(c.direction<2?(x<w/2?-1:1):(y<h/2?-1:1));if(c.direction<2)au-=direction*float(w*s.e*.25);else av-=direction*float(h*s.e*.25);}const bool bounded=c.style==2||c.style==3;Pixel pa=sample(a,w,h,au,av,false,bounded),pb=sample(b,w,h,bu,bv,false,bounded);if(blurAmount>.01){pa=pa*float(1-blurAmount)+sample(blurA.data(),lw,lh,(au+.5f)*lw/w-.5f,(av+.5f)*lh/h-.5f,true)*float(blurAmount);pb=pb*float(1-blurAmount)+sample(blurB.data(),lw,lh,(bu+.5f)*lw/w-.5f,(bv+.5f)*lh/h-.5f,true)*float(blurAmount);}float m=float(mask(float(x),float(y),w,h,s,c));if(c.style==13){const float lum=(.2126f*pb.r+.7152f*pb.g+.0722f*pb.b)/255.f;const float feather=float(.03+.2*c.softness);m=float(window(s.e*1.4-.2,1-lum-feather,1-lum+feather));}Pixel mixed=pa*(1-m)+pb*m;if(c.style==12){float glow=float(bell(p)*(.22+.52*c.strength)*std::clamp(1.2f-std::abs(x/float(w)-.3f)*1.1f,0.f,1.f))*mixed.a; mixed.r=std::min(mixed.a,mixed.r+glow);mixed.g=std::min(mixed.a,mixed.g+glow*.87f);mixed.b=std::min(mixed.a,mixed.b+glow*.7f);}if(c.style==14){const float env=float(bell(p));const float shift=(float((y/7)%3)-1.f)*env*float(2+12*c.strength);Pixel red=sample(b,w,h,bu+shift,bv),blue=sample(a,w,h,au-shift,av);mixed.r=std::clamp(mixed.r*(1-env*.45f)+red.r*env*.45f,0.f,mixed.a);mixed.b=std::clamp(mixed.b*(1-env*.45f)+blue.b*env*.45f,0.f,mixed.a);}result=result+mixed;}store(out+(size_t(y)*w+x)*4,result*(1.f/samples));}}}
void blur8(std::vector<uint8_t>&buf,int w,int h,int r,std::vector<uint8_t>&tmp){if(r<1||w<1||h<1)return;r=std::min(r,128);tmp.resize(buf.size());int d=2*r+1;for(int pass=0;pass<3;pass++){for(int y=0;y<h;y++){int sum=0;for(int k=-r;k<=r;k++)sum+=buf[size_t(y)*w+std::clamp(k,0,w-1)];for(int x=0;x<w;x++){tmp[size_t(y)*w+x]=uint8_t((sum+d/2)/d);sum+=int(buf[size_t(y)*w+std::clamp(x+r+1,0,w-1)])-buf[size_t(y)*w+std::clamp(x-r,0,w-1)];}}for(int x=0;x<w;x++){int sum=0;for(int k=-r;k<=r;k++)sum+=tmp[size_t(std::clamp(k,0,h-1))*w+x];for(int y=0;y<h;y++){buf[size_t(y)*w+x]=uint8_t((sum+d/2)/d);sum+=int(tmp[size_t(std::clamp(y+r+1,0,h-1))*w+x])-tmp[size_t(std::clamp(y-r,0,h-1))*w+x];}}}}
}
