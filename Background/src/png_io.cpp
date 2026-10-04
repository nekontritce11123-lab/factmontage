// SPDX-License-Identifier: GPL-3.0-only
#include "studio_background/core.hpp"
#include <png.h>
#include <stdexcept>
namespace sbg {
Image loadPng(const std::string& path){
    png_image p{};p.version=PNG_IMAGE_VERSION;
    if(!png_image_begin_read_from_file(&p,path.c_str()))throw std::runtime_error("PNG read: "+std::string(p.message));
    try {p.format=PNG_FORMAT_RGBA;Image im(p.width,p.height);
        if(!png_image_finish_read(&p,nullptr,im.rgba.data(),0,nullptr))throw std::runtime_error("PNG decode: "+std::string(p.message));
        png_image_free(&p);return im;
    }catch(...){png_image_free(&p);throw;}
}
void savePng(const std::string& path,const Image& im){im.validate();png_image p{};p.version=PNG_IMAGE_VERSION;p.width=im.w;p.height=im.h;p.format=PNG_FORMAT_RGBA;
    if(!png_image_write_to_file(&p,path.c_str(),0,im.rgba.data(),0,nullptr))throw std::runtime_error("PNG write: "+std::string(p.message));
}
}
