// SPDX-License-Identifier: GPL-3.0-only
// Native CPU-only human segmentation worker. Input is a stream of *the same pre-effect
// MLT frames* that will be rendered; ffmpeg is not substituted for the editor decoder.
#include "studio_background/cache.hpp"
#include <onnxruntime_cxx_api.h>
#include <algorithm>
#include <csignal>
#include <iostream>
#include <map>
#include <memory>
#include <vector>
#include <unistd.h>
using namespace sbg;
namespace {
volatile std::sig_atomic_t canceled=0;
void cancelSignal(int){canceled=1;}
constexpr int ModelWidth=256, ModelHeight=144;
class HumanSeg {
    Ort::Env env_{ORT_LOGGING_LEVEL_WARNING,"studio-background"};
    Ort::SessionOptions options_;
    std::unique_ptr<Ort::Session> session_;
    std::vector<float> rgb_ = std::vector<float>(3*ModelWidth*ModelHeight);
public:
    HumanSeg(const std::string& path,int threads){
        options_.SetIntraOpNumThreads(threads);
        options_.SetInterOpNumThreads(1);
        options_.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        options_.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        options_.AddConfigEntry("session.intra_op.allow_spinning","0");
        options_.AddConfigEntry("session.inter_op.allow_spinning","0");
        session_=std::make_unique<Ort::Session>(env_,path.c_str(),options_);
        // Static ONNX export of official PP-HumanSegV2-Lite portrait 256x144.
        Ort::AllocatorWithDefaultOptions allocator;
        if(session_->GetInputCount()!=1||session_->GetOutputCount()!=1)throw std::runtime_error("Expected PP-HumanSegV2-Lite ONNX model");
        auto in=session_->GetInputNameAllocated(0,allocator);
        auto out=session_->GetOutputNameAllocated(0,allocator);
        if(std::string(in.get())!="x"||std::string(out.get())!="save_infer_model/scale_0.tmp_0"
           ||session_->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetElementType()!=ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT
           ||session_->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape()!=std::vector<int64_t>{1,3,ModelHeight,ModelWidth})
            throw std::runtime_error("Unexpected PP-HumanSegV2-Lite input contract");
    }
    Mask next(const Image& im){
        im.validate();
        const auto sample=[&](float x,float y,int channel){
            x=std::clamp(x,0.f,float(im.w-1));y=std::clamp(y,0.f,float(im.h-1));
            const int x0=int(x),y0=int(y),x1=std::min(x0+1,im.w-1),y1=std::min(y0+1,im.h-1);
            const float fx=x-x0,fy=y-y0;
            const auto at=[&](int xx,int yy){return float(im.rgba[(std::size_t(yy)*im.w+xx)*4+channel]);};
            return ((1-fx)*at(x0,y0)+fx*at(x1,y0))*(1-fy)+((1-fx)*at(x0,y1)+fx*at(x1,y1))*fy;
        };
        constexpr std::size_t plane=ModelWidth*ModelHeight;
        for(int y=0;y<ModelHeight;++y)for(int x=0;x<ModelWidth;++x)for(int c=0;c<3;++c)
            rgb_[c*plane+y*ModelWidth+x]=sample((x+.5f)*im.w/ModelWidth-.5f,(y+.5f)*im.h/ModelHeight-.5f,c)/127.5f-1.f;
        auto memory=Ort::MemoryInfo::CreateCpu(OrtArenaAllocator,OrtMemTypeDefault);
        std::array<int64_t,4> srcShape{1,3,ModelHeight,ModelWidth};
        auto input=Ort::Value::CreateTensor<float>(memory,rgb_.data(),rgb_.size(),srcShape.data(),srcShape.size());
        const char* inNames[]={"x"},*outNames[]={"save_infer_model/scale_0.tmp_0"};
        auto output=session_->Run(Ort::RunOptions{nullptr},inNames,&input,1,outNames,1);
        auto info=output[0].GetTensorTypeAndShapeInfo();
        if(info.GetElementType()!=ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT||info.GetShape()!=std::vector<int64_t>{1,2,ModelHeight,ModelWidth})throw std::runtime_error("Unexpected human alpha shape");
        Mask mask(im.w,im.h);const float* alpha=output[0].GetTensorData<float>()+plane;
        for(int y=0;y<im.h;++y)for(int x=0;x<im.w;++x){
            const float px=std::clamp((x+.5f)*ModelWidth/im.w-.5f,0.f,float(ModelWidth-1));
            const float py=std::clamp((y+.5f)*ModelHeight/im.h-.5f,0.f,float(ModelHeight-1));
            const int x0=int(px),y0=int(py),x1=std::min(x0+1,ModelWidth-1),y1=std::min(y0+1,ModelHeight-1);
            const float fx=px-x0,fy=py-y0;
            const float value=((1-fx)*alpha[y0*ModelWidth+x0]+fx*alpha[y0*ModelWidth+x1])*(1-fy)
                             +((1-fx)*alpha[y1*ModelWidth+x0]+fx*alpha[y1*ModelWidth+x1])*fy;
            if(!std::isfinite(value))throw std::runtime_error("Human segmentation returned nonfinite alpha");
            mask.alpha[std::size_t(y)*im.w+x]=std::clamp(value,0.f,1.f);
        }
        return mask;
    }
};
}
int main(int argc,char** argv){try{
    if(argc==2&&std::string(argv[1])=="--capabilities"){
        std::cout<<"{\"protocol\":1,\"engine\":\"pp-humansegv2-lite-portrait\",\"execution\":\"cpu\",\"input\":\"rgba8888-raw\",\"analysis_sizes\":[512,768,1280],\"max_threads\":2}\n";return 0;}
    std::signal(SIGTERM,cancelSignal);std::signal(SIGINT,cancelSignal);
    std::map<std::string,std::string> args;
    for(int i=1;i<argc;i+=2){if(i+1>=argc||std::string(argv[i]).rfind("--",0)!=0)throw std::invalid_argument("Expected --key value");args[argv[i]+2]=argv[i+1];}
    const std::vector<std::string> keys={"model","model-sha256","output","width","height","frames","fps-num","fps-den","reference-w","reference-h","first-sample","source-sha256","source-file","recipe-sha256","threads"};
    for(const auto& [key,value]:args)if(std::find(keys.begin(),keys.end(),key)==keys.end())throw std::invalid_argument("Unknown argument: "+key);
    auto value=[&](const char* key,const std::string& def=""){auto it=args.find(key);return it==args.end()?def:it->second;};
    auto number=[&](const char* key,int64_t def){auto s=value(key,std::to_string(def));std::size_t end=0;auto n=std::stoll(s,&end);if(end!=s.size()||n<0||n>1000000000)throw std::invalid_argument("Invalid numeric argument");return n;};
    const std::string model=value("model"),expected=value("model-sha256");
    if(expected.size()!=64||sha256File(model)!=expected)throw std::runtime_error("Model missing or SHA256 mismatch; no downloads happen inside analysis");
    int threads=int(number("threads",2));if(threads<1||threads>2)throw std::invalid_argument("Threads must be 1..2");
    CacheInfo info;info.width=number("width",0);info.height=number("height",0);info.frames=number("frames",0);info.fpsNum=number("fps-num",25);info.fpsDen=number("fps-den",1);info.referenceW=number("reference-w",info.width);info.referenceH=number("reference-h",info.height);info.firstSample=number("first-sample",0);info.sourceSha256=value("source-sha256");
    if(info.sourceSha256.empty())info.sourceSha256=sha256File(value("source-file"));
    info.recipeSha256=value("recipe-sha256");info.validate();
    if(std::max(info.width,info.height)>1280)throw std::invalid_argument("Analysis max long side is 1280");
    // MLT supplies the selected analysis size; the model resizes it to 256x144.
    if(canceled)return 130;
    HumanSeg modelRunner(model,threads);CacheWriter writer(value("output"),info);Image frame(info.width,info.height);
    for(uint32_t i=0;i<info.frames;++i){
        if(canceled)return 130;
        if(!std::cin.read(reinterpret_cast<char*>(frame.rgba.data()),frame.rgba.size()))throw std::runtime_error("Получено "+std::to_string(i)+" из "+std::to_string(info.frames)+" кадров MLT");
        if(canceled)return 130;
        auto matte=modelRunner.next(frame);if(canceled)return 130;writer.append(matte);
        std::cout<<"{\"done\":"<<i+1<<",\"total\":"<<info.frames<<"}\n"<<std::flush;
    }
    if(canceled)return 130;
    writer.finish();std::cout<<"{\"published\":true,\"source_sha256\":\""<<info.sourceSha256<<"\"}\n";return 0;
}catch(const std::exception& e){std::cerr<<"StudioBackground analysis: "<<e.what()<<'\n';return 2;}}
