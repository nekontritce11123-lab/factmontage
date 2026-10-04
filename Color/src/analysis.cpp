// SPDX-License-Identifier: GPL-3.0-or-later
#include "studio_color/analysis.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <set>
namespace studio_color {
namespace {
double median(std::vector<double> v){if(v.empty())return 0;std::sort(v.begin(),v.end());const auto n=v.size();return n%2?v[n/2]:(v[n/2-1]+v[n/2])*.5;}
double quantile(const std::array<std::uint64_t,1024>& bins,std::uint64_t n,double q){
 if(!n)return 0;
 const auto threshold=static_cast<std::uint64_t>(std::ceil(q*static_cast<double>(n)));std::uint64_t sum=0;
 for(std::size_t i=0;i<bins.size();++i){sum+=bins[i];if(sum>=threshold)return std::expm1((static_cast<double>(i)+.5)/1024*std::log(1025.0))/1024;}
 return 1;
}
double lum(double r,double g,double b){return .2126*r+.7152*g+.0722*b;}
double clamp(double x,double lo,double hi){return std::clamp(x,lo,hi);}
}
FrameStats analyzeRGBA(const std::uint8_t* data,int width,int height,std::ptrdiff_t stride,int transfer,ROI roi){
 if(width<=0||height<=0||!data)return {};
 if(width>32768||height>32768)throw std::invalid_argument("Analysis dimensions exceed limit");
 if(transfer!=0&&transfer!=1)throw std::invalid_argument("Unsupported transfer");
 if(!std::isfinite(roi.x)||!std::isfinite(roi.y)||!std::isfinite(roi.width)||!std::isfinite(roi.height)||roi.width<=0||roi.height<=0)throw std::invalid_argument("Invalid analysis ROI");
 const auto rowBytes=static_cast<std::ptrdiff_t>(width)*4;if(!stride)stride=rowBytes;if(stride<rowBytes)throw std::invalid_argument("Analysis stride too small");
 const int x0=static_cast<int>(clamp(roi.x,0,1)*width),y0=static_cast<int>(clamp(roi.y,0,1)*height);
 const int x1=static_cast<int>(clamp(roi.x+roi.width,0,1)*width),y1=static_cast<int>(clamp(roi.y+roi.height,0,1)*height);
 if(x1<=x0||y1<=y0)return {};
 std::array<double,256> linear{};for(int i=0;i<256;++i)linear[static_cast<std::size_t>(i)]=decodeSignal(i/255.,transfer);
 std::array<std::uint64_t,1024> bins{};std::array<bool,16> neutralTiles{};
 std::vector<double> reds,blues;std::uint64_t saturated=0,clipped=0,total=0,black=0;
 FrameStats s;
 const int step=std::max(1,static_cast<int>(std::ceil(std::sqrt(static_cast<double>(x1-x0)*(y1-y0)/65536.0))));
 for(int y=y0;y<y1;y+=step)for(int x=x0;x<x1;x+=step){
  const auto* p=data+static_cast<std::ptrdiff_t>(y)*stride+static_cast<std::ptrdiff_t>(x)*4;
  if(p[3]<192)continue; // ignore nearly transparent edges and invisible hidden RGB
  ++total;
  const int mx=std::max({p[0],p[1],p[2]}),mn=std::min({p[0],p[1],p[2]});
  if(mx<=3){++black;continue;} // black bars/empty padding must not drive exposure
  const double r=linear[p[0]],g=linear[p[1]],b=linear[p[2]],yy=lum(r,g,b);
  const double sat=mx?double(mx-mn)/mx:0;
  if(sat>.50)++saturated;
  if(mx>=254)++clipped;
  const auto bin=static_cast<std::size_t>(clamp(std::log1p(1024*yy)/std::log(1025.0)*1024,0,1023));
  ++bins[bin];++s.pixels;
  // Confidence-gated near-neutral estimator, not whole-frame grey-world correction.
  if(sat<.18 && mn>=28 && mx<=232 && yy>.025 && yy<.80){
   reds.push_back(std::log2((r+1e-8)/(g+1e-8)));blues.push_back(std::log2((b+1e-8)/(g+1e-8)));
   const int tx=std::min(3,4*(x-x0)/(x1-x0)),ty=std::min(3,4*(y-y0)/(y1-y0));neutralTiles[static_cast<std::size_t>(ty*4+tx)]=true;
  }
 }
 if(s.pixels<32)return s;
 s.p10=quantile(bins,s.pixels,.10);s.p50=quantile(bins,s.pixels,.50);s.p90=quantile(bins,s.pixels,.90);s.p99=quantile(bins,s.pixels,.99);
 s.neutralPixels=reds.size();s.neutralFraction=double(reds.size())/double(s.pixels);
 s.neutralCoverage=double(std::count(neutralTiles.begin(),neutralTiles.end(),true))/16.0;
 s.saturatedFraction=double(saturated)/double(s.pixels);s.clippedFraction=double(clipped)/double(s.pixels);
 s.logRedGreen=median(std::move(reds));s.logBlueGreen=median(std::move(blues));
 const double contrast=std::log2((s.p90+.005)/(s.p10+.005));
 const double nonBlack=total?1-double(black)/double(total):0;
 s.brightnessConfidence=clamp((contrast-.2)/2,0,1)*clamp((s.p50-.004)/.035,0,1)*clamp((1-s.clippedFraction)/.8,0,1)*clamp(nonBlack/.25,0,1);
 s.whiteConfidence=clamp((s.neutralFraction-.015)/.12,0,1)*clamp((s.neutralCoverage-.20)/.55,0,1);
 if(s.pixels<256)s.whiteConfidence*=double(s.pixels)/256;
 for(std::size_t i=0;i<bins.size();++i)s.histogram[i/32]+=double(bins[i])/double(s.pixels);
 s.valid=s.p50>.002 && s.clippedFraction<.80;
 return s;
}
ShotStats aggregate(const std::vector<FrameStats>& frames){
 ShotStats out;std::vector<const FrameStats*> good;
 for(const auto& f:frames)if(f.valid)good.push_back(&f);
 if(good.empty())return out;
 out.valid=true;out.validFrames=good.size();out.median=*good.front();
 auto md=[&](double FrameStats::*member){std::vector<double> values;values.reserve(good.size());for(auto f:good)values.push_back(f->*member);return median(std::move(values));};
 for(auto member:{&FrameStats::p10,&FrameStats::p50,&FrameStats::p90,&FrameStats::p99,&FrameStats::logRedGreen,&FrameStats::logBlueGreen,&FrameStats::neutralFraction,&FrameStats::neutralCoverage,&FrameStats::saturatedFraction,&FrameStats::clippedFraction,&FrameStats::brightnessConfidence,&FrameStats::whiteConfidence})out.median.*member=md(member);
 std::vector<double> levels;for(auto f:good)levels.push_back(std::log2(f->p50+.005));std::sort(levels.begin(),levels.end());
 // A robust 10–90% spread. One flash should not set an entire clip's white balance.
 const auto low=static_cast<std::size_t>(std::floor(double(levels.size()-1)*.1));const auto high=static_cast<std::size_t>(std::ceil(double(levels.size()-1)*.9));
 out.variationEV=levels[high]-levels[low];
 double worst=0;
 for(std::size_t i=1;i<good.size();++i){double d=0;for(std::size_t b=0;b<32;++b)d+=std::abs(good[i]->histogram[b]-good[i-1]->histogram[b]);worst=std::max(worst,d*.5);}
 out.mixedContent=out.variationEV>1.25 || (worst>.70 && good.size()>2);
 if(out.mixedContent){out.median.brightnessConfidence*=.25;out.median.whiteConfidence*=.5;}
 return out;
}
AutoCorrection balanceShot(const ShotStats& shot){
 AutoCorrection c;
 if(!shot.valid||shot.mixedContent){c.warnings.push_back("Недостаточно стабильных кадров для автоматической коррекции.");return c;}
 const auto& s=shot.median;c.confidence=s.brightnessConfidence;
 // A single clip has no objective reference exposure. Keep its correction intentionally small.
 if(s.brightnessConfidence>=.35 && s.p50>=.04 && s.p50<=.45){
  c.exposure=clamp(std::log2(.18/(s.p50+.001))*.35,-.35,.35)*s.brightnessConfidence;
  if(s.p99>.85)c.exposure=std::min(c.exposure,.15);
  c.applied=true;
 }else c.warnings.push_back("Без эталона намеренно тёмный или светлый кадр не нормализуется принудительно.");
 if(s.whiteConfidence>=.5){c.redEV=clamp(-s.logRedGreen,-.18,.18)*s.whiteConfidence;c.blueEV=clamp(-s.logBlueGreen,-.18,.18)*s.whiteConfidence;c.applied=true;}
 else c.warnings.push_back("Ненадёжные нейтральные участки: баланс белого не меняется.");
 if(s.clippedFraction>.005)c.warnings.push_back("Уже потерянные в пересвете детали восстановить нельзя.");
 return c;
}
AutoCorrection matchShot(const ShotStats& shot,const ShotStats& ref,MatchMode mode){
 AutoCorrection c;
 if(!shot.valid||!ref.valid){c.warnings.push_back("Недостаточно пригодных кадров; автоматическая поправка не меняется.");return c;}
 const auto& s=shot.median;const auto& t=ref.median;
 if(shot.mixedContent){c.warnings.push_back("В клипе сильно меняется свет или есть склейка: автоматическое выравнивание пропущено. Разделите клип.");return c;}
 if(s.clippedFraction>.005)c.warnings.push_back("В исходнике уже есть клиппинг; потерянные детали восстановить нельзя.");
 c.confidence=std::min(s.brightnessConfidence,t.brightnessConfidence);
 const double delta=std::log2((t.p50+.005)/(s.p50+.005));
 const double sourceSpread=std::log2((s.p90+.005)/(s.p10+.005)),refSpread=std::log2((t.p90+.005)/(t.p10+.005));
 const bool different=std::abs(delta)>1.5 || std::abs(sourceSpread-refSpread)>2.0 || std::abs(s.saturatedFraction-t.saturatedFraction)>.45;
 const double conservative=(different&&mode==MatchMode::ConservativeAuto)?.25:1;
 if(different)c.warnings.push_back("Содержание или освещение заметно отличаются от эталона; автоматическая поправка ограничена.");
 if(c.confidence>=.15){
  c.exposure=clamp(delta,-1,1)*c.confidence*conservative;
  // Do not aggressively amplify dark noise or push already bright highlights.
  if(s.p50<.02)c.exposure=std::min(c.exposure,.25);
  if(c.exposure>0 && s.p99>.85)c.exposure=std::min(c.exposure,.35);
  if(sourceSpread>.75&&refSpread>.75)c.contrast=clamp(std::log2(refSpread/sourceSpread),-.12,.12)*c.confidence*conservative;
 }
 const double wc=std::min(s.whiteConfidence,t.whiteConfidence);
 if(wc>=.35){c.redEV=clamp(t.logRedGreen-s.logRedGreen,-.25,.25)*wc*conservative;c.blueEV=clamp(t.logBlueGreen-s.logBlueGreen,-.25,.25)*wc*conservative;}
 else c.warnings.push_back("Мало надёжных нейтральных участков: баланс белого оставлен без изменения.");
 c.applied=(c.confidence>=.15 || wc>=.35);
 if(!c.applied)c.warnings.push_back("Низкая уверенность: сохранена предыдущая автоматическая поправка.");
 return c;
}
Params applyCorrection(Params p,const AutoCorrection& c){if(c.applied){p.auto_ev=c.exposure;p.auto_r=c.redEV;p.auto_b=c.blueEV;p.auto_contrast=c.contrast;}return sanitized(p);}
BatchResult planBatch(const std::vector<VisualClip>& clips,const std::string& explicitReference,bool matchGraphics,const std::atomic_bool* cancel){
 BatchResult result;
 auto cancelled=[&](){return cancel&&cancel->load(std::memory_order_relaxed);};
 if(cancelled()){result.cancelled=true;return result;}
 std::set<std::string> ids;
 for(const auto& c:clips)if(c.id.empty()||!ids.insert(c.id).second)throw std::invalid_argument("Clip IDs must be nonempty and unique");
 const VisualClip* reference=nullptr;std::vector<const VisualClip*> candidates;
 for(const auto& clip:clips){
  if(!clip.hasVideo){++result.audioSkipped;continue;}
  if(!clip.supportedSDR){++result.unsupportedSkipped;continue;}
  if(clip.locked){++result.lockedSkipped;continue;}
  if((!clip.graphic||matchGraphics)&&clip.statistics.valid&&!clip.statistics.mixedContent)candidates.push_back(&clip);
 }
 if(!explicitReference.empty()){
  for(auto clip:candidates)if(clip->id==explicitReference)reference=clip;
  if(!reference)throw std::invalid_argument("Reference is not an eligible selected visual clip");
 }else if(!candidates.empty()){
  // Robust representative, not the brightest clip or an average of unrelated sources.
  std::vector<double> values;for(auto c:candidates)values.push_back(std::log2(c->statistics.median.p50+.005));const double centre=median(std::move(values));
  double best=-std::numeric_limits<double>::infinity();
  for(auto c:candidates){const auto& s=c->statistics.median;const double score=s.brightnessConfidence+.3*s.whiteConfidence-std::abs(std::log2(s.p50+.005)-centre)*.35-s.clippedFraction*2;if(score>best){best=score;reference=c;}}
 }
 if(!reference)return result;
 result.referenceId=reference->id;
 for(const auto& clip:clips){
  if(cancelled()){result.cancelled=true;result.changes.clear();result.reports.clear();return result;}
  if(!clip.hasVideo||!clip.supportedSDR||clip.locked)continue;
  AutoCorrection correction;
  if(clip.graphic&&!matchGraphics)correction.warnings.push_back("Графика: только общий стиль, без автоматической нейтрализации фирменных цветов.");
  else if(&clip==reference){
   if(candidates.size()==1 && explicitReference.empty())correction=balanceShot(clip.statistics);
   else{correction.applied=true;correction.confidence=1;}
  }
  else correction=matchShot(clip.statistics,reference->statistics,explicitReference.empty()?MatchMode::ConservativeAuto:MatchMode::Reference);
  if(correction.applied)result.changes.push_back({clip.id,applyCorrection(clip.existing,correction)});
  result.reports.push_back({clip.id,std::move(correction)});
 }
 return result;
}
std::vector<std::int64_t> samplePositions(std::int64_t length,int maximum){
 if(length<=0||maximum<=0)return {};
 maximum=std::clamp(maximum,1,48);
 const int n=static_cast<int>(std::min<std::int64_t>(length,maximum));std::vector<std::int64_t> out;
 for(int i=0;i<n;++i){const auto f=static_cast<std::int64_t>((static_cast<long double>(i)+.5L)*length/n);out.push_back(std::min(f,length-1));}
 out.erase(std::unique(out.begin(),out.end()),out.end());return out;
}
} // namespace studio_color
