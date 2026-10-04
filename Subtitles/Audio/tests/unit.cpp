// SPDX-License-Identifier: MIT
#include "audio.hpp"
#include <cmath>
#include <fstream>
#include <iostream>
#include <unistd.h>
#include <limits>
using namespace studio_audio;
int checks = 0;
void check(bool b, const char*t) {
    ++ checks;
    if (!b) throw Error(std::string("FAILED: ")+ t);
}
template < class Fn > void throws(Fn fn, const char*t) {
    bool threw = false;
    try {
        fn();
    } catch (const std::exception&) {
        threw = true;
    }
    check(threw, t);
}
int main() {
    try {
        auto s = speechFromSilence("silence_start: 0\nsilence_end: 1\nsilence_start: 2\nsilence_end: 4\nsilence_start: 5\n", 6);
        check(s.size() == 2, "two speech intervals");
        check(s[0].start == 1 && s[0].end == 2, "leading silence preserved");
        check(s[1].start == 4 && s[1].end == 5, "trailing silence preserved");
        check(speechFromSilence("silence_start: 0\nsilence_end: 5", 5).empty(), "all silence");
        check(speechFromSilence("", 4).size() == 1, "continuous sound");
        auto cuts = pauseCuts({{0, 1}, {1.8, 2.4}, {3.0, 4.0}}, 4);
        check(cuts.size() == 1, "only pauses longer than 650 ms shortened");
        check(std::abs(cuts[0].start - 1.18) < 1e-12 && std::abs(cuts[0].end - 1.68) < 1e-12, "pause keeps 180 ms after and 120 ms before speech");
        check(pauseCuts({{1, 2}}, 4).empty(), "edge silence is preserved");
        auto merged = mergeIntervals( {
            {
                4, 5
            }, {
                1, 2
            }, {
                2.2, 3
            }
        }, 6, .65);
        check(merged.size() == 2 && merged[0].end == 3, "short pauses merge");
        throws([] {
            mergeIntervals( {
                {
                    2, 1
                }
            }, 4, 0);
        }, "invalid intervals");
        throws([] {
            mergeIntervals( {
                {
                    0, std::numeric_limits < double > ::quiet_NaN()
                }
            }, 4, 0);
        }, "NaN interval");
        std::vector < Interval > speech {
            {
                1, 2
            }, {
                4, 4.5
            }
        };
        check(duckGainDb(0, speech, 9) == 0, "no duck before lead");
        check(duckGainDb(1, speech, 9) == - 9, "full duck before first word");
        check(duckGainDb(2.2, speech, 9) == - 9, "hold short pause");
        check(std::abs(duckGainDb(3.3, speech, 9)) < 1e-12, "release complete");
        check(duckGainDb(2.6, speech, 0) == 0, "zero priority bypass");
        for (int i = 0; i <= 6000; ++ i) {
            double d = duckGainDb(i*.001, speech, 12);
            check(d <= 0 && d >= - 12, "gain bounded");
        }
        check(std::abs(duckGainDb(1- 1e-7, speech, 9)- duckGainDb(1+ 1e-7, speech, 9)) < 1e-8, "onset continuous");
        auto pts = envelopePoints(speech, 6, 9);
        check(pts.front().time == 0 && pts.back().time == 6, "envelope endpoints");
        check(pts.size() < 150, "compact envelope");
        auto m = parseLoudnorm(R"({"input_i":"-21.0","input_tp":"-4.2","input_lra":"3","input_thresh":"-31.2","target_offset":"0.2"})");
        check(m.finite && m.integrated == - 21, "measure JSON");
        auto silence = parseLoudnorm(R"({"input_i":"-inf","input_tp":"-inf","input_lra":"0","input_thresh":"-70","target_offset":"inf"})");
        check(!silence.finite, "silent loudnorm");
        throws([] {
            parseLoudnorm("not json");
        }, "missing analysis rejected");
        auto eb = parseEbur128("Summary:\n Integrated loudness:\n I: -16.0 LUFS\n Threshold: -26.3 LUFS\n Loudness range:\n LRA: 2.1 LU\n Threshold: -37 LUFS\n True peak:\n Peak: -1.6 dBFS\n");
        check(eb.finite && eb.integrated == - 16 && eb.truePeak == - 1.6 && eb.threshold == - 26.3, "final EBU summary");
        check(!parseEbur128("Summary: I: -70 LUFS Threshold: 0 LUFS LRA: 0 LU Peak: -inf dBFS").finite, "silent EBU summary");
        throws([] {
            parseEbur128("no summary");
        }, "incomplete EBU summary rejected");
        Settings a;
        check(loudnormFilter(a, &m).find("measured_I=-21") != std::string::npos, "two pass uses analysis");
        check(loudnormFilter(a, &m).find("dual_mono=false") != std::string::npos, "stereo not double counted");
        throws([&] {
            loudnormFilter(a, &silence);
        }, "silent normalization rejected");
        a.strength = 0;
        check(voiceFilter(a, 0).find("compressor") == std::string::npos, "strength zero bypass");
        a.noise = "fft";
        check(voiceFilter(a, 0).find("afftdn") != std::string::npos, "noise independent");
        check(jsonString("a\"b\n\\") == "\"a\\\"b\\n\\\\\"", "JSON escape");
        check(number(std::numeric_limits < double > ::infinity()) == "null", "finite JSON only");
        auto temp = fs::temp_directory_path()/("studio-audio-unit-"+ std::to_string(getpid()));
        fs::create_directories(temp);
        {
            std::ofstream f(temp/ "sha");
            f<< "abc";
        }
        check(sha256File(temp/ "sha") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256 vector abc");
        {
            std::ofstream f(temp/ "sha");
        }
        check(sha256File(temp/ "sha") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256 empty");
        std::atomic_bool cancel {
            false
        };
        auto p = runProcess( {
            "/bin/echo", "one; two $HOME 'quoted'"
        }, temp, 5, &cancel);
        check(p.exitCode == 0 && p.output == "one; two $HOME 'quoted'\n", "no shell interpretation");
        check(runProcess( {
            "/bin/false"
        }, temp, 5).exitCode != 0, "exit error captured");
        throws([&] {
            runProcess( {
                "/bin/sleep", "3"
            }, temp, 1);
        }, "timeout kills child");
        cancel = true;
        throws([&] {
            runProcess( {
                "/bin/true"
            }, temp, 5, &cancel);
        }, "cancellation");
        fs::remove_all(temp);
        std::cout<< checks<< " assertions passed\n";
        return 0;
    } catch (const std::exception&e) {
        std::cerr<< e.what()<< '\n';
        return 1;
    }
}
