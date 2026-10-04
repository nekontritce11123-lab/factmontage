// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace sunimo {
constexpr double PI=3.14159265358979323846;
inline double clamp(double v,double a=0,double b=1){return std::max(a,std::min(b,v));}
inline double smooth(double x){x=clamp(x);return x*x*x*(x*(x*6-15)+10);}
inline double mix(double a,double b,double x){return a+(b-a)*x;}
inline uint32_t hash(uint32_t x){x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;x^=x>>16;return x;}
inline double noise(uint32_t x){return (hash(x)&0xffffff)/16777215.0;}
struct Recipe {int kernel,ease;float direction,amplitude,rotation;int mask,order;};
#include "recipes.inc"
inline const Recipe& recipe(int n){return RECIPES[std::max(0,std::min(100,n))];}
inline double bezier(double x,double x1,double y1,double x2,double y2){
    if(x<=0)return 0;
    if(x>=1)return 1;
    auto at=[](double t,double a,double b){return 3*(1-t)*(1-t)*t*a+3*(1-t)*t*t*b+t*t*t;};
    double lo=0,hi=1,t=x;
    for(int i=0;i<16;i++){double q=at(t,x1,x2);if(q<x)lo=t;else hi=t;t=(lo+hi)*.5;}
    return at(t,y1,y2);
}
inline double ease(double p,int e){
    p=clamp(p);if(p==0||p==1)return p;
    switch(e){
    case 0:return bezier(p,.22,1,.36,1);
    case 1:return bezier(p,.16,1,.3,1);
    case 2:return smooth(p);
    case 3:{double x=p-1;return 1+2.05*x*x*x+1.05*x*x;}
    case 4:case 5:{double w=e==4?12.:9.,d=e==4?9.:11.;
        auto f=[&](double q){return 1-std::exp(-d*q)*(std::cos(w*q)+d/w*std::sin(w*q));};
        double v=f(p)/f(1),slope=std::exp(-d)*(w+d*d/w)*std::sin(w)/f(1);
        return v-slope*p*p*(p-1);}
    default:return p;
    }
}
struct Config {
    std::array<float,32> v{};
    double duration()const{return clamp(v[0],.1,120);}
    int in()const{return int(v[6]);} int out()const{return int(v[7]);}
    int group()const{return int(v[8]);} int seed()const{return int(v[5]);}
};
struct Sprite {
    float x=0,y=0,w=0,h=0,ax=0,ay=0;
    uint32_t pw=0,ph=0,word=0,line=0,flags=0;
    std::vector<uint8_t> pixels;
    std::array<std::vector<uint8_t>,3> blur;
};
struct Reader{
    const uint8_t* p;size_t n,pos=0;
    Reader(const void*d,size_t s):p(static_cast<const uint8_t*>(d)),n(s){}
    void take(void*d,size_t s){if(s>n-pos)throw std::runtime_error("Truncated STXT file");std::memcpy(d,p+pos,s);pos+=s;}
    uint32_t u(){uint8_t b[4];take(b,4);return uint32_t(b[0])|(uint32_t(b[1])<<8)|(uint32_t(b[2])<<16)|(uint32_t(b[3])<<24);}
    float f(){uint32_t b=u();float x;std::memcpy(&x,&b,4);if(!std::isfinite(x))throw std::runtime_error("Non-finite STXT number");return x;}
};
inline std::vector<uint8_t> boxblur(const std::vector<uint8_t>&src,int w,int h,int r){
    std::vector<uint8_t> a=src,b(src.size());int div=2*r+1;
    for(int pass=0;pass<3;pass++){
        for(int y=0;y<h;y++)for(int c=0;c<4;c++){
            int sum=0;for(int k=0;k<=r&&k<w;k++)sum+=a[(size_t(y)*w+k)*4+c];
            for(int x=0;x<w;x++){
                b[(size_t(y)*w+x)*4+c]=uint8_t((sum+div/2)/div);
                int leave=x-r,enter=x+r+1;
                if(leave>=0)sum-=a[(size_t(y)*w+leave)*4+c];
                if(enter<w)sum+=a[(size_t(y)*w+enter)*4+c];
            }
        }
        for(int x=0;x<w;x++)for(int c=0;c<4;c++){
            int sum=0;for(int k=0;k<=r&&k<h;k++)sum+=b[(size_t(k)*w+x)*4+c];
            for(int y=0;y<h;y++){
                a[(size_t(y)*w+x)*4+c]=uint8_t((sum+div/2)/div);
                int leave=y-r,enter=y+r+1;
                if(leave>=0)sum-=b[(size_t(leave)*w+x)*4+c];
                if(enter<h)sum+=b[(size_t(enter)*w+x)*4+c];
            }
        }
    }return a;
}
struct Scene {
    uint32_t rw=1920,rh=1080;Config c;std::string metadata;std::vector<Sprite> sprites;
    static std::shared_ptr<Scene> load(const void* data,size_t size){
        if(size>128u*1024u*1024u)throw std::runtime_error("STXT exceeds 128 MiB limit");
        Reader r(data,size);char magic[8];r.take(magic,8);
        if(std::memcmp(magic,"SUNMTX1\0",8))throw std::runtime_error("Not a SUNIMO Text scene");
        uint32_t version=r.u();if(version!=1&&version!=2&&version!=3)throw std::runtime_error("Unsupported STXT version");
        auto s=std::make_shared<Scene>();uint32_t ml=r.u(),count=r.u();s->rw=r.u();s->rh=r.u();
        if(r.u()!=32||ml>2*1024*1024||count>512||s->rw<64||s->rw>7680||s->rh<64||s->rh>7680)throw std::runtime_error("Invalid STXT header");
        for(auto&v:s->c.v)v=r.f();
        auto&v=s->c.v;
        if(v[0]<.1||v[0]>120||v[1]<0||v[1]>30||v[2]<0||v[2]>30||v[3]<0||v[3]>1.5||v[4]<0||v[4]>.9||v[5]<0||v[5]>16777215||v[6]<0||v[6]>100||v[7]<-1||v[7]>100||v[8]<0||v[8]>3||v[9]<0||v[9]>5||v[10]<0||v[10]>9||v[11]<0||v[11]>1||v[12]<.2||v[12]>2||v[13]<0||v[13]>2||v[14]<0||v[14]>100||v[15]<0||v[15]>1||v[16]<0||v[16]>1||v[17]<0||v[17]>1||v[18]<0||v[18]>100||v[21]<0||v[21]>1||v[22]<0||v[22]>1||v[23]<1||v[23]>120000||v[24]<1||v[24]>1001)throw std::runtime_error("STXT configuration out of bounds");
        if(version==1&&(v[10]>8||v[18]!=0))throw std::runtime_error("STXT v1 cannot contain v2 life parameters");
        s->metadata.resize(ml);r.take(s->metadata.data(),ml);uint64_t pixels=0;
        s->sprites.reserve(count);
        bool needblur=v[16]>0;
        for(int id:{s->c.in(),s->c.out()<0?s->c.in():s->c.out(),int(v[14])}){int k=recipe(id).kernel;if(k==4||k==42||k==46)needblur=true;}
        for(uint32_t i=0;i<count;i++){
            Sprite z;z.x=r.f();z.y=r.f();z.w=r.f();z.h=r.f();z.ax=r.f();z.ay=r.f();
            z.pw=r.u();z.ph=r.u();z.word=r.u();z.line=r.u();z.flags=r.u();uint32_t bytes=r.u();
            pixels+=uint64_t(z.pw)*z.ph;
            if(z.pw<1||z.ph<1||z.pw>4096||z.ph>4096||pixels>16*1024*1024||bytes!=uint64_t(z.pw)*z.ph*4||z.w<=0||z.h<=0||z.w>16384||z.h>16384||std::abs(z.x)>32768||std::abs(z.y)>32768||std::abs(z.ax)>32768||std::abs(z.ay)>32768||z.word>4096||z.line>512||z.flags>1)throw std::runtime_error("Invalid STXT sprite");
            z.pixels.resize(bytes);r.take(z.pixels.data(),bytes);
            // Interpolation, blur and blending use premultiplied alpha internally.
            for(size_t j=0;j<bytes;j+=4)for(int c=0;c<3;c++)z.pixels[j+c]=uint8_t((unsigned(z.pixels[j+c])*z.pixels[j+3]+127)/255);
            if(needblur)for(int level=0;level<3;level++)z.blur[level]=boxblur(z.pixels,z.pw,z.ph,1<<(level+1));
            s->sprites.push_back(std::move(z));
        }
        if(r.pos!=r.n)throw std::runtime_error("Trailing bytes in STXT file");
        return s;
    }
    static std::shared_ptr<Scene> file(const std::string&p){
        std::ifstream f(p,std::ios::binary|std::ios::ate);if(!f)throw std::runtime_error("Cannot open scene: "+p);
        auto n=f.tellg();if(n<0||n>128*1024*1024)throw std::runtime_error("Invalid scene file size");
        std::vector<uint8_t>d(static_cast<size_t>(n));f.seekg(0);if(!f.read(reinterpret_cast<char*>(d.data()),n))throw std::runtime_error("Cannot read complete scene");return load(d.data(),d.size());
    }
};
struct Pose {double x=0,y=0,sx=1,sy=1,rz=0,rx=0,ry=0,skew=0,alpha=1,blur=0,maskP=1;int mask=0;double strip=0,glint=0,scan=-10;int substitute=-1;};
inline Pose pose(const Recipe&r,double p,double index,double rank,double strength,uint32_t seed){
    Pose z;if(r.kernel==0)return z;p=clamp(p);z.maskP=p;z.mask=r.mask;
    if(p<=0){z.alpha=0;return z;}if(p>=1)return Pose{};
    double e=ease(p,r.ease),q=1-e,amp=r.amplitude*strength,theta=r.direction*PI/180;
    double env=std::sin(PI*p)*(1-p),j=noise(seed+uint32_t(index)*31),j2=noise(seed+uint32_t(index)*127+91);
    double a=amp*q,angle=r.rotation*PI/180,phase=2*PI*p+index*.65;
    z.alpha=smooth(clamp(p*3.5));
    switch(r.kernel){
    case 1:z.alpha=e;break;
    case 2:z.x=std::cos(theta)*a;z.y=std::sin(theta)*a;break;
    case 3:z.x=std::cos(theta)*a;z.y=std::sin(theta)*a;z.rz=angle*q;break;
    case 4:z.x=std::cos(theta)*a;z.y=std::sin(theta)*a;z.rz=angle*q;z.blur=(1-p)*.95;break;
    case 5:z.sx=z.sy=1-a;z.alpha=smooth(p);break;
    case 6:z.sx=z.sy=std::max(.05,1-a);z.x=std::cos(theta)*amp*.12*q;z.y=std::sin(theta)*amp*.18*q;z.rz=angle*q;break;
    case 7:z.y=a;z.rz=angle*q;break;
    case 8:z.sx=1+amp*.5*q;z.sy=std::max(.08,1-amp*.8*q);z.y=a*.4;break;
    case 9:z.sx=std::max(.08,1-a*.9);z.sy=1+a*.9;z.y=std::sin(theta)*a*.3;break;
    case 10:z.rx=angle*q;z.y=a*.5;break;
    case 11:z.ry=angle*q;z.x=a*.15;break;
    case 12:z.rx=angle*q;z.y=std::sin(theta)*a*.25;break;
    case 13:z.rz=angle*q;z.sx=z.sy=std::max(.05,1-.65*q);z.y=a*.25;break;
    case 14:z.x=amp*q*std::cos(phase);z.y=amp*q*std::sin(phase)*.5;z.rz=angle*q;break;
    case 15:z.x=amp*q*std::sin(PI*p)*(j<.5?-1:1);z.y=std::sin(theta)*a;z.rz=angle*q;break;
    case 16:z.x=a*std::cos(phase*1.2);z.y=a*std::sin(phase*1.2);z.rz=angle*q;z.sx=z.sy=1-.7*q;break;
    case 17:z.x=(j-.5)*2*a;z.y=(j2-.5)*2*a;z.rz=(j-.5)*2*angle*q;break;
    case 18:z.x=(rank-.5)*6*a;break;
    case 19:z.x=std::cos(theta)*a;z.y=amp*env*std::sin(phase)+std::sin(theta)*a*.5;z.rz=angle*env*std::cos(phase);break;
    case 20:z.x=std::cos(theta)*a;z.y=amp*q*std::sin(phase)+std::sin(theta)*a*.4;z.skew=angle*q;break;
    case 21:z.rz=angle*std::sin(p*PI*3+index*.3)*q;z.y=a;break;
    case 22:z.y=a+amp*.4*env*std::sin(p*PI*4+index*.8);z.rz=angle*q;break;
    case 23:{double sign=(int(index)%2)?-1:1;z.x=sign*std::cos(theta)*a;z.y=sign*std::sin(theta)*a;z.rz=sign*angle*q;break;}
    case 24:z.rz=(rank-.5)*2*angle*q;z.x=(rank-.5)*a*2;z.y=a*(1-std::abs(rank-.5));break;
    case 25:z.ry=((int(index)%2)?-1:1)*angle*q;z.x=((int(index)%2)?-1:1)*a*.25;z.y=std::sin(theta)*a*.4;break;
    case 26:z.alpha=r.amplitude>.7?(p>.12?1:0):smooth(p*8);break;
    case 27:if(p<.72)z.substitute=int(hash(seed+uint32_t(index)*99+uint32_t(p*18))&0x7fffffffu);z.alpha=smooth(p*5);break;
    case 28:z.strip=amp*std::sin(p*24)*q*q;break;
    case 29:z.mask=11;z.maskP=p;break;
    case 30:case 31:case 32:case 36:z.alpha=1;break;
    case 33:z.mask=12;z.strip=amp*q*.25;z.alpha=1;break;
    case 34:z.y=a;z.alpha=1;break;
    case 35:z.y=std::sin(theta)*a*.4;z.alpha=1;break;
    case 37:z.mask=12;z.alpha=1;break;
    case 38:z.mask=11;z.maskP=smooth(p);break;
    case 39:z.x=amp*q*q*std::sin(p*45)*.25;z.y=amp*q*q*std::cos(p*37)*.1;z.rz=angle*q*std::sin(p*31);break;
    case 40:z.skew=angle*q;z.x=std::cos(theta)*a;break;
    case 41:z.sx=z.sy=std::max(.02,1-a);z.ry=angle*q;break;
    case 42:z.sx=z.sy=1+amp*q;z.x=(rank-.5)*amp*q*1.3;z.blur=(1-p)*.85;z.alpha=smooth(p);break;
    case 43:z.y=amp*(q-.28*std::pow(std::sin(PI*p*2),2)*(1-p));break;
    case 44:z.x=std::cos(theta)*a;z.rz=angle*q;break;
    case 45:z.x=std::cos(theta)*a;z.y=std::sin(theta)*a+amp*env*.3;z.rz=angle*q;break;
    case 46:z.sx=z.sy=1+amp*q;z.blur=1-p;z.alpha=smooth(p);break;
    case 47:z.x=amp*q*(j-.5)+std::cos(theta)*a*.5;z.y=amp*q*std::sin(p*PI+j2*3)+std::sin(theta)*a*.5;z.rz=angle*q*std::sin(p*3+j);break;
    case 48:z.sx=1+amp*env*std::sin(phase);z.sy=1-amp*env*std::sin(phase);z.y=std::sin(theta)*a*.3;break;
    }z.alpha=clamp(z.alpha);z.sx=clamp(z.sx,.03,3);z.sy=clamp(z.sy,.03,3);return z;
}
 #include "life.hpp"
