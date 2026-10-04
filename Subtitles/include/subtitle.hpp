// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <filesystem>
#include <string>
#include <string_view>
#include <stdexcept>
#include <vector>

namespace studio_subtitles {
namespace fs = std::filesystem;

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Cancelled : Error {
    Cancelled(): Error("Обработка субтитров отменена; результат не опубликован.") {}
};

struct Word {
    std::int64_t startMs = 0;
    std::int64_t endMs = 0;
    std::string text;
    double confidence = 1.0;
};

struct Block {
    std::uint64_t id = 0;
    std::int64_t startMs = 0;
    std::int64_t endMs = 0;
    std::int64_t speechEndMs = 0;
    bool timingConflict = false;
    std::string line1;
    std::vector<Word> words;
    bool timedWords = true;
};

struct LayoutOptions {
    std::int64_t minDurationMs = 650;
    std::int64_t maxDurationMs = 3200;
    double maxReadingCps = 24.0;
    double maxWidth = 28.0;
    std::function<double(std::string_view)> measure;
};

struct RenderOptions {
    std::string title = "Studio Subtitles";
    std::string font = "DejaVu Sans";
    int fontSize = 64;
    bool bold = true;
    std::uint32_t textColor = 0x00FFFFFF;
    std::uint32_t outlineColor = 0x00000000;
    std::uint32_t backgroundColor = 0x66000000;
    int outline = 3;
    int shadow = 0;
    int alignment = 2;
    int background = 1;
    int marginV = 60;
    std::string animation = "none";
};

struct WorkerSettings {
    fs::path audio;
    fs::path model;
    fs::path vadModel;
    fs::path whisperCli = "whisper-cli";
    fs::path outDir;
    std::string sourceSha256;
    std::string modelSha256;
    std::int64_t zoneStartMs = 0;
    std::int64_t zoneEndMs = 0;
    int timeoutSeconds = 7200;
    LayoutOptions layout;
    RenderOptions render;
};

struct WorkerResult {
    fs::path wordsJson;
    fs::path blocksJson;
    fs::path ass;
    std::string sourceSha256;
    std::string modelSha256;
    bool cacheHit = false;
};

using Progress = std::function<void(int, const std::string&)>;

double defaultMeasure(std::string_view text);
std::vector<Word> parseWhisperJson(std::string_view json, std::int64_t zoneStartMs = 0);
std::vector<Block> layoutWords(const std::vector<Word>& words, const LayoutOptions& options = {});
bool splitBlock(std::vector<Block>& blocks, std::size_t blockIndex, std::size_t wordIndex, const LayoutOptions& options = {});
bool mergeBlocks(std::vector<Block>& blocks, std::size_t leftIndex, const LayoutOptions& options = {});
std::string renderAss(const std::vector<Block>& blocks, const RenderOptions& options = {});
std::vector<Block> parseRenderBlocksJson(std::string_view json);
std::string renderWordsJson(const std::vector<Word>& words);
std::string renderBlocksJson(const std::vector<Block>& blocks);
WorkerResult runWorker(const WorkerSettings& settings, const std::atomic_bool& cancelled, Progress progress = {});

} // namespace studio_subtitles
