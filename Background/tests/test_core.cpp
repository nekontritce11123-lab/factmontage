// SPDX-License-Identifier: GPL-3.0-only
#include "studio_background/core.hpp"
#include "studio_background/cache.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <functional>
#include <random>
#include <unistd.h>
using namespace sbg;
static void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
static void throws(const std::function<void()>& f){bool caught=false;try{f();}catch(const std::exception&){caught=true;}require(caught,"Expected an exception");}
static Image solid(int w,int h,int r,int g,int b,int a=255){Image im(w,h);for(std::size_t i=0;i<im.rgba.size();i+=4){im.rgba[i]=r;im.rgba[i+1]=g;im.rgba[i+2]=b;im.rgba[i+3]=a;}return im;}
static float mean(const Mask& m){double sum=0;for(auto a:m.alpha)sum+=a;return float(sum/m.alpha.size());}
int main(){int passed=0,failed=0;
 auto test=[&](const char* name,const std::function<void()>& f){try{f();++passed;std::cout<<"PASS "<<name<<'\n';}catch(const std::exception& e){++failed;std::cerr<<"FAIL "<<name<<": "<<e.what()<<'\n';}};
 auto root=std::filesystem::temp_directory_path()/("sbg-tests-"+std::to_string(getpid()));std::filesystem::create_directory(root);
 Params p;p.feather=0;
 test("dimension limits",[&]{throws([]{Image a(0,1);});throws([]{Image a(100000,100000);});Image a(1,1);a.validate();});
 test("parameter range and NaN",[&]{Params a;throws([&]{a.set("blur",-1);});throws([&]{a.set("blur",NAN);});throws([&]{a.set("method",.5);});throws([&]{a.set("not_a_param",1);});});
 test("green becomes transparent",[&]{auto im=solid(32,16,0,255,0);auto o=render(im,p);for(std::size_t i=3;i<o.rgba.size();i+=4)require(o.rgba[i]==0,"Green survived");});
 test("feather never reintroduces fully keyed screen pixels",[&]{auto im=solid(9,5,0,255,0);for(int y=0;y<5;++y)for(int x=4;x<9;++x){auto at=(y*9+x)*4;im.rgba[at]=255;im.rgba[at+1]=0;}auto q=p;q.feather=1.2;q.despill=0;auto o=render(im,q);for(int y=0;y<5;++y)for(int x=0;x<4;++x)require(o.rgba[(y*9+x)*4+3]==0,"Feather revealed green pixels");require(o.rgba[(2*9+4)*4+3]>0&&o.rgba[(2*9+4)*4+3]<255,"Foreground edge not softened");});
 test("red foreground retained",[&]{auto im=solid(32,16,255,0,0);auto o=render(im,p);require(im.rgba==o.rgba,"Red was damaged");});
 test("neutral white key preserves black",[&]{auto q=p;q.key_r=q.key_g=q.key_b=1;auto a=chroma(solid(8,8,255,255,255),q);auto b=chroma(solid(8,8,0,0,0),q);require(mean(a)==0&&mean(b)==1,"Neutral key bad");});
 test("dark gray is not black key",[&]{auto q=p;q.key_r=q.key_g=q.key_b=0;q.tolerance=.02;require(mean(chroma(solid(8,8,100,100,100),q))>.99,"Black key removed gray");});
 test("blue key",[&]{auto q=p;q.key_g=0;q.key_b=1;require(mean(chroma(solid(8,8,0,0,255),q))==0,"Blue survived");});
 test("smooth chroma matte has intermediate alpha",[&]{auto q=p;q.tolerance=.05;q.transition=.3;bool partial=false;for(int g=0;g<255;++g){auto m=chroma(solid(1,1,0,g,0),q);if(m.alpha[0]>.05&&m.alpha[0]<.95)partial=true;}require(partial,"No soft matte");});
 test("existing source alpha multiplied, not replaced",[&]{auto im=solid(8,8,255,0,0,80);auto o=render(im,p);require(o.rgba[3]==80,"Alpha overwritten");});
 test("blur/fill preserve source transparency",[&]{auto im=solid(8,8,0,255,0,50);for(int mode:{1,2}){auto q=p;q.output=mode;auto o=render(im,q);require(o.rgba[3]==50,"Transparency lost");}});
 test("sampling median resists isolated outlier",[&]{auto im=solid(9,9,3,240,5);im.rgba[4*(4*9+4)]=255;auto s=pickKey(im,4,4);require(std::abs(s.rgb[0]-3/255.)<1e-8,"Median failed");});
 test("sampling rejects padding and transparent regions",[&]{auto im=solid(9,9,0,255,0,0);throws([&]{pickKey(im,4,4);});throws([&]{pickKey(im,-1,0);});});
 test("box blur matches naive clamped convolution",[&]{std::vector<float> v={0,1,0,.5,1,0},ref(v.size());int w=3,h=2,r=4;
     for(int y=0;y<h;++y)for(int x=0;x<w;++x){double sum=0;for(int yy=y-r;yy<=y+r;++yy)for(int xx=x-r;xx<=x+r;++xx)sum+=v[std::clamp(yy,0,h-1)*w+std::clamp(xx,0,w-1)];ref[y*w+x]=sum/81;}
     boxBlur(v,w,h,r);for(std::size_t i=0;i<v.size();++i)require(std::abs(v[i]-ref[i])<1e-6,"Incorrect blur");});
 test("mask resize preserves constants and 1px",[&]{Mask m(1,1,.3);auto b=resizeMask(m,31,13);for(float a:b.alpha)require(std::abs(a-.3)<1e-6,"Resize boundary");});
 test("premultiplied resize avoids invisible RGB fringe",[&]{auto im=solid(2,1,255,0,0,255);im.rgba[4]=0;im.rgba[5]=0;im.rgba[6]=255;im.rgba[7]=0;auto r=resizeImage(im,3,1);require(r.rgba[4]>250&&r.rgba[6]==0,"Invisible blue bled into red");});
 test("expansion and contraction",[&]{Mask a(21,21);a.alpha[10*21+10]=1;auto im=solid(21,21,0,0,0);auto q=p;q.edge_shift=2;refineMask(a,im,q,21,21);require(std::abs(mean(a)*441-25)<.001,"Expansion not 5x5");q.edge_shift=-2;refineMask(a,im,q,21,21);require(std::abs(mean(a)*441-1)<.001,"Contraction failed");});
 test("refinement bounded",[&]{auto im=solid(43,29,20,100,40);Mask a(43,29,.4);auto q=p;q.method=1;q.refine=1;q.feather=4;refineMask(a,im,q,43,29);a.validate();});
 test("human mode never falls back to source",[&]{auto q=p;q.method=1;throws([&]{render(solid(4,4,0,0,0),q);});});
 test("mask nonfinite rejected",[&]{Mask a(2,2);a.alpha[0]=NAN;throws([&]{a.validate();});});
 test("full foreground stays sharp under background blur",[&]{auto im=solid(32,16,255,0,0);auto q=p;q.output=1;q.blur=50;auto o=render(im,q);require(o.rgba==im.rgba,"Foreground blurred");});
 test("zero blur is identity",[&]{auto im=solid(16,8,0,255,0);auto q=p;q.output=1;q.blur=0;require(render(im,q).rgba==im.rgba,"Zero blur changes frame");});
 test("blur excludes foreground color from background kernel",[&]{auto im=solid(64,32,0,0,255);Mask mask(64,32);for(int y=0;y<32;++y)for(int x=20;x<44;++x){auto i=y*64+x;mask.alpha[i]=1;im.rgba[4*i]=255;im.rgba[4*i+2]=0;}auto q=p;q.method=1;q.refine=0;q.output=1;q.blur=20;auto o=render(im,q,&mask);require(o.rgba[(16*64+19)*4]==0,"Red foreground leaked into blur");});
 test("PNG RGBA roundtrip with unicode path",[&]{auto im=solid(13,9,16,72,98,117);auto path=(root/"маска с пробелами.png").string();savePng(path,im);require(loadPng(path).rgba==im.rgba,"PNG mismatch");});
 test("SHA256 standard empty file",[&]{auto path=(root/"empty").string();std::ofstream(path).close();require(sha256File(path)=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855","Wrong SHA256");});
 CacheInfo info;info.width=19;info.height=11;info.frames=5;info.referenceW=1920;info.referenceH=1080;info.firstSample=72;info.fpsNum=30000;info.fpsDen=1001;info.sourceSha256=std::string(64,'a');info.recipeSha256=std::string(64,'b');auto asset=(root/"тест маски.sbg").string();
 test("atomic mask stream and random access",[&]{CacheWriter w(asset,info);for(int i=0;i<5;++i)w.append(Mask(19,11,float(i)/4));require(!std::filesystem::exists(asset),"Partial file published");w.finish();CacheReader r(asset);for(int i:{4,0,2,1,4}){auto m=r.readSample(72+i);require(std::abs(mean(m)-i/4.f)<=1/255.f,"Wrong frame after seek");}r.requireIdentity(info.sourceSha256,info.recipeSha256,30000,1001);});
 test("out of range never clamps to wrong frame",[&]{CacheReader r(asset);throws([&]{r.readSample(71);});throws([&]{r.readSample(77);});});
 test("stale source, recipe and FPS fail closed",[&]{CacheReader r(asset);throws([&]{r.requireIdentity(std::string(64,'c'),info.recipeSha256,30000,1001);});throws([&]{r.requireIdentity(info.sourceSha256,std::string(64,'c'),30000,1001);});throws([&]{r.requireIdentity(info.sourceSha256,info.recipeSha256,30,1);});});
 test("immutable assets cannot be overwritten",[&]{auto old=sha256File(asset);throws([&]{CacheWriter w(asset,info);});require(sha256File(asset)==old,"Existing asset changed");});
 test("cancel/incomplete deletes own temporary file",[&]{auto path=(root/"canceled.sbg").string();{CacheWriter w(path,info);w.append(Mask(19,11,.5));throws([&]{w.finish();});}require(!std::filesystem::exists(path),"Canceled asset published");for(auto& f:std::filesystem::directory_iterator(root))require(f.path().string().find(".part.")==std::string::npos,"Temporary file leaked");});
 test("corrupted header rejected",[&]{auto bad=(root/"bad-header.sbg").string();std::filesystem::copy_file(asset,bad);std::fstream f(bad,std::ios::in|std::ios::out|std::ios::binary);f.seekp(12);f.put('\xff');f.close();throws([&]{CacheReader r(bad);});});
 test("truncated index rejected",[&]{auto bad=(root/"bad-index.sbg").string();std::filesystem::copy_file(asset,bad);std::filesystem::resize_file(bad,std::filesystem::file_size(bad)-1);throws([&]{CacheReader r(bad);});});
 test("corrupt compressed frame rejected",[&]{auto bad=(root/"bad-frame.sbg").string();std::filesystem::copy_file(asset,bad);std::fstream f(bad,std::ios::in|std::ios::out|std::ios::binary);f.seekp(257);f.put('\0');f.close();CacheReader r(bad);throws([&]{r.readSample(72);});});
 test("deterministic repeated render",[&]{std::mt19937 rng(12);Image im(48,35);for(auto& b:im.rgba)b=uint8_t(rng());auto q=p;q.feather=2;q.edge_shift=-1;q.output=1;auto a=render(im,q);auto b=render(im,q);require(a.rgba==b.rgba,"Non-deterministic frame");});
 test("random odd dimensions and parameter endpoints",[&]{std::mt19937 rng(72);for(int i=0;i<40;++i){int w=1+rng()%47,h=1+rng()%39;Image im(w,h);for(auto& b:im.rgba)b=uint8_t(rng());auto q=p;q.feather=i%13;q.edge_shift=i%17-8;q.output=i%3;q.blur=i%81;auto o=render(im,q);o.validate();}});
 std::filesystem::remove_all(root);
 std::cout<<"RESULT "<<passed<<" passed, "<<failed<<" failed\n";return failed?1:0;
}
