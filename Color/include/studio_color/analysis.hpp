// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "color.hpp"
#include <atomic>
#include <functional>
namespace studio_color {
struct ROI { double x=0,y=0,width=1,height=1; };
struct FrameStats {
 bool valid=false;
 std::uint64_t pixels=0,neutralPixels=0;
 double p10=0,p50=0,p90=0,p99=0; // Linear-light luminance quantiles, not signal levels.
 double logRedGreen=0,logBlueGreen=0;
 double neutralFraction=0,neutralCoverage=0,saturatedFraction=0,clippedFraction=0;
 double brightnessConfidence=0,whiteConfidence=0;
 std::array<double,32> histogram{};
};
FrameStats analyzeRGBA(const std::uint8_t* data,int width,int height,std::ptrdiff_t stride=0, int transfer=0,ROI roi={});
struct ShotStats {
 bool valid=false;
 FrameStats median;
 std::size_t validFrames=0;
 double variationEV=0;
 bool mixedContent=false;
};
ShotStats aggregate(const std::vector<FrameStats>& frames);
enum class MatchMode { ConservativeAuto, Reference };
struct AutoCorrection {
 double exposure=0,redEV=0,blueEV=0,contrast=0;
 double confidence=0;
 bool applied=false;
 std::vector<std::string> warnings;
};
AutoCorrection balanceShot(const ShotStats& shot);
AutoCorrection matchShot(const ShotStats& shot,const ShotStats& reference,MatchMode mode=MatchMode::ConservativeAuto);
Params applyCorrection(Params params,const AutoCorrection& autoCorrection);
struct VisualClip {
 std::string id;
 bool hasVideo=true;
 bool supportedSDR=true;
 bool locked=false;
 bool graphic=false; // Host metadata: titles, solid colours, logos. Never guessed just from hue.
 std::string fingerprint; // Original content/in/out/retime/upstream effects, supplied by host.
 Params existing;
 ShotStats statistics;
};
struct BatchResult {
 std::vector<std::pair<std::string,Params>> changes;
 std::vector<std::pair<std::string,AutoCorrection>> reports;
 std::string referenceId;
 std::size_t audioSkipped=0,unsupportedSkipped=0,lockedSkipped=0;
 bool cancelled=false;
};
// Pure planner: never changes a timeline or writes a project. Host commits the whole plan atomically.
BatchResult planBatch(const std::vector<VisualClip>& clips,const std::string& explicitReference={},
 bool matchGraphics=false,const std::atomic_bool* cancel=nullptr);
// Sampling positions are relative to the visible, retimed clip; decoding is the host's job.
std::vector<std::int64_t> samplePositions(std::int64_t visibleFrames,int maximum=12);
} // namespace studio_color
