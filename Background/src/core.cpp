// SPDX-License-Identifier: GPL-3.0-only
#include "studio_background/core.hpp"
#include <algorithm>
#include <cmath>
#include <deque>
#include <stdexcept>
namespace sbg {
static std::size_t pixels(int w,int h) {
    if(w<1 || h<1 || w>16384 || h>16384 || uint64_t(w)*h>MaxPixels)
        throw std::invalid_argument("Unsupported image dimensions (SDR maximum 4096x2304 pixels)");
    return std::size_t(w)*h;
}
static float clip(float v) {return std::clamp(v,0.f,1.f);}
static uint8_t byte(float v) {return uint8_t(clip(v)*255.f+.5f);}
static float smooth(float v) {v=clip(v);return v*v*(3-2*v);}
Image::Image(int width,int height):w(width),h(height),rgba(pixels(w,h)*4){}
void Image::validate() const {if(rgba.size()!=pixels(w,h)*4)throw std::invalid_argument("Invalid RGBA size");}
Mask::Mask(int width,int height,float v):w(width),h(height),alpha(pixels(w,h),v){}
void Mask::validate() const {
    if(alpha.size()!=pixels(w,h))throw std::invalid_argument("Invalid mask size");
    for(float v:alpha) if(!std::isfinite(v)||v<0||v>1)throw std::invalid_argument("Invalid alpha value");
}
static std::array<float,3> keySpace(float r,float g,float b,bool neutral) {
    if(neutral)return {r*0.57735027f,g*0.57735027f,b*0.57735027f};
    const float y=.2126f*r+.7152f*g+.0722f*b;
    return {(b-y)*.5389f,(r-y)*.6350f,0.f};
}
static float distance(const std::array<float,3>& a,const std::array<float,3>& b) {
    float d=0;for(int c=0;c<3;++c)d+=(a[c]-b[c])*(a[c]-b[c]);return std::sqrt(d);
}
Sample pickKey(const Image& im,int x,int y,int rad) {
    im.validate();if(x<0||y<0||x>=im.w||y>=im.h||rad<0||rad>16)throw std::invalid_argument("Pick outside image");
    std::array<std::vector<int>,3> values;
    for(int yy=std::max(0,y-rad);yy<=std::min(im.h-1,y+rad);++yy)
        for(int xx=std::max(0,x-rad);xx<=std::min(im.w-1,x+rad);++xx){
            auto i=(std::size_t(yy)*im.w+xx)*4;
            if(im.rgba[i+3]<128)continue;
            for(int c=0;c<3;++c)values[c].push_back(im.rgba[i+c]);
        }
    if(values[0].empty())throw std::invalid_argument("Cannot sample a transparent patch");
    Sample s;
    for(int c=0;c<3;++c){auto &v=values[c];std::nth_element(v.begin(),v.begin()+v.size()/2,v.end());s.rgb[c]=v[v.size()/2]/255.;}
    bool neutral=*std::max_element(s.rgb.begin(),s.rgb.end())-*std::min_element(s.rgb.begin(),s.rgb.end())<.08;
    auto key=keySpace(s.rgb[0],s.rgb[1],s.rgb[2],neutral);
    std::vector<float> deviations;
    for(int yy=std::max(0,y-rad);yy<=std::min(im.h-1,y+rad);++yy)
        for(int xx=std::max(0,x-rad);xx<=std::min(im.w-1,x+rad);++xx){auto i=(std::size_t(yy)*im.w+xx)*4;
            if(im.rgba[i+3]>=128)deviations.push_back(distance(key,keySpace(im.rgba[i]/255.f,im.rgba[i+1]/255.f,im.rgba[i+2]/255.f,neutral)));}
    std::sort(deviations.begin(),deviations.end());
    s.tolerance=std::clamp(double(deviations[(deviations.size()-1)*9/10])*1.8+.05,.06,.24);
    return s;
}
Mask chroma(const Image& im,const Params& p) {
    im.validate();p.validate();Mask m(im.w,im.h);
    const bool neutral=std::max({p.key_r,p.key_g,p.key_b})-std::min({p.key_r,p.key_g,p.key_b})<.08;
    const auto k=keySpace(p.key_r,p.key_g,p.key_b,neutral);
    for(std::size_t i=0;i<m.alpha.size();++i){
        auto q=keySpace(im.rgba[4*i]/255.f,im.rgba[4*i+1]/255.f,im.rgba[4*i+2]/255.f,neutral);
        float ds=0;for(int c=0;c<3;++c)ds+=(k[c]-q[c])*(k[c]-q[c]);
        const float low=p.tolerance*p.tolerance,high=(p.tolerance+p.transition)*(p.tolerance+p.transition);
        m.alpha[i]=ds<=low?0.f:ds>=high?1.f:smooth((std::sqrt(ds)-p.tolerance)/p.transition);
    }return m;
}
void boxBlur(std::vector<float>& a,int w,int h,int r) {
    if(a.size()!=pixels(w,h)||r<0||r>512)throw std::invalid_argument("Invalid blur");
    if(!r)return;
    std::vector<float> t(a.size()); const double den=2.*r+1;
    for(int y=0;y<h;++y){
        auto row=std::size_t(y)*w;double sum=0;
        for(int x=-r;x<=r;++x)sum+=a[row+std::clamp(x,0,w-1)];
        for(int x=0;x<w;++x){t[row+x]=float(sum/den);sum+=a[row+std::min(w-1,x+r+1)]-a[row+std::max(0,x-r)];}
    }
    // Column accumulators permit contiguous row access (important on low-power CPUs).
    std::vector<double> sums(w,0.);
    for(int y=-r;y<=r;++y){const auto row=std::size_t(std::clamp(y,0,h-1))*w;for(int x=0;x<w;++x)sums[x]+=t[row+x];}
    for(int y=0;y<h;++y){const auto row=std::size_t(y)*w,add=std::size_t(std::min(h-1,y+r+1))*w,sub=std::size_t(std::max(0,y-r))*w;
        for(int x=0;x<w;++x){a[row+x]=float(sums[x]/den);sums[x]+=t[add+x]-t[sub+x];}
    }
}
static void morph(Mask& m,int r,bool expand) {
    if(r==0)return;
    std::vector<float> t(m.alpha.size());
    // Sliding max/min over a clipped rectangular neighbourhood. O(width*height).
    auto pass=[&](const std::vector<float>& in,std::vector<float>& out,int length,int lines,int step,int lineStep){
        for(int line=0;line<lines;++line){std::deque<int> q;int next=0;const int base=line*lineStep;
            for(int x=0;x<length;++x){
                while(next<std::min(length,x+r+1)){
                    while(!q.empty() && (expand ? in[base+next*step]>=in[base+q.back()*step] : in[base+next*step]<=in[base+q.back()*step]))q.pop_back();
                    q.push_back(next++);
                }
                while(!q.empty()&&q.front()<x-r)q.pop_front();
                out[base+x*step]=in[base+q.front()*step];
            }
        }
    };
    pass(m.alpha,t,m.w,m.h,1,m.w);pass(t,m.alpha,m.h,m.w,m.w,1);
}
Mask resizeMask(const Mask& a,int w,int h){
    a.validate();if(a.w==w&&a.h==h)return a;Mask b(w,h);
    for(int y=0;y<h;++y){float sy=std::clamp((y+.5f)*a.h/h-.5f,0.f,float(a.h-1));int y0=int(sy),y1=std::min(a.h-1,y0+1);float fy=sy-y0;
        for(int x=0;x<w;++x){float sx=std::clamp((x+.5f)*a.w/w-.5f,0.f,float(a.w-1));int x0=int(sx),x1=std::min(a.w-1,x0+1);float fx=sx-x0;
            b.alpha[std::size_t(y)*w+x]=(1-fy)*((1-fx)*a.alpha[std::size_t(y0)*a.w+x0]+fx*a.alpha[std::size_t(y0)*a.w+x1])+fy*((1-fx)*a.alpha[std::size_t(y1)*a.w+x0]+fx*a.alpha[std::size_t(y1)*a.w+x1]);
        }
    }return b;
}
Image resizeImage(const Image& a,int w,int h){
    a.validate();if(a.w==w&&a.h==h)return a;Image b(w,h);
    // Interpolate premultiplied values internally, return straight RGBA.
    for(int y=0;y<h;++y){float sy=std::clamp((y+.5f)*a.h/h-.5f,0.f,float(a.h-1));int y0=int(sy),y1=std::min(a.h-1,y0+1);float fy=sy-y0;
        for(int x=0;x<w;++x){float sx=std::clamp((x+.5f)*a.w/w-.5f,0.f,float(a.w-1));int x0=int(sx),x1=std::min(a.w-1,x0+1);float fx=sx-x0;
            std::array<std::size_t,4> ix={std::size_t(y0)*a.w+x0,std::size_t(y0)*a.w+x1,std::size_t(y1)*a.w+x0,std::size_t(y1)*a.w+x1};
            float weights[4]={(1-fx)*(1-fy),fx*(1-fy),(1-fx)*fy,fx*fy},v[4]={};
            for(int j=0;j<4;++j){float al=a.rgba[ix[j]*4+3]/255.f;v[3]+=weights[j]*al;for(int c=0;c<3;++c)v[c]+=weights[j]*al*a.rgba[ix[j]*4+c]/255.f;}
            const auto o=(std::size_t(y)*w+x)*4;for(int c=0;c<3;++c)b.rgba[o+c]=byte(v[3]>1e-6f?v[c]/v[3]:0);b.rgba[o+3]=byte(v[3]);
        }
    }return b;
}
void refineMask(Mask& m,const Image& im,const Params& p,int rw,int rh){
    m.validate();im.validate();p.validate();if(m.w!=im.w||m.h!=im.h||rw<1||rh<1)throw std::invalid_argument("Mask/reference mismatch");
    const float scale=std::min(float(im.w)/rw,float(im.h)/rh);
    if(p.method==1&&p.refine>0){
        // Scalar guided filter at <=640px wide/long. It refines a supplied matte,
        // it cannot invent hair absent from that matte.
        float s=std::min(1.f,640.f/std::max(im.w,im.h));int w=std::max(1,int(im.w*s)),h=std::max(1,int(im.h*s));
        Image small=resizeImage(im,w,h);Mask low=resizeMask(m,w,h);
        const auto n=low.alpha.size();std::vector<float> I(n),meanI(n),meanP=low.alpha,ii(n),ip(n);
        for(std::size_t i=0;i<n;++i){I[i]=(.2126f*small.rgba[4*i]+.7152f*small.rgba[4*i+1]+.0722f*small.rgba[4*i+2])/255.f;ii[i]=I[i]*I[i];ip[i]=I[i]*low.alpha[i];}
        meanI=I;boxBlur(meanI,w,h,2);boxBlur(meanP,w,h,2);boxBlur(ii,w,h,2);boxBlur(ip,w,h,2);
        for(std::size_t i=0;i<n;++i){ii[i]=(ip[i]-meanI[i]*meanP[i])/(std::max(0.f,ii[i]-meanI[i]*meanI[i])+.0025f);ip[i]=meanP[i]-ii[i]*meanI[i];}
        boxBlur(ii,w,h,2);boxBlur(ip,w,h,2);
        for(std::size_t i=0;i<n;++i)low.alpha[i]=clip(ii[i]*I[i]+ip[i]);
        low=resizeMask(low,im.w,im.h);
        for(std::size_t i=0;i<m.alpha.size();++i){float a=m.alpha[i];float edge=std::min(1.f,4.f*a*(1.f-a));m.alpha[i]=clip(a+float(p.refine)*edge*(low.alpha[i]-a));}
    }
    morph(m,int(std::lround(std::abs(p.edge_shift)*scale)),p.edge_shift>0);
    // One small spatial feather only, never temporal averaging (no motion ghosts).
    const float radius=float(p.feather)*scale;
    if(radius>0){int r=std::max(1,int(std::ceil(radius)));std::vector<float> soft=m.alpha;boxBlur(soft,m.w,m.h,r);float mix=std::min(1.f,radius);
        // Feather a chroma edge inward by default. Outward softening would reveal
        // fully keyed green/blue RGB again, creating a colored one-pixel halo.
        // Explicit expansion and manual restore remain deliberate user choices.
        const bool inward=p.method==0&&p.edge_shift<=0;
        for(std::size_t i=0;i<m.alpha.size();++i){float previous=m.alpha[i],value=clip(previous*(1-mix)+soft[i]*mix);m.alpha[i]=inward?std::min(previous,value):value;}
    }
}
Image render(const Image& src,const Params& p,const Mask* cached,int rw,int rh){
    src.validate();p.validate();if(!rw)rw=src.w;if(!rh)rh=src.h;
    Mask m;
    if(p.method==0)m=chroma(src,p);else{if(!cached)throw std::runtime_error("Human matte missing: run analysis first");m=resizeMask(*cached,src.w,src.h);}
    refineMask(m,src,p,rw,rh);
    Image out(src.w,src.h);
    std::vector<float> bg[3]; int bw=0,bh=0;
    if(p.output==1 && p.blur>0){
        // Normalized background-only blur: never feed opaque foreground into blur.
        // <=640 px working image; no full-resolution RGB blur allocations.
        const float s=std::min(1.f,640.f/std::max(src.w,src.h));bw=std::max(1,int(src.w*s));bh=std::max(1,int(src.h*s));
        auto small=resizeImage(src,bw,bh);auto a=resizeMask(m,bw,bh);
        std::vector<float> weights(a.alpha.size());
        for(auto& c:bg)c.resize(a.alpha.size());
        for(std::size_t i=0;i<a.alpha.size();++i){weights[i]=(1-a.alpha[i])*small.rgba[4*i+3]/255.f;for(int c=0;c<3;++c)bg[c][i]=small.rgba[4*i+c]/255.f*weights[i];}
        const int r=std::max(1,int(std::lround(p.blur*std::min(float(src.w)/rw,float(src.h)/rh)*s*.58)));
        for(int pass=0;pass<3;++pass){boxBlur(weights,bw,bh,r);for(auto& c:bg)boxBlur(c,bw,bh,r);}
        for(std::size_t i=0;i<weights.size();++i)for(int c=0;c<3;++c)bg[c][i]=weights[i]>1e-5?clip(bg[c][i]/weights[i]):small.rgba[4*i+c]/255.f;
    }
    int spillChannel=-1;
    if(p.method==0){if(p.key_g>p.key_r+.15&&p.key_g>p.key_b+.15)spillChannel=1;else if(p.key_b>p.key_r+.15&&p.key_b>p.key_g+.15)spillChannel=2;}
    if(p.output==0){
        // Common playback path: retain source RGB bytes exactly and touch alpha.
        // Do not round-trip every RGB channel through float when it is unchanged.
        out.rgba=src.rgba;
        for(std::size_t i=0;i<m.alpha.size();++i){const auto ix=i*4;const float a=clip(m.alpha[i]);out.rgba[ix+3]=uint8_t(a*src.rgba[ix+3]+.5f);
            if(spillChannel>=0&&p.despill>0&&a>0&&a<.999f){int c=spillChannel;const float other=std::max(src.rgba[ix+(c+1)%3],src.rgba[ix+(c+2)%3]);
                const float excess=std::max(0.f,float(src.rgba[ix+c])-other);out.rgba[ix+c]=uint8_t(std::clamp(float(src.rgba[ix+c])-excess*float(p.despill)*(1-a),0.f,255.f)+.5f);}
        }
        return out;
    }
    for(int y=0;y<src.h;++y)for(int x=0;x<src.w;++x){const auto i=std::size_t(y)*src.w+x,ix=i*4;float a=clip(m.alpha[i]);
        float rgb[3]={src.rgba[ix]/255.f,src.rgba[ix+1]/255.f,src.rgba[ix+2]/255.f};
        if(spillChannel>=0&&p.despill>0&&a<.999f){int c=spillChannel;float other=std::max(rgb[(c+1)%3],rgb[(c+2)%3]);rgb[c]-=std::max(0.f,rgb[c]-other)*float(p.despill)*(1-a);}
        float back[3]={float(p.fill_r),float(p.fill_g),float(p.fill_b)};
        if(p.output==1){for(int c=0;c<3;++c)back[c]=src.rgba[ix+c]/255.f;
            if(bw){float sx=std::clamp((x+.5f)*bw/src.w-.5f,0.f,float(bw-1)),sy=std::clamp((y+.5f)*bh/src.h-.5f,0.f,float(bh-1));int x0=int(sx),y0=int(sy),x1=std::min(bw-1,x0+1),y1=std::min(bh-1,y0+1);float fx=sx-x0,fy=sy-y0;
                for(int c=0;c<3;++c)back[c]=(1-fy)*((1-fx)*bg[c][std::size_t(y0)*bw+x0]+fx*bg[c][std::size_t(y0)*bw+x1])+fy*((1-fx)*bg[c][std::size_t(y1)*bw+x0]+fx*bg[c][std::size_t(y1)*bw+x1]);}
        }
        for(int c=0;c<3;++c)out.rgba[ix+c]=byte(rgb[c]*a+back[c]*(1-a));
        out.rgba[ix+3]=src.rgba[ix+3]; // Existing alpha is never made opaque by blur/fill.
    }return out;
}
}
