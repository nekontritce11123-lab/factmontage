// SPDX-License-Identifier: MIT
#include "subtitle.hpp"
#include "audio.hpp"
#include "subtitle_parameters.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <utility>

namespace studio_subtitles {
namespace {

using studio_audio::jsonString;
using studio_audio::runProcess;
using studio_audio::sha256File;

std::uint16_t little16(const unsigned char* bytes) {
    return std::uint16_t(bytes[0]) | (std::uint16_t(bytes[1]) << 8);
}

std::uint32_t little32(const unsigned char* bytes) {
    return std::uint32_t(bytes[0]) | (std::uint32_t(bytes[1]) << 8) | (std::uint32_t(bytes[2]) << 16) | (std::uint32_t(bytes[3]) << 24);
}

void checkRecognitionAudio(const fs::path& path, const std::atomic_bool& cancelled) {
    std::ifstream input(path, std::ios::binary);
    std::array<unsigned char, 12> header{};
    input.read(reinterpret_cast<char*>(header.data()), std::streamsize(header.size()));
    if (!input || std::string_view(reinterpret_cast<const char*>(header.data()), 4) != "RIFF"
        || std::string_view(reinterpret_cast<const char*>(header.data() + 8), 4) != "WAVE")
        throw Error("Звук клипа повреждён: ожидался WAV PCM.");
    bool formatOk = false;
    while (input) {
        std::array<unsigned char, 8> chunk{};
        input.read(reinterpret_cast<char*>(chunk.data()), std::streamsize(chunk.size()));
        if (!input) break;
        const std::uint32_t size = little32(chunk.data() + 4);
        const std::string_view type(reinterpret_cast<const char*>(chunk.data()), 4);
        if (type == "fmt ") {
            if (size < 16 || size > 4096) throw Error("Звук клипа повреждён: неверный WAV fmt.");
            std::array<unsigned char, 40> fmt{};
            input.read(reinterpret_cast<char*>(fmt.data()), std::streamsize(std::min<std::uint32_t>(size, fmt.size())));
            if (!input) throw Error("Звук клипа повреждён: неполный WAV fmt.");
            const auto format = little16(fmt.data());
            formatOk = (format == 1 || (format == 0xfffe && size >= 40 && little16(fmt.data() + 24) == 1))
                && little16(fmt.data() + 2) == 1 && little32(fmt.data() + 4) == 16000 && little16(fmt.data() + 14) == 16;
            input.seekg(std::streamoff(size - std::min<std::uint32_t>(size, fmt.size()) + (size & 1)), std::ios::cur);
        } else if (type == "data") {
            if (!formatOk || size < 2 || (size & 1)) throw Error("В клипе нет подходящего звука для распознавания.");
            std::array<unsigned char, 65536> samples{};
            std::uint32_t remaining = size;
            double squareSum = 0;
            std::uint64_t sampleCount = 0;
            while (remaining) {
                if (cancelled.load()) throw Cancelled();
                const std::size_t count = std::min<std::size_t>(remaining, samples.size());
                input.read(reinterpret_cast<char*>(samples.data()), std::streamsize(count));
                if (!input) throw Error("Звук клипа повреждён: неполные PCM-данные.");
                for (std::size_t i = 0; i < count; i += 2) {
                    const auto value = std::int16_t(little16(samples.data() + i));
                    squareSum += double(value) * value;
                    ++sampleCount;
                }
                remaining -= std::uint32_t(count);
            }
            // Conservative gate: reject only approximately digital silence, not quiet speech.
            if (sampleCount == 0 || std::sqrt(squareSum / double(sampleCount)) <= 32.0)
                throw Error("Звук клипа пустой или почти неслышный; субтитры не созданы.");
            return;
        } else {
            input.seekg(std::streamoff(size + (size & 1)), std::ios::cur);
        }
        if (!input) throw Error("Звук клипа повреждён: неполный WAV.");
    }
    throw Error("В клипе нет подходящего звука для распознавания.");
}

std::size_t nextUtf8(std::string_view s, std::size_t i, std::uint32_t& codepoint) {
    if (i >= s.size()) return i;
    const auto c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
        codepoint = c;
        return i + 1;
    }
    const int count = (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : 4;
    if (i + static_cast<std::size_t>(count) > s.size()) {
        codepoint = 0xfffd;
        return i + 1;
    }
    codepoint = c & (count == 2 ? 0x1f : count == 3 ? 0x0f : 0x07);
    for (int n = 1; n < count; ++n) {
        const auto part = static_cast<unsigned char>(s[i + static_cast<std::size_t>(n)]);
        if ((part & 0xc0) != 0x80) {
            codepoint = 0xfffd;
            return i + 1;
        }
        codepoint = (codepoint << 6) | (part & 0x3f);
    }
    return i + static_cast<std::size_t>(count);
}

double codepointWidth(std::uint32_t cp) {
    if (cp == ' ' || cp == '\t') return 0.35;
    if (cp < 0x80 && std::ispunct(static_cast<unsigned char>(cp))) return 0.45;
    if ((cp >= 0x2e80 && cp <= 0x9fff) || (cp >= 0x1f300 && cp <= 0x1faff)) return 1.6;
    return 1.0;
}

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string jsonUnescape(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '\\' || i + 1 >= value.size()) {
            result.push_back(value[i]);
            continue;
        }
        const char escaped = value[++i];
        switch (escaped) {
        case 'n': result.push_back('\n'); break;
        case 'r': result.push_back('\r'); break;
        case 't': result.push_back('\t'); break;
        case '"': result.push_back('"'); break;
        case '\\': result.push_back('\\'); break;
        case '/': result.push_back('/'); break;
        default: result.push_back(escaped); break;
        }
    }
    return result;
}

