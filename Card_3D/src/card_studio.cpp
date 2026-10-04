/* Card 3D 1.0, MIT.
 * Single deterministic Frei0r RGBA filter. Modules: source fitting, material,
 * entrance, rigid perspective, stylized slab, silhouette shadow, compositing.
 * No files/network, no Qt/OpenCV/GPU dependency, no playback-history state.
 * Cached data never changes the result. C ABI catches all allocation errors.
 */
#include "frei0r_minimal.h"
#include "renderer_parameters.hpp"
#include "parameters.hpp"
#include "motion.hpp"
#include <vector>
#include <array>
#include <mutex>
#include <new>
#include <limits>

namespace card3d {
constexpr size_t MAX_PIXELS=16777216; // up to UHD 4K; explicit bounded memory
struct Pixel { float r=0,g=0,b=0,a=0; };
struct Tex { int w=0,h=0; std::vector<uint8_t> data; };
struct Box { int x=0,y=0,w=0,h=0; bool operator==(const Box& b)const{return x==b.x&&y==b.y&&w==b.w&&h==b.h;} };
struct Anim { double progress=1,opacity=1,remain=0,scale=1,sx=1,sy=1,shear=0,taper=0; int kind=0,dirx=-1,diry=1; Pose pose{}; };
struct Mat { double f[9]{},i[9]{}; double minx=0,maxx=0,miny=0,maxy=0,lod=0; };
static double sat(double x){return clampd(x,0,1);}
static double smooth(double x){x=sat(x);return x*x*x*(x*(6*x-15)+10);}
static uint8_t byte(double x){return (uint8_t)clampd(x*255+.5,0,255);}
static double vnoise(double x,uint32_t seed){
    int n=(int)floor(x); double f=x-n; f=f*f*(3-2*f);
    double a=(double)hash32((uint32_t)n+seed)/4294967295.;
    double b=(double)hash32((uint32_t)(n+1)+seed)/4294967295.;
    return a+(b-a)*f;
}
static double fiber(double x,uint32_t seed){
    return .49*vnoise(x*31,seed)+.28*vnoise(x*93,seed+991)+.16*vnoise(x*221,seed+1553)+.07*vnoise(x*487,seed+9919);
}
static double rounded_sdf(double x,double y,double w,double h,double radius){
    double qx=fabs(x-w*.5)-(w*.5-radius),qy=fabs(y-h*.5)-(h*.5-radius);
    double ex=std::max(qx,0.),ey=std::max(qy,0.);
    return sqrt(ex*ex+ey*ey)+std::min(std::max(qx,qy),0.)-radius;
}
static Pixel sample(const Tex& t,double x,double y){
    if (!isfinite(x)||!isfinite(y)||x<=-1||y<=-1||x>=t.w||y>=t.h)return {};
    int x0=(int)floor(x),y0=(int)floor(y);
    float fx=(float)(x-x0),fy=(float)(y-y0);
    float ws[4]={(1-fx)*(1-fy),fx*(1-fy),(1-fx)*fy,fx*fy};
    Pixel r;
    for(int k=0;k<4;k++){
        int xx=x0+(k&1),yy=y0+(k>>1);
        if(xx<0||yy<0||xx>=t.w||yy>=t.h)continue;
        const auto* p=&t.data[4*((size_t)yy*t.w+xx)]; float w=ws[k]/255.f;
        r.r+=p[0]*w;r.g+=p[1]*w;r.b+=p[2]*w;r.a+=p[3]*w;
    }
    return r;
}
static Pixel mix(Pixel a,Pixel b,double t){float f=(float)t,g=1-f;return {g*a.r+f*b.r,g*a.g+f*b.g,g*a.b+f*b.b,g*a.a+f*b.a};}
static void over(Pixel& d,Pixel s){float q=1-s.a;d={s.r+q*d.r,s.g+q*d.g,s.b+q*d.b,s.a+q*d.a};}

class Studio {
public:
    unsigned w,h; double p[PARAMS]{}; f0r_param_color_t colors[PARAMS]{};
    double controls[PUBLIC_PARAMS]{};
    std::mutex lock; Organic motion;
    Box crop{},oldcrop{}; int pad=0,cw=0,ch=0,sw=0,sh=0,halo=0;
    std::vector<Tex> mip; std::vector<float> mask,innerMask,grain,shadow,tmp;
    std::vector<Pixel> surface; std::vector<uint8_t> aliased,lastInput;
    bool textureDirty=true; int validMips=0;
    bool materialDirty=true,fitDirty=true; double fit=1;
    explicit Studio(unsigned W,unsigned H):w(W),h(H){
        for(int i=0;i<PARAMS;i++)p[i]=SPECS[i].def;
        colors[FRAME_COLOR]={238.f/255,234.f/255,226.f/255};
        colors[SIDE_COLOR]={37.f/255,41.f/255,51.f/255}; init_noise(&motion);
        for(int i=0;i<PUBLIC_PARAMS;i++)controls[i]=PUBLIC_SPECS[i].def;
        apply_controls();
    }
    double val(int k)const{return SPECS[k].lo+(SPECS[k].hi-SPECS[k].lo)*p[k];}
    int choice(int k)const{return (int)floor(val(k)+.5);}
    void set(int k,const void* ptr){
        if(k<0||k>=PARAMS||!ptr)return;
        if(SPECS[k].type==F0R_PARAM_COLOR){
            auto x=*(const f0r_param_color_t*)ptr;
            if(isfinite(x.r)&&isfinite(x.g)&&isfinite(x.b)){
                x={(float)sat(x.r),(float)sat(x.g),(float)sat(x.b)};
                if(colors[k].r!=x.r||colors[k].g!=x.g||colors[k].b!=x.b){colors[k]=x;textureDirty=true;}
            }
        }else{
            double x=*(const double*)ptr;if(!isfinite(x))return; x=sat(x);
            if(p[k]==x)return;
            p[k]=x;
            if(k==SHADOW&&val(SHADOW)==0){std::vector<float>().swap(shadow);std::vector<float>().swap(tmp);}
            if(k==EDGE||k==RADIUS||k==BORDER||k==TEAR||k==VARIANT)materialDirty=true;
            if(materialDirty||k==PLATE||k==AUTOCROP)textureDirty=true;
            if(k==SWING||k==ORGANIC||k==BASE_YAW||k==BASE_PITCH)fitDirty=true;
        }
    }
    void set_value(int k,double value){
        double normalized=(value-SPECS[k].lo)/(SPECS[k].hi-SPECS[k].lo);
        set(k,&normalized);
    }
    void apply_controls(){
        // All preset values live here. Always set the whole preset so switching
        // paper -> photo -> panel is independent of playback and edit history.
        int style=selected_control(UI_STYLE),movement=selected_control(UI_MOTION);
        set_value(ENTRANCE,selected_control(UI_ENTRANCE));set_value(CORNER,selected_control(UI_CORNER));
        set_value(PLACEMENT,selected_control(UI_POSITION_MODE)==1?7:selected_control(UI_PLACEMENT));
        set_value(SIZE,controls[UI_SIZE]*PUBLIC_SPECS[UI_SIZE].hi);
        set_value(DURATION,controls[UI_DURATION]*PUBLIC_SPECS[UI_DURATION].hi);
        set_value(DELAY,0);set_value(DEFORM,45);
        set_value(X,controls[UI_X]*PUBLIC_SPECS[UI_X].hi);set_value(Y,controls[UI_Y]*PUBLIC_SPECS[UI_Y].hi);
        set_value(EDGE,style==1?1:0);set_value(RADIUS,controls[UI_ROUNDING]*PUBLIC_SPECS[UI_ROUNDING].hi);
        set_value(BORDER,style==0?.5:style==1?4:3);set_value(TEAR,style==1?1.5:0);
        set_value(DEPTH,style==0?.7:style==1?.12:.2);
        f0r_param_color_t frame=style==0?f0r_param_color_t{182.f/255,195.f/255,209.f/255}:
            style==1?f0r_param_color_t{242.f/255,236.f/255,219.f/255}:f0r_param_color_t{1,1,1};
        f0r_param_color_t side={37.f/255,41.f/255,51.f/255};
        set(FRAME_COLOR,&frame);set(SIDE_COLOR,&side);
        set_value(SHADOW,controls[UI_SHADOW]*PUBLIC_SPECS[UI_SHADOW].hi);set_value(SOFTNESS,1.6);set_value(DISTANCE,1);
        set_value(SWING,movement==0?0:movement==1?5:10);
        set_value(PERIOD,movement==2?6:8);set_value(ORGANIC,movement==0?0:movement==1?50:75);
        set_value(BASE_YAW,movement==0?0:-6);set_value(BASE_PITCH,movement==0?0:3);
        set_value(VARIANT,0);set_value(PLATE,selected_control(UI_BACKGROUND)==0?100:0);set_value(AUTOCROP,selected_control(UI_SOURCE)==0?1:0);
    }
    int selected_control(int k)const{return (int)std::round(controls[k]*PUBLIC_SPECS[k].hi);}
    void set_control(int k,const void* ptr){
        if(k<0||k>=PUBLIC_PARAMS||!ptr)return;
        double value=*(const double*)ptr;if(!isfinite(value))return;
        const auto& spec=PUBLIC_SPECS[k];
        value=clampd(value,spec.lo/spec.hi,1);
        if(spec.menu)value=std::round(value*spec.hi)/spec.hi;
        if(controls[k]==value)return;
        controls[k]=value;apply_controls();
    }
    Box detect(const uint8_t* src){
        if(choice(AUTOCROP)==0)return {0,0,(int)w,(int)h};
        int minx=w,miny=h,maxx=-1,maxy=-1;
        // Scan every pixel, not "first frame" sampling: seeks are deterministic.
        for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++)if(src[4*((size_t)y*w+x)+3]>1){
            minx=std::min(minx,(int)x);maxx=std::max(maxx,(int)x);miny=std::min(miny,(int)y);maxy=std::max(maxy,(int)y);
        }
        if(maxx<minx||maxy<miny)return {};
        return {minx,miny,maxx-minx+1,maxy-miny+1};
    }
    void motion_setup(){
        motion.w=cw?cw:1;motion.h=ch?ch:1;
        motion.p[0]=val(SWING)/30;motion.p[1]=val(PERIOD)/20;motion.p[2]=val(ORGANIC)/100;
        motion.p[3]=(val(BASE_YAW)+35)/70;motion.p[4]=(val(BASE_PITCH)+20)/40;
        if(fitDirty&&cw&&ch){fit=safe_fit(&motion);fitDirty=false;}
    }
    void material(const uint8_t* src){
        size_t bytes=(size_t)w*h*4;
        if(!textureDirty&&lastInput.size()==bytes&&memcmp(lastInput.data(),src,bytes)==0){motion_setup();return;}
        lastInput.assign(src,src+bytes);textureDirty=false;validMips=0;
        crop=detect(src);if(!crop.w||!crop.h){cw=ch=0;return;}
        int nPad=(int)ceil(std::min(crop.w,crop.h)*val(BORDER)/100.);
        if(!(crop==oldcrop)||pad!=nPad){materialDirty=true;fitDirty=true;}
        pad=nPad;cw=crop.w+2*pad;ch=crop.h+2*pad;
        if((size_t)cw*ch>MAX_PIXELS*2)throw std::bad_alloc();
        if(mip.empty())mip.resize(1);
        mip[0].w=cw;mip[0].h=ch;mip[0].data.resize((size_t)cw*ch*4);
        if(materialDirty){
            size_t count=(size_t)cw*ch;mask.resize(count);innerMask.resize(count);grain.resize(count);
            double shortSide=std::min(cw,ch),radius=shortSide*val(RADIUS)/100.;
            double amp=choice(EDGE)?shortSide*val(TEAR)/100.:0;
            uint32_t seed=hash32((uint32_t)choice(VARIANT)+1234567);
            for(int y=0;y<ch;y++)for(int x=0;x<cw;x++){
                size_t n=(size_t)y*cw+x;double xx=x+.5,yy=y+.5;
                double sd=rounded_sdf(xx,yy,cw,ch,radius);
                if(amp>0){
                    double ds[4]={xx,cw-xx,yy,ch-yy};int side=0;
                    for(int z=1;z<4;z++)if(ds[z]<ds[side])side=z;
                    double q=side<2?yy/shortSide:xx/shortSide;
                    sd+=amp*(.18+.82*fiber(q,seed+(side+1)*12347));
                }
                mask[n]=(float)sat(.5-sd);
                double di=rounded_sdf(xx-pad,yy-pad,crop.w,crop.h,std::max(0.,radius-pad));
                innerMask[n]=pad>0?(float)sat(.5-di):1.f;
                // Paper grain only affects the frame/plate, never baked into footage.
                grain[n]=choice(EDGE)?(float)(.983+.034*vnoise((xx+yy*1.618)/2,seed+939)):1.f;
            }
            materialDirty=false;oldcrop=crop;
        }
        const auto fc=colors[FRAME_COLOR];double plate=val(PLATE)/100.;
        auto* dst=mip[0].data.data();
        for(int y=0;y<ch;y++)for(int x=0;x<cw;x++){
            size_t n=(size_t)y*cw+x;double ma=mask[n];
            if(ma<=0){memset(dst+4*n,0,4);continue;}
            int ix=x-pad,iy=y-pad;double sa=0,rr=0,gg=0,bb=0,inside=innerMask[n];
            if(ix>=0&&iy>=0&&ix<crop.w&&iy<crop.h){
                const auto* in=src+4*((size_t)(iy+crop.y)*w+ix+crop.x);
                sa=in[3]/255.;rr=in[0]/255.*sa;gg=in[1]/255.*sa;bb=in[2]/255.*sa;
            }
            // Colored outside margin; adjustable plate under transparent source.
            double backing=1-inside+inside*plate*(1-sa),a=inside*sa+backing;
            dst[4*n]=byte(ma*(inside*rr+std::min(1.,(double)fc.r*grain[n])*backing));
            dst[4*n+1]=byte(ma*(inside*gg+std::min(1.,(double)fc.g*grain[n])*backing));
            dst[4*n+2]=byte(ma*(inside*bb+std::min(1.,(double)fc.b*grain[n])*backing));
            dst[4*n+3]=byte(ma*a);
        }
        motion_setup();
    }
    Anim animation(double time)const{
        Anim a;double t=isfinite(time)?clampd(time,-1e8,1e8):0;
        a.kind=choice(ENTRANCE);int corner=choice(CORNER);
        a.dirx=(corner==0||corner==2)?1:-1;a.diry=corner<2?1:-1;
        double dur=a.kind?val(DURATION):0,local=t-val(DELAY);
        a.progress=dur>0?sat(local/dur):1;
        a.opacity=local<0?0:(a.kind?smooth(a.progress/.16):1);
        double u=a.progress,q=1-u;double deformation=val(DEFORM)/100;
        a.remain=a.kind?(q*q*q*q):0; // monotonic ease-out; no positional overshoot
        if(a.kind==2){
            double env=std::min(1.,3*sin(PI*u)*q*q)*deformation;
            a.sx=1+.13*env;a.sy=1-.07*env;
            a.shear=a.dirx*a.diry*.16*env;a.taper=a.dirx*.035*env;
        }else if(a.kind==3){
            a.scale=1-.30*q*q*q;a.shear=a.dirx*a.diry*.07*deformation*q*q;
        }else if(a.kind==4){
            a.shear=a.dirx*a.diry*.035*deformation*q*q;
        }
        double idle=std::max(0.,local-dur),ramp=smooth(idle/.7);
        Pose moving=pose_at(&motion,idle+val(VARIANT)*.731);
        a.pose={val(BASE_YAW)+ramp*(moving.yaw-val(BASE_YAW)),
                val(BASE_PITCH)+ramp*(moving.pitch-val(BASE_PITCH)),ramp*moving.roll};
        if(selected_control(UI_EXIT_ENABLED)==1){
            const double exitAt=controls[UI_EXIT_AT]*PUBLIC_SPECS[UI_EXIT_AT].hi;
            const double exitDuration=controls[UI_EXIT_DURATION]*PUBLIC_SPECS[UI_EXIT_DURATION].hi;
            const double exitStart=std::max(0.,exitAt-exitDuration);
            double exit=exitAt>0?sat((t-exitStart)/std::max(.001,exitAt-exitStart)):0;
            if(exit>0){
                a.kind=selected_control(UI_EXIT_ANIMATION);if(a.kind==0)a.kind=choice(ENTRANCE);
                double exitU=1-exit,exitQ=exit,exitDeform=val(DEFORM)/100;
                a.progress=exitU;a.opacity=a.kind?smooth(exitU/.16):1;a.remain=a.kind?exitQ*exitQ*exitQ*exitQ:0;
                a.scale=a.sx=a.sy=1;a.shear=a.taper=0;
                if(a.kind==2){
                    double env=std::min(1.,3*sin(PI*exitU)*exitQ*exitQ)*exitDeform;
                    a.sx=1+.13*env;a.sy=1-.07*env;
                    a.shear=a.dirx*a.diry*.16*env;a.taper=a.dirx*.035*env;
                }else if(a.kind==3){
                    a.scale=1-.30*exitQ*exitQ*exitQ;a.shear=a.dirx*a.diry*.07*exitDeform*exitQ*exitQ;
                }else if(a.kind==4){
                    a.shear=a.dirx*a.diry*.035*exitDeform*exitQ*exitQ;
                }
            }
        }
        return a;
    }
    void allocate(){
        double m=std::min(w,h); double sigma=m*val(SOFTNESS)/100.;
        halo=(int)ceil(3*sigma+m*val(DISTANCE)/100.+m*.03+5);
        if(val(SHADOW)==0)halo=(int)ceil(m*.03+5);
        sw=(int)w+2*halo;sh=(int)h+2*halo;
        if((size_t)sw*sh>MAX_PIXELS*3)throw std::bad_alloc();
        surface.assign((size_t)sw*sh,Pixel{});
        if(val(SHADOW)>0){shadow.resize((size_t)sw*sh);tmp.resize((size_t)sw*sh);}
    }
    Mat matrix(const Anim& a,double z)const{
        double L=std::max(cw,ch),ax=cw/L,ay=ch/L,r[9];rotation(a.pose,r);
        // Use continuous normalized units for layout. Integer buffer guards must
        // not make preview/proxy and export have different card sizes.
        double m=std::min(w,h);
        double margin=m*(.025+.03+(val(SHADOW)>0?(3*val(SOFTNESS)+val(DISTANCE))/100.:0));
        double availw=std::max(16.,w-2*margin),availh=std::max(16.,h-2*margin);
        double unit=val(SIZE)/100.*std::min(availw/(2*ax),availh/(2*ay));
        double bx=unit*ax,by=unit*ay,F=unit*fit*a.scale;
        int placement=choice(PLACEMENT);double px=.5,py=.5;
        if(placement==1||placement==3||placement==5)px=0;
        if(placement==2||placement==4||placement==6)px=1;
        if(placement==3||placement==4)py=0;
        if(placement==5||placement==6)py=1;
        if(placement==7){px=val(X)/100.;py=val(Y)/100.;}
        double tx=margin+bx+px*std::max(0.,w-2*(margin+bx));
        double ty=margin+by+py*std::max(0.,h-2*(margin+by));
        double startx=a.dirx>0?w+bx+halo:-bx-halo;
        double starty=a.diry>0?h+by+halo:-by-halo;
        if(a.kind==3){startx=a.dirx>0?w:w*.0;starty=a.diry>0?h:0;}
        if(a.kind==4){startx=tx+a.dirx*(double)w*.065;starty=ty+a.diry*(double)h*.065;}
        tx+=(startx-tx)*a.remain;ty+=(starty-ty)*a.remain;
        tx+=halo;ty+=halo;
        // Rigid projected plate followed by a transient projective skew/stretch.
        double r0=r[0]*a.sx,r1=r[0]*a.shear+r[1]*a.sy;
        double r3=r[3]*a.sx,r4=r[3]*a.shear+r[4]*a.sy;
        double r6=r[6]*a.sx,r7=r[6]*a.shear+r[7]*a.sy;
        Mat M;double d0=-r6+a.taper,d1=-r7,d2=CAMERA-r[8]*z;
        M.f[0]=F*CAMERA*r0+tx*d0;M.f[1]=F*CAMERA*r1+tx*d1;M.f[2]=F*CAMERA*r[2]*z+tx*d2;
        M.f[3]=F*CAMERA*r3+ty*d0;M.f[4]=F*CAMERA*r4+ty*d1;M.f[5]=F*CAMERA*r[5]*z+ty*d2;
        M.f[6]=d0;M.f[7]=d1;M.f[8]=d2;
        if(!inverse3(M.f,M.i))throw std::bad_alloc();
        M.minx=M.miny=1e20;M.maxx=M.maxy=-1e20;double footprint=1;
        for(int k=0;k<5;k++){
            double u=k==4?0:(k&1?ax:-ax),v=k==4?0:(k&2?ay:-ay);
            double d=M.f[6]*u+M.f[7]*v+M.f[8],X=(M.f[0]*u+M.f[1]*v+M.f[2])/d,Y=(M.f[3]*u+M.f[4]*v+M.f[5])/d;
            if(k<4){M.minx=std::min(M.minx,X);M.maxx=std::max(M.maxx,X);M.miny=std::min(M.miny,Y);M.maxy=std::max(M.maxy,Y);}
            double den=M.i[6]*X+M.i[7]*Y+M.i[8];
            double ux=(M.i[0]-u*M.i[6])/den,uy=(M.i[1]-u*M.i[7])/den;
            double vx=(M.i[3]-v*M.i[6])/den,vy=(M.i[4]-v*M.i[7])/den;
            footprint=std::max(footprint,L*.5*sqrt((ux*ux+uy*uy+vx*vx+vy*vy)*.5));
        }
        M.lod=clampd(log2(footprint),0,7);return M;
    }
    void build_mips(int needed){
        needed=std::min(needed,7);if(needed<=validMips)return;
        if((int)mip.size()<needed+1)mip.resize(needed+1);
        for(int n=validMips+1;n<=needed;n++){
            const auto& in=mip[n-1];auto& out=mip[n];out.w=(in.w+1)/2;out.h=(in.h+1)/2;out.data.resize((size_t)out.w*out.h*4);
            for(int y=0;y<out.h;y++)for(int x=0;x<out.w;x++){
                int sum[4]={};
                for(int dy=0;dy<2;dy++)for(int dx=0;dx<2;dx++){
                    int xx=2*x+dx,yy=2*y+dy;
                    if(xx<in.w&&yy<in.h){auto* q=&in.data[4*((size_t)yy*in.w+xx)];for(int k=0;k<4;k++)sum[k]+=q[k];}
                }
            auto* q=&out.data[4*((size_t)y*out.w+x)];for(int k=0;k<4;k++)q[k]=(uint8_t)((sum[k]+2)/4);
            }
        }
        validMips=needed;
    }
    void raster(const Mat& M,const Anim& a,bool side,double brightness=1){
        int x0=(int)clampd(floor(M.minx)-3,0,sw),x1=(int)clampd(ceil(M.maxx)+3,0,sw);
        int y0=(int)clampd(floor(M.miny)-3,0,sh),y1=(int)clampd(ceil(M.maxy)+3,0,sh);
        double L=std::max(cw,ch),ax=cw/L,ay=ch/L;
        int lo=std::min((int)floor(M.lod),validMips),hi=std::min(lo+1,validMips);
        double blend=hi==lo?0:M.lod-floor(M.lod),dl=(double)(1<<lo),dh=(double)(1<<hi);
        auto sidecolor=colors[SIDE_COLOR];
        double reveal=1-pow(1-a.progress,3);
        for(int y=y0;y<y1;y++){
            double nx=M.i[0]*(x0+.5)+M.i[1]*(y+.5)+M.i[2];
            double ny=M.i[3]*(x0+.5)+M.i[4]*(y+.5)+M.i[5];
            double dn=M.i[6]*(x0+.5)+M.i[7]*(y+.5)+M.i[8];
            for(int x=x0;x<x1;x++,nx+=M.i[0],ny+=M.i[3],dn+=M.i[6]){
                Pixel& destination=surface[(size_t)y*sw+x];
                if(side&&destination.a>=.99999f)continue;
                if(fabs(dn)<1e-14)continue;
                double u=nx/dn,v=ny/dn,xx=(u*L+cw)*.5,yy=(v*L+ch)*.5;
                if(xx< -dh||yy< -dh||xx>cw+dh||yy>ch+dh)continue;
                Pixel c=sample(mip[lo],xx/dl-.5,yy/dl-.5);
                if(blend>.001)c=mix(c,sample(mip[hi],xx/dh-.5,yy/dh-.5),blend);
                if(c.a<.00001f)continue;
                if(a.kind==4&&a.progress<1){
                    double U=(u+ax)/(2*ax),V=(v+ay)/(2*ay);
                    if(a.dirx>0)U=1-U;
                    if(a.diry>0)V=1-V;
                    float f=(float)smooth((reveal*1.06-std::max(U,V))/.055);
                    c.r*=f;c.g*=f;c.b*=f;c.a*=f;
                }
                if(side)c={(float)(sidecolor.r*brightness*c.a),(float)(sidecolor.g*brightness*c.a),(float)(sidecolor.b*brightness*c.a),c.a};
                if(side){float q=1-destination.a;destination={destination.r+q*c.r,destination.g+q*c.g,destination.b+q*c.b,destination.a+q*c.a};}
                else over(destination,c);
            }
        }
    }
    void box_blur(int radius){
        const int r=std::max(1,radius);const float div=1.f/(2*r+1);
        for(int y=0;y<sh;y++){
            size_t row=(size_t)y*sw;double sum=0;
            for(int x=0;x<=r&&x<sw;x++)sum+=shadow[row+x];
            for(int x=0;x<sw;x++){
                tmp[row+x]=(float)(sum*div);
                if(x-r>=0)sum-=shadow[row+x-r];
                if(x+r+1<sw)sum+=shadow[row+x+r+1];
            }
        }
        for(int x=0;x<sw;x++){
            double sum=0;for(int y=0;y<=r&&y<sh;y++)sum+=tmp[(size_t)y*sw+x];
            for(int y=0;y<sh;y++){
                shadow[(size_t)y*sw+x]=(float)(sum*div);
                if(y-r>=0)sum-=tmp[(size_t)(y-r)*sw+x];
                if(y+r+1<sh)sum+=tmp[(size_t)(y+r+1)*sw+x];
            }
        }
    }
    float shadow_at(double x,double y)const{
        if(x<0||y<0||x>=sw-1||y>=sh-1)return 0;
        int X=(int)x,Y=(int)y;float fx=(float)(x-X),fy=(float)(y-Y);size_t n=(size_t)Y*sw+X;
        return (1-fy)*((1-fx)*shadow[n]+fx*shadow[n+1])+fy*((1-fx)*shadow[n+sw]+fx*shadow[n+sw+1]);
    }
    void render(double t,const uint8_t* input,uint8_t* output){
        size_t bytes=(size_t)w*h*4;
        if(!input){memset(output,0,bytes);return;}
        if(input==output){aliased.assign(input,input+bytes);input=aliased.data();}
        // An inexpensive time gate avoids decoding style/allocating before delay.
        if(isfinite(t)&&t<val(DELAY)){memset(output,0,bytes);return;}
        material(input);if(!cw||!ch){memset(output,0,bytes);return;}
        Anim a=animation(t);if(a.opacity<=0){memset(output,0,bytes);return;}
        allocate();Mat front=matrix(a,0);
        double depth=2.*std::min(cw,ch)/std::max(cw,ch)*val(DEPTH)/100.;
        Mat back=matrix(a,-depth);int needed=(int)ceil(std::max(front.lod,back.lod));build_mips(needed);
        raster(front,a,false);
        if(depth>0){
            // Four projected layers form a shallow stylized slab; not a free 3D mesh.
            // A tiny lower-right offset remains visible even for a front-facing card.
            for(int s=1;s<=4;s++){Mat b=matrix(a,-depth*s/4.);
                double dx=std::min(w,h)*val(DEPTH)/100.*.25*s/4.;double dy=dx*1.4;
                for(int j=0;j<3;j++){b.f[j]+=dx*b.f[6+j];b.f[3+j]+=dy*b.f[6+j];}
                inverse3(b.f,b.i);b.minx+=dx;b.maxx+=dx;b.miny+=dy;b.maxy+=dy;
                raster(b,a,true,.72+.07*(4-s));
            }
        }
        if(val(SHADOW)>0){for(size_t n=0;n<surface.size();n++)shadow[n]=surface[n].a;
            int r=(int)floor(std::min(w,h)*val(SOFTNESS)/100.+.5);
            for(int n=0;n<3;n++)box_blur(r);
        }
        double sy=std::min(w,h)*val(DISTANCE)/100.,sx=sy*.65,opacity=val(SHADOW)/100.;
        for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++){
            Pixel q=surface[(size_t)(y+halo)*sw+x+halo];
            double sa=opacity>0?opacity*shadow_at(x+halo-sx,y+halo-sy):0;
            double alpha=sat(q.a+sa*(1-q.a));auto* out=output+4*((size_t)y*w+x);
            out[3]=byte(alpha*a.opacity);
            if(out[3]&&alpha>.00001){out[0]=byte(q.r/alpha);out[1]=byte(q.g/alpha);out[2]=byte(q.b/alpha);}
            else out[0]=out[1]=out[2]=0;
        }
    }
};
} // namespace card3d

