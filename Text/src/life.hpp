// SPDX-License-Identifier: MIT
// Analytic hold phase: no accumulated offsets, per-frame RNG or independent glyph jitter.
// Included inside namespace sunimo after Pose is declared.
struct LifeRecipe {int kind;float period,amount,spread,drift;};
#include "life_recipes.inc"
struct LifeResult {Pose local,body;double echo=0,track=0;};
inline double pulse(double x,double start,double width){
    double p=(x-start)/width;if(p<=0||p>=1)return 0;
    double q=std::sin(PI*p);return q*q*q*q;
}
inline LifeResult living(const LifeRecipe&r,double t,double speed,double u,double ordinal,uint32_t seed){
    LifeResult out;auto&z=out.local;auto&b=out.body;
    // All randomness is fixed per scene; time is evaluated directly, so seeking is stable.
    double base=noise(seed+859)*.6,w=2*PI*t*speed/r.period+base;
    double phase=w-u*r.spread,si=std::sin(phase),co=std::cos(phase),amp=r.amount;
    b.x=r.drift*.022*(std::sin(w*.61)+.35*std::sin(w*.97+.5));
    b.y=r.drift*.030*std::sin(w*.73+.7);
    b.rz=r.drift*.010*std::sin(w*.57);
    switch(r.kind){
    case 0:z.y=.035*amp*si;break;
    case 1:z.y=.092*amp*si;z.rz=.014*amp*std::cos(phase*.91);break;
    case 2:z.x=.028*amp*si;z.y=.010*amp*std::sin(phase*.73);break;
    case 3:b.sx=b.sy=1+.014*amp*std::sin(w);z.y=.013*amp*si;break;
    case 4:z.y=.100*amp*si;z.rz=.019*amp*co;break;
    case 5:z.y=.062*amp*si;z.sx=1+.022*amp*si;z.sy=1-.026*amp*si;z.rz=.009*amp*co;break;
    case 6:z.rz=.034*amp*si;z.y=.025*amp*(.5-.5*std::cos(phase*2));break;
    case 7:z.x=.023*amp*co;z.y=.067*amp*si;z.rz=.016*amp*si;break;
    case 8:z.x=.020*amp*(std::sin(phase*.61)+.35*std::sin(phase*.97));z.y=.072*amp*(.72*si+.28*std::sin(phase*.71+.8));z.rz=.016*amp*co;break;
    case 9:out.track=.055*amp*std::sin(w);b.y+=.009*amp*std::cos(w);break;
    case 10:z.ry=.065*amp*si;z.y=.014*amp*co;break;
    case 11:z.rx=.053*amp*si;z.y=.020*amp*co;break;
    case 12:z.rz=u*.07*amp*std::sin(w);z.y=.035*amp*(1-std::abs(u)*2)*si;break;
    case 13:z.ry=.065*amp*si;z.rz=.011*amp*co;z.y=.025*amp*si;break;
    case 14:{double q=std::fmod(t*speed/r.period-u*.18+8,1.);double kick=pulse(q,.16,.50);z.y=-.090*amp*kick*std::sin((q-.16)*PI*6);z.sy=1+.016*amp*kick;break;}
    case 15:{double lift=std::pow(.5+.5*si,4);z.sx=z.sy=1+.016*amp*lift;z.y=-.016*amp*lift;z.glint=.035*amp*lift;break;}
    case 16:z.x=.020*amp*co;z.y=.060*amp*si;out.echo=.34*amp*std::pow(.5+.5*std::sin(w),4);break;
    case 17:{double q=std::fmod(t*speed/r.period-ordinal*.033+8,1.);double kick=pulse(q,.20,.15);z.y=-.014*amp*kick;z.rz=.008*amp*kick;break;}
    case 18:case 19:{
        // Sparse designed glitch, not skipped render frames. At most a short burst per cycle.
        double q=std::fmod(t*speed/r.period-ordinal*.019+8,1.);
        double event=pulse(q,.25,.105)+.55*pulse(q,.395,.055);
        double sign=noise(seed+uint32_t(ordinal)*29)>.5?1.:-1.;
        z.x=sign*.017*amp*event;
        if(r.kind==19)z.strip=.020*amp*event;
        z.glint=.13*amp*event;break;
    }
    case 20:case 24:{double travel=std::fmod(t*speed/r.period+base+8,1.)*1.5-.25;z.scan=r.spread<0?1-travel:travel;z.glint=.22*amp;z.y=.006*amp*si;break;}
    case 21:z.sx=1+.010*amp*si;z.sy=1-.010*amp*si;z.glint=.025*amp*(.5+.5*co);break;
    case 22:z.y=.054*amp*std::sin(w+ordinal*PI*.72);z.rz=.025*amp*std::sin(w+ordinal*PI);break;
    case 23:z.x=.012*amp*si;z.y=.041*amp*std::sin(phase*.83);z.rz=.014*amp*co;break;
    case 25:b.rz+=.013*amp*std::sin(w);z.rz=.012*amp*si;z.y=.025*amp*co;break;
    }
    return out;
}
inline void addLifePose(Pose&dst,const Pose&src,double k){
    dst.x+=src.x*k;dst.y+=src.y*k;dst.rz+=src.rz*k;dst.rx+=src.rx*k;dst.ry+=src.ry*k;dst.skew+=src.skew*k;
    dst.sx*=1+(src.sx-1)*k;dst.sy*=1+(src.sy-1)*k;dst.strip+=src.strip*k;dst.glint+=src.glint*k;if(src.scan>-2&&k>0)dst.scan=src.scan;
}
