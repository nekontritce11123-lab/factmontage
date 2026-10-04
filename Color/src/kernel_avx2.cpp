// SPDX-License-Identifier: GPL-3.0-or-later
// Runtime dispatch: the baseline binary does not require AVX2.
#include "studio_color/color.hpp"
#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>
namespace studio_color {
bool avx2Available() noexcept { static const bool available=__builtin_cpu_supports("avx2"); return available; }
__attribute__((target("avx2")))
int processAvx2(const std::uint8_t* src,std::uint8_t* dst,int width,const LutNode* nodes) noexcept {
 static_assert(LutSize==33 && sizeof(LutNode)==8,"AVX2 layout must track the LUT contract");
 const auto z=_mm256_setzero_si256(), one=_mm256_set1_epi32(1), mask8=_mm256_set1_epi32(255);
 const auto mask16=_mm256_set1_epi32(65535), n31=_mm256_set1_epi32(31), n33=_mm256_set1_epi32(33);
 const auto n1089=_mm256_set1_epi32(1089), diag=_mm256_set1_epi32(1123);
 int x=0;
 for(;x+8<=width;x+=8,src+=32,dst+=32){
  const auto in=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(src));
  const auto rp=_mm256_slli_epi32(_mm256_and_si256(in,mask8),5);
  const auto gp=_mm256_slli_epi32(_mm256_and_si256(_mm256_srli_epi32(in,8),mask8),5);
  const auto bp=_mm256_slli_epi32(_mm256_and_si256(_mm256_srli_epi32(in,16),mask8),5);
  // floor(p/255) is exact here (p in [0,8160]); keep endpoint in cell 31.
  const auto ri=_mm256_min_epi32(n31,_mm256_srli_epi32(_mm256_add_epi32(_mm256_add_epi32(rp,one),_mm256_srli_epi32(rp,8)),8));
  const auto gi=_mm256_min_epi32(n31,_mm256_srli_epi32(_mm256_add_epi32(_mm256_add_epi32(gp,one),_mm256_srli_epi32(gp,8)),8));
  const auto bi=_mm256_min_epi32(n31,_mm256_srli_epi32(_mm256_add_epi32(_mm256_add_epi32(bp,one),_mm256_srli_epi32(bp,8)),8));
  const auto rf=_mm256_sub_epi32(rp,_mm256_sub_epi32(_mm256_slli_epi32(ri,8),ri));
  const auto gf=_mm256_sub_epi32(gp,_mm256_sub_epi32(_mm256_slli_epi32(gi,8),gi));
  const auto bf=_mm256_sub_epi32(bp,_mm256_sub_epi32(_mm256_slli_epi32(bi,8),bi));
  const auto hi=_mm256_max_epi32(rf,_mm256_max_epi32(gf,bf));
  const auto lo=_mm256_min_epi32(rf,_mm256_min_epi32(gf,bf));
  const auto mid=_mm256_sub_epi32(_mm256_add_epi32(rf,_mm256_add_epi32(gf,bf)),_mm256_add_epi32(hi,lo));
  const auto offHi=_mm256_blendv_epi8(_mm256_blendv_epi8(n1089,n33,_mm256_cmpeq_epi32(gf,hi)),one,_mm256_cmpeq_epi32(rf,hi));
  const auto offLo=_mm256_blendv_epi8(_mm256_blendv_epi8(n1089,n33,_mm256_cmpeq_epi32(gf,lo)),one,_mm256_cmpeq_epi32(rf,lo));
  const auto base=_mm256_add_epi32(ri,_mm256_add_epi32(_mm256_mullo_epi32(gi,n33),_mm256_mullo_epi32(bi,n1089)));
  const __m256i offsets[4]={base,_mm256_add_epi32(base,offHi),_mm256_add_epi32(base,_mm256_sub_epi32(diag,offLo)),_mm256_add_epi32(base,diag)};
  const __m256i weights[4]={_mm256_sub_epi32(mask8,hi),_mm256_sub_epi32(hi,mid),_mm256_sub_epi32(mid,lo),lo};
  auto r=z,g=z,b=z;
  for(int corner=0;corner<4;++corner){
   // Gather accesses have intrinsic unaligned/alias semantics; no scalar type-punned reads.
   const auto rg=_mm256_i32gather_epi32(reinterpret_cast<const int*>(nodes),offsets[corner],8);
   const auto bb=_mm256_i32gather_epi32(reinterpret_cast<const int*>(reinterpret_cast<const char*>(nodes)+4),offsets[corner],8);
   r=_mm256_add_epi32(r,_mm256_mullo_epi32(_mm256_and_si256(rg,mask16),weights[corner]));
   g=_mm256_add_epi32(g,_mm256_mullo_epi32(_mm256_srli_epi32(rg,16),weights[corner]));
   b=_mm256_add_epi32(b,_mm256_mullo_epi32(_mm256_and_si256(bb,mask16),weights[corner]));
  }
  r=_mm256_add_epi32(r,_mm256_set1_epi32(32767));g=_mm256_add_epi32(g,_mm256_set1_epi32(32767));b=_mm256_add_epi32(b,_mm256_set1_epi32(32767));
  // Exact unsigned division by 65535 for these bounded sums (<2^24).
  r=_mm256_srli_epi32(_mm256_add_epi32(_mm256_add_epi32(r,one),_mm256_srli_epi32(r,16)),16);
  g=_mm256_srli_epi32(_mm256_add_epi32(_mm256_add_epi32(g,one),_mm256_srli_epi32(g,16)),16);
  b=_mm256_srli_epi32(_mm256_add_epi32(_mm256_add_epi32(b,one),_mm256_srli_epi32(b,16)),16);
  const auto alpha=_mm256_and_si256(in,_mm256_set1_epi32(static_cast<int>(0xff000000u)));
  auto out=_mm256_or_si256(alpha,_mm256_or_si256(r,_mm256_or_si256(_mm256_slli_epi32(g,8),_mm256_slli_epi32(b,16))));
  out=_mm256_blendv_epi8(out,in,_mm256_cmpeq_epi32(alpha,z));
  _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst),out);
 }
 return x;
}
}
#else
namespace studio_color {
bool avx2Available() noexcept { return false; }
int processAvx2(const std::uint8_t*,std::uint8_t*,int,const LutNode*) noexcept { return 0; }
}
#endif