std::size_t matching(std::string_view text, std::size_t open, char left, char right) {
    int depth = 0;
    bool quoted = false;
    bool escaped = false;
    for (std::size_t i = open; i < text.size(); ++i) {
        const char c = text[i];
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
            continue;
        }
        if (c == '"') {
            quoted = true;
            continue;
        }
        if (c == left) ++depth;
        else if (c == right && --depth == 0) return i;
    }
    return std::string_view::npos;
}

std::string stringField(std::string_view object, std::string_view key) {
    const std::string needle = '"' + std::string(key) + '"';
    const auto keyPos = object.find(needle);
    if (keyPos == std::string_view::npos) return {};
    auto pos = object.find(':', keyPos + needle.size());
    if (pos == std::string_view::npos) return {};
    while (++pos < object.size() && std::isspace(static_cast<unsigned char>(object[pos]))) {}
    if (pos >= object.size() || object[pos] != '"') return {};
    ++pos;
    const auto start = pos;
    bool escaped = false;
    for (; pos < object.size(); ++pos) {
        if (escaped) {
            escaped = false;
            continue;
        }
        if (object[pos] == '\\') {
            escaped = true;
            continue;
        }
        if (object[pos] == '"') return jsonUnescape(object.substr(start, pos - start));
    }
    return {};
}

double numberField(std::string_view object, std::string_view key, double fallback = 0.0) {
    const std::string needle = '"' + std::string(key) + '"';
    const auto keyPos = object.find(needle);
    if (keyPos == std::string_view::npos) return fallback;
    auto pos = object.find(':', keyPos + needle.size());
    if (pos == std::string_view::npos) return fallback;
    ++pos;
    while (pos < object.size() && std::isspace(static_cast<unsigned char>(object[pos]))) ++pos;
    std::size_t end = pos;
    while (end < object.size() && (std::isdigit(static_cast<unsigned char>(object[end])) || object[end] == '-' || object[end] == '+' || object[end] == '.' || object[end] == 'e' || object[end] == 'E')) ++end;
    try {
        return std::stod(std::string(object.substr(pos, end - pos)));
    } catch (...) {
        return fallback;
    }
}

std::int64_t timestampField(std::string_view object, std::string_view key, std::int64_t fallback) {
    const double numeric = numberField(object, key, -1.0);
    if (numeric >= 0.0) return static_cast<std::int64_t>(std::llround(numeric));
    const auto text = stringField(object, key);
    if (text.empty()) return fallback;
    int hours = 0, minutes = 0, seconds = 0, millis = 0;
    if (std::sscanf(text.c_str(), "%d:%d:%d,%d", &hours, &minutes, &seconds, &millis) == 4 ||
        std::sscanf(text.c_str(), "%d:%d:%d.%d", &hours, &minutes, &seconds, &millis) == 4) {
        return ((hours * 60LL + minutes) * 60LL + seconds) * 1000LL + millis;
    }
    return fallback;
}

std::vector<std::string_view> arrayObjects(std::string_view object, std::string_view key) {
    const std::string needle = '"' + std::string(key) + '"';
    const auto keyPos = object.find(needle);
    if (keyPos == std::string_view::npos) return {};
    const auto open = object.find('[', keyPos + needle.size());
    if (open == std::string_view::npos) return {};
    const auto close = matching(object, open, '[', ']');
    if (close == std::string_view::npos) return {};
    std::vector<std::string_view> result;
    for (std::size_t i = open + 1; i < close;) {
        while (i < close && (std::isspace(static_cast<unsigned char>(object[i])) || object[i] == ',')) ++i;
        if (i >= close) break;
        if (object[i] != '{') {
            ++i;
            continue;
        }
        const auto end = matching(object, i, '{', '}');
        if (end == std::string_view::npos || end > close) break;
        result.push_back(object.substr(i, end - i + 1));
        i = end + 1;
    }
    return result;
}

std::vector<std::string_view> transcriptionObjects(std::string_view json) {
    return arrayObjects(json, "transcription");
}

