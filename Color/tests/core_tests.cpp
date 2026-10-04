// SPDX-License-Identifier: GPL-3.0-or-later
#include "studio_color/color.hpp"
#include "studio_color/analysis.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <thread>
using namespace studio_color;
namespace {
int passed=0,failed=0;
template<class F> void expectThrows(F f){bool caught=false;try{f();}catch(const std::exception&){caught=true;}if(!caught)throw std::runtime_error("Expected an exception");}
void expect(bool condition,const char* msg){if(!condition)throw std::runtime_error(msg);}
void near(double a,double b,double eps,const char* msg){if(std::abs(a-b)>eps){std::cerr<<" got "<<a<<" vs "<<b<<" tolerance "<<eps<<'\n';throw std::runtime_error(msg);}}
void test(const char* name,const std::function<void()>& body){try{body();std::cout<<"PASS "<<name<<'\n';++passed;}catch(const std::exception& e){std::cerr<<"FAIL "<<name<<": "<<e.what()<<'\n';++failed;}}
std::vector<std::uint8_t> randomFrame(int w,int h){std::vector<std::uint8_t> v(static_cast<std::size_t>(w)*h*4);std::mt19937 rng(1234);for(auto& b:v)b=static_cast<std::uint8_t>(rng());return v;}
std::vector<std::uint8_t> neutralRamp(int w,int h,double scale=1){std::vector<std::uint8_t> v(static_cast<std::size_t>(w)*h*4);for(int y=0;y<h;++y)for(int x=0;x<w;++x){double lin=.02+.60*double(x)/std::max(1,w-1);const auto val=static_cast<std::uint8_t>(std::lround(encodeSignal(std::min(1.0,lin*scale))*255));auto i=static_cast<std::size_t>(y*w+x)*4;v[i]=v[i+1]=v[i+2]=val;v[i+3]=255;}return v;}
ShotStats rampStats(double gain=1){auto im=neutralRamp(160,90,gain);return aggregate({analyzeRGBA(im.data(),160,90)});}
}
int main(){
 test("default identity bit exact all 256 channel values",[]{auto in=randomFrame(256,4),out=in;Lut l(Params{});l.process(in.data(),out.data(),256,4);expect(in==out,"identity changed pixels");});
 test("identity in-place",[]{auto in=randomFrame(17,11),original=in;Lut(Params{}).process(in.data(),in.data(),17,11);expect(in==original,"in-place failed");});
 test("alpha including hidden RGB is preserved",[]{auto in=randomFrame(123,17),out=in;Params p;p.exposure=.7;p.saturation=120;Lut(p).process(in.data(),out.data(),123,17);for(std::size_t i=0;i<in.size();i+=4){expect(in[i+3]==out[i+3],"alpha changed");if(!in[i+3])expect(std::memcmp(in.data()+i,out.data()+i,4)==0,"invisible RGB changed");}});
 test("padded stride and odd dimensions",[]{constexpr int w=17,h=7,stride=80;std::vector<std::uint8_t> in(stride*h,123),out(stride*h,231);for(int y=0;y<h;++y)for(int x=0;x<w;++x)in[y*stride+x*4+3]=255;Params p;p.exposure=.4;Lut(p).process(in.data(),out.data(),w,h,stride,stride);for(int y=0;y<h;++y)for(int x=w*4;x<stride;++x)expect(out[y*stride+x]==231,"wrote row padding");});
 test("invalid stride rejected",[]{std::uint8_t a[32]{};bool threw=false;try{Lut(Params{}).process(a,a,4,2,8,16);}catch(...){threw=true;}expect(threw,"invalid stride accepted");});
 test("zero-sized frame no-op",[]{Lut(Params{}).process(nullptr,nullptr,0,100);});
 test("null nonempty buffer rejected",[]{bool threw=false;try{Lut(Params{}).process(nullptr,nullptr,4,4);}catch(...){threw=true;}expect(threw,"null accepted");});
 test("NaN and infinity sanitized",[]{Params p;p.exposure=std::numeric_limits<double>::quiet_NaN();p.temperature=std::numeric_limits<double>::infinity();p.saturation=-100;auto q=sanitized(p);expect(q.exposure==0&&q.temperature==0&&q.saturation==0,"sanitization failed");});
 test("unknown and out-of-range settings rejected",[]{Params p;expect(!setParameter(p,"no_such_control",1),"unknown parameter");expect(!setParameter(p,"exposure",99),"bad range");expect(!setParameter(p,"transfer",.3),"fractional transfer");expect(setParameter(p,"exposure",.25),"valid rejected");});
 test("preset switch is a full manual reset, not parameter accumulation",[]{Params p;p.auto_ev=.33;p.auto_r=.1;p.transfer=1;p.tint=90;for(auto preset:builtinPresets())p=withManual(p,preset.params);p=withManual(p,Params{});expect(p.tint==0&&p.saturation==100&&p.auto_ev==.33&&p.auto_r==.1&&p.transfer==1,"preset polluted internal values");});
 test("look strength zero retains auto correction",[]{Params p;p.auto_ev=.5;p.temperature=80;p.mix=0;auto a=transformExact({.4,.4,.4},p);p.temperature=0;p.mix=100;auto b=transformExact({.4,.4,.4},p);near(a.r,b.r,1e-10,"look mix removes matching");expect(a.r>.4,"auto disappeared");});
 test("auto strength zero retains manual adjustment",[]{Params p;p.auto_ev=.8;p.auto_strength=0;p.exposure=.3;auto a=transformExact({.4,.4,.4},p);p.auto_ev=0;auto b=transformExact({.4,.4,.4},p);near(a.r,b.r,1e-12,"auto strength failed");});
 test("Rec709 and sRGB transfer roundtrip",[]{for(int tr=0;tr<2;++tr)for(int i=0;i<=255;++i)near(encodeSignal(decodeSignal(i/255.,tr),tr),i/255.,.0003,"transfer roundtrip");});
 test("monochrome neutral channels",[]{Params p;p.saturation=0;auto in=randomFrame(51,9),out=in;Lut(p).process(in.data(),out.data(),51,9);for(std::size_t i=0;i<out.size();i+=4)if(out[i+3])expect(out[i]==out[i+1]&&out[i+1]==out[i+2],"nonneutral monochrome");});
 test("positive exposure brightens midtones",[]{Params p;p.exposure=.5;auto a=transformExact({.4,.4,.4},p);expect(a.r>.4&&a.r<.8,"wrong exposure");});
 test("protection reduces new clipping versus hard clamp",[]{Params p;p.exposure=2;auto a=transformExact({.8,.8,.8},p);p.protection=0;auto b=transformExact({.8,.8,.8},p);expect(a.r<b.r&&a.r<1&&b.r==1,"shoulder failed");});
 test("all presets grayscale ramps monotonic",[]{for(const auto& preset:builtinPresets()){Lut l(preset.params);double last=-1;for(int i=0;i<=1024;++i){auto q=l.sample(i/1024.,i/1024.,i/1024.);const double y=.2126*q.r+.7152*q.g+.0722*q.b;expect(y+1e-6>=last,"tonal inversion");last=y;}}});
 test("scalar byte kernel agrees with continuous tetrahedral sample",[]{Params p;p.exposure=.7;p.temperature=23;p.contrast=12;p.saturation=112;Lut l(p);auto in=randomFrame(257,37),out=in;l.process(in.data(),out.data(),257,37);for(std::size_t i=0;i<in.size();i+=4)if(in[i+3]){auto q=l.sample(in[i]/255.,in[i+1]/255.,in[i+2]/255.);near(out[i],std::lround(q.r*255),0,"tetra r");near(out[i+1],std::lround(q.g*255),0,"tetra g");near(out[i+2],std::lround(q.b*255),0,"tetra b");}});
 test("LUT approximation accuracy on moderate grade",[]{Params p;p.exposure=.4;p.temperature=18;p.contrast=8;p.saturation=108;Lut l(p);std::mt19937 rng(42);double maxerr=0,sum=0;for(int i=0;i<10000;++i){RGB v{(rng()%256)/255.,(rng()%256)/255.,(rng()%256)/255.};auto a=transformExact(v,p),b=l.sample(v.r,v.g,v.b);double e=std::max({std::abs(a.r-b.r),std::abs(a.g-b.g),std::abs(a.b-b.b)})*255;maxerr=std::max(maxerr,e);sum+=e;}std::cout<<"  moderate_lut_error max_lsb="<<maxerr<<" mean_max_channel_lsb="<<sum/10000<<'\n';expect(maxerr<3.0,"LUT too inaccurate");});
 test("out-of-order frame rendering is deterministic",[]{Params p;p.temperature=-15;p.shadows=7;Lut l(p);auto a=randomFrame(32,27),b=a,c=a;l.process(a.data(),b.data(),32,27);auto other=neutralRamp(16,9);l.process(other.data(),other.data(),16,9);l.process(a.data(),c.data(),32,27);expect(b==c,"render depends on seek history");});
 test("immutable LUT can process concurrent frames",[]{Params p;p.exposure=.3;Lut l(p);auto in=randomFrame(127,83);std::vector<std::uint8_t> a=in,b=in;std::thread t1([&]{l.process(in.data(),a.data(),127,83);}),t2([&]{l.process(in.data(),b.data(),127,83);});t1.join();t2.join();expect(a==b,"threaded mismatch");});
 test("bounded cache and shared instances",[]{LutCache c(2);Params p;p.temperature=4;auto a=c.get(p),b=c.get(p);expect(a==b,"cache missed");p.temperature=5;c.get(p);p.temperature=6;c.get(p);expect(c.size()==2,"cache unbounded");auto in=randomFrame(3,3),out=in;a->process(in.data(),out.data(),3,3);});
 test("neutral statistics have strong WB confidence",[]{auto s=rampStats();expect(s.valid&&s.median.whiteConfidence>.9,"neutral confidence");near(s.median.logRedGreen,0,1e-8,"neutral red");expect(s.median.p90>s.median.p50&&s.median.p50>s.median.p10,"quantiles");});
 test("all-transparent frame is invalid for analysis",[]{auto v=randomFrame(160,90);for(std::size_t i=3;i<v.size();i+=4)v[i]=0;expect(!analyzeRGBA(v.data(),160,90).valid,"transparent drives analysis");});
 test("all-black frame is invalid for analysis",[]{std::vector<std::uint8_t> v(64*64*4,0);for(std::size_t i=3;i<v.size();i+=4)v[i]=255;expect(!analyzeRGBA(v.data(),64,64).valid,"black drives analysis");});
 test("white clipped frame is invalid for analysis",[]{std::vector<std::uint8_t> v(64*64*4,255);expect(!analyzeRGBA(v.data(),64,64).valid,"white drives analysis");});
 test("saturated green frame does not trigger white balance",[]{std::vector<std::uint8_t> v(64*64*4,0);for(std::size_t i=0;i<v.size();i+=4){v[i]=15;v[i+1]=180;v[i+2]=20;v[i+3]=255;}auto s=analyzeRGBA(v.data(),64,64);expect(s.whiteConfidence==0,"grey-world green disaster");});
 test("black bars do not drive exposure percentiles",[]{auto a=neutralRamp(160,90);std::vector<std::uint8_t> b(160*130*4,0);for(std::size_t i=3;i<b.size();i+=4)b[i]=255;std::memcpy(b.data()+160*20*4,a.data(),a.size());auto s=analyzeRGBA(a.data(),160,90),t=analyzeRGBA(b.data(),160,130);near(s.p50,t.p50,1e-12,"bars alter median");});
 test("ROI validation",[]{auto v=neutralRamp(32,32);bool threw=false;try{analyzeRGBA(v.data(),32,32,0,0,{0,0,-1,1});}catch(...){threw=true;}expect(threw,"invalid ROI");});
 test("batch exposure match reduces synthetic exposure error",[]{auto s=rampStats(.7),r=rampStats();auto c=matchShot(s,r,MatchMode::Reference);expect(c.applied&&c.exposure>0&&c.exposure<=1,"wrong exposure match");const double before=std::abs(std::log2(s.median.p50/r.median.p50)),after=std::abs(std::log2(s.median.p50*std::exp2(c.exposure)/r.median.p50));expect(after<before*.2,"does not align");});
 test("mixed-light clip is conservatively skipped",[]{std::vector<FrameStats> frames;for(double g:{.1,.1,1.,1.}){auto a=neutralRamp(160,90,g);frames.push_back(analyzeRGBA(a.data(),160,90));}auto s=aggregate(frames);expect(s.mixedContent,"mixed not detected");auto c=matchShot(s,rampStats());expect(!c.applied&&!c.warnings.empty(),"unsafe match");});
 test("audio HDR locked clips are skipped; images participate",[]{std::vector<VisualClip> v(5);for(int i=0;i<5;++i){v[i].id=std::to_string(i);v[i].statistics=rampStats(i==1?.7:1);}v[2].hasVideo=false;v[3].supportedSDR=false;v[4].locked=true;auto p=planBatch(v,"0");expect(p.audioSkipped==1&&p.unsupportedSkipped==1&&p.lockedSkipped==1,"skips");expect(p.changes.size()==2,"image not included");});
 test("graphics excluded only from matching, never reported as audio",[]{std::vector<VisualClip> v(2);v[0].id="video";v[1].id="logo";for(auto& c:v)c.statistics=rampStats();v[1].graphic=true;auto p=planBatch(v);expect(p.changes.size()==1&&p.audioSkipped==0&&p.reports.size()==2,"graphic handling");});
 test("batch cancellation never returns partial changes",[]{std::vector<VisualClip> v(2);for(auto& c:v)c.statistics=rampStats();std::atomic_bool cancel{true};auto p=planBatch(v,"",false,&cancel);expect(p.cancelled&&p.changes.empty(),"partial cancellation");});
 test("invalid explicit reference rejects instead of guessing",[]{std::vector<VisualClip> v(1);v[0].id="a";v[0].statistics=rampStats();bool threw=false;try{planBatch(v,"missing");}catch(...){threw=true;}expect(threw,"guessed reference");});
 test("sampling short long and huge clips stays in visible range",[]{for(std::int64_t n:{1LL,2LL,7LL,100LL,10000000000LL}){auto s=samplePositions(n);expect(!s.empty()&&s.size()<=12,"sample count");expect(std::is_sorted(s.begin(),s.end()),"sample order");for(auto f:s)expect(f>=0&&f<n,"sample outside cut");}expect(samplePositions(0).empty(),"empty samples");});
 test("all builtin parameters in range",[]{for(auto p:builtinPresets())expect(equalParams(p.params,sanitized(p.params)),"preset out of bounds");});

 test("AVX2/scalar exact equality for every 24-bit RGB value",[]{
  Params p;p.exposure=.4;p.temperature=-28;p.shadows=12;p.contrast=15;p.saturation=130;
  Lut l(p);constexpr int width=4096,height=4096;std::vector<std::uint8_t> in(std::size_t(width)*height*4),a(in.size()),b(in.size());
  for(std::uint32_t i=0;i<16777216u;++i){in[i*4]=i&255;in[i*4+1]=(i>>8)&255;in[i*4+2]=(i>>16)&255;in[i*4+3]=(i%257==0)?0:static_cast<std::uint8_t>(i%254+1);}
  l.process(in.data(),a.data(),width,height,0,0,false);l.process(in.data(),b.data(),width,height,0,0,true);
  expect(a==b,"SIMD changed RGB or alpha");std::cout<<"  exhaustive RGB count=16777216 backend="<<(avx2Available()?"avx2":"scalar fallback")<<'\n';
 });
 test("SIMD tail, in-place, unaligned and padded rows",[]{
  for(int width=1;width<35;++width){Params p;p.temperature=21;Lut l(p);const int stride=width*4+5;auto in=randomFrame(stride,4);std::vector<std::uint8_t>a=in,b=in;l.process(in.data()+1,a.data()+1,width,3,stride,stride,false);l.process(b.data()+1,b.data()+1,width,3,stride,stride,true);expect(a==b,"SIMD row/tail corrupted padding");}
 });
 test("nonfinite LUT samples are safe",[]{Params p;p.exposure=.3;Lut l(p);auto a=l.sample(NAN,INFINITY,-INFINITY);expect(std::isfinite(a.r)&&std::isfinite(a.g)&&std::isfinite(a.b),"nonfinite sample");});
 test("stride arithmetic overflow is rejected",[]{auto a=randomFrame(1,1);Lut l(Params{});expectThrows([&]{l.process(a.data(),a.data(),1,2,std::numeric_limits<std::ptrdiff_t>::max(),4);});});
 test("duplicate batch IDs rejected",[]{VisualClip a;a.id="x";expectThrows([&]{planBatch({a,a});});});
 test("single clip conservative auto balance",[]{ShotStats s;s.valid=true;s.median.p50=.10;s.median.p99=.8;s.median.brightnessConfidence=1;s.median.whiteConfidence=1;s.median.logRedGreen=.15;auto c=balanceShot(s);expect(c.applied&&c.exposure>0&&c.exposure<=.35&&c.redEV<0,"single shot auto balance failed");});
 test("reanalysis replaces rather than accumulates auto correction",[]{Params p;p.auto_ev=.8;p.auto_r=.2;p.auto_b=-.2;p.auto_contrast=.1;AutoCorrection c;c.applied=true;c.exposure=-.25;c.redEV=-.05;c.blueEV=.07;c.contrast=-.03;auto q=applyCorrection(p,c);near(q.auto_ev,c.exposure,1e-12,"auto exposure accumulated");near(q.auto_r,c.redEV,1e-12,"auto red accumulated");near(q.auto_b,c.blueEV,1e-12,"auto blue accumulated");near(q.auto_contrast,c.contrast,1e-12,"auto contrast accumulated");});
 test("single clip dark intention preserved",[]{ShotStats s;s.valid=true;s.median.p50=.01;s.median.brightnessConfidence=1;auto c=balanceShot(s);expect(c.exposure==0,"night was normalized to day");});
 test("extreme parameters produce finite bounded LUTs",[]{std::mt19937 rng(986);for(int k=0;k<12;++k){Params p;for(auto& s:paramSpecs)p.*(s.field)=(k&1)?s.minimum:s.maximum;p.transfer=k%2;p.mix=100;p.temperature=(rng()%201)-100.;Lut l(p);for(int n=0;n<1000;++n){auto a=l.sample((rng()%256)/255.,(rng()%256)/255.,(rng()%256)/255.);expect(std::isfinite(a.r)&&std::isfinite(a.g)&&std::isfinite(a.b)&&a.r>=0&&a.r<=1.00001&&a.g>=0&&a.g<=1.00001&&a.b>=0&&a.b<=1.00001,"bad extreme output");}}});
 std::cout<<"RESULT "<<passed<<" passed; "<<failed<<" failed\n";return failed?1:0;
}