extern "C" {
F0R_API int f0r_init(){return 1;}
F0R_API void f0r_deinit(){}
F0R_API void f0r_get_plugin_info(f0r_plugin_info_t* i){if(!i)return;
    *i={"FactMontage Cards","Card 3D contributors",F0R_PLUGIN_TYPE_FILTER,F0R_COLOR_MODEL_RGBA8888,1,1,0,PUBLIC_PARAMS,
    "Corner entrance, rounded/torn card, frame, shadow, slab and deterministic organic perspective."};}
F0R_API void f0r_get_param_info(f0r_param_info_t* i,int n){if(!i)return;
    if(n<0||n>=PUBLIC_PARAMS){*i={"Invalid",F0R_PARAM_DOUBLE,"Invalid parameter"};return;}
    *i={PUBLIC_SPECS[n].name,F0R_PARAM_DOUBLE,PUBLIC_SPECS[n].help};}
F0R_API f0r_instance_t f0r_construct(unsigned w,unsigned h){
    if(!w||!h||w>8192||h>8192||(size_t)w*h>card3d::MAX_PIXELS)return nullptr;
    try{return new card3d::Studio(w,h);}catch(...){return nullptr;}}
F0R_API void f0r_destruct(f0r_instance_t v){delete static_cast<card3d::Studio*>(v);}
F0R_API void f0r_set_param_value(f0r_instance_t v,f0r_param_t p,int n){if(!v)return;
    auto* c=static_cast<card3d::Studio*>(v);try{std::lock_guard<std::mutex> guard(c->lock);c->set_control(n,p);}catch(...){} }
F0R_API void f0r_get_param_value(f0r_instance_t v,f0r_param_t p,int n){if(!v||!p||n<0||n>=PUBLIC_PARAMS)return;
    auto* c=static_cast<card3d::Studio*>(v);std::lock_guard<std::mutex> guard(c->lock);
    *(double*)p=c->controls[n];}
F0R_API void f0r_update(f0r_instance_t v,double t,const uint32_t* in,uint32_t* out){if(!v||!out)return;
    auto* c=static_cast<card3d::Studio*>(v);
    try{std::lock_guard<std::mutex> guard(c->lock);c->render(t,(const uint8_t*)in,(uint8_t*)out);}
    catch(...){memset(out,0,(size_t)c->w*c->h*4);}}
#ifdef CARD3D_TEST_API
// Private renderer regression hooks, deliberately absent from the release ABI.
F0R_API void card3d_test_set(f0r_instance_t v,f0r_param_t p,int n){
    auto* c=static_cast<card3d::Studio*>(v);std::lock_guard<std::mutex> guard(c->lock);c->set(n,p);}
F0R_API void card3d_test_get(f0r_instance_t v,f0r_param_t p,int n){if(!v||!p||n<0||n>=PARAMS)return;
    auto* c=static_cast<card3d::Studio*>(v);std::lock_guard<std::mutex> guard(c->lock);
    if(SPECS[n].type==F0R_PARAM_COLOR)*(f0r_param_color_t*)p=c->colors[n];else *(double*)p=c->p[n];}
F0R_API void card3d_test_reset(f0r_instance_t v){auto* c=static_cast<card3d::Studio*>(v);
    std::lock_guard<std::mutex> guard(c->lock);
    for(int i=0;i<PARAMS;i++)if(SPECS[i].type==F0R_PARAM_DOUBLE)c->set(i,&SPECS[i].def);
    f0r_param_color_t frame={238.f/255,234.f/255,226.f/255};c->set(FRAME_COLOR,&frame);}
F0R_API void card3d_debug_anim(f0r_instance_t v,double t,double* out){auto* c=static_cast<card3d::Studio*>(v);auto a=c->animation(t);
    double k[]={a.progress,a.opacity,a.remain,a.scale,a.sx,a.sy,a.shear,a.taper,a.pose.yaw,a.pose.pitch,a.pose.roll};memcpy(out,k,sizeof k);}
F0R_API void card3d_debug_matrix(f0r_instance_t v,double t,double* f,double* inv){auto* c=static_cast<card3d::Studio*>(v);auto M=c->matrix(c->animation(t),0);memcpy(f,M.f,9*sizeof(double));memcpy(inv,M.i,9*sizeof(double));}
#endif
}