std::vector<std::pair<std::int64_t, std::int64_t>> vadSegments(std::string_view json, std::int64_t zoneStartMs = 0) {
    std::vector<std::pair<std::int64_t, std::int64_t>> result;
    for (const auto segment : arrayObjects(json, "vad_segments")) {
        const auto start = timestampField(segment, "from", -1);
        const auto end = timestampField(segment, "to", -1);
        if (start < 0 || end <= start || (!result.empty() && start + zoneStartMs < result.back().second))
            throw Error("Некорректные интервалы детектора речи; субтитры не созданы.");
        result.emplace_back(start + zoneStartMs, end + zoneStartMs);
    }
    return result;
}

std::vector<Block> layoutSpeechWords(const std::vector<Word>& words, std::string_view json,
                                    std::int64_t zoneStartMs, const LayoutOptions& options) {
    std::vector<Block> result;
    for (const auto& span : vadSegments(json, zoneStartMs)) {
        std::vector<Word> selected;
        for (const auto& word : words)
            if (word.startMs >= span.first && word.endMs <= span.second) selected.push_back(word);
        auto blocks = layoutWords(selected, options);
        for (auto& block : blocks) {
            // Reading-time padding must not extend a recognized cue into a removed pause.
            block.endMs = std::min(block.endMs, span.second);
            result.push_back(std::move(block));
        }
    }
    return result;
}

std::vector<Word> fallbackWords(std::string text, std::int64_t start, std::int64_t end) {
    std::istringstream input(text);
    std::vector<std::string> tokens;
    for (std::string token; input >> token;) tokens.push_back(std::move(token));
    if (tokens.empty()) return {};
    if (end <= start) end = start + static_cast<std::int64_t>(tokens.size()) * 250;
    const auto duration = std::max<std::int64_t>(1, end - start);
    std::vector<Word> result;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const auto a = start + duration * static_cast<std::int64_t>(i) / static_cast<std::int64_t>(tokens.size());
        const auto b = start + duration * static_cast<std::int64_t>(i + 1) / static_cast<std::int64_t>(tokens.size());
        result.push_back({a, std::max(a + 1, b), tokens[i], -1.0});
    }
    return result;
}

std::string joinWords(const std::vector<Word>& words, std::size_t first, std::size_t last) {
    std::string result;
    for (std::size_t i = first; i < last; ++i) {
        if (!result.empty()) result.push_back(' ');
        result += words[i].text;
    }
    return result;
}

std::uint64_t stableId(const std::vector<Word>& words) {
    std::uint64_t hash = 1469598103934665603ULL;
    const auto add = [&hash](std::string_view value) {
        for (const unsigned char c : value) {
            hash ^= c;
            hash *= 1099511628211ULL;
        }
    };
    if (!words.empty()) {
        add(std::to_string(words.front().startMs));
        add(words.front().text);
        add(std::to_string(words.back().endMs));
    }
    return hash == 0 ? 1 : hash;
}

double measureWith(const LayoutOptions& options, std::string_view text) {
    return options.measure ? options.measure(text) : defaultMeasure(text);
}

std::string assTime(std::int64_t ms) {
    ms = std::max<std::int64_t>(0, ms);
    const auto hours = ms / 3600000;
    ms %= 3600000;
    const auto minutes = ms / 60000;
    ms %= 60000;
    const auto seconds = ms / 1000;
    const auto centiseconds = (ms % 1000) / 10;
    std::ostringstream out;
    out << hours << ':' << std::setfill('0') << std::setw(2) << minutes << ':' << std::setw(2) << seconds << '.' << std::setw(2) << centiseconds;
    return out.str();
}

std::string assEscape(std::string text) {
    std::string result;
    result.reserve(text.size());
    for (const char c : text) {
        if (c == '{' || c == '}' || c == '\\' || c == '\n' || c == '\r') result.push_back(' ');
        else result.push_back(c);
    }
    return result;
}

std::string managedEffect(const Block& block) {
    if (block.words.empty()) return {};
    std::ostringstream out;
    out << "SUNIMO1:" << block.id << ':' << block.startMs << ':' << block.endMs << ':';
    out << std::hex << std::setfill('0');
    const auto text = joinWords(block.words, 0, block.words.size());
    for (unsigned char byte : text) out << std::setw(2) << static_cast<unsigned>(byte);
    out << std::dec << ':';
    if (block.timedWords) {
        for (std::size_t i = 0; i < block.words.size(); ++i) {
            if (i) out << ';';
            out << block.words[i].startMs << '-' << block.words[i].endMs;
        }
    }
    return out.str();
}

std::string assAlpha(std::uint32_t color) {
    std::ostringstream out;
    out << "&H" << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << ((color >> 24) & 0xff) << '&';
    return out.str();
}

std::string revealAt(std::int64_t offsetMs, const RenderOptions& options) {
    return "{\\1a&HFF&\\3a&HFF&\\4a&HFF&\\t(" + std::to_string(offsetMs) + ',' + std::to_string(offsetMs + 1)
        + ",\\1a" + assAlpha(options.textColor) + "\\3a" + assAlpha(options.outlineColor)
        + "\\4a" + assAlpha(options.backgroundColor) + ")}";
}

