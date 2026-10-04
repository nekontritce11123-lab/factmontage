// SPDX-License-Identifier: GPL-3.0-or-later
#include "studio_color/color.hpp"
#include "studio_color/analysis.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
namespace sc=studio_color;
namespace fs=std::filesystem;
namespace {
std::string escaped(const std::string& s){std::ostringstream out;for(unsigned char c:s){switch(c){case '"':out<<"\\\"";break;case '\\':out<<"\\\\";break;case '\n':out<<"\\n";break;case '\r':out<<"\\r";break;case '\t':out<<"\\t";break;default:if(c<32)out<<"\\u"<<std::hex<<std::setfill('0')<<std::setw(4)<<static_cast<int>(c)<<std::dec;else out<<static_cast<char>(c);}}return out.str();}
int integer(const std::string& s,int lo=1,int hi=32768){std::size_t n=0;long long x;try{x=std::stoll(s,&n);}catch(...){throw std::invalid_argument("Expected integer: "+s);}if(n!=s.size()||x<lo||x>hi)throw std::invalid_argument("Integer outside supported range: "+s);return static_cast<int>(x);}
double number(const std::string& s){std::size_t n=0;double x;try{x=std::stod(s,&n);}catch(...){throw std::invalid_argument("Expected number: "+s);}if(n!=s.size()||!std::isfinite(x))throw std::invalid_argument("Non-finite/invalid number");return x;}
std::vector<std::uint8_t> readRaw(const std::string& path,int w,int h){
 std::ifstream f(path,std::ios::binary|std::ios::ate);if(!f)throw std::runtime_error("Cannot read: "+path);
 const auto size=f.tellg();const auto frame=static_cast<std::uint64_t>(w)*h*4;
 if(size<=0||static_cast<std::uint64_t>(size)>512ULL*1024*1024||static_cast<std::uint64_t>(size)%frame)throw std::runtime_error("Raw input must be complete RGBA frames and <=512 MiB");
 std::vector<std::uint8_t> b(static_cast<std::size_t>(size));f.seekg(0);f.read(reinterpret_cast<char*>(b.data()),size);if(!f)throw std::runtime_error("Short read");return b;
}
sc::ShotStats statsRaw(const std::string& path,int w,int h,int transfer=0){
 const auto data=readRaw(path,w,h);const auto bytes=static_cast<std::size_t>(w)*h*4;const auto frames=data.size()/bytes;
 std::vector<sc::FrameStats> stats;
 for(auto pos:sc::samplePositions(static_cast<std::int64_t>(frames),12))stats.push_back(sc::analyzeRGBA(data.data()+static_cast<std::size_t>(pos)*bytes,w,h,0,transfer));
 return sc::aggregate(stats);
}
void ensureNew(const fs::path& path){if(fs::exists(path))throw std::runtime_error("Refusing to overwrite existing output: "+path.string());}
void correctionJson(const sc::AutoCorrection& c){
 std::cout<<"{\"applied\":"<<(c.applied?"true":"false")<<",\"auto_ev\":"<<c.exposure<<",\"auto_r\":"<<c.redEV<<",\"auto_b\":"<<c.blueEV<<",\"auto_contrast\":"<<c.contrast<<",\"confidence\":"<<c.confidence<<",\"warnings\":[";
 for(std::size_t i=0;i<c.warnings.size();++i){if(i)std::cout<<',';std::cout<<'"'<<escaped(c.warnings[i])<<'"';}std::cout<<"]}";
}
void statsJson(const sc::ShotStats& s){const auto& f=s.median;std::cout<<"{\"schema\":\"studio.color.stats/1\",\"valid\":"<<(s.valid?"true":"false")<<",\"valid_frames\":"<<s.validFrames<<",\"p10\":"<<f.p10<<",\"p50\":"<<f.p50<<",\"p90\":"<<f.p90<<",\"p99\":"<<f.p99<<",\"red_green_ev\":"<<f.logRedGreen<<",\"blue_green_ev\":"<<f.logBlueGreen<<",\"white_confidence\":"<<f.whiteConfidence<<",\"brightness_confidence\":"<<f.brightnessConfidence<<",\"clipped_fraction\":"<<f.clippedFraction<<",\"variation_ev\":"<<s.variationEV<<",\"mixed_content\":"<<(s.mixedContent?"true":"false")<<"}\n";}
struct Options{sc::Params p;std::string reference;bool graphics=false;bool scalar=false;std::vector<std::string> positional;};
Options options(int argc,char** argv){
 Options o;
 for(int i=2;i<argc;++i){std::string a=argv[i];
  if(a=="--preset"){
   if(++i>=argc)throw std::invalid_argument("Missing preset id");bool found=false;
   for(const auto& preset:sc::builtinPresets())if(argv[i]==std::string(preset.id)){o.p=sc::withManual(o.p,preset.params);found=true;break;}
   if(!found)throw std::invalid_argument("Unknown preset id");
  }else if(a=="--set"){
   if(++i>=argc)throw std::invalid_argument("Missing name=value");const std::string s=argv[i];const auto eq=s.find('=');
   if(eq==std::string::npos||!sc::setParameter(o.p,s.substr(0,eq),number(s.substr(eq+1))))throw std::invalid_argument("Invalid/unknown parameter: "+s);
  }else if(a=="--reference"){if(++i>=argc)throw std::invalid_argument("Missing reference id");o.reference=argv[i];}
  else if(a=="--match-graphics")o.graphics=true;
  else if(a=="--scalar")o.scalar=true;
  else if(a.rfind("--",0)==0)throw std::invalid_argument("Unknown option: "+a);
  else o.positional.push_back(a);
 }
 return o;
}
void requireSize(const Options& o,std::size_t n){if(o.positional.size()!=n)throw std::invalid_argument("Wrong argument count; run studio-color help");}
void help(){std::cout<<R"(Studio Color 0.1.0 — EXPERIMENTAL SOURCE/ENGINE TOOL, not a Kdenlive installer.
Input .rgba files: packed straight RGBA8888, full range, explicit SDR transfer.
Commands:
  presets
  cube OUTPUT.cube [--preset ID] [--set name=value ...]
  process INPUT.rgba OUTPUT.rgba WIDTH HEIGHT [--preset ID] [--set name=value ...]
  analyze INPUT.rgba WIDTH HEIGHT [--set transfer=0|1]
  match SOURCE.rgba REFERENCE.rgba WIDTH HEIGHT [--set transfer=0|1]
  batch MANIFEST.tsv NEW_OUTPUT_DIRECTORY [--preset ID] [--reference ID] [--match-graphics]
  benchmark WIDTH HEIGHT ITERATIONS [--preset ID] [--scalar]
Manifest: UTF-8 TSV, one clip per line: id<TAB>video|image|graphic|audio|hdr<TAB>raw_path<TAB>width<TAB>height
Paths in a manifest are relative to that manifest. No header; # comments allowed.
Batch plans matching once, skips audio/HDR, preserves alpha, writes only to a NEW directory.
Never passes raw filenames to a shell. Input files and Kdenlive projects are never overwritten.
)";}
}
int main(int argc,char** argv){
 try{
  std::cout.imbue(std::locale::classic());std::cout<<std::setprecision(10);
  if(argc<2||std::string(argv[1])=="help"||std::string(argv[1])=="--help"){help();return 0;}
  const std::string cmd=argv[1];const Options o=options(argc,argv);const auto& a=o.positional;
  if(cmd=="presets"){
   requireSize(o,0);for(const auto& p:sc::builtinPresets())std::cout<<p.id<<"\t"<<p.name<<"\t"<<p.description<<"\n";return 0;
  }
  if(cmd=="cube"){requireSize(o,1);ensureNew(a[0]);sc::Lut(o.p).writeCube(a[0]);return 0;}
  if(cmd=="process"){
   requireSize(o,4);int w=integer(a[2]),h=integer(a[3]);ensureNew(a[1]);auto data=readRaw(a[0],w,h);const auto frame=static_cast<std::size_t>(w)*h*4;sc::Lut lut(o.p);
   for(std::size_t off=0;off<data.size();off+=frame)lut.process(data.data()+off,data.data()+off,w,h);
   std::ofstream out(a[1],std::ios::binary);out.write(reinterpret_cast<const char*>(data.data()),static_cast<std::streamsize>(data.size()));if(!out)throw std::runtime_error("Output write failed");return 0;
  }
  if(cmd=="analyze"){requireSize(o,3);statsJson(statsRaw(a[0],integer(a[1]),integer(a[2]),static_cast<int>(o.p.transfer)));return 0;}
  if(cmd=="match"){
   requireSize(o,4);int w=integer(a[2]),h=integer(a[3]);auto s=statsRaw(a[0],w,h,static_cast<int>(o.p.transfer)),r=statsRaw(a[1],w,h,static_cast<int>(o.p.transfer));correctionJson(sc::matchShot(s,r,sc::MatchMode::Reference));std::cout<<'\n';return 0;
  }
  if(cmd=="batch"){
   requireSize(o,2);ensureNew(a[1]);std::ifstream mf(a[0]);if(!mf)throw std::runtime_error("Cannot read manifest");
   struct Input{std::string id,path;int w=0,h=0;};std::vector<Input> inputs;std::vector<sc::VisualClip> clips;std::string line;
   while(std::getline(mf,line)){
    if(!line.empty()&&line.back()=='\r')line.pop_back();if(line.empty()||line[0]=='#')continue;
    if(line.size()>32768)throw std::runtime_error("Manifest line too long");
    std::istringstream fields(line);std::vector<std::string> f;std::string field;while(std::getline(fields,field,'\t'))f.push_back(field);
    if(f.size()!=5)throw std::runtime_error("Manifest needs five TSV fields");
    if(std::any_of(clips.begin(),clips.end(),[&](const auto& c){return c.id==f[0];}))throw std::runtime_error("Duplicate clip id");
    if(clips.size()>=10000)throw std::runtime_error("Manifest exceeds 10000 items");
    if(f[1]!="video"&&f[1]!="image"&&f[1]!="graphic"&&f[1]!="audio"&&f[1]!="hdr")throw std::runtime_error("Unknown visual type");
    sc::VisualClip c;c.id=f[0];c.hasVideo=f[1]!="audio";c.supportedSDR=f[1]!="hdr";c.graphic=f[1]=="graphic";c.existing=o.p;
    Input in{f[0],(fs::path(a[0]).parent_path()/fs::path(f[2])).string(),0,0};
    if(c.hasVideo&&c.supportedSDR){in.w=integer(f[3]);in.h=integer(f[4]);c.statistics=statsRaw(in.path,in.w,in.h,static_cast<int>(o.p.transfer));}
    clips.push_back(c);inputs.push_back(in);
   }
   auto plan=sc::planBatch(clips,o.reference,o.graphics);
   // Validate all inputs and the plan before creating any output directory.
   if(!fs::create_directory(a[1]))throw std::runtime_error("Cannot create new batch output directory");
   for(std::size_t i=0;i<clips.size();++i){const auto& c=clips[i];if(!c.hasVideo||!c.supportedSDR)continue;auto params=c.existing;
    for(const auto& change:plan.changes)if(change.first==c.id)params=change.second;
    auto raw=readRaw(inputs[i].path,inputs[i].w,inputs[i].h);sc::Lut lut(params);const auto frame=static_cast<std::size_t>(inputs[i].w)*inputs[i].h*4;
    for(std::size_t off=0;off<raw.size();off+=frame)lut.process(raw.data()+off,raw.data()+off,inputs[i].w,inputs[i].h);
    const auto target=fs::path(a[1])/fs::path("clip-"+std::to_string(i)+".rgba");std::ofstream out(target,std::ios::binary);out.write(reinterpret_cast<const char*>(raw.data()),static_cast<std::streamsize>(raw.size()));if(!out)throw std::runtime_error("Partial batch output: write failed");
    lut.writeCube((fs::path(a[1])/fs::path("clip-"+std::to_string(i)+".cube")).string());
   }
   std::cout<<"{\"reference\":\""<<escaped(plan.referenceId)<<"\",\"audio_skipped\":"<<plan.audioSkipped<<",\"unsupported_skipped\":"<<plan.unsupportedSkipped<<",\"reports\":[";
   for(std::size_t i=0;i<plan.reports.size();++i){if(i)std::cout<<',';std::cout<<"{\"clip\":\""<<escaped(plan.reports[i].first)<<"\",\"result\":";correctionJson(plan.reports[i].second);std::cout<<'}';}std::cout<<"]}\n";return 0;
  }
  if(cmd=="benchmark"){
   requireSize(o,3);int w=integer(a[0],1,4096),h=integer(a[1],1,4096),n=integer(a[2],3,1000);std::vector<std::uint8_t> in(static_cast<std::size_t>(w)*h*4),out(in.size());std::uint32_t state=17;
   for(std::size_t i=0;i<in.size();i+=4){state=state*1664525U+1013904223U;in[i]=static_cast<std::uint8_t>(state>>24);in[i+1]=static_cast<std::uint8_t>(state>>16);in[i+2]=static_cast<std::uint8_t>(state>>8);in[i+3]=255;}
   const auto start=std::chrono::steady_clock::now();sc::Lut lut(o.p);const double build=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
   for(int i=0;i<3;++i)lut.process(in.data(),out.data(),w,h,0,0,!o.scalar);std::vector<double> times;
   for(int i=0;i<n;++i){const auto t=std::chrono::steady_clock::now();lut.process(in.data(),out.data(),w,h,0,0,!o.scalar);times.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t).count());}
   std::sort(times.begin(),times.end());std::uint64_t checksum=0;for(auto b:out)checksum+=b;
   std::cout<<"{\"width\":"<<w<<",\"height\":"<<h<<",\"iterations\":"<<n<<",\"threads\":1,\"backend\":\""<<(!o.scalar&&sc::avx2Available()?"avx2":"scalar")<<"\",\"lut_build_ms\":"<<build<<",\"lut_bytes\":"<<lut.nodes().size()*sizeof(sc::LutNode)<<",\"min_ms\":"<<times.front()<<",\"median_ms\":"<<times[times.size()/2]<<",\"p90_ms\":"<<times[static_cast<std::size_t>((times.size()-1)*.9)]<<",\"checksum\":"<<checksum<<",\"scope\":\"kernel only; no decode, MLT or export\"}\n";return 0;
  }
  throw std::invalid_argument("Unknown command: "+cmd);
 }catch(const std::exception& e){std::cerr<<"studio-color: "<<e.what()<<'\n';return 2;}
}
