// SPDX-License-Identifier: MIT
#pragma once
#include "parameters.hpp"
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
namespace studio_audio {
    namespace fs = std::filesystem;
    struct Error: std::runtime_error {
        using std::runtime_error::runtime_error;
    };
    struct Cancelled: Error {
        Cancelled(): Error("Обработка отменена; результат не опубликован.") {
        }
    };
    struct Interval {
        double start = 0, end = 0;
    };
    struct Point {
        double time = 0, gainDb = 0;
    };
    struct Meter {
        double integrated = 0, truePeak = 0, range = 0, threshold = 0, offset = 0;
        bool finite = false;
    };
    struct Settings {
        std::string mode = "mix", noise = "off";
        int strength = defaults::strength, presence = defaults::presence, priority = defaults::priority;
        bool normalize = true;
        double target = - 16, truePeak = defaults::true_peak_dbtp;
        // Whole-selection stems share timeline time zero. Leading silence is significant.
        fs::path voice, music, outputDir, rnnoiseLibrary, levelSegments;
        std::string ffmpeg = "ffmpeg", ffprobe = "ffprobe";
        // Host revision token is opaque; the host MUST check it immediately before applying.
        std::string snapshotToken;
        int timeoutSeconds = 7200;
        bool keepStems = false;
    };
    struct Result {
        fs::path directory, audio, report;
        Meter before, after;
        bool targetMet = false, silent = false;
        std::vector < std::string > warnings;
    };
    struct PauseResult {
        fs::path directory, report;
        std::vector < Interval > cuts;
        std::vector < std::string > warnings;
    };
    struct ProcessResult {
        int exitCode = 0;
        std::string output;
    };
    using Progress = std::function < void(int, const std::string&) > ;
    std::string jsonString(const std::string&);
    std::string number(double);
    std::string sha256File(const fs::path&, const std::atomic_bool*cancel = nullptr);
    ProcessResult runProcess(const std::vector < std::string > &argv, const fs::path&workingDir, int timeoutSeconds,
                             const std::atomic_bool*cancel = nullptr, std::function<void(std::string_view)> onOutput = {});
    Meter parseLoudnorm(const std::string&);
    Meter parseEbur128(const std::string&);
    std::vector < Interval > speechFromSilence(const std::string&, double duration);
    std::vector < Interval > mergeIntervals(std::vector < Interval > , double duration, double gap);
    std::vector < Interval > pauseCuts(const std::vector < Interval > &speech, double duration,
                                      double minimumPause = defaults::merge_gap_s,
                                      double keepAfter = defaults::pause_after_s,
                                      double keepBefore = defaults::pause_before_s);
    double duckGainDb(double t, const std::vector < Interval > &speech, double depth);
    std::vector < Point > envelopePoints(const std::vector < Interval > &, double duration, double depth);
    std::string voiceFilter(const Settings&, double trimDb);
    std::string loudnormFilter(const Settings&, const Meter*measured = nullptr);
    void validate(const Settings&);
    PauseResult analyzePauses(const Settings&, std::atomic_bool&cancel, Progress progress = {});
    Result process(const Settings&, std::atomic_bool&cancel, Progress progress = {
    });
}