std::string typewriterWord(const Word& word, std::int64_t cueOffsetMs, std::int64_t durationMs, const RenderOptions& options) {
    std::vector<std::string> glyphs;
    for (std::size_t at = 0; at < word.text.size();) {
        const unsigned char lead = static_cast<unsigned char>(word.text[at]);
        std::size_t length = lead < 0x80 ? 1 : (lead & 0xe0) == 0xc0 ? 2 : (lead & 0xf0) == 0xe0 ? 3 : 4;
        length = std::min(length, word.text.size() - at);
        glyphs.push_back(assEscape(word.text.substr(at, length)));
        at += length;
    }
    std::string result;
    for (std::size_t i = 0; i < glyphs.size(); ++i) {
        const auto start = cueOffsetMs + durationMs * static_cast<std::int64_t>(i) / static_cast<std::int64_t>(glyphs.size());
        result += revealAt(start, options) + glyphs[i];
    }
    return result;
}

std::string atomicWrite(const fs::path& path, const std::string& content) {
    const auto temporary = path.string() + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) throw Error("Не удалось создать временный файл: " + temporary);
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!out) throw Error("Не удалось записать файл: " + temporary);
    }
    std::error_code ec;
    fs::rename(temporary, path, ec);
    if (ec) {
        fs::remove(path, ec);
        fs::rename(temporary, path, ec);
    }
    if (ec) throw Error("Не удалось опубликовать файл: " + path.string());
    return path.string();
}

} // namespace

double defaultMeasure(std::string_view text) {
    double width = 0.0;
    for (std::size_t i = 0; i < text.size();) {
        std::uint32_t cp = 0;
        i = nextUtf8(text, i, cp);
        width += codepointWidth(cp);
    }
    return width;
}

std::vector<Word> parseWhisperJson(std::string_view json, std::int64_t zoneStartMs) {
    std::vector<Word> result;
    for (const auto segment : transcriptionObjects(json)) {
        const auto timestamps = [&] {
            const auto key = segment.find("\"timestamps\"");
            if (key == std::string_view::npos) return segment;
            const auto open = segment.find('{', key);
            const auto close = open == std::string_view::npos ? std::string_view::npos : matching(segment, open, '{', '}');
            return close == std::string_view::npos ? segment : segment.substr(open, close - open + 1);
        }();
        const auto segmentStart = timestampField(timestamps, "from", 0) + zoneStartMs;
        const auto segmentEnd = timestampField(timestamps, "to", segmentStart + 1) + zoneStartMs;
        auto tokens = arrayObjects(segment, "tokens");
        const auto segmentFirst = result.size();
        bool tokenFound = false;
        for (const auto token : tokens) {
            const auto raw = stringField(token, "text");
            const auto text = trim(raw);
            if (text.empty() || text.front() == '[') continue;
            const auto tokenStart = timestampField(token, "from", segmentStart - zoneStartMs) + zoneStartMs;
            const auto tokenEnd = timestampField(token, "to", segmentEnd - zoneStartMs) + zoneStartMs;
            if (result.size() > segmentFirst && !std::isspace(static_cast<unsigned char>(raw.front()))) {
                auto& word = result.back();
                word.text += text;
                word.endMs = std::max(word.endMs, tokenEnd);
                word.confidence = std::min(word.confidence, numberField(token, "p", 0.0));
            } else {
                result.push_back({tokenStart, std::max(tokenStart + 1, tokenEnd), text, numberField(token, "p", 1.0)});
            }
            tokenFound = true;
        }
        if (!tokenFound) {
            auto fallback = fallbackWords(stringField(segment, "text"), segmentStart, segmentEnd);
            result.insert(result.end(), fallback.begin(), fallback.end());
        }
    }
    if (json.find("\"vad_segments\"") != std::string_view::npos) {
        const auto spans = vadSegments(json, zoneStartMs);
        std::vector<Word> constrained;
        for (const auto& word : result) {
            Word supported = word;
            std::int64_t longest = 0;
            for (const auto& span : spans) {
                const auto start = std::max(word.startMs, span.first);
                const auto end = std::min(word.endMs, span.second);
                if (end - start > longest) {
                    longest = end - start;
                    supported.startMs = start;
                    supported.endMs = end;
                }
            }
            // A seam token may touch two speech spans. Keep one supported interval,
            // never the removed silence between them; unconfirmed tokens are omitted.
            if (longest > 0) constrained.push_back(std::move(supported));
        }
        result = std::move(constrained);
    }
    std::stable_sort(result.begin(), result.end(), [](const Word& a, const Word& b) { return a.startMs < b.startMs; });
    return result;
}

