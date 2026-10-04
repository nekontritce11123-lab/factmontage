// SPDX-License-Identifier: MIT
#include "audio.hpp"
#include <atomic>
#include <csignal>
#include <iostream>
#include <map>
#include <set>
namespace {
    std::atomic_bool cancelled {
        false
    };
    static_assert(std::atomic_bool::is_always_lock_free);
    void onSignal(int) {
        cancelled.store(true);
    }
}
int main(int argc, char**argv) {
    using namespace studio_audio;
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    try {
        Settings s;
        std::map < std::string, std::string > a;
        std::set < std::string > valid {
            "mode", "voice", "music", "out-dir", "strength", "presence", "priority", "noise", "normalize", "target", "true-peak", "rnnoise-library", "ffmpeg", "ffprobe", "timeout", "snapshot-token", "keep-stems", "level-segments"
        };
        for (int i = 1; i < argc; ++ i) {
            std::string k = argv[i];
            if (k == "--help") {
                std::cout<< "StudioAudio 0.2 — offline audio worker for a Qt/MLT host.\nRequired: --mode voice|music|mix|pauses --out-dir NEW_DIRECTORY\nInputs: --voice FILE --music FILE (time-aligned selection stems)\nOptions: --strength 0..100 --noise off|fft|rnnoise --presence 0..100\n --priority 0..100 --normalize on|off --target -16 --true-peak -1.5\n --rnnoise-library PATH --snapshot-token TOKEN --timeout SECONDS\n --keep-stems on|off --level-segments FILE --ffmpeg PATH --ffprobe PATH\n";
                return 0;
            }
            if (k.rfind("--", 0) != 0 || !valid.count(k.substr(2)) || i+ 1 >= argc) throw Error("Неизвестный или неполный аргумент: "+ k);
            if (a.count(k.substr(2))) throw Error("Повторный аргумент: "+ k);
            a[k.substr(2)] = argv[++ i];
        }
        auto text = [&](const char*k, std::string&out) {
            if (a.count(k)) out = a[k];
        };
        auto path = [&](const char*k, fs::path&out) {
            if (a.count(k)) out = fs::u8path(a[k]);
        };
        auto integer = [&](const char*k, int&out) {
            if (a.count(k)) {
                size_t pos = 0;
                out = std::stoi(a[k], &pos);
                if (pos != a[k].size()) throw Error("Неверное целое: "+ a[k]);
            }
        };
        auto real = [&](const char*k, double&out) {
            if (a.count(k)) {
                size_t pos = 0;
                out = std::stod(a[k], &pos);
                if (pos != a[k].size()) throw Error("Неверное число: "+ a[k]);
            }
        };
        auto boolean = [&](const char*k, bool&out) {
            if (a.count(k)) {
                if (a[k] != "on" && a[k] != "off") throw Error("Ожидалось on/off: "+ a[k]);
                out = a[k] == "on";
            }
        };
        text("mode", s.mode);
        text("noise", s.noise);
        text("ffmpeg", s.ffmpeg);
        text("ffprobe", s.ffprobe);
        text("snapshot-token", s.snapshotToken);
        path("voice", s.voice);
        path("music", s.music);
        path("out-dir", s.outputDir);
        path("rnnoise-library", s.rnnoiseLibrary);
        path("level-segments", s.levelSegments);
        integer("strength", s.strength);
        integer("presence", s.presence);
        integer("priority", s.priority);
        integer("timeout", s.timeoutSeconds);
        real("target", s.target);
        real("true-peak", s.truePeak);
        boolean("normalize", s.normalize);
        boolean("keep-stems", s.keepStems);
        auto progress = [](int p, const std::string&t) {
            std::cerr<< "PROGRESS\t"<< p<< '\t'<< t<< '\n';
        };
        if (s.mode == "pauses") {
            auto result = analyzePauses(s, cancelled, progress);
            std::cout<< "{\"report\":"<< jsonString(result.report.string())<< ",\"cut_count\":"<< result.cuts.size()<< "}\n";
            return 0;
        }
        auto result = process(s, cancelled, progress);
        std::cout<< "{\"audio\":"<< jsonString(result.audio.string())<< ",\"report\":"<< jsonString(result.report.string())<< ",\"target_met\":"<<(result.targetMet? "true": "false")<< "}\n";
        return 0;
    } catch (const Cancelled&e) {
        std::cerr<< "CANCELLED\t"<< e.what()<< '\n';
        return 130;
    } catch (const std::exception&e) {
        std::cerr<< "ERROR\t"<< e.what()<< '\n';
        return 1;
    }
}
