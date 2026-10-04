// SPDX-License-Identifier: GPL-3.0-or-later
#include "studio_color/color.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <stdexcept>
namespace studio_color {
int processAvx2(const std::uint8_t*,std::uint8_t*,int,const LutNode*) noexcept;
namespace {
double clamp01(double x) noexcept { return std::clamp(std::isfinite(x)?x:0.0,0.0,1.0); }
double luma(RGB x) noexcept { return .2126*x.r+.7152*x.g+.0722*x.b; }
RGB scale(RGB x, double s) noexcept { return {x.r*s,x.g*s,x.b*s}; }
RGB rawTransform(RGB in, const Params& p, bool automaticOnly) noexcept {
 const int tr=static_cast<int>(p.transfer);
 RGB x={decodeSignal(in.r,tr),decodeSignal(in.g,tr),decodeSignal(in.b,tr)};
 const double originalMax=std::max({x.r,x.g,x.b});
 const double a=p.auto_strength/100.0;
 const double t=automaticOnly?0:p.temperature/100.0;
 const double tint=automaticOnly?0:p.tint/100.0;
 const double er=std::exp2(a*p.auto_r+.35*t+.12*tint);
 const double eg=std::exp2(-.24*tint);
 const double eb=std::exp2(a*p.auto_b-.35*t+.12*tint);
 const double gain=std::exp2(a*p.auto_ev+(automaticOnly?0:p.exposure))/(.2126*er+.7152*eg+.0722*eb);
 x={x.r*er*gain,x.g*eg*gain,x.b*eb*gain};
 const double y=luma(x);
 if(y>1e-15){
  const double contrast=std::exp2(a*p.auto_contrast+(automaticOnly?0:p.contrast/100.0));
  double toned=.18*std::pow(y/.18,contrast);
  if(!automaticOnly){
   // Smooth monotonic shadow lift: no per-frame histogram equalization or local halos.
   const double s=p.shadows>=0?p.shadows/50.0:p.shadows/100.0;
   toned*=1+s*std::exp(-toned/.20);
   if(p.highlights<0) toned/=1+(-p.highlights/50.0)*toned;
   else toned*=1+(p.highlights/100.0)*(toned/(toned+.40));
  }
  x=scale(x,toned/y);
 }
 const double requestedSat=automaticOnly?1:p.saturation/100.0;
 const double yy=luma(x);
 // Positive saturation gently approaches the available chroma headroom.
 // Already saturated colours are protected instead of crossing a hard gamut cusp.
 const double minimum=std::max(0.0,std::min({x.r,x.g,x.b}));
 const double boost=std::max(0.0,requestedSat-1);
 const double headroomDenominator=minimum+boost*std::max(0.0,yy-minimum);
 const double sat=requestedSat<=1?requestedSat:1+(headroomDenominator>1e-15?boost*minimum/headroomDenominator:0);
 x={yy+sat*(x.r-yy),yy+sat*(x.g-yy),yy+sat*(x.b-yy)};
 // Compress out-of-gamut chroma toward the same luminance instead of hard-clipping a channel.
 const double mn=std::min({x.r,x.g,x.b});
 if(mn<0 && yy>0){
  const double chroma=yy/(yy-mn);
  x={yy+(x.r-yy)*chroma,yy+(x.g-yy)*chroma,yy+(x.b-yy)*chroma};
 }
 const double mx=std::max({x.r,x.g,x.b});
 // Anchor the shoulder at original highlights: identity remains bit-exact.
 // This prevents new floating-point hard clipping, not recovery of already clipped source data.
 const double knee=std::max(.72,originalMax);
 const double protection=automaticOnly?1:p.protection/100.0;
 if(mx>knee && mx>0){
  const double room=std::max(0.0,1-knee);
  const double soft=room>1e-12?knee-room*std::expm1(-(mx-knee)/room):1.0;
  const double target=mx+(soft-mx)*protection;
  x=scale(x,target/mx);
 }
 return {encodeSignal(clamp01(x.r),tr),encodeSignal(clamp01(x.g),tr),encodeSignal(clamp01(x.b),tr)};
}
}
Params sanitized(Params p) noexcept {
 for(const auto& s:paramSpecs){
  double& v=p.*(s.field);
  v=std::isfinite(v)?std::clamp(v,s.minimum,s.maximum):s.defaultValue;
 }
 p.transfer=p.transfer>=.5?1.0:0.0;
 return p;
}
bool equalParams(const Params& a,const Params& b) noexcept {
 for(const auto& s:paramSpecs) if(a.*(s.field)!=b.*(s.field)) return false;
 return true;
}
bool setParameter(Params& p,std::string_view id,double value) noexcept {
 for(const auto& s:paramSpecs) if(id==s.id){
  if(!std::isfinite(value)||value<s.minimum||value>s.maximum)return false;
  if(id=="transfer" && value!=0 && value!=1) return false;
  p.*(s.field)=value; return true;
 }
 return false;
}
Params withManual(const Params& existing,const Params& preset) noexcept {
 Params out=existing;
 for(const auto& s:paramSpecs) if(std::string_view(s.group)!="internal"&&std::string_view(s.group)!="auto") out.*(s.field)=preset.*(s.field);
 return sanitized(out);
}
bool isIdentity(const Params& pp) noexcept {
 const Params p=sanitized(pp);
 const bool noAuto=p.auto_strength==0||(p.auto_ev==0&&p.auto_r==0&&p.auto_b==0&&p.auto_contrast==0);
 const bool noLook=p.mix==0||(p.exposure==0&&p.temperature==0&&p.tint==0&&p.contrast==0&&p.shadows==0&&p.highlights==0&&p.saturation==100);
 return noAuto&&noLook;
}
double decodeSignal(double v,int transfer) noexcept {
 v=clamp01(std::isfinite(v)?v:0);
 if(transfer==1) return v<=.04045?v/12.92:std::pow((v+.055)/1.055,2.4);
 return v<.081?v/4.5:std::pow((v+.099)/1.099,1/.45);
}
double encodeSignal(double x,int transfer) noexcept {
 x=clamp01(std::isfinite(x)?x:0);
 if(transfer==1) return x<=.0031308?12.92*x:1.055*std::pow(x,1/2.4)-.055;
 return x<.018?4.5*x:1.099*std::pow(x,.45)-.099;
}
RGB transformExact(RGB in,Params pp) noexcept {
 const Params p=sanitized(pp);
 in={clamp01(std::isfinite(in.r)?in.r:0),clamp01(std::isfinite(in.g)?in.g:0),clamp01(std::isfinite(in.b)?in.b:0)};
 if(isIdentity(p))return in;
 // Look strength does not undo shot matching; at 0% the automatically balanced image remains.
 RGB base=rawTransform(in,p,true);
 if(p.mix==0)return base;
 RGB look=rawTransform(in,p,false);
 const double mix=p.mix/100.0;
 return {base.r+mix*(look.r-base.r),base.g+mix*(look.g-base.g),base.b+mix*(look.b-base.b)};
}
Lut::Lut(Params p):p_(sanitized(p)),identity_(isIdentity(p_)){
 for(int v=0;v<256;++v){
  const int product=v*(LutSize-1);
  const int cell=std::min(product/255,LutSize-2);
  axes_[static_cast<std::size_t>(v)]={cell,product-cell*255};
 }
 if(identity_)return;
 nodes_.resize(LutSize*LutSize*LutSize);
 for(int b=0;b<LutSize;++b)for(int g=0;g<LutSize;++g)for(int r=0;r<LutSize;++r){
  const RGB x=transformExact({r/double(LutSize-1),g/double(LutSize-1),b/double(LutSize-1)},p_);
  nodes_[static_cast<std::size_t>((b*LutSize+g)*LutSize+r)]={
   static_cast<std::uint16_t>(std::lround(clamp01(x.r)*65535)),static_cast<std::uint16_t>(std::lround(clamp01(x.g)*65535)),
   static_cast<std::uint16_t>(std::lround(clamp01(x.b)*65535)),0};
 }
}
namespace {
inline void tetra(int r,int g,int b,int& o1,int& o2,int& w0,int& w1,int& w2,int& w3) noexcept {
 constexpr int R=1,G=LutSize,B=LutSize*LutSize;
 int hi,mid,lo;
 if(r>=g){
  if(g>=b){o1=R;o2=R+G;hi=r;mid=g;lo=b;}
  else if(r>=b){o1=R;o2=R+B;hi=r;mid=b;lo=g;}
  else{o1=B;o2=B+R;hi=b;mid=r;lo=g;}
 }else{
  if(r>=b){o1=G;o2=G+R;hi=g;mid=r;lo=b;}
  else if(g>=b){o1=G;o2=G+B;hi=g;mid=b;lo=r;}
  else{o1=B;o2=B+G;hi=b;mid=g;lo=r;}
 }
 w0=255-hi;w1=hi-mid;w2=mid-lo;w3=lo;
}
}
void Lut::process(const std::uint8_t* src,std::uint8_t* dst,int width,int height,std::ptrdiff_t srcStride,std::ptrdiff_t dstStride,bool allowSimd)const{
 if(width<0||height<0)throw std::invalid_argument("Negative frame dimension");
 if(width==0||height==0)return;
 if(width>32768||height>32768)throw std::invalid_argument("Frame dimensions exceed safety limit");
 if(!src||!dst)throw std::invalid_argument("Null image buffer");
 const auto rowBytes=static_cast<std::ptrdiff_t>(width)*4;
 if(!srcStride)srcStride=rowBytes;
 if(!dstStride)dstStride=rowBytes;
 if(srcStride<rowBytes||dstStride<rowBytes)throw std::invalid_argument("Invalid RGBA stride");
 if(srcStride>(std::numeric_limits<std::ptrdiff_t>::max()-rowBytes)/height || dstStride>(std::numeric_limits<std::ptrdiff_t>::max()-rowBytes)/height)throw std::invalid_argument("RGBA stride overflow");
 constexpr int diagonal=1+LutSize+LutSize*LutSize;
 for(int y=0;y<height;++y){
  const std::uint8_t* a=src+static_cast<std::ptrdiff_t>(y)*srcStride;
  std::uint8_t* d=dst+static_cast<std::ptrdiff_t>(y)*dstStride;
  if(identity_){if(a!=d)std::memmove(d,a,static_cast<std::size_t>(rowBytes));continue;}
  const int done=(allowSimd&&avx2Available())?processAvx2(a,d,width,nodes_.data()):0;
  a+=done*4;d+=done*4;
  for(int x=done;x<width;++x,a+=4,d+=4){
   const std::uint8_t alpha=a[3];
   if(alpha==0){if(a!=d)std::memcpy(d,a,4);continue;}
   const Axis r=axes_[a[0]],g=axes_[a[1]],b=axes_[a[2]];
   const int base=(b.index*LutSize+g.index)*LutSize+r.index;
   int o1,o2,w0,w1,w2,w3;tetra(r.fraction,g.fraction,b.fraction,o1,o2,w0,w1,w2,w3);
   const auto& n0=nodes_[static_cast<std::size_t>(base)];
   const auto& n1=nodes_[static_cast<std::size_t>(base+o1)];
   const auto& n2=nodes_[static_cast<std::size_t>(base+o2)];
   const auto& n3=nodes_[static_cast<std::size_t>(base+diagonal)];
   // 4 node reads, fixed-point barycentric interpolation. No pow, log, heap or mutex per pixel.
   d[0]=static_cast<std::uint8_t>((n0.r*w0+n1.r*w1+n2.r*w2+n3.r*w3+32767)/65535);
   d[1]=static_cast<std::uint8_t>((n0.g*w0+n1.g*w1+n2.g*w2+n3.g*w3+32767)/65535);
   d[2]=static_cast<std::uint8_t>((n0.b*w0+n1.b*w1+n2.b*w2+n3.b*w3+32767)/65535);
   d[3]=alpha;
  }
 }
}
RGB Lut::sample(double r,double g,double b)const noexcept {
 if(identity_)return {clamp01(r),clamp01(g),clamp01(b)};
 // Continuous sample used by tests and external LUT export checks, not frame hot loop.
 const double cr=clamp01(r)*(LutSize-1),cg=clamp01(g)*(LutSize-1),cb=clamp01(b)*(LutSize-1);
 const int ir=std::min(static_cast<int>(cr),LutSize-2),ig=std::min(static_cast<int>(cg),LutSize-2),ib=std::min(static_cast<int>(cb),LutSize-2);
 std::array<std::pair<double,int>,3> order{{{cr-ir,1},{cg-ig,LutSize},{cb-ib,LutSize*LutSize}}};
 std::sort(order.begin(),order.end(),[](auto x,auto y){return x.first>y.first;});
 const int idx=(ib*LutSize+ig)*LutSize+ir;
 const std::array<int,4> indices{{idx,idx+order[0].second,idx+order[0].second+order[1].second,idx+1+LutSize+LutSize*LutSize}};
 const std::array<double,4> weights{{1-order[0].first,order[0].first-order[1].first,order[1].first-order[2].first,order[2].first}};
 RGB out{0,0,0};
 for(int i=0;i<4;++i){const auto& n=nodes_[static_cast<std::size_t>(indices[static_cast<std::size_t>(i)])];const double w=weights[static_cast<std::size_t>(i)]/65535;out.r+=w*n.r;out.g+=w*n.g;out.b+=w*n.b;}
 return out;
}
void Lut::writeCube(const std::string& path)const{
 std::ofstream out(path,std::ios::binary|std::ios::trunc);out.imbue(std::locale::classic());
 if(!out)throw std::runtime_error("Cannot create LUT: "+path);
 out<<"# Studio Color 0.1.0 / sdr-primary-v1 / NOT an HDR or log transform\nTITLE \"Studio Color SDR\"\nLUT_3D_SIZE "<<LutSize<<"\nDOMAIN_MIN 0 0 0\nDOMAIN_MAX 1 1 1\n"<<std::fixed<<std::setprecision(8);
 for(int b=0;b<LutSize;++b)for(int g=0;g<LutSize;++g)for(int r=0;r<LutSize;++r){
  if(identity_)out<<r/double(LutSize-1)<<' '<<g/double(LutSize-1)<<' '<<b/double(LutSize-1)<<'\n';
  else{const auto& n=nodes_[static_cast<std::size_t>((b*LutSize+g)*LutSize+r)];out<<n.r/65535.0<<' '<<n.g/65535.0<<' '<<n.b/65535.0<<'\n';}
 }
 if(!out)throw std::runtime_error("LUT write failed: "+path);
}
std::shared_ptr<const Lut> LutCache::get(Params pp){
 const Params p=sanitized(pp);
 {std::lock_guard<std::mutex> lock(mutex_);for(auto i=lru_.begin();i!=lru_.end();++i)if(equalParams(i->p,p)){auto out=i->lut;lru_.splice(lru_.begin(),lru_,i);return out;}}
 auto lut=std::make_shared<const Lut>(p);
 {std::lock_guard<std::mutex> lock(mutex_);for(auto i=lru_.begin();i!=lru_.end();++i)if(equalParams(i->p,p)){auto out=i->lut;lru_.splice(lru_.begin(),lru_,i);return out;}
  lru_.push_front({p,lut});while(lru_.size()>limit_)lru_.pop_back();}
 return lut;
}
std::size_t LutCache::size()const{std::lock_guard<std::mutex> lock(mutex_);return lru_.size();}
} // namespace studio_color