std::vector<Block> layoutWords(const std::vector<Word>& words, const LayoutOptions& options) {
    std::vector<Block> blocks;
    if (words.empty()) return blocks;
    std::size_t first = 0;
    while (first < words.size()) {
        std::size_t last = first + 1;
        while (last < words.size()) {
            // Whisper can assign the same timestamp to adjacent short words.
            // Keep them together so identical starts do not create a zero-length cue.
            if (words[last].startMs <= words[last - 1].startMs) { ++last; continue; }
            const auto& previous = words[last - 1].text;
            if (!previous.empty() && (previous.back() == '.' || previous.back() == '!' || previous.back() == '?')) break;
            const auto duration = words[last].endMs - words[first].startMs;
            const auto gap = words[last].startMs - words[last - 1].endMs;
            const auto text = joinWords(words, first, last + 1);
            const auto characters = std::count_if(text.begin(), text.end(), [](unsigned char c) { return (c & 0xc0) != 0x80; });
            const auto cps = duration > 0 ? static_cast<double>(characters) * 1000.0 / static_cast<double>(duration) : 1e9;
            if (duration > options.maxDurationMs || gap > 650
                || (duration >= options.minDurationMs && cps > options.maxReadingCps)) break;
            if (measureWith(options, text) > options.maxWidth) break;
            ++last;
        }
        Block block;
        block.id = stableId(std::vector<Word>(words.begin() + static_cast<std::ptrdiff_t>(first), words.begin() + static_cast<std::ptrdiff_t>(last)));
        block.startMs = words[first].startMs;
        for (std::size_t word = first; word < last; ++word) block.speechEndMs = std::max(block.speechEndMs, words[word].endMs);
        block.words.assign(words.begin() + static_cast<std::ptrdiff_t>(first), words.begin() + static_cast<std::ptrdiff_t>(last));
        block.timedWords = std::all_of(block.words.begin(), block.words.end(), [](const Word& word) { return word.confidence >= 0.0; });
        block.line1 = joinWords(words, first, last);
        blocks.push_back(std::move(block));
        first = last;
    }
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        auto& block = blocks[i];
        if (i && block.startMs < blocks[i - 1].endMs) {
            block.startMs = blocks[i - 1].endMs;
            block.timingConflict = blocks[i - 1].timingConflict = true;
        }
        block.endMs = std::max(block.startMs + options.minDurationMs, block.speechEndMs + 250);
        if (i + 1 < blocks.size()) {
            const auto nextStart = blocks[i + 1].startMs;
            if (nextStart < block.speechEndMs) {
                block.timingConflict = blocks[i + 1].timingConflict = true;
                block.endMs = std::max(block.startMs + 1, block.speechEndMs);
            } else if (nextStart > block.startMs) {
                block.endMs = std::min(block.endMs, nextStart);
            }
        }
    }
    return blocks;
}

bool splitBlock(std::vector<Block>& blocks, std::size_t blockIndex, std::size_t wordIndex, const LayoutOptions& options) {
    if (blockIndex >= blocks.size() || wordIndex == 0 || wordIndex >= blocks[blockIndex].words.size()) return false;
    const auto source = blocks[blockIndex];
    std::vector<Word> left(source.words.begin(), source.words.begin() + static_cast<std::ptrdiff_t>(wordIndex));
    std::vector<Word> right(source.words.begin() + static_cast<std::ptrdiff_t>(wordIndex), source.words.end());
    auto leftBlocks = layoutWords(left, options);
    auto rightBlocks = layoutWords(right, options);
    if (leftBlocks.empty() || rightBlocks.empty()) return false;
    leftBlocks.front().id = source.id == 0 ? stableId(left) : source.id;
    rightBlocks.front().id = stableId(right) ^ 0x53504c4954ULL;
    blocks.erase(blocks.begin() + static_cast<std::ptrdiff_t>(blockIndex));
    blocks.insert(blocks.begin() + static_cast<std::ptrdiff_t>(blockIndex), rightBlocks.begin(), rightBlocks.end());
    blocks.insert(blocks.begin() + static_cast<std::ptrdiff_t>(blockIndex), leftBlocks.begin(), leftBlocks.end());
    return true;
}

bool mergeBlocks(std::vector<Block>& blocks, std::size_t leftIndex, const LayoutOptions& options) {
    if (leftIndex + 1 >= blocks.size()) return false;
    const auto leftId = blocks[leftIndex].id;
    std::vector<Word> words = blocks[leftIndex].words;
    words.insert(words.end(), blocks[leftIndex + 1].words.begin(), blocks[leftIndex + 1].words.end());
    auto merged = layoutWords(words, options);
    if (merged.empty()) return false;
    merged.front().id = leftId == 0 ? stableId(words) : leftId;
    blocks.erase(blocks.begin() + static_cast<std::ptrdiff_t>(leftIndex), blocks.begin() + static_cast<std::ptrdiff_t>(leftIndex + 2));
    blocks.insert(blocks.begin() + static_cast<std::ptrdiff_t>(leftIndex), merged.begin(), merged.end());
    return true;
}

std::string renderWordsJson(const std::vector<Word>& words) {
    std::ostringstream out;
    out << "{\"schema\":1,\"words\":[";
    for (std::size_t i = 0; i < words.size(); ++i) {
        if (i) out << ',';
        out << "{\"start_ms\":" << words[i].startMs << ",\"end_ms\":" << words[i].endMs
            << ",\"text\":" << jsonString(words[i].text) << ",\"confidence\":" << words[i].confidence << '}';
    }
    out << "]}\n";
    return out.str();
}

