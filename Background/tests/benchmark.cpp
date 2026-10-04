// SPDX-License-Identifier: GPL-3.0-only
#include "studio_background/core.hpp"
#include <chrono>
#include <cstdio>
#include <vector>
#include <algorithm>
using namespace sbg;
int main(){std::puts("width,height,mode,median_ms,p95_ms,frames,warmup,checksum");
 for(auto dims:std::vector<std::pair<int,int>>{{640,360},{1280,720},{1920,1080}}){
  Image im(dims.first,dims.second);Mask mask(dims.first/4,dims.second/4);
  for(int y=0;y<im.h;++y)for(int x=0;x<im.w;++x){auto i=(std::size_t(y)*im.w+x)*4;bool subject=(x>im.w/3&&x<im.w*2/3&&y>im.h/8);im.rgba[i]=subject?220:20;im.rgba[i+1]=subject?60:210;im.rgba[i+2]=subject?85:30;im.rgba[i+3]=255;}
  for(int y=0;y<mask.h;++y)for(int x=0;x<mask.w;++x)mask.alpha[std::size_t(y)*mask.w+x]=(x>mask.w/3&&x<mask.w*2/3&&y>mask.h/8)?1:0;
  for(int mode=0;mode<3;++mode){Params p;p.key_r=20/255.;p.key_g=210/255.;p.key_b=30/255.;p.method=mode?1:0;p.output=mode==2?1:0;std::vector<double> samples;uint64_t checksum=0;
   for(int i=0;i<7;++i){auto start=std::chrono::steady_clock::now();auto out=render(im,p,p.method?&mask:nullptr);auto end=std::chrono::steady_clock::now();checksum+=out.rgba[out.rgba.size()/2];if(i>=2)samples.push_back(std::chrono::duration<double,std::milli>(end-start).count());}
   std::sort(samples.begin(),samples.end());std::printf("%d,%d,%s,%.3f,%.3f,5,2,%llu\n",im.w,im.h,mode==0?"chroma":mode==1?"cached_alpha":"cached_alpha_blur",samples[2],samples.back(),(unsigned long long)checksum);std::fflush(stdout);
  }
 }
}
