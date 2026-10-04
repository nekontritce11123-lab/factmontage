// SPDX-License-Identifier: GPL-3.0-only
#include "studio_background/core.hpp"
#include <openssl/evp.h>
#include <fstream>
#include <memory>
#include <iomanip>
#include <sstream>
namespace sbg {
std::string sha256File(const std::string& path){
    std::ifstream f(path,std::ios::binary);if(!f)throw std::runtime_error("Cannot hash file: "+path);
    std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(),EVP_MD_CTX_free);
    if(!ctx||EVP_DigestInit_ex(ctx.get(),EVP_sha256(),nullptr)!=1)throw std::runtime_error("SHA256 initialization failed");
    char buf[65536];while(f){f.read(buf,sizeof(buf));if(EVP_DigestUpdate(ctx.get(),buf,f.gcount())!=1)throw std::runtime_error("SHA256 update failed");}
    if(!f.eof())throw std::runtime_error("Read failed during SHA256");
    unsigned char digest[EVP_MAX_MD_SIZE];unsigned len=0;
    if(EVP_DigestFinal_ex(ctx.get(),digest,&len)!=1)throw std::runtime_error("SHA256 finalization failed");
    std::ostringstream out;out<<std::hex<<std::setfill('0');for(unsigned i=0;i<len;++i)out<<std::setw(2)<<int(digest[i]);return out.str();
}
}