std::string renderBlocksJson(const std::vector<Block>& blocks) {
    std::ostringstream out;
    out << "{\"schema\":2,\"blocks\":[";
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        if (i) out << ',';
        out << "{\"id\":" << blocks[i].id << ",\"start_ms\":" << blocks[i].startMs << ",\"end_ms\":" << blocks[i].endMs
            << ",\"speech_start_ms\":" << blocks[i].words.front().startMs
            << ",\"speech_end_ms\":" << blocks[i].speechEndMs
            << ",\"timing_conflict\":" << (blocks[i].timingConflict ? "true" : "false")
            << ",\"text\":" << jsonString(blocks[i].line1) << '}';
    }
    out << "]}\n";
    return out.str();
}

std::vector<Block> parseRenderBlocksJson(std::string_view json) {
    if (json.size() > 8 * 1024 * 1024 || numberField(json, "schema", -1) != 3)
        throw Error("Неверный формат блоков для оформления.");
    const auto objects = arrayObjects(json, "blocks");
    if (objects.empty() || objects.size() > 1000) throw Error("Нет блоков для оформления.");
    std::vector<Block> blocks;
    blocks.reserve(objects.size());
    for (const auto object : objects) {
        Block block;
        const auto id = stringField(object, "id");
        std::size_t consumed = 0;
        try { block.id = std::stoull(id, &consumed); }
        catch (...) { throw Error("Повреждена связь блока субтитров."); }
        if (consumed != id.size()) throw Error("Повреждена связь блока субтитров.");
        block.startMs = timestampField(object, "start_ms", -1);
        block.endMs = timestampField(object, "end_ms", -1);
        block.line1 = stringField(object, "text");
        if (block.startMs < 0 || block.endMs <= block.startMs || block.endMs > 86400000
            || block.line1.empty() || block.line1.size() > 8192)
            throw Error("Повреждены текст или границы блока субтитров.");
        const auto marks = arrayObjects(object, "words");
        if (marks.empty()) {
            block.timedWords = false;
            block.words.push_back({block.startMs, block.endMs, block.line1, 0.0});
        } else {
            if (marks.size() > 200) throw Error("Слишком много меток слов в блоке.");
            for (const auto mark : marks) {
                Word word;
                word.startMs = timestampField(mark, "start_ms", -1);
                word.endMs = timestampField(mark, "end_ms", -1);
                word.text = stringField(mark, "text");
                if (word.startMs < block.startMs || word.endMs <= word.startMs || word.endMs > block.endMs
                    || word.text.empty()) throw Error("Повреждены метки слов.");
                block.words.push_back(std::move(word));
            }
            if (joinWords(block.words, 0, block.words.size()) != block.line1)
                throw Error("Метки слов больше не совпадают с текстом блока.");
        }
        blocks.push_back(std::move(block));
    }
    return blocks;
}

