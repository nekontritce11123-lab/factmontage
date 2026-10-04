// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "params.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <vector>
namespace sbg {
constexpr std::size_t MaxPixels = 4096u * 2304u;
struct Image {
    int w=0,h=0;
    std::vector<uint8_t> rgba;
    Image() = default;
    Image(int width,int height);
    void validate() const;
};
struct Mask {
    int w=0,h=0;
    std::vector<float> alpha;
    Mask()=default;
    Mask(int width,int height,float value=0);
    void validate() const;
};
struct Sample { std::array<double,3> rgb; double tolerance; };
Sample pickKey(const Image&,int x,int y,int radius=3);
Mask chroma(const Image&,const Params&);
Mask resizeMask(const Mask&,int w,int h);
Image resizeImage(const Image&,int w,int h);
void boxBlur(std::vector<float>& plane,int w,int h,int radius);
void refineMask(Mask&,const Image&,const Params&,int referenceW,int referenceH);
// No state between frames: seeking and threaded export must yield identical results.
Image render(const Image&,const Params&,const Mask* cached=nullptr,
             int referenceW=0,int referenceH=0);
Image loadPng(const std::string& path);
void savePng(const std::string& path,const Image&);
std::string sha256File(const std::string& path);
}
