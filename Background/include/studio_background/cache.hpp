// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "core.hpp"
#include <cstdio>
#include <fstream>
#include <memory>
namespace sbg {
struct CacheInfo {
    uint32_t width=0,height=0,frames=0,fpsNum=25,fpsDen=1,referenceW=0,referenceH=0;
    uint64_t firstSample=0;
    std::string sourceSha256,recipeSha256;
    void validate() const;
};
// Portable little-endian file. Independent zlib frames, bounded random access,
// header+per-frame CRCs and exact sample addressing. Not a recomputable temp cache:
// the published file is an immutable project asset.
class CacheWriter {
    std::string target_,temp_;
    FILE* file_=nullptr;
    CacheInfo info_;
    struct Entry {uint64_t offset;uint32_t bytes,crc;};
    std::vector<Entry> index_;
    bool published_=false;
public:
    CacheWriter(const std::string& target,const CacheInfo&);
    ~CacheWriter();
    CacheWriter(const CacheWriter&)=delete;
    CacheWriter& operator=(const CacheWriter&)=delete;
    void append(const Mask&);
    void finish();
};
class CacheReader {
    std::ifstream file_;
    CacheInfo info_;
    struct Entry {uint64_t offset;uint32_t bytes,crc;};
    std::vector<Entry> index_;
public:
    explicit CacheReader(const std::string& path);
    const CacheInfo& info() const {return info_;}
    void requireIdentity(const std::string& source,const std::string& recipe,uint32_t fpsNum,uint32_t fpsDen) const;
    Mask readSample(uint64_t sample);
};
}