std::string renderAss(const std::vector<Block>& blocks, const RenderOptions& options) {
    const auto color = [](std::uint32_t value) {
        std::ostringstream out;
        out << "&H" << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << value;
        return out.str();
    };
    const std::uint32_t back = options.background == 1 ? options.backgroundColor & 0x00FFFFFF
                                : options.backgroundColor;
    const bool separateBox = options.background != 0 && options.outline > 0;
    std::ostringstream definition;
    definition << options.font << ',' << options.fontSize << ',' << color(options.textColor) << ",&H0000FFFF,"
               << color(options.outlineColor) << ',' << color(back) << ',' << (options.bold ? 1 : 0)
               << ",0,0,0,100,100,0,0,"
               << (separateBox ? 1 : options.background ? 3 : 1) << ',' << options.outline << ',' << options.shadow << ',' << options.alignment
               << ",80,80," << options.marginV << ",1";
    std::uint64_t styleHash = 1469598103934665603ULL;
    for (unsigned char byte : definition.str()) { styleHash ^= byte; styleHash *= 1099511628211ULL; }
    std::ostringstream styleId;
    styleId << "Studio-" << std::hex << styleHash;
    const std::string style = styleId.str();
    std::ostringstream out;
    out << "[Script Info]\nScriptType: v4.00+\nPlayResX: 1920\nPlayResY: 1080\nTitle: " << options.title << "\n\n"
        << "[V4+ Styles]\nFormat: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
        << "Style: " << style << ',' << definition.str() << "\n";
    if (separateBox) {
        out << "Style: " << style << "-Box," << options.font << ',' << options.fontSize
            << ",&HFFFFFFFF,&HFFFFFFFF," << color(back) << ',' << color(back) << ',' << (options.bold ? 1 : 0)
            << ",0,0,0,100,100,0,0,3," << std::max(3, options.outline) << ",0," << options.alignment
            << ",80,80," << options.marginV << ",1\n";
    }
    out << "\n"
        << "[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";
    for (const auto& block : blocks) {
        std::string text, boxText;
        const auto effect = managedEffect(block);
        const std::string animation = !block.timedWords && (options.animation == "words" || options.animation == "reveal_words"
            || options.animation == "letters") ? "fade" : options.animation;
        for (std::size_t i = 0; i < block.words.size(); ++i) {
            if (i) { text.push_back(' '); boxText.push_back(' '); }
            const auto wordStart = std::max(block.startMs, block.words[i].startMs);
            const auto nextStart = i + 1 < block.words.size() ? std::min(block.endMs, block.words[i + 1].startMs) : block.endMs;
            const auto durationMs = std::max<std::int64_t>(1, nextStart - wordStart);
            const auto letterDurationMs = std::max<std::int64_t>(1, std::min(nextStart, block.words[i].endMs) - wordStart);
            const auto duration = std::max<std::int64_t>(1, durationMs / 10);
            if (animation == "words" || animation == "karaoke") {
                text += "{\\k" + std::to_string(duration) + "}";
            }
            if (animation == "reveal_words") {
                const auto offset = wordStart - block.startMs;
                text += revealAt(offset, options);
            }
            text += animation == "typewriter" || animation == "letters"
                ? typewriterWord(block.words[i], wordStart - block.startMs, letterDurationMs, options) : assEscape(block.words[i].text);
            boxText += assEscape(block.words[i].text);
        }
        if (animation == "pop") text = "{\\fad(70,90)\\fscx108\\fscy108\\t(0,120,\\fscx100\\fscy100)}" + text;
        std::string wholeMotion;
        if (animation == "fade") wholeMotion = "{\\fad(160,0)}";
        else if (animation == "rise" || animation == "slide") {
            const int y = options.alignment == 5 ? 540 : options.alignment == 8 ? options.marginV : 1080 - options.marginV;
            const int x = 960;
            std::ostringstream move;
            if (animation == "rise") move << "{\\fad(100,0)\\move(" << x << ',' << y + 40 << ',' << x << ',' << y << ",0,180)}";
            else move << "{\\fad(100,0)\\move(" << x - 120 << ',' << y << ',' << x << ',' << y << ",0,200)}";
            wholeMotion = move.str();
        } else if (animation == "accent") wholeMotion = "{\\fad(70,0)\\fscx105\\fscy105\\t(0,150,\\fscx100\\fscy100)}";
        text = wholeMotion + text;
        boxText = wholeMotion + boxText;
        if (separateBox) out << "Dialogue: 0," << assTime(block.startMs) << ',' << assTime(block.endMs) << ',' << style
                             << "-Box,,0,0,0," << effect << ',' << boxText << "\n";
        out << "Dialogue: " << (separateBox ? 1 : 0) << ',' << assTime(block.startMs) << ',' << assTime(block.endMs)
            << ',' << style << ",,0,0,0," << effect << ',' << text << "\n";
    }
    return out.str();
}

