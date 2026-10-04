// SPDX-License-Identifier: MIT
#include "audio.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <regex>
#include <sstream>
#include <unistd.h>
namespace studio_audio {
    std::string number(double v) {
        if (!std::isfinite(v)) return "null";
        std::ostringstream s;
        s.imbue(std::locale::classic());
        s<< std::setprecision(12)<< v;
        return s.str();
    }
    std::string jsonString(const std::string&v) {
        std::ostringstream s;
        s<< '"';
        for (unsigned char c: v) {
            switch (c) {
                case '"': s<< "\\\"";
                break;
                case '\\': s<< "\\\\";
                break;
                case '\n': s<< "\\n";
                break;
                case '\r': s<< "\\r";
                break;
                case '\t': s<< "\\t";
                break;
                default: if (c < 32) s<< "\\u00"<< std::hex<< std::setw(2)<< std::setfill('0')<< int(c)<< std::dec;
                else s<< c;
            }
        }
        s<< '"';
        return s.str();
    }
    namespace {
        double parseNumber(const std::string&s) {
            if (s == "-inf") return- std::numeric_limits < double > ::infinity();
            if (s == "inf" || s == "+inf") return std::numeric_limits < double > ::infinity();
            if (s == "nan") return std::numeric_limits < double > ::quiet_NaN();
            std::istringstream f(s);
            f.imbue(std::locale::classic());
            double a = 0;
            if (!(f>> a) || f.peek() != EOF) throw Error("Неверное число в ответе FFmpeg: "+ s);
            return a;
        }
        double jsonNumber(const std::string&text, const std::string&key) {
            std::regex r("\""+ key+ "\"\\s*:\\s*\"([^\"]+)\"");
            std::sregex_iterator it(text.begin(), text.end(), r), end;
            std::string found;
            for (; it != end; ++ it) found = (*it)[1];
            if (found.empty()) throw Error("FFmpeg не вернул измерение "+ key);
            return parseNumber(found);
        }
        double smooth(double x) {
            x = std::clamp(x, 0.0, 1.0);
            return x*x*x*(x*(x*6- 15)+ 10);
        }
        std::string meterJson(const Meter&m) {
            return "{\"integrated_lufs\":"+ number(m.integrated)+ ",\"true_peak_dbtp\":"+ number(m.truePeak)+ ",\"range_lu\":"+ number(m.range)+ ",\"threshold_lufs\":"+ number(m.threshold)+ ",\"finite\":"+(m.finite? "true": "false")+ "}";
        }
        void writeText(const fs::path&path, const std::string&content) {
            std::ofstream f(path, std::ios::binary);
            f<< content;
            f.close();
            if (!f) throw Error("Не удалось записать: "+ path.string());
        }
        struct TempDir {
            fs::path path;
            bool published = false;
            ~TempDir() {
                if (!published) {
                    std::error_code e;
                    fs::remove_all(path, e);
                }
            }
        };
        struct Source {
            fs::path path;
            std::string hash;
            double duration = 0;
            int channels = 0;
        };
        std::vector < std::string > base(const Settings&s) {
            return {
                s.ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "info", "-threads", "2", "-filter_threads", "1", "-filter_complex_threads", "1", "-n"
            };
        }
        void outputArgs(std::vector < std::string > &a, const fs::path&p, bool final = false) {
            a.insert(a.end(), {
                "-vn", "-sn", "-dn", "-map_metadata", "-1", "-ar", "48000", "-ac", "2", "-c:a", final? "pcm_s24le": "pcm_f32le", "-threads", "2", "-rf64", "auto", p.string()
            });
        }
        std::string checked(const std::vector < std::string > &cmd, const fs::path&work, const Settings&s, const std::atomic_bool&cancel) {
            auto r = runProcess(cmd, work, s.timeoutSeconds, &cancel);
            if (r.exitCode != 0) throw Error("Обработчик завершился с кодом "+ std::to_string(r.exitCode)+ ":\n"+ r.output.substr(r.output.size() > 6000? r.output.size()- 6000: 0));
            return r.output;
        }
        Source inspectSource(const fs::path&path, const Settings&s, const fs::path&work, const std::atomic_bool&cancel) {
            Source result;
            result.path = fs::canonical(path);
            result.hash = sha256File(result.path, &cancel);
            auto text = checked( {
                s.ffprobe, "-v", "error", "-select_streams", "a:0", "-show_entries", "stream=channels:format=duration", "-of", "default=noprint_wrappers=1", result.path.string()
            }, work, s, cancel);
            std::istringstream f(text);
            std::string line;
            while (std::getline(f, line)) {
                if (line.rfind("channels=", 0) == 0) result.channels = std::stoi(line.substr(9));
                if (line.rfind("duration=", 0) == 0 && line.substr(9) != "N/A") result.duration = parseNumber(line.substr(9));
            }
            if (result.channels < 1 || result.channels > 2) throw Error("Нужна моно- или стереодорожка; автоматическое сведение многоканального звука отключено: "+ path.string());
            if (!std::isfinite(result.duration) || result.duration < .4 || result.duration > 7200) {
                throw Error("Нужен конечный аудиофрагмент от 0,4 секунды до 2 часов: "+ path.string());
            }
            return result;
        }
        Meter measure(const fs::path&p, const Settings&s, const fs::path&work, const std::atomic_bool&cancel) {
            auto a = base(s);
            a.insert(a.end(), {
                "-i", p.string(), "-map", "0:a:0", "-af", loudnormFilter(s), "-f", "null", "-"
            });
            return parseLoudnorm(checked(a, work, s, cancel));
        }
        // A separate meter checks the rendered file, not loudnorm's self-reported output.
        Meter verifyEbur128(const fs::path&p, const Settings&s, const fs::path&work, const std::atomic_bool&cancel) {
            auto a = base(s);
            a.insert(a.end(), {
                "-i", p.string(), "-map", "0:a:0", "-af", "ebur128=peak=true", "-f", "null", "-"
            });
            return parseEbur128(checked(a, work, s, cancel));
        }
        void renderFilter(const fs::path&input, const fs::path&output, const std::string&filter, const Settings&s, const fs::path&work, const std::atomic_bool&cancel, bool final = false) {
            auto a = base(s);
            a.insert(a.end(), {
                "-i", input.string(), "-map", "0:a:0", "-af", filter
            });
            outputArgs(a, output, final);
            checked(a, work, s, cancel);
        }
        uint32_t le32(const unsigned char*p) {
            return uint32_t(p[0])|(uint32_t(p[1])<< 8)|(uint32_t(p[2])<< 16)|(uint32_t(p[3])<< 24);
        }
        uint64_t le64(const unsigned char*p) {
            return uint64_t(le32(p))|(uint64_t(le32(p+ 4))<< 32);
        }
        struct WaveInfo {
            uint64_t samples = 0;
            uint64_t dataOffset = 0;
            double peak = 0, threshold = - 42;
        };
        WaveInfo inspectFloatWave(const fs::path&path, const std::atomic_bool&cancel) {
            std::ifstream f(path, std::ios::binary);
            std::array < unsigned char, 12 > header {
            };
            f.read(reinterpret_cast < char* > (header.data()), 12);
            if (!f || std::memcmp(header.data()+ 8, "WAVE", 4) != 0) throw Error("Ожидался внутренний WAV.");
            uint64_t rf64Data = 0, dataSize = 0;
            bool fmtOk = false;
            for (; ; ) {
                std::array < unsigned char, 8 > h {
                };
                f.read(reinterpret_cast < char* > (h.data()), 8);
                if (!f) throw Error("Неполный WAV.");
                uint64_t n = le32(h.data()+ 4);
                std::string tag(reinterpret_cast < char* > (h.data()), 4);
                if (tag == "data") {
                    dataSize = n == 0xffffffff? rf64Data: n;
                    break;
                }
                if (tag == "ds64") {
                    std::array < unsigned char, 28 > d {
                    };
                    if (n < 28) throw Error("Неверный RF64.");
                    f.read(reinterpret_cast < char* > (d.data()), 28);
                    rf64Data = le64(d.data()+ 8);
                    f.seekg(std::streamoff(n- 28+(n&1)), std::ios::cur);
                } else if (tag == "fmt ") {
                    if (n < 16 || n > 4096) throw Error("Неверный WAV fmt.");
                    std::vector < unsigned char > d(size_t(n), 0);
                    f.read(reinterpret_cast < char* > (d.data()), std::streamsize(n));
                    int format = d[0]|(d[1]<< 8);
                    if (format == 0xfffe && n >= 40) format = d[24]|(d[25]<< 8);
                    fmtOk = format == 3 && d[2] == 2 && d[3] == 0 && le32(d.data()+ 4) == 48000 && d[14] == 32 && d[15] == 0;
                    if (n&1) f.seekg(1, std::ios::cur);
                } else f.seekg(std::streamoff(n+(n&1)), std::ios::cur);
            }
            if (!fmtOk || dataSize == 0 || dataSize% 8) throw Error("Внутренний WAV должен быть stereo float32 / 48 kHz.");
            WaveInfo info;
            info.samples = dataSize/ 8;
            info.dataOffset = uint64_t(f.tellg());
            std::array < float, 1920 > buf {
            };
            std::vector < double > db;
            db.reserve(size_t(info.samples/ 960+ 1));
            uint64_t remain = info.samples;
            while (remain) {
                if (cancel.load()) throw Cancelled();
                size_t count = size_t(std::min < uint64_t > (960, remain));
                f.read(reinterpret_cast < char* > (buf.data()), std::streamsize(count*8));
                if (!f) throw Error("Оборванный WAV.");
                double l = 0, r = 0;
                for (size_t i = 0; i < count; ++ i) {
                    float x = buf[i*2], y = buf[i*2+ 1];
                    if (!std::isfinite(x) || !std::isfinite(y)) throw Error("Не-числовые аудиосэмплы.");
                    l += double(x)*x;
                    r += double(y)*y;
                    info.peak = std::max(info.peak, std::max(std::abs(double(x)), std::abs(double(y))));
                }
                db.push_back(10*std::log10(std::max(std::max(l, r)/ double(count), 1e-20)));
                remain -= count;
            }
            // Max-channel energy preserves anti-phase stereo; this is NOT a neural speech recognizer.
            std::sort(db.begin(), db.end());
            double floor = db[size_t(double(db.size()- 1)*.12)], speech = db[size_t(double(db.size()- 1)*.88)];
            info.threshold = std::clamp(std::min(floor+ 10, speech- 18), - 58.0, - 28.0);
            return info;
        }
        struct LevelSegment {
            uint64_t start = 0, end = 0;
            Meter before, after;
            double gainDb = 0;
        };
        std::vector<LevelSegment> readLevelSegments(const fs::path&path, uint64_t samples) {
            std::ifstream input(path);
            if (!input) throw Error("Не удалось открыть границы аудиоклипов.");
            input.imbue(std::locale::classic());
            std::vector<LevelSegment> result;
            std::string line;
            while (std::getline(input, line)) {
                std::istringstream values(line);
                values.imbue(std::locale::classic());
                double start = 0, end = 0;
                if (!(values >> start >> end) || !(values >> std::ws).eof() || !std::isfinite(start) || !std::isfinite(end)
                    || start < 0 || end <= start || end > double(samples) / 48000 + .05 || result.size() >= 10000)
                    throw Error("Некорректные границы аудиоклипов.");
                LevelSegment segment;
                segment.start = uint64_t(std::llround(start * 48000));
                segment.end = std::min(samples, uint64_t(std::llround(end * 48000)));
                if (segment.end <= segment.start || (!result.empty() && segment.start < result.back().end))
                    throw Error("Аудиоклипы пересекаются или имеют пустой диапазон.");
                result.push_back(segment);
            }
            if (!input.eof() || result.empty()) throw Error("Границы аудиоклипов отсутствуют или повреждены.");
            return result;
        }
        Meter segmentMeter(const fs::path&path, const LevelSegment&segment, bool final,
                           const Settings&s, const fs::path&work, const std::atomic_bool&cancel) {
            auto args = base(s);
            const std::string trim = "atrim=start_sample=" + std::to_string(segment.start) + ":end_sample="
                + std::to_string(segment.end) + ",asetpts=PTS-STARTPTS,";
            args.insert(args.end(), {"-i", path.string(), "-map", "0:a:0", "-af",
                                     trim + (final ? "ebur128=peak=true" : loudnormFilter(s)), "-f", "null", "-"});
            const auto output = checked(args, work, s, cancel);
            return final ? parseEbur128(output) : parseLoudnorm(output);
        }
        void applySegmentGains(const fs::path&path, const WaveInfo&info,
                               const std::vector<LevelSegment>&segments, const std::atomic_bool&cancel) {
            std::fstream output(path, std::ios::binary | std::ios::in | std::ios::out);
            if (!output) throw Error("Не удалось открыть рабочий WAV для выравнивания.");
            std::array<float, 16384> buffer{};
            for (const auto&segment : segments) {
                if (segment.gainDb <= 0) continue;
                const double gain = std::pow(10, segment.gainDb / 20);
                for (uint64_t at = segment.start; at < segment.end;) {
                    if (cancel.load()) throw Cancelled();
                    const auto frames = size_t(std::min<uint64_t>(buffer.size() / 2, segment.end - at));
                    const auto offset = std::streamoff(info.dataOffset + at * 8);
                    output.seekg(offset);
                    output.read(reinterpret_cast<char*>(buffer.data()), std::streamsize(frames * 8));
                    if (!output) throw Error("Не удалось прочитать фрагмент рабочего WAV.");
                    for (size_t i = 0; i < frames * 2; ++i) buffer[i] = float(double(buffer[i]) * gain);
                    output.seekp(offset);
                    output.write(reinterpret_cast<const char*>(buffer.data()), std::streamsize(frames * 8));
                    if (!output) throw Error("Не удалось записать выровненный WAV.");
                    at += frames;
                }
            }
        }
        void writeControl(const fs::path&path, uint64_t samples, const std::vector < Interval > &speech, double depth, const std::atomic_bool&cancel) {
            std::ofstream f(path, std::ios::binary);
            std::array < float, 4800 > buffer {
            };
            uint64_t done = 0;
            size_t first = 0;
            double lastDb = std::numeric_limits < double > ::quiet_NaN();
            float lastGain = 1;
            while (done < samples) {
                if (cancel.load()) throw Cancelled();
                size_t n = size_t(std::min < uint64_t > (buffer.size(), samples- done));
                for (size_t i = 0; i < n; ++ i) {
                    double t = double(done+ i)/ 48000;
                    while (first < speech.size() && speech[first].end+ defaults::hold_s+ defaults::release_s < t)++ first;
                    double db = 0;
                    for (size_t j = first; j < speech.size() && speech[j].start- defaults::lookahead_s <= t; ++ j) {
                        auto v = speech[j];
                        double value = 0;
                        if (t < v.start) value = - depth*smooth((t-(v.start- defaults::lookahead_s))/ defaults::lookahead_s);
                        else if (t <= v.end+ defaults::hold_s) value = - depth;
                        else value = - depth*(1- smooth((t- v.end- defaults::hold_s)/ defaults::release_s));
                        db = std::min(db, value);
                    }
                    if (db != lastDb) {
                        lastGain = float(std::pow(10, db/ 20));
                        lastDb = db;
                    }
                    buffer[i] = lastGain;
                }
                f.write(reinterpret_cast < const char* > (buffer.data()), std::streamsize(n*4));
                if (!f) throw Error("Не удалось записать огибающую; проверьте свободное место.");
                done += n;
            }
        }
    }
    Meter parseLoudnorm(const std::string&text) {
        Meter m;
        m.integrated = jsonNumber(text, "input_i");
        m.truePeak = jsonNumber(text, "input_tp");
        m.range = jsonNumber(text, "input_lra");
        m.threshold = jsonNumber(text, "input_thresh");
        m.offset = jsonNumber(text, "target_offset");
        m.finite = std::isfinite(m.integrated) && std::isfinite(m.truePeak) && std::isfinite(m.range) && std::isfinite(m.threshold) && std::isfinite(m.offset);
        return m;
    }
    Meter parseEbur128(const std::string&text) {
        const auto at = text.rfind("Summary:");
        if (at == std::string::npos) throw Error("FFmpeg не вернул итог ebur128.");
        const auto summary = text.substr(at);
        auto value = [&](const std::string&key) {
            std::smatch match;
            std::regex pattern(key+ R"(\s*:\s*([-+a-zA-Z0-9.]+))");
            if (!std::regex_search(summary, match, pattern)) throw Error("Неполное измерение ebur128: "+ key);
            return parseNumber(match[1]);
        };
        Meter m;
        m.integrated = value("I");
        m.truePeak = value("Peak");
        m.range = value("LRA");
        m.threshold = value("Threshold");
        m.finite = std::isfinite(m.integrated) && std::isfinite(m.truePeak) && std::isfinite(m.range) && std::isfinite(m.threshold);
        return m;
    }
    std::vector < Interval > mergeIntervals(std::vector < Interval > a, double duration, double gap) {
        if (!std::isfinite(duration) || duration <= 0 || !std::isfinite(gap) || gap < 0) throw Error("Неверная длительность или интервал объединения.");
        for (auto&v: a) {
            if (!std::isfinite(v.start) || !std::isfinite(v.end) || v.end < v.start) throw Error("Неверный интервал речи.");
            v.start = std::clamp(v.start, 0.0, duration);
            v.end = std::clamp(v.end, 0.0, duration);
        }
        std::sort(a.begin(), a.end(), [](auto x, auto y) {
            return x.start < y.start;
        });
        std::vector < Interval > out;
        for (auto v: a) {
            if (v.end <= v.start) continue;
            if (!out.empty() && v.start <= out.back().end+ gap) out.back().end = std::max(out.back().end, v.end);
            else out.push_back(v);
        }
        return out;
    }
    std::vector < Interval > speechFromSilence(const std::string&log, double duration) {
        std::regex r("silence_(start|end):\\s*([-+0-9.eE]+)");
        std::vector < Interval > silence;
        double start = - 1;
        for (std::sregex_iterator it(log.begin(), log.end(), r), end; it != end; ++ it) {
            double t = std::clamp(parseNumber((*it)[2]), 0.0, duration);
            if ((*it)[1] == "start") start = t;
            else {
                if (start < 0) start = 0;
                silence.push_back( {
                    start, std::max(t, start)
                });
                start = - 1;
            }
        }
        if (start >= 0) {
            silence.push_back( {
                start, duration
            });
        }
        silence = mergeIntervals(silence, duration, 0);
        std::vector < Interval > speech;
        double t = 0;
        for (auto s: silence) {
            if (s.start > t) speech.push_back( {
                t, s.start
            });
            t = std::max(t, s.end);
        }
        if (t < duration) speech.push_back( {
            t, duration
        });
        return mergeIntervals(speech, duration, defaults::merge_gap_s);
    }
    std::vector < Interval > pauseCuts(const std::vector < Interval > &speech, double duration,
                                      double minimumPause, double keepAfter, double keepBefore) {
        if (!std::isfinite(duration) || duration <= 0 || !std::isfinite(minimumPause) || minimumPause < 0
            || !std::isfinite(keepAfter) || keepAfter < 0 || !std::isfinite(keepBefore) || keepBefore < 0) {
            throw Error("Неверные настройки сокращения пауз.");
        }
        const auto ordered = mergeIntervals(speech, duration, 0);
        std::vector < Interval > cuts;
        for (size_t i = 1; i < ordered.size(); ++i) {
            const double gap = ordered[i].start - ordered[i - 1].end;
            const Interval cut{ordered[i - 1].end + keepAfter, ordered[i].start - keepBefore};
            if (gap > minimumPause && cut.end > cut.start) cuts.push_back(cut);
        }
        return cuts;
    }
    double duckGainDb(double t, const std::vector < Interval > &speech, double depth) {
        double db = 0;
        auto it = std::lower_bound(speech.begin(), speech.end(), t, [](const Interval&v, double at) {
            return v.end+ defaults::hold_s+ defaults::release_s < at;
        });
        for (; it != speech.end() && it->start- defaults::lookahead_s <= t; ++ it) {
            double v;
            if (t < it->start) v = - depth*smooth((t- it->start+ defaults::lookahead_s)/ defaults::lookahead_s);
            else if (t <= it->end+ defaults::hold_s) v = - depth;
            else v = - depth*(1- smooth((t- it->end- defaults::hold_s)/ defaults::release_s));
            db = std::min(db, v);
        }
        return std::clamp(db, - depth, 0.0);
    }
    std::vector < Point > envelopePoints(const std::vector < Interval > &speech, double duration, double depth) {
        std::vector < Point > a;
        for (uint64_t i = 0; double(i)*.02 < duration; ++ i) {
            double t = double(i)*.02;
            a.push_back( {
                t, duckGainDb(t, speech, depth)
            });
        }
        a.push_back( {
            duration, duckGainDb(duration, speech, depth)
        });
        if (a.size() < 3) return a;
        std::vector < bool > keep(a.size(), false);
        keep.front() = keep.back() = true;
        std::vector < std::pair < size_t, size_t>> stack {
            {
                0, a.size()- 1
            }
        };
        while (!stack.empty()) {
            auto[l, r] = stack.back();
            stack.pop_back();
            double err = 0;
            size_t index = l;
            for (size_t i = l+ 1; i < r; ++ i) {
                double y = a[l].gainDb+(a[r].gainDb- a[l].gainDb)*(a[i].time- a[l].time)/(a[r].time- a[l].time);
                double e = std::abs(y- a[i].gainDb);
                if (e > err) {
                    err = e;
                    index = i;
                }
            }
            if (err > .045) {
                keep[index] = true;
                stack.push_back( {
                    l, index
                });
                stack.push_back( {
                    index, r
                });
            }
        }
        std::vector < Point > out;
        for (size_t i = 0; i < a.size(); ++ i) if (keep[i]) out.push_back(a[i]);
        return out;
    }
    std::string voiceFilter(const Settings&s, double trimDb) {
        std::string filter = "aresample=48000,aformat=sample_fmts=flt:channel_layouts=stereo";
        if (trimDb != 0) filter += ",volume="+ number(trimDb)+ "dB";
        if (s.strength > 0) filter += ",highpass=f=70:p=2";
        // afftdn delays 25 ms at the fixed 48 kHz working rate. Flush its
        // tail before removing that delay so clip edges and length survive.
        if (s.noise == "fft") filter += ",apad=pad_len=1200,afftdn=nr=6:nf=-45:tn=1:gs=5,atrim=start_sample=1200,asetpts=PTS-STARTPTS";
        if (s.noise == "rnnoise") filter += ",ladspa=file=./rnnoise.so:plugin=noise_suppressor_stereo:controls=0|200|0|0|0:latency=1";
        if (s.strength > 0) {
            double x = double(s.strength)/ 100;
            filter += ",deesser=i="+ number(.12+.18*x)+ ":m="+ number(.12+.22*x)+ ":f=0.5:s=o";
            filter += ",acompressor=threshold=0.125:ratio="+ number(1.2+ 1.3*x)+ ":attack=15:release=220:knee=2.828427:makeup=1:link=maximum:detection=rms:mix="+ number(.3+.5*x);
        }
        return filter;
    }
    std::string loudnormFilter(const Settings&s, const Meter*m) {
        // Keep the requested LRA identical in both passes; never remaster music dynamics intentionally.
        double lra = 50;
        std::string f = "loudnorm=I="+ number(s.target)+ ":TP="+ number(s.truePeak)+ ":LRA="+ number(lra)+ ":dual_mono=false:print_format=json";
        if (m) {
            if (!m->finite) throw Error("Нельзя нормализовать неизмеримый сигнал.");
            f += ":measured_I="+ number(m->integrated)+ ":measured_TP="+ number(m->truePeak)+ ":measured_LRA="+ number(m->range)+ ":measured_thresh="+ number(m->threshold)+ ":offset="+ number(m->offset)+ ":linear=true";
        }
        return f;
    }
    void validate(const Settings&s) {
        if (s.mode != "voice" && s.mode != "music" && s.mode != "mix" && s.mode != "pauses") throw Error("Режим: voice, music, mix или pauses.");
        if (s.noise != "off" && s.noise != "fft" && s.noise != "rnnoise") throw Error("Неизвестное шумоподавление.");
        if (s.strength < 0 || s.strength > 100 || s.presence < 0 || s.presence > 100 || s.priority < 0 || s.priority > 100) throw Error("Сила настроек должна быть от 0 до 100.");
        if (!std::isfinite(s.target) || s.target < - 23 || s.target > - 14 || !std::isfinite(s.truePeak) || s.truePeak < - 9 || s.truePeak > -.5) throw Error("Цель: −23…−14 LUFS; потолок: −9…−0,5 dBTP.");
        if (s.timeoutSeconds < 1 || s.timeoutSeconds > 86400) throw Error("Недопустимый тайм-аут.");
        if (s.outputDir.empty() || fs::exists(s.outputDir)) throw Error("Каталог результата должен быть новым; существующие данные не перезаписываются.");
        const bool needsVoice = s.mode == "voice" || s.mode == "mix" || s.mode == "pauses";
        const bool needsMusic = s.mode == "music" || s.mode == "mix";
        for (auto p: {needsVoice ? s.voice : fs::path{}, needsMusic ? s.music : fs::path{}}) {
            if (!p.empty() && !fs::is_regular_file(p)) throw Error("Источник не является доступным файлом: "+ p.string());
        }
        if ((needsVoice && s.voice.empty()) || (needsMusic && s.music.empty())) throw Error("Не выбраны необходимые голос / музыка.");
        if (s.mode == "mix" && fs::equivalent(s.voice, s.music)) throw Error("Голос и музыка должны быть отдельными источниками, не одним смешанным файлом.");
        if (!s.levelSegments.empty() && (!s.normalize || (s.mode != "voice" && s.mode != "music") || !fs::is_regular_file(s.levelSegments)))
            throw Error("Выравнивание клипов требует режим голоса или громкости, нормализацию и файл границ.");
        if (needsVoice && s.mode != "pauses" && s.noise == "rnnoise" && (s.rnnoiseLibrary.empty() || !fs::is_regular_file(s.rnnoiseLibrary))) throw Error("RNNoise выбран явно, но LADSPA-библиотека недоступна. Скрытая замена другим фильтром запрещена.");
    }
    PauseResult analyzePauses(const Settings&input, std::atomic_bool&cancel, Progress cb) {
        Settings s = input;
        if (s.mode != "pauses") throw Error("Анализ пауз требует режим pauses.");
        validate(s);
        s.outputDir = fs::absolute(s.outputDir);
        fs::create_directories(s.outputDir.parent_path());
        std::string pattern = (s.outputDir.parent_path()/ ".studio-audio-XXXXXX").string();
        std::vector < char > path(pattern.begin(), pattern.end());
        path.push_back(0);
        char *dir = mkdtemp(path.data());
        if (!dir) throw Error("Не удалось создать рабочий каталог.");
        TempDir temp{fs::path(dir), false};
        const auto work = temp.path;
        auto progress = [&](int p, const std::string &text) {
            if (cancel.load()) throw Cancelled();
            if (cb) cb(p, text);
        };
        progress(5, "Проверка голосового фрагмента");
        const auto ffVersion = checked({s.ffmpeg, "-version"}, work, s, cancel);
        const auto filters = checked({s.ffmpeg, "-hide_banner", "-filters"}, work, s, cancel);
        for (auto name: {"silencedetect", "aresample", "aformat"}) {
            if (filters.find(name) == std::string::npos) throw Error(std::string("В этой сборке FFmpeg нет фильтра ") + name);
        }
        const auto source = inspectSource(s.voice, s, work, cancel);
        const auto raw = work/ "voice.raw.wav";
        progress(25, "Подготовка голоса для поиска пауз");
        renderFilter(source.path, raw, "aresample=48000,aformat=sample_fmts=flt:channel_layouts=stereo", s, work, cancel);
        const auto info = inspectFloatWave(raw, cancel);
        progress(55, "Поиск длинных пауз");
        auto args = base(s);
        args.insert(args.end(), {"-i", raw.string(), "-af", "silencedetect=noise=" + number(info.threshold) + "dB:d=" + number(defaults::merge_gap_s), "-f", "null", "-"});
        const auto log = checked(args, work, s, cancel);
        auto speech = speechFromSilence(log, double(info.samples) / defaults::sample_rate);
        if (info.peak < 1e-7) speech.clear();
        PauseResult result;
        result.directory = s.outputDir;
        result.report = s.outputDir/ "report.json";
        result.cuts = pauseCuts(speech, source.duration);
        if (speech.empty()) result.warnings.push_back("Речь не найдена; монтаж не изменён.");
        std::ostringstream report;
        report.imbue(std::locale::classic());
        report << "{\n\"schema\":" << defaults::result_schema << ",\"module\":\"StudioAudio\",\"version\":" << jsonString(defaults::result_version) << ",\n"
               << "\"snapshot_token\":" << jsonString(s.snapshotToken) << ",\"mode\":\"pauses\","
               << "\"duration_seconds\":" << number(source.duration) << ",\n"
               << "\"settings\":{\"minimum_pause_s\":" << number(defaults::merge_gap_s)
               << ",\"keep_after_s\":" << number(defaults::pause_after_s)
               << ",\"keep_before_s\":" << number(defaults::pause_before_s) << "},\n"
               << "\"source\":{\"path\":" << jsonString(source.path.string()) << ",\"sha256\":" << jsonString(source.hash) << "},\n"
               << "\"pause_cuts\":[";
        for (size_t i = 0; i < result.cuts.size(); ++i) {
            if (i) report << ',';
            report << '[' << number(result.cuts[i].start) << ',' << number(result.cuts[i].end) << ']';
        }
        report << "],\n\"warnings\":[";
        for (size_t i = 0; i < result.warnings.size(); ++i) {
            if (i) report << ',';
            report << jsonString(result.warnings[i]);
        }
        report << "],\n\"ffmpeg_version\":" << jsonString(ffVersion.substr(0, ffVersion.find('\n')))
               << ",\"host_integration_tested\":false,\"steam_deck_tested\":false\n}\n";
        writeText(work/ "report.json", report.str());
        progress(90, "Проверка неизменности источника");
        if (sha256File(source.path, &cancel) != source.hash) throw Error("Исходник изменился во время анализа; результат отклонён.");
        fs::remove(raw);
        if (fs::exists(s.outputDir)) throw Error("Каталог назначения появился во время работы; перезапись запрещена.");
        fs::rename(work, s.outputDir);
        temp.published = true;
        if (cb) cb(100, "Паузы найдены; проект пока не изменён");
        return result;
    }
    Result process(const Settings&input, std::atomic_bool&cancel, Progress cb) {
        Settings s = input;
        if (s.mode == "pauses") throw Error("Для пауз используйте analyzePauses().");
        validate(s);
        s.outputDir = fs::absolute(s.outputDir);
        auto parent = s.outputDir.parent_path();
        fs::create_directories(parent);
        std::string pattern = (parent/ ".studio-audio-XXXXXX").string();
        std::vector < char > path(pattern.begin(), pattern.end());
        path.push_back(0);
        char*dir = mkdtemp(path.data());
        if (!dir) throw Error("Не удалось создать рабочий каталог.");
        TempDir temp {
            fs::path(dir), false
        };
        const auto work = temp.path;
        Result result;
        result.directory = s.outputDir;
        result.audio = s.outputDir/ "result.wav";
        result.report = s.outputDir/ "report.json";
        auto progress = [&](int p, const std::string&t) {
            if (cancel.load()) throw Cancelled();
            if (cb) cb(p, t);
        };
        progress(2, "Проверка источников и доступных фильтров");
        const auto ffVersion = checked( {
            s.ffmpeg, "-version"
        }, work, s, cancel);
        const auto filters = checked( {
            s.ffmpeg, "-hide_banner", "-filters"
        }, work, s, cancel);
        for (auto name: {
            "loudnorm", "ebur128", "aresample", "aformat"
        }) if (filters.find(name) == std::string::npos) throw Error(std::string("В этой сборке FFmpeg нет фильтра ")+ name);
        if (!s.levelSegments.empty() && filters.find("alimiter") == std::string::npos)
            throw Error("В этой сборке FFmpeg нет фильтра alimiter");
        if (s.mode != "music" && s.noise == "rnnoise") {
            fs::create_symlink(fs::canonical(s.rnnoiseLibrary), work/ "rnnoise.so");
            result.warnings.push_back("RNNoise: качество, задержка и скорость на Deck ещё требуют проверки; VAD-затвор отключён.");
        }
        std::vector < Source > sources;
        Source v, m;
        if (s.mode != "music") {
            v = inspectSource(s.voice, s, work, cancel);
            sources.push_back(v);
        }
        if (s.mode != "voice") {
            m = inspectSource(s.music, s, work, cancel);
            sources.push_back(m);
        }
        const double maxDuration = std::max(v.duration, m.duration);
        auto space = fs::space(work);
        auto reserve = uint64_t(maxDuration*48000*2*4*5)+ 128*1024*1024;
        if (space.available < reserve) throw Error("Недостаточно свободного места для безопасной обработки; временный бюджет около "+ std::to_string(reserve/(1024*1024))+ " МиБ.");
        fs::path prepared;
        WaveInfo vi, mi;
        Meter voiceRaw, voicePrepared, musicRaw;
        std::vector < Interval > speech;
        std::vector < Point > points;
        uint64_t totalSamples = 0;
        double depth = 0;
        if (s.mode != "music") {
            progress(8, "Подготовка голосовой дорожки");
            auto raw = work/ "voice.raw.wav";
            renderFilter(v.path, raw, "aresample=48000,aformat=sample_fmts=flt:channel_layouts=stereo", s, work, cancel);
            voiceRaw = measure(raw, s, work, cancel);
            // Relative processing needs a stable working level, but bypass really bypasses it.
            double trim = 0;
            if (s.strength > 0 && voiceRaw.finite) trim = std::clamp(- 20- voiceRaw.integrated, - 18.0, 18.0);
            auto dst = work/ "voice.processed.wav";
            renderFilter(raw, dst, voiceFilter(s, trim), s, work, cancel);
            fs::remove(raw);
            vi = inspectFloatWave(dst, cancel);
            voicePrepared = measure(dst, s, work, cancel);
            if (voiceRaw.finite && voiceRaw.integrated < - 45) result.warnings.push_back("Голос очень тихий: автоматическое усиление может поднять шум. Проверьте результат на слух.");
            if (vi.peak > 1) result.warnings.push_back("В рабочем голосовом сигнале есть пики выше 0 dBFS; финальная нормализация необходима.");
            prepared = dst;
            totalSamples = vi.samples;
        }
        if (s.mode != "voice") {
            progress(25, "Подготовка музыки без речевых фильтров");
            auto dst = work/ "music.prepared.wav";
            renderFilter(m.path, dst, "aresample=48000,aformat=sample_fmts=flt:channel_layouts=stereo", s, work, cancel);
            mi = inspectFloatWave(dst, cancel);
            musicRaw = measure(dst, s, work, cancel);
            totalSamples = std::max(totalSamples, mi.samples);
            if (s.mode == "music") prepared = dst;
        }
        const double duration = double(totalSamples)/ 48000;
        if (s.mode == "mix") {
            progress(38, "Поиск пауз и расчёт плавного приглушения");
            auto a = base(s);
            a.insert(a.end(), {
                "-i", (work/ "voice.processed.wav").string(), "-af", "silencedetect=noise="+ number(vi.threshold)+ "dB:d=0.18", "-f", "null", "-"
            });
            const auto log = checked(a, work, s, cancel);
            speech = speechFromSilence(log, double(vi.samples)/ 48000);
            if (vi.peak < 1e-7) speech.clear();
            double speechTime = 0;
            for (auto z: speech) speechTime += z.end- z.start;
            if (speechTime > double(vi.samples)/ 48000*.95) result.warnings.push_back("Почти не найдено пауз. Детектор энергии не отличает речь от постоянного шума или чужой музыки; проверьте назначение дорожки.");
            if (speech.empty()) result.warnings.push_back("Речь не найдена; автоматического приглушения музыки нет.");
            depth = double(s.priority)*.15;
            points = envelopePoints(speech, duration, depth);
            writeControl(work/ "duck.f32", totalSamples, speech, depth, cancel);
            double vg = voicePrepared.finite? std::clamp(- 18- voicePrepared.integrated, - 18.0, 18.0): 0;
            double mt = - 29+.12*s.presence;
            double mg = musicRaw.finite? std::clamp(mt- musicRaw.integrated, - 30.0, 18.0): 0;
            progress(50, "Сведение голоса и музыки");
            a = base(s);
            a.insert(a.end(), {
                "-i", (work/ "voice.processed.wav").string(), "-i", (work/ "music.prepared.wav").string(), "-f", "f32le", "-ar", "48000", "-ac", "1", "-i", (work/ "duck.f32").string()
            });
            std::string graph = "[0:a]volume="+ number(vg)+ "dB,apad=whole_len="+ std::to_string(totalSamples)+ ",atrim=end_sample="+ std::to_string(totalSamples)+ "[v];[1:a]volume="+ number(mg)+ "dB,apad=whole_len="+ std::to_string(totalSamples)+ ",atrim=end_sample="+ std::to_string(totalSamples)+ "[m];[2:a]pan=stereo|c0=c0|c1=c0[e];[m][e]amultiply[d];[v][d]amix=inputs=2:duration=longest:dropout_transition=0:normalize=0[o]";
            a.insert(a.end(), {
                "-filter_complex", graph, "-map", "[o]"
            });
            prepared = work/ "mix.prepared.wav";
            outputArgs(a, prepared);
            checked(a, work, s, cancel);
            fs::remove(work/ "duck.f32");
            if (!s.keepStems) {
                fs::remove(work/ "voice.processed.wav");
                fs::remove(work/ "music.prepared.wav");
            }
        }
        std::vector<LevelSegment> levels;
        std::string segmentHash;
        if (!s.levelSegments.empty()) {
            progress(58, "Измерение громкости каждого клипа дорожки");
            segmentHash = sha256File(s.levelSegments, &cancel);
            const auto info = inspectFloatWave(prepared, cancel);
            levels = readLevelSegments(s.levelSegments, info.samples);
            double reference = -std::numeric_limits<double>::infinity();
            for (auto&segment : levels) {
                segment.before = segmentMeter(prepared, segment, false, s, work, cancel);
                if (segment.before.finite) reference = std::max(reference, segment.before.integrated);
            }
            for (auto&segment : levels) {
                if (!segment.before.finite) continue;
                const double desired = reference - segment.before.integrated;
                // The working WAV is float32. The final loudnorm pass limits true peaks.
                segment.gainDb = std::clamp(desired, 0.0, 24.0);
            }
            applySegmentGains(prepared, info, levels, cancel);
        }
        progress(65, "Измерение громкости и настоящих пиков всего выделения");
        result.before = measure(prepared, s, work, cancel);
        result.silent = !result.before.finite;
        auto final = work/ "result.wav";
        std::string tail = "aresample=48000,apad=whole_len="+ std::to_string(totalSamples)+ ",atrim=end_sample="+ std::to_string(totalSamples);
        int correctivePasses = 0;
        if (s.normalize && result.before.finite && !levels.empty()) {
            // loudnorm may switch to a dynamic pass and undo equal levels at clip boundaries.
            // Use one gain for the entire prepared track and leave true-peak headroom in the limiter.
            double gain = s.target - result.before.integrated;
            const std::string limiter = "alimiter=limit=" + number(std::pow(10, (s.truePeak - 1.2) / 20)) + ":level=false:latency=true,";
            progress(75, "Выравнивание всей дорожки с ограничением пиков");
            for (int pass = 0; pass < 3; ++pass) {
                const auto rendered = pass ? work/("verified-" + std::to_string(pass) + ".wav") : final;
                renderFilter(prepared, rendered, "volume=" + number(gain) + "dB," + limiter + tail, s, work, cancel, true);
                if (pass) { fs::remove(final); fs::rename(rendered, final); }
                progress(87, "Независимый контроль громкости после обработки");
                result.after = verifyEbur128(final, s, work, cancel);
                if (!result.after.finite || (std::abs(result.after.integrated - s.target) <= .15 && result.after.truePeak <= s.truePeak + .02) || pass == 2) break;
                gain += s.target - result.after.integrated;
                ++correctivePasses;
            }
        } else if (s.normalize && result.before.finite) {
            progress(75, "Второй проход нормализации");
            renderFilter(prepared, final, loudnormFilter(s, &result.before)+ ","+ tail, s, work, cancel, true);
        } else {
            if (s.normalize) result.warnings.push_back("Тишина или сигнал ниже измерительного порога: нормализация пропущена, звук не усилен.");
            if (!s.normalize && result.before.finite && result.before.truePeak > 0) throw Error("Пики превышают 0 dBTP. Включите нормализацию или уменьшите громкость; перегруженный PCM не будет опубликован.");
            renderFilter(prepared, final, tail, s, work, cancel, true);
        }
        if (levels.empty()) {
            progress(87, "Независимый контроль громкости после обработки");
            result.after = verifyEbur128(final, s, work, cancel);
            // Short programs can be measured differently by loudnorm's dynamic path.
            // Correct only by a constant gain, with a true-peak margin, then remeasure.
            // Never keep retrying indefinitely or silently smash dynamics to force the target.
            while (s.normalize && result.after.finite && correctivePasses < 2) {
                const double desired = s.target- result.after.integrated;
                const double headroom = s.truePeak-.10- result.after.truePeak;
                const double adjustment = std::min(desired, headroom);
                if (std::abs(desired) <= .15 && result.after.truePeak <= s.truePeak+.02) break;
                if (std::abs(adjustment) < .05) break;
                const auto corrected = work/("verified-"+ std::to_string(correctivePasses)+ ".wav");
                renderFilter(final, corrected, "volume="+ number(adjustment)+ "dB,"+ tail, s, work, cancel, true);
                fs::remove(final);
                fs::rename(corrected, final);
                ++ correctivePasses;
                result.after = verifyEbur128(final, s, work, cancel);
            }
        }
        bool levelMet = true;
        if (!levels.empty()) {
            double quietest = std::numeric_limits<double>::infinity();
            double loudest = -std::numeric_limits<double>::infinity();
            for (auto&segment : levels) {
                segment.after = segmentMeter(final, segment, true, s, work, cancel);
                levelMet &= segment.before.finite && segment.after.finite;
                if (segment.after.finite) {
                    quietest = std::min(quietest, segment.after.integrated);
                    loudest = std::max(loudest, segment.after.integrated);
                }
            }
            levelMet &= loudest - quietest <= 1.0;
            if (!levelMet) result.warnings.push_back("Клипы дорожки не удалось выровнять в пределах 1 LU; проверьте тихие фрагменты и пики.");
        }
        result.targetMet = (!s.normalize || (result.after.finite && std::abs(result.after.integrated- s.target) <= defaults::lufs_tolerance && result.after.truePeak <= s.truePeak+.05)) && levelMet;
        if (s.normalize && !result.targetMet && !result.silent) result.warnings.push_back("Контроль не подтвердил заданные LUFS / true peak в допуске. Результат требует проверки, это не успешная приёмка.");
        // Refuse altered inputs rather than silently bind old analysis to a changed timeline/file.
        progress(94, "Проверка неизменности источников и публикация результата");
        for (auto&source: sources) if (sha256File(source.path, &cancel) != source.hash) throw Error("Исходник изменился во время обработки; результат отклонён.");
        if (!s.levelSegments.empty() && sha256File(s.levelSegments, &cancel) != segmentHash) throw Error("Границы аудиоклипов изменились во время обработки; результат отклонён.");
        if (!s.keepStems && prepared != final) {
            fs::remove(prepared);
        }
        fs::remove(work/ "rnnoise.so");
        std::ostringstream envelope;
        envelope.imbue(std::locale::classic());
        envelope<< "time_seconds\tgain_db\n";
        for (auto p: points) envelope<< number(p.time)<< '\t'<< number(p.gainDb)<< '\n';
        writeText(work/ "ducking.tsv", envelope.str());
        std::ostringstream report;
        report.imbue(std::locale::classic());
        report<< "{\n\"schema\":" << defaults::result_schema << ",\"module\":\"StudioAudio\",\"version\":" << jsonString(defaults::result_version) << ",\n\"status\":"<< jsonString(result.targetMet? "verified_numeric_targets":(result.silent? "silence_or_below_gate": "needs_review"))<< ",\n\"snapshot_token\":"<< jsonString(s.snapshotToken)<< ",\n\"mode\":"<< jsonString(s.mode)<< ",\"sample_rate\":48000,\"channels\":2,\"samples\":"<< totalSamples<< ",\"duration_seconds\":"<< number(duration)<< ",\n\"settings\":{\"strength\":"<< s.strength<< ",\"noise\":"<< jsonString(s.noise)<< ",\"presence\":"<< s.presence<< ",\"priority\":"<< s.priority<< ",\"normalize\":"<<(s.normalize? "true": "false")<< ",\"target_lufs\":"<< number(s.target)<< ",\"true_peak_dbtp\":"<< number(s.truePeak)<< "},\n\"before\":"<< meterJson(result.before)<< ",\n\"after\":"<< meterJson(result.after)<< ",\n\"measurement\":{\"before\":\"loudnorm_input\",\"after\":\"ebur128_final_file\",\"constant_gain_corrections\":"<< correctivePasses<< "},\n\"target_met\":"<<(result.targetMet? "true": "false")<< ",\"audio_file\":\"result.wav\",\"audio_sha256\":"<< jsonString(sha256File(final, &cancel))<< ",\n\"ffmpeg_version\":"<< jsonString(ffVersion.substr(0, ffVersion.find('\n')))<< ",\n\"sources\":[";
        for (size_t i = 0; i < sources.size(); ++ i) {
            if (i) report<< ',';
            report<< "{\"path\":"<< jsonString(sources[i].path.string())<< ",\"sha256\":"<< jsonString(sources[i].hash)<< "}";
        }
        report << "],\n\"track_leveling\":{\"enabled\":" << (!s.levelSegments.empty() ? "true" : "false")
               << ",\"verified\":" << (levelMet ? "true" : "false") << ",\"segments\":[";
        for (size_t i = 0; i < levels.size(); ++i) {
            if (i) report << ',';
            const auto&segment = levels[i];
            report << "{\"start_sample\":" << segment.start << ",\"end_sample\":" << segment.end
                   << ",\"gain_db\":" << number(segment.gainDb) << ",\"before\":" << meterJson(segment.before)
                   << ",\"after\":" << meterJson(segment.after) << '}';
        }
        report << "]},\n\"speech_intervals\":[";
        for (size_t i = 0; i < speech.size(); ++ i) {
            if (i) report<< ',';
            report<< '['<< number(speech[i].start)<< ','<< number(speech[i].end)<< ']';
        }
        report<< "],\n\"ducking\":{\"depth_db\":"<< number(depth)<< ",\"lookahead_s\":"<< number(defaults::lookahead_s)<< ",\"hold_s\":"<< number(defaults::hold_s)<< ",\"release_s\":"<< number(defaults::release_s)<< ",\"detector\":\"silencedetect_energy_not_semantic_vad\",\"threshold_dbfs\":"<< number(vi.threshold)<< "},\n\"warnings\":[";
        for (size_t i = 0; i < result.warnings.size(); ++ i) {
            if (i) report<< ',';
            report<< jsonString(result.warnings[i]);
        }
        report<< "],\n\"host_integration_tested\":false,\"steam_deck_tested\":false,\"retained_stems_are_pre_master\":true\n}\n";
        writeText(work/ "report.json", report.str());
        if (cancel.load()) throw Cancelled();
        if (fs::exists(s.outputDir)) throw Error("Каталог назначения появился во время работы; перезапись запрещена.");
        fs::rename(work, s.outputDir);
        temp.published = true;
        if (cb) cb(100, "Готов аудиорезультат; проект пока не изменён");
        return result;
    }
}
