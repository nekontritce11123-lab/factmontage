// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "params_generated.hpp"
#include <array>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <mutex>
#include <list>
#include <string>
#include <vector>
namespace studio_color {
inline constexpr int LutSize = 33;
bool avx2Available() noexcept;
struct RGB { double r, g, b; };
Params sanitized(Params p) noexcept;
bool equalParams(const Params& a, const Params& b) noexcept;
bool setParameter(Params& p, std::string_view id, double value) noexcept;
Params withManual(const Params& existing, const Params& preset) noexcept;
// Identity is exact including hidden RGB and alpha; protection alone does not alter a frame.
bool isIdentity(const Params& p) noexcept;
double decodeSignal(double signal, int transfer=0) noexcept;
double encodeSignal(double linear, int transfer=0) noexcept;
RGB transformExact(RGB signal, Params p) noexcept;
struct LutNode { std::uint16_t r, g, b, padding; };
class Lut {
public:
 explicit Lut(Params p);
 Params parameters() const noexcept { return p_; }
 void process(const std::uint8_t* input, std::uint8_t* output, int width, int height,
              std::ptrdiff_t inputStride=0, std::ptrdiff_t outputStride=0, bool allowSimd=true) const;
 RGB sample(double r, double g, double b) const noexcept;
 void writeCube(const std::string& path) const;
 const std::vector<LutNode>& nodes() const noexcept { return nodes_; }
private:
 struct Axis { int index; int fraction; };
 Params p_;
 bool identity_;
 std::array<Axis,256> axes_{};
 std::vector<LutNode> nodes_;
};
// Eight immutable LUTs (~2.2 MiB of node data), shared across effects. Build outside lock.
class LutCache {
public:
 explicit LutCache(std::size_t limit=8):limit_(limit?limit:1){}
 std::shared_ptr<const Lut> get(Params p);
 std::size_t size() const;
private:
 struct Entry { Params p; std::shared_ptr<const Lut> lut; };
 std::size_t limit_;
 mutable std::mutex mutex_;
 std::list<Entry> lru_;
};
} // namespace studio_color