WorkerResult runWorker(const WorkerSettings& settings, const std::atomic_bool& cancelled, Progress progress) {
    if (settings.audio.empty() || !fs::is_regular_file(settings.audio)) throw Error("Не найден WAV-файл для субтитров.");
    if (settings.model.empty() || !fs::is_regular_file(settings.model)) throw Error("Не найдена локальная модель whisper.cpp.");
    if (settings.vadModel.empty() || !fs::is_regular_file(settings.vadModel)) throw Error("Не найдена локальная модель детектора речи (VAD); субтитры не созданы.");
    if (settings.outDir.empty()) throw Error("Не задан каталог результата.");
    if (cancelled.load()) throw Cancelled();
    checkRecognitionAudio(settings.audio, cancelled);
    fs::create_directories(settings.outDir);
    const auto sourceHash = settings.sourceSha256.empty() ? sha256File(settings.audio, &cancelled) : settings.sourceSha256;
    const auto modelHash = settings.modelSha256.empty() ? sha256File(settings.model, &cancelled) : settings.modelSha256;
    const auto recognitionKey = "-asr-vad-v2-" + sha256File(settings.vadModel, &cancelled).substr(0, 16);
    const auto styleKey = recognitionKey + "-v" + contract::version + "-studio-style-v4-z" + std::to_string(settings.zoneStartMs)
        + "-w" + std::to_string(settings.layout.maxWidth)
        + "-font" + std::to_string(std::hash<std::string>{}(settings.render.font))
        + "-f" + std::to_string(settings.render.fontSize)
        + "-bold" + std::to_string(settings.render.bold)
        + "-colors" + std::to_string(settings.render.textColor) + ":" + std::to_string(settings.render.outlineColor)
        + ":" + std::to_string(settings.render.backgroundColor)
        + "-o" + std::to_string(settings.render.outline) + "-s" + std::to_string(settings.render.shadow)
        + "-p" + std::to_string(settings.render.alignment) + "-b" + std::to_string(settings.render.background)
        + "-m" + std::to_string(settings.render.marginV) + "-a" + settings.render.animation
        + "-timing" + std::to_string(settings.layout.minDurationMs) + ":" + std::to_string(settings.layout.maxDurationMs)
        + ":" + std::to_string(settings.layout.maxReadingCps);
    const auto base = settings.outDir / (sourceHash.substr(0, 16) + "-" + modelHash.substr(0, 16) + styleKey);
    const auto wordsPath = base.string() + ".words.json";
    const auto blocksPath = base.string() + ".blocks.json";
    const auto assPath = base.string() + ".ass";
    if (fs::is_regular_file(wordsPath) && fs::is_regular_file(blocksPath) && fs::is_regular_file(assPath)) {
        if (progress) progress(100, "Субтитры найдены в локальном кэше.");
        return {wordsPath, blocksPath, assPath, sourceHash, modelHash, true};
    }
    const auto whisperBase = settings.outDir / (sourceHash.substr(0, 16) + "-" + modelHash.substr(0, 16) + recognitionKey + "-whisper");
    fs::path whisperJson = whisperBase;
    whisperJson += ".json";
    std::string json;
    if (fs::is_regular_file(whisperJson)) {
        std::ifstream input(whisperJson, std::ios::binary);
        json.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        const auto last = json.find_last_not_of(" \t\r\n");
        try {
            if (last == std::string::npos || json[last] != '}' || stringField(json, "token_timestamps") != "original"
                || json.find("\"vad_segments\"") == std::string::npos || parseWhisperJson(json).empty()) json.clear();
        } catch (const Error&) { json.clear(); }
    }
    if (json.empty()) {
        if (progress) progress(5, "Запуск локального распознавания речи.");
        const auto temporaryBase = whisperBase.string() + "-run-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        const fs::path temporaryJson = temporaryBase + ".json";
        try {
            const std::vector<std::string> args = {
                settings.whisperCli.string(), "-m", settings.model.string(), "-f", settings.audio.string(),
                "-l", "ru", "-ojf", "-of", temporaryBase, "--no-prints", "--print-progress",
                "--vad", "--vad-model", settings.vadModel.string()
            };
            std::string outputLine;
            int lastProgress = 0;
            const auto process = runProcess(args, settings.outDir, settings.timeoutSeconds, &cancelled,
                [&](std::string_view chunk) {
                    for (char character : chunk) {
                        if (character != '\n' && character != '\r') {
                            if (outputLine.size() < 512) outputLine.push_back(character);
                            continue;
                        }
                        const auto marker = outputLine.find("progress =");
                        if (marker != std::string::npos) {
                            const auto start = outputLine.find_first_of("0123456789", marker + 10);
                            const auto end = start == std::string::npos ? start : outputLine.find('%', start);
                            if (start != std::string::npos && end != std::string::npos) {
                                int percent = 0;
                                const auto parsed = std::from_chars(outputLine.data() + start, outputLine.data() + end, percent);
                                if (parsed.ec == std::errc{} && parsed.ptr == outputLine.data() + end
                                    && percent > lastProgress && percent <= 100) {
                                    lastProgress = percent;
                                    if (progress) progress(5 + percent * 60 / 100, "Распознавание речи: " + std::to_string(percent) + "%");
                                }
                            }
                        }
                        outputLine.clear();
                    }
                });
            if (process.exitCode != 0) throw Error("whisper.cpp завершился с кодом " + std::to_string(process.exitCode) + ".");
            if (!fs::is_regular_file(temporaryJson)) throw Error("whisper.cpp не создал JSON с временными метками.");
            std::ifstream input(temporaryJson, std::ios::binary);
            json.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
            if (stringField(json, "token_timestamps") != "original")
                throw Error("Версия whisper.cpp не сохраняет исходные временные метки; субтитры не созданы.");
            if (json.find("\"vad_segments\"") == std::string::npos)
                throw Error("Версия whisper.cpp не возвращает интервалы детектора речи; субтитры не созданы.");
            if (parseWhisperJson(json).empty()) throw Error("В клипе не найдена речь; субтитры не созданы.");
            atomicWrite(whisperJson, json);
        } catch (...) {
            std::error_code ignored;
            fs::remove(temporaryJson, ignored);
            throw;
        }
        std::error_code ignored;
        fs::remove(temporaryJson, ignored);
    } else if (progress) {
        progress(5, "Речь загружена из локального кэша.");
    }
    const auto words = parseWhisperJson(json, settings.zoneStartMs);
    if (words.empty()) throw Error("В речи не найдено ни одного слова.");
    if (progress) progress(70, "Разметка строк и подготовка ASS.");
    const auto blocks = layoutSpeechWords(words, json, settings.zoneStartMs, settings.layout);
    atomicWrite(wordsPath, renderWordsJson(words));
    atomicWrite(blocksPath, renderBlocksJson(blocks));
    atomicWrite(assPath, renderAss(blocks, settings.render));
    if (progress) progress(100, "Субтитры готовы.");
    return {wordsPath, blocksPath, assPath, sourceHash, modelHash, false};
}

} // namespace studio_subtitles
