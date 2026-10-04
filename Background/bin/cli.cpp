// SPDX-License-Identifier: GPL-3.0-only
#include "studio_background/core.hpp"
#include "studio_background/cache.hpp"
#include <algorithm>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <filesystem>
using namespace sbg;
int main(int argc,char** argv){try{
    if(argc<2){std::cout<<"sbg-cli png|stream|verify|hash --input file --output file --width N --height N --frames N\n"
      "Parameters: --key_r 0 --key_g 1 --key_b 0 --tolerance 0.13 --transition 0.08\n"
      " --method 0|1 --output_mode 0|1|2 --feather 1.2 --edge_shift 0 --blur 24\n"
      " --cache file.sbg --source_sha256 HASH --recipe_sha256 HASH --sample 0\n"
      "Stream input/output: packed straight RGBA8888 on stdin/stdout. No audio modification.\n";return 0;}
    std::string cmd=argv[1];std::map<std::string,std::string> a;
    for(int i=2;i<argc;i+=2){if(i+1>=argc||std::string(argv[i]).rfind("--",0)!=0)throw std::invalid_argument("Expected --key value pairs");a[std::string(argv[i]+2)]=argv[i+1];}
    auto get=[&](std::string k,std::string d=""){auto it=a.find(k);return it==a.end()?d:it->second;};
    auto integer=[&](std::string k,int64_t d){auto text=get(k,std::to_string(d));std::size_t used=0;auto v=std::stoll(text,&used);if(used!=text.size())throw std::invalid_argument("Invalid integer: "+k);return v;};
    if(cmd=="hash"){std::cout<<sha256File(get("input"))<<'\n';return 0;}
    if(cmd=="verify"){CacheReader c(get("input"));for(uint32_t i=0;i<c.info().frames;++i)c.readSample(c.info().firstSample+i);std::cout<<"OK: "<<c.info().frames<<" frames; "<<c.info().width<<'x'<<c.info().height<<'\n';return 0;}
    Params p;const std::vector<std::string> reserved={"input","output","width","height","frames","cache","sample","reference_w","reference_h","source_sha256","recipe_sha256","fps_num","fps_den"};
    for(const auto& [k,v]:a){if(std::find(reserved.begin(),reserved.end(),k)!=reserved.end())continue;std::size_t used=0;double value=std::stod(v,&used);if(used!=v.size())throw std::invalid_argument("Invalid number");p.set(k=="output_mode"?"output":k,value);}
    std::unique_ptr<CacheReader> cache;if(p.method==1){cache=std::make_unique<CacheReader>(get("cache"));cache->requireIdentity(get("source_sha256"),get("recipe_sha256"),integer("fps_num",25),integer("fps_den",1));}
    auto sample=integer("sample",0);if(sample<0)throw std::invalid_argument("Negative sample");
    auto rw64=integer("reference_w",0),rh64=integer("reference_h",0);
    if(rw64<0||rw64>16384||rh64<0||rh64>16384)throw std::invalid_argument("Invalid reference dimensions");
    int rw=int(rw64),rh=int(rh64);
    if(cache){if(!rw)rw=cache->info().referenceW;if(!rh)rh=cache->info().referenceH;}
    auto process=[&](const Image& src,int64_t n){Mask m;if(cache)m=cache->readSample(n);return render(src,p,cache?&m:nullptr,rw,rh);};
    if(cmd=="png"){
        if(get("input").empty()||get("output").empty())throw std::invalid_argument("PNG needs input and output paths");
        if(std::filesystem::exists(get("output")))throw std::runtime_error("Output already exists; choose a new path to preserve original files");
        auto im=loadPng(get("input"));savePng(get("output"),process(im,sample));return 0;
    }
    if(cmd=="stream"){
        auto wi=integer("width",0),hi=integer("height",0),count=integer("frames",0);
        if(wi<1||wi>16384||hi<1||hi>16384||count<1||count>1000000)throw std::invalid_argument("Invalid stream dimensions/count");
        Image im{int(wi),int(hi)};
        for(int64_t i=0;i<count;++i){if(!std::cin.read(reinterpret_cast<char*>(im.rgba.data()),im.rgba.size()))throw std::runtime_error("Truncated input stream");auto out=process(im,sample+i);std::cout.write(reinterpret_cast<const char*>(out.rgba.data()),out.rgba.size());if(!std::cout)throw std::runtime_error("Output stream failed");}
        return 0;
    }
    throw std::invalid_argument("Unknown command: "+cmd);
}catch(const std::exception& e){std::cerr<<"StudioBackground: "<<e.what()<<'\n';return 2;}}