struct Group {double minx=1e30,miny=1e30,maxx=-1e30,maxy=-1e30;int ordinal=0,count=1;double rank=0;};
struct Mat {double a[9];};
inline Mat multiply(const Mat&a,const Mat&b){Mat out{{0,0,0,0,0,0,0,0,0}};for(int r=0;r<3;r++)for(int c=0;c<3;c++)for(int k=0;k<3;k++)out.a[r*3+c]+=a.a[r*3+k]*b.a[k*3+c];return out;}
inline Mat inverse(const Mat&m){const double*p=m.a;Mat o{{p[4]*p[8]-p[5]*p[7],p[2]*p[7]-p[1]*p[8],p[1]*p[5]-p[2]*p[4],p[5]*p[6]-p[3]*p[8],p[0]*p[8]-p[2]*p[6],p[2]*p[3]-p[0]*p[5],p[3]*p[7]-p[4]*p[6],p[1]*p[6]-p[0]*p[7],p[0]*p[4]-p[1]*p[3]}};double d=p[0]*o.a[0]+p[1]*o.a[3]+p[2]*o.a[6];if(std::abs(d)<1e-12)throw std::runtime_error("Singular text transform");for(double&v:o.a)v/=d;return o;}
inline std::array<double,2> project(const Mat&m,double x,double y){double d=m.a[6]*x+m.a[7]*y+m.a[8];return {{(m.a[0]*x+m.a[1]*y+m.a[2])/d,(m.a[3]*x+m.a[4]*y+m.a[5])/d}};}
inline double maskValue(int mask,double p,double u,double v,uint32_t seed){
    if(mask==0||p>=.999999)return 1;
    if(p<=0)return 0;
    u=clamp(u);v=clamp(v);double threshold=0;
    switch(mask){case 1:threshold=u;break;case 2:threshold=1-u;break;case 3:threshold=1-v;break;case 4:threshold=v;break;case 5:threshold=std::abs(u-.5)*2;break;case 6:threshold=std::abs(v-.5)*2;break;case 7:threshold=std::hypot(u-.5,v-.5)*1.414214;break;case 8:threshold=std::abs(u-.5)+std::abs(v-.5);break;case 9:threshold=(u+1-v)*.5;break;case 10:threshold=std::fmod(v*6,1.);break;case 11:threshold=noise(uint32_t(u*36)+uint32_t(v*22)*83+seed);break;case 12:threshold=(int(v*6)%2)?1-u:u;break;case 13:threshold=std::max(u,1-v);break;default:return 1;}
    return clamp((p-threshold)*140+.5);
}
struct Renderer {
    std::shared_ptr<Scene> scene;std::vector<uint8_t> overlay,temp;std::vector<uint16_t> accum;std::vector<Group> groups,lineBounds;Group block;
    void set(std::shared_ptr<Scene>s){scene=std::move(s);prepare();}
    void prepare(){
        groups.clear();lineBounds.clear();block=Group{};
        if(!scene){std::vector<uint8_t>().swap(overlay);std::vector<uint8_t>().swap(temp);std::vector<uint16_t>().swap(accum);return;}
        if(scene->c.v[22]<=0||scene->c.v[13]==0){std::vector<uint8_t>().swap(temp);std::vector<uint16_t>().swap(accum);}
        groups.resize(scene->sprites.size());auto&ss=scene->sprites;int mode=scene->c.group();
        lineBounds.resize(513);
        for(const auto&s:ss)if(!s.flags){auto&l=lineBounds[s.line];l.minx=std::min(l.minx,double(s.ax));l.maxx=std::max(l.maxx,double(s.ax));l.miny=std::min(l.miny,double(s.ay));l.maxy=std::max(l.maxy,double(s.ay));block.minx=std::min(block.minx,double(s.x));block.maxx=std::max(block.maxx,double(s.x+s.w));block.miny=std::min(block.miny,double(s.y));block.maxy=std::max(block.maxy,double(s.y+s.h));}
        if(block.minx>block.maxx){block.minx=block.miny=0;block.maxx=scene->rw;block.maxy=scene->rh;}
        std::vector<int> keys;for(size_t i=0;i<ss.size();i++)if(!ss[i].flags)keys.push_back(mode==0?int(i):mode==1?ss[i].word:mode==2?ss[i].line:0);
        std::sort(keys.begin(),keys.end());keys.erase(std::unique(keys.begin(),keys.end()),keys.end());
        std::vector<int> permutation(keys.size());for(size_t i=0;i<keys.size();i++)permutation[i]=int(i);
        int order=int(scene->c.v[9]);uint32_t seed=scene->c.seed();
        if(order==4)std::stable_sort(permutation.begin(),permutation.end(),[&](int a,int b){return hash(seed+uint32_t(a)*73)<hash(seed+uint32_t(b)*73);});
        for(size_t i=0;i<ss.size();i++){
            int key=mode==0?int(i):mode==1?ss[i].word:mode==2?ss[i].line:0;auto&g=groups[i];
            g.count=std::max(1,int(keys.size()));g.ordinal=int(std::lower_bound(keys.begin(),keys.end(),key)-keys.begin());
            for(size_t j=0;j<ss.size();j++){
                int k=mode==0?int(j):mode==1?ss[j].word:mode==2?ss[j].line:0;
                if((ss[i].flags||k==key)&&!ss[j].flags){g.minx=std::min(g.minx,double(ss[j].x));g.miny=std::min(g.miny,double(ss[j].y));g.maxx=std::max(g.maxx,double(ss[j].x+ss[j].w));g.maxy=std::max(g.maxy,double(ss[j].y+ss[j].h));}
            }
            if(g.minx>g.maxx){g.minx=ss[i].x;g.maxx=ss[i].x+ss[i].w;g.miny=ss[i].y;g.maxy=ss[i].y+ss[i].h;}
            double rank=g.count>1?double(g.ordinal)/(g.count-1):0;
            if(order==1)rank=1-rank;
            if(order==2)rank=std::abs(rank-.5)*2;
            if(order==3)rank=1-std::abs(rank-.5)*2;
            if(order==4)rank=g.count>1?double(std::find(permutation.begin(),permutation.end(),g.ordinal)-permutation.begin())/(g.count-1):0;
            if(order==5)rank=g.count>1?double((g.ordinal%2)*(g.count+1)/2+g.ordinal/2)/(g.count-1):0;
            // Normalize center/edges orders so one group always starts at phase zero.
            if((order==2||order==3)&&g.count>1&&g.count%2==0){double d=1.0/(g.count-1);if(order==2)rank=(rank-d)/std::max(1e-9,1-d);else rank=rank/std::max(1e-9,1-d);}
            g.rank=clamp(rank);if(ss[i].flags){g.rank=0;g.ordinal=0;g.count=1;}
        }
    }
    struct State{Pose primary,secondary,body;double p=1,font=96,lifeEcho=0;int id=0;bool active=false;};
    State state(size_t i,double t,double duration)const{
        const auto&c=scene->c;const auto&g=groups[i];const auto&s=scene->sprites[i];State a;
        a.font=clamp(double(c.v[28]),12,512);double di=c.in()!=0?c.v[1]:0,doo=(c.out()==0||(c.out()==-1&&c.in()==0))?0:c.v[2];
        if(di+doo>duration*.9){double fac=duration*.9/std::max(.001,di+doo);di*=fac;doo*=fac;}
        double phase=1;
        if(t<0||t>=duration){a.primary.alpha=0;return a;}
        if(c.in()!=0&&di>0&&t<di){a.id=c.in();phase=t/di;a.active=true;}
        else if(c.out()!=0&&doo>0&&t>duration-doo){a.id=c.out()<0?c.in():c.out();phase=(duration-t)/doo;a.active=true;}
        // No entrance + reverse means no exit, exactly as requested.
        if(a.id==0)a.active=false;
        double lag=clamp(c.v[4],0,.9)*.72;
        a.p=a.active?clamp((phase-lag*g.rank)/(1-lag)):1;
        double idx=s.flags?0:g.ordinal,rank=g.count>1?double(g.ordinal)/(g.count-1):.5;
        // Spacing acts on glyph positions even when the timing unit is a whole line.
        auto rankFor=[&](int id){int k=recipe(id).kernel;return (k==18||k==42)&&c.group()!=0&&!s.flags?clamp((s.ax-g.minx)/std::max(1.,g.maxx-g.minx)):rank;};
        a.primary=pose(recipe(a.id),a.p,idx,rankFor(a.id),c.v[3],c.seed());
        if(a.active&&c.v[14]>0&&c.v[15]>0){
            a.secondary=pose(recipe(int(c.v[14])),a.p,idx,rankFor(int(c.v[14])),c.v[3],c.seed());double k=c.v[15];
            a.primary.x+=a.secondary.x*k;a.primary.y+=a.secondary.y*k;a.primary.rz+=a.secondary.rz*k;a.primary.rx+=a.secondary.rx*k;a.primary.ry+=a.secondary.ry*k;a.primary.skew+=a.secondary.skew*k;
            a.primary.sx*=mix(1,a.secondary.sx,k);a.primary.sy*=mix(1,a.secondary.sy,k);a.primary.alpha*=mix(1,a.secondary.alpha,k);a.primary.blur=std::max(a.primary.blur,a.secondary.blur*k);
        }
        if(a.active)a.primary.blur=std::max(a.primary.blur,double(c.v[16])*(1-a.p));
        // Lifecycle: transition and hold share one continuous, absolute-time clock.
        // Life starts from the entrance character, never switches abruptly to the exit's.
        double envelope=(di>0?smooth(t/di):1)*(doo>0?smooth((duration-t)/doo):1);
        const auto&line=lineBounds[s.line];
        double u=c.group()==3?0:c.group()==2?(g.count>1?double(g.ordinal)/(g.count-1)-.5:0):
            clamp(((g.minx+g.maxx)*.5-line.minx)/std::max(1.,line.maxx-line.minx))-.5;
        if(c.group()==0&&!s.flags)u=clamp((s.ax-line.minx)/std::max(1.,line.maxx-line.minx))-.5;
        int mode=int(c.v[10]);double amount=c.v[11]*envelope;
        auto apply=[&](const LifeRecipe&lr,double weight){
            auto life=living(lr,t,clamp(c.v[12],.2,2),u,s.flags?0:g.ordinal,c.seed());
            double k=amount*weight;addLifePose(a.body,life.body,k);
            if(!s.flags){
                addLifePose(a.primary,life.local,k);a.lifeEcho+=life.echo*k;
                // Tracking is a bounded offset inside each line, never an accumulated advance.
                double q=(s.ax-(line.minx+line.maxx)*.5)/std::max(1.,line.maxx-line.minx);
                a.primary.x+=q*life.track*k;
            }
        };
        if(mode==9&&amount>0){
            int source=int(c.v[18]);if(source==0)source=c.in()>0?c.in():0;
            double other=c.v[18]==0&&c.v[14]>0?c.v[15]:0;
            apply(LIFE_RECIPES[source],1-other*.5);
            if(other>0)apply(LIFE_RECIPES[int(c.v[14])],other*.5);
        }else if(mode>0&&amount>0){
            static const int types[9]={0,8,4,3,7,6,10,2,5};
            apply(LifeRecipe{types[std::min(mode,8)],5.5f,.82f,2.f,.16f},1);
        }
        a.primary.alpha*=c.v[21];return a;
    }
    Mat matrix(size_t i,const State&a,int W,int H)const{
        const auto&s=scene->sprites[i];const auto&g=groups[i];const auto&p=a.primary;
        double px=(g.minx+g.maxx)/2,py=(g.miny+g.maxy)/2;
        if(scene->c.group()==0&&!s.flags){px=s.ax;py=s.ay;}
        double cx=std::cos(p.rx),sx=std::sin(p.rx),cy=std::cos(p.ry),sy=std::sin(p.ry),cz=std::cos(p.rz),sz=std::sin(p.rz);
        double r00=cz*cy,r01=cz*sy*sx-sz*cx,r10=sz*cy,r11=sz*sy*sx+cz*cx,r20=-sy,r21=cy*sx;
        double scaleX=p.sx,scaleY=p.sy,sh=std::tan(clamp(p.skew,-.7,.7));
        double A=r00*scaleX,B=(r00*sh+r01)*scaleY,C=r10*scaleX,D=(r10*sh+r11)*scaleY,F=std::max(600.,a.font*7);
        double G=r20*scaleX/F,J=(r20*sh+r21)*scaleY/F;
        double tx=px+p.x*a.font,ty=py+p.y*a.font;
        double ux=s.x-px,uy=s.y-py,du=s.w/s.pw,dv=s.h/s.ph;
        double sxr=double(W)/scene->rw,syr=double(H)/scene->rh;
        Mat local{{(A+tx*G)*du*sxr,(B+tx*J)*dv*sxr,((A+tx*G)*ux+(B+tx*J)*uy+tx)*sxr,(C+ty*G)*du*syr,(D+ty*J)*dv*syr,((C+ty*G)*ux+(D+ty*J)*uy+ty)*syr,G*du,J*dv,1+G*ux+J*uy}};
        // A shared block transform prevents the old per-letter "breathing gaps" bug.
        const auto&b=a.body;double bx=(block.minx+block.maxx)*.5*sxr,by=(block.miny+block.maxy)*.5*syr;
        double bc=std::cos(b.rz),bs=std::sin(b.rz),aa=bc*b.sx,bb=-bs*b.sy,cc=bs*b.sx,dd=bc*b.sy;
        Mat body{{aa,bb,bx-aa*bx-bb*by+b.x*a.font*sxr,cc,dd,by-cc*bx-dd*by+b.y*a.font*syr,0,0,1}};
        return multiply(body,local);
    }
    void draw(size_t i,const State&a,int W,int H,std::vector<uint8_t>&dst,double extraAlpha=1){
        const auto&s=scene->sprites[i];const auto&g=groups[i];const Pose&p=a.primary;
        if(p.alpha*extraAlpha<.001)return;
        Mat m=matrix(i,a,W,H);auto inv=inverse(m);
        double minx=1e30,miny=1e30,maxx=-1e30,maxy=-1e30;
        for(auto q:std::array<std::array<double,2>,4>{{{{0,0}},{{double(s.pw),0}},{{0,double(s.ph)}},{{double(s.pw),double(s.ph)}}}}){auto pt=project(m,q[0],q[1]);minx=std::min(minx,pt[0]);miny=std::min(miny,pt[1]);maxx=std::max(maxx,pt[0]);maxy=std::max(maxy,pt[1]);}
        if(!std::isfinite(minx)||!std::isfinite(maxx)||!std::isfinite(miny)||!std::isfinite(maxy))return;
        int x0=int(clamp(std::floor(minx)-1,0,W)),x1=int(clamp(std::ceil(maxx)+1,0,W)),y0=int(clamp(std::floor(miny)-1,0,H)),y1=int(clamp(std::ceil(maxy)+1,0,H));
        const Sprite*texture=&s;
        if(p.substitute>=0&&!s.flags){size_t index=size_t(p.substitute)%scene->sprites.size();if(!scene->sprites[index].flags)texture=&scene->sprites[index];}
        const auto&sp=*texture;const std::vector<uint8_t>*tex0=&sp.pixels,*tex1=tex0;double blend=0;
        if(p.blur>.01&&!sp.blur[0].empty()){
            double q=clamp(p.blur)*3;int k=std::min(2,int(q));blend=q-k;tex0=k?&sp.blur[k-1]:&sp.pixels;tex1=&sp.blur[k];
        }
        bool masking=p.mask!=0||a.secondary.mask!=0;
        bool stationaryClip=recipe(a.id).kernel==34&&a.p<.999999;
        double baseAlpha=clamp(p.alpha*extraAlpha),secondaryMix=scene->c.v[15];
        for(int y=y0;y<y1;y++){
            double X=inv.a[0]*(x0+.5)+inv.a[1]*(y+.5)+inv.a[2],Y=inv.a[3]*(x0+.5)+inv.a[4]*(y+.5)+inv.a[5],Z=inv.a[6]*(x0+.5)+inv.a[7]*(y+.5)+inv.a[8];
            for(int x=x0;x<x1;x++,X+=inv.a[0],Y+=inv.a[3],Z+=inv.a[6]){
                if(stationaryClip){double worldX=(x+.5)*scene->rw/W,worldY=(y+.5)*scene->rh/H;if(worldX<g.minx||worldX>g.maxx||worldY<g.miny||worldY>g.maxy)continue;}
                if(std::abs(Z)<1e-9)continue;
                double u=X/Z-.5,v=Y/Z-.5;
                if(p.strip!=0)u+=p.strip*a.font*(((int(v/std::max(1.,s.ph/7.))%2)?-1:1));
                if(u<0||v<0||u>s.pw-1||v>s.ph-1)continue;
                double op=baseAlpha;
                if(masking){double gu=(s.x+(u+.5)*s.w/s.pw-g.minx)/std::max(1.,g.maxx-g.minx),gv=(s.y+(v+.5)*s.h/s.ph-g.miny)/std::max(1.,g.maxy-g.miny);
                    op*=maskValue(p.mask,p.maskP,gu,gv,scene->c.seed());op*=mix(1,maskValue(a.secondary.mask,a.secondary.maskP,gu,gv,scene->c.seed()),secondaryMix);if(op<.001)continue;}
                u=u*sp.pw/s.pw;v=v*sp.ph/s.ph;int ix=std::min(int(u),int(sp.pw)-1),iy=std::min(int(v),int(sp.ph)-1);int ix1=std::min(ix+1,int(sp.pw)-1),iy1=std::min(iy+1,int(sp.ph)-1);
                double fx=u-ix,fy=v-iy,w0=(1-fx)*(1-fy),w1=fx*(1-fy),w2=(1-fx)*fy,w3=fx*fy;
                size_t t0=(size_t(iy)*sp.pw+ix)*4,t1=(size_t(iy)*sp.pw+ix1)*4,t2=(size_t(iy1)*sp.pw+ix)*4,t3=(size_t(iy1)*sp.pw+ix1)*4;
                auto sample=[&](int c){double v0=(*tex0)[t0+c]*w0+(*tex0)[t1+c]*w1+(*tex0)[t2+c]*w2+(*tex0)[t3+c]*w3;if(blend>0)v0=mix(v0,(*tex1)[t0+c]*w0+(*tex1)[t1+c]*w1+(*tex1)[t2+c]*w2+(*tex1)[t3+c]*w3,blend);return v0*op;};
                double alpha=sample(3);if(alpha<.01)continue;
                double light=1+p.glint;
                if(p.scan>-2){double gu=(s.x+(u+.5)*s.w/s.pw-block.minx)/std::max(1.,block.maxx-block.minx);light=1+p.glint*smooth(1-std::abs(gu-p.scan)/.16);}
                double one=1-alpha/255;size_t d=(size_t(y)*W+x)*4;
                for(int c=0;c<3;c++)dst[d+c]=uint8_t(clamp(std::round(std::min(alpha,sample(c)*light)+dst[d+c]*one),0,255));
                dst[d+3]=uint8_t(clamp(std::round(alpha+dst[d+3]*one),0,255));
            }
        }
    }
    void sample(double t,double duration,int W,int H,std::vector<uint8_t>&dst){
        dst.assign(size_t(W)*H*4,0);if(!scene||t<0||t>=duration)return;
        for(size_t i=0;i<scene->sprites.size();i++){
            auto p=state(i,t,duration);double echo=scene->c.v[17];if(recipe(p.id).kernel==45)echo=std::max(.65,echo);
            if(echo>0&&p.active){for(int k=2;k>=1;k--){double dt=(scene->c.out()==-1&&t>duration/2)?.045*k:-.045*k;auto prev=state(i,t+dt,duration);draw(i,prev,W,H,dst,echo*(k==2?.10:.18));}}
            if(p.lifeEcho>.001&&!scene->sprites[i].flags){
                auto tail=state(i,t-.08,duration);tail.primary.x-=.032;draw(i,tail,W,H,dst,p.lifeEcho*.12);
            }
            draw(i,p,W,H,dst);
        }
    }
    void render(double t,int W,int H,const uint8_t*in,uint8_t*out,double durationOverride=0){
        if(W<1||H<1||W>7680||H>7680||uint64_t(W)*H>33554432)throw std::runtime_error("Output dimensions exceed safety limit");
        size_t n=size_t(W)*H*4;if(!scene){if(in&&in!=out)std::memcpy(out,in,n);else if(!in)std::memset(out,0,n);return;}
        double duration=durationOverride>0?clamp(durationOverride,.1,120):scene->c.duration();
        int samples=scene->c.v[22]>0?(scene->c.v[13]>=2?5:3):1;
        if(scene->c.v[13]==0)samples=1;
        if(samples==1)sample(t,duration,W,H,overlay);
        else{
            accum.assign(n,0);double shutter=.5*scene->c.v[22]*scene->c.v[24]/scene->c.v[23];
            for(int j=0;j<samples;j++){sample(t+shutter*(double(j)/(samples-1)-.5),duration,W,H,temp);for(size_t k=0;k<n;k++)accum[k]+=temp[k];}
            overlay.resize(n);for(size_t k=0;k<n;k++)overlay[k]=uint8_t((accum[k]+samples/2)/samples);
        }
        // Single compositing pass. The host uses straight RGBA; do not assume an opaque input.
        for(size_t i=0;i<n;i+=4){unsigned a=overlay[i+3];
            if(a==0){if(in){if(out!=in)std::memcpy(out+i,in+i,4);}else std::memset(out+i,0,4);continue;}
            double ia=in?in[i+3]/255.:0,one=1-a/255.,oa=a+ia*255*one;
            for(int c=0;c<3;c++){double prem=overlay[i+c]+(in?in[i+c]*ia*one:0);out[i+c]=uint8_t(clamp(std::round(prem*255/oa),0,255));}
            out[i+3]=uint8_t(clamp(std::round(oa),0,255));
        }
    }
};
} // namespace sunimo
