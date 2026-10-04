// SPDX-License-Identifier: GPL-3.0-only
#include "studio_background/cache.hpp"
#include <zlib.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <fcntl.h>
#include <unistd.h>
namespace sbg {
static constexpr uint64_t HeaderBytes=256;
static constexpr uint32_t MaxFrames=1000000;
static bool hashValid(const std::string& s){return s.size()==64&&std::all_of(s.begin(),s.end(),[](char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f');});}
void CacheInfo::validate() const {
    if(width<1||height<1||width>4096||height>4096||uint64_t(width)*height>1920u*1080u||frames<1||frames>MaxFrames||fpsNum<1||fpsNum>1000000||fpsDen<1||fpsDen>1000000||double(fpsNum)/fpsDen<1||double(fpsNum)/fpsDen>240||referenceW<1||referenceH<1||referenceW>16384||referenceH>16384||uint64_t(referenceW)*referenceH>MaxPixels||firstSample>1000000000ull||!hashValid(sourceSha256)||!hashValid(recipeSha256))
        throw std::invalid_argument("Invalid SBG header/identity/fps");
}
static void put32(uint8_t* p,uint32_t n){for(int i=0;i<4;++i)p[i]=uint8_t(n>>(8*i));}
static void put64(uint8_t* p,uint64_t n){for(int i=0;i<8;++i)p[i]=uint8_t(n>>(8*i));}
static uint32_t get32(const uint8_t* p){uint32_t v=0;for(int i=0;i<4;++i)v|=uint32_t(p[i])<<(8*i);return v;}
static uint64_t get64(const uint8_t* p){uint64_t v=0;for(int i=0;i<8;++i)v|=uint64_t(p[i])<<(8*i);return v;}
static void writeAll(FILE* f,const void* p,std::size_t n){if(fwrite(p,1,n,f)!=n)throw std::runtime_error("SBG write failed (disk full or I/O error)");}
static void readAll(std::ifstream& f,void* p,std::size_t n){if(!f.read(static_cast<char*>(p),std::streamsize(n)))throw std::runtime_error("Truncated SBG asset");}
static std::array<uint8_t,HeaderBytes> header(const CacheInfo& i,uint64_t index){
    std::array<uint8_t,HeaderBytes> h{};std::memcpy(h.data(),"SBG0001",8);
    put32(h.data()+8,1);put32(h.data()+12,i.width);put32(h.data()+16,i.height);put32(h.data()+20,i.frames);put32(h.data()+24,i.fpsNum);put32(h.data()+28,i.fpsDen);put32(h.data()+32,i.referenceW);put32(h.data()+36,i.referenceH);put64(h.data()+40,i.firstSample);put64(h.data()+48,index);
    std::memcpy(h.data()+56,i.sourceSha256.data(),64);std::memcpy(h.data()+120,i.recipeSha256.data(),64);
    put32(h.data()+252,uint32_t(crc32(0,h.data(),252)));return h;
}
CacheWriter::CacheWriter(const std::string& target,const CacheInfo& info):target_(target),info_(info){
    info_.validate();if(std::filesystem::exists(target_))throw std::runtime_error("SBG target already exists; assets are immutable");
    std::string pattern=target_+".part.XXXXXX";std::vector<char> name(pattern.begin(),pattern.end());name.push_back(0);
    int fd=mkstemp(name.data());if(fd<0)throw std::runtime_error("Cannot create project asset temporary file");temp_=name.data();
    file_=fdopen(fd,"w+b");if(!file_){close(fd);unlink(temp_.c_str());throw std::runtime_error("Cannot open temporary asset stream");}
    try{auto h=header(info_,0);writeAll(file_,h.data(),h.size());index_.reserve(info_.frames);}catch(...){fclose(file_);file_=nullptr;unlink(temp_.c_str());throw;}
}
CacheWriter::~CacheWriter(){if(file_)fclose(file_);if(!temp_.empty())unlink(temp_.c_str());}
void CacheWriter::append(const Mask& mask){
    if(!file_||published_||index_.size()>=info_.frames)throw std::runtime_error("Invalid SBG append state");
    mask.validate();if(mask.w!=int(info_.width)||mask.h!=int(info_.height))throw std::invalid_argument("SBG mask dimensions changed");
    std::vector<uint8_t> raw(mask.alpha.size());for(std::size_t i=0;i<raw.size();++i)raw[i]=uint8_t(std::lround(mask.alpha[i]*255));
    uLongf n=compressBound(raw.size());std::vector<uint8_t> compressed(n);
    if(compress2(compressed.data(),&n,raw.data(),raw.size(),Z_BEST_SPEED)!=Z_OK)throw std::runtime_error("SBG compression failed");
    auto pos=ftello(file_);if(pos<0)throw std::runtime_error("SBG tell failed");
    writeAll(file_,compressed.data(),n);
    index_.push_back({uint64_t(pos),uint32_t(n),uint32_t(crc32(0,raw.data(),raw.size()))});
}
void CacheWriter::finish(){
    if(!file_||published_||index_.size()!=info_.frames)throw std::runtime_error("Incomplete SBG cannot be published");
    auto at=ftello(file_);if(at<0)throw std::runtime_error("SBG index position failed");
    std::array<uint8_t,16> b{};for(const auto& e:index_){put64(b.data(),e.offset);put32(b.data()+8,e.bytes);put32(b.data()+12,e.crc);writeAll(file_,b.data(),b.size());}
    auto h=header(info_,uint64_t(at));if(fseeko(file_,0,SEEK_SET))throw std::runtime_error("SBG header seek failed");writeAll(file_,h.data(),h.size());
    if(fflush(file_)||fsync(fileno(file_)))throw std::runtime_error("SBG fsync failed");
    if(fclose(file_)){file_=nullptr;throw std::runtime_error("SBG close failed");}file_=nullptr;
    // Publish in the same directory without overwriting an existing asset.
    // Linux uses renameat2; another POSIX host uses the hard-link fallback.
    // Filesystem support, especially microSD/exFAT, still needs device testing.
#ifdef __linux__
    if(renameat2(AT_FDCWD,temp_.c_str(),AT_FDCWD,target_.c_str(),RENAME_NOREPLACE)!=0)
        throw std::runtime_error("SBG atomic publish failed: "+std::string(strerror(errno)));
#else
    if(link(temp_.c_str(),target_.c_str())!=0)throw std::runtime_error("SBG atomic publish failed");
    if(unlink(temp_.c_str())!=0)throw std::runtime_error("SBG temporary unlink failed");
#endif
    temp_.clear();published_=true;
    auto parent=std::filesystem::path(target_).parent_path();if(parent.empty())parent=".";
    int dfd=open(parent.c_str(),O_RDONLY|O_DIRECTORY);
    if(dfd<0)throw std::runtime_error("SBG published but directory open for fsync failed");
    int rc=fsync(dfd);int saved=errno;close(dfd);
    if(rc && saved!=EINVAL)throw std::runtime_error("SBG published but directory fsync failed");
}
CacheReader::CacheReader(const std::string& path):file_(path,std::ios::binary){
    if(!file_)throw std::runtime_error("Mask asset missing or unreadable: "+path);
    file_.seekg(0,std::ios::end);auto end=file_.tellg();if(end<std::streamoff(HeaderBytes))throw std::runtime_error("Truncated SBG header");file_.seekg(0);
    std::array<uint8_t,HeaderBytes> h{};readAll(file_,h.data(),h.size());
    if(std::memcmp(h.data(),"SBG0001",8)||get32(h.data()+8)!=1||get32(h.data()+252)!=crc32(0,h.data(),252))throw std::runtime_error("Corrupted or unsupported SBG header");
    info_.width=get32(h.data()+12);info_.height=get32(h.data()+16);info_.frames=get32(h.data()+20);info_.fpsNum=get32(h.data()+24);info_.fpsDen=get32(h.data()+28);info_.referenceW=get32(h.data()+32);info_.referenceH=get32(h.data()+36);info_.firstSample=get64(h.data()+40);info_.sourceSha256.assign(reinterpret_cast<char*>(h.data()+56),64);info_.recipeSha256.assign(reinterpret_cast<char*>(h.data()+120),64);info_.validate();
    uint64_t indexAt=get64(h.data()+48),endSize=uint64_t(end);
    if(indexAt<HeaderBytes||indexAt>endSize||uint64_t(info_.frames)*16!=endSize-indexAt)throw std::runtime_error("Corrupted SBG index size");
    file_.seekg(std::streamoff(indexAt));index_.reserve(info_.frames);std::array<uint8_t,16> b{};uint64_t next=HeaderBytes;
    const uint64_t bound=compressBound(uint64_t(info_.width)*info_.height);
    for(uint32_t i=0;i<info_.frames;++i){readAll(file_,b.data(),b.size());Entry e{get64(b.data()),get32(b.data()+8),get32(b.data()+12)};
        if(e.offset!=next||e.offset>indexAt||e.bytes<1||e.bytes>bound||e.bytes>indexAt-e.offset)throw std::runtime_error("Corrupted SBG index entry");
        next=e.offset+e.bytes;index_.push_back(e);}
    if(next!=indexAt)throw std::runtime_error("SBG contains unindexed data");
}
void CacheReader::requireIdentity(const std::string& source,const std::string& recipe,uint32_t n,uint32_t d) const {
    if(source!=info_.sourceSha256||recipe!=info_.recipeSha256||uint64_t(n)*info_.fpsDen!=uint64_t(d)*info_.fpsNum)
        throw std::runtime_error("Stale matte: source, analysis recipe or project FPS changed");
}
Mask CacheReader::readSample(uint64_t sample){
    if(sample<info_.firstSample||sample-info_.firstSample>=info_.frames)throw std::out_of_range("Frame is outside analyzed range; re-analyze this clip");
    const auto& e=index_[std::size_t(sample-info_.firstSample)];std::vector<uint8_t> compressed(e.bytes),raw(std::size_t(info_.width)*info_.height);
    file_.clear();file_.seekg(std::streamoff(e.offset));readAll(file_,compressed.data(),compressed.size());uLongf count=raw.size();
    if(uncompress(raw.data(),&count,compressed.data(),compressed.size())!=Z_OK||count!=raw.size()||crc32(0,raw.data(),raw.size())!=e.crc)throw std::runtime_error("Corrupted SBG frame");
    Mask out(info_.width,info_.height);for(std::size_t i=0;i<raw.size();++i)out.alpha[i]=raw[i]/255.f;return out;
}
}
