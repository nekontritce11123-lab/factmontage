// SPDX-License-Identifier: MIT
#include "subtitle.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <csignal>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <set>

namespace {
std::atomic_bool cancelled{false};
void onSignal(int) { cancelled.store(true); }

std::string required(const std::map<std::string, std::string>& args, const char* key) {
    const auto it = args.find(key);
    if (it == args.end() || it->second.empty()) throw studio_subtitles::Error(std::string("Не задан параметр --") + key + ".");
    return it->second;
}

std::int64_t integer(const std::map<std::string, std::string>& args, const char* key, std::int64_t fallback) {
    const auto it = args.find(key);
    if (it == args.end()) return fallback;
    std::size_t position = 0;
    const auto result = std::stoll(it->second, &position);
    if (position != it->second.size()) throw studio_subtitles::Error("Неверное целое: " + it->second);
    return result;
}

std::uint32_t assColor(const std::string& value) {
    if (value.size() != 7 && value.size() != 9) throw studio_subtitles::Error("Цвет должен иметь вид #RRGGBB или #AARRGGBB.");
    if (value.front() != '#' || !std::all_of(value.begin() + 1, value.end(),
                                            [](unsigned char c) { return std::isxdigit(c) != 0; }))
        throw studio_subtitles::Error("Неверное значение цвета.");
    const auto number = std::stoul(value.substr(1), nullptr, 16);
    const std::uint32_t alpha = value.size() == 9 ? 255 - ((number >> 24) & 255) : 0;
    const std::uint32_t red = (number >> 16) & 255, green = (number >> 8) & 255, blue = number & 255;
    return (alpha << 24) | (blue << 16) | (green << 8) | red;
}

} // namespace

int main(int argc, char** argv) {
    using namespace studio_subtitles;
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    try {
        std::map<std::string, std::string> args;
        const std::set<std::string> valid = {
            "audio", "model", "vad-model", "whisper-cli", "out-dir", "source-sha256", "model-sha256",
            "zone-start-ms", "zone-end-ms", "timeout", "max-width", "font", "font-size",
            "outline", "shadow", "animation", "position", "background", "safe-margin", "bold",
            "text-color", "outline-color", "background-color", "min-duration", "max-duration", "max-reading-cps", "preview-text", "render-blocks-json", "help"
        };
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument == "--help") {
                std::cout << "StudioSubtitles 0.1 — локальный worker русских субтитров.\n"
                             "Обязательно для распознавания: --audio WAV --model MODEL --vad-model MODEL --out-dir DIRECTORY\n"
                             "Переоформление без распознавания: --render-blocks-json FILE\n"
                             "Опции: --whisper-cli PATH --zone-start-ms N --zone-end-ms N --timeout SEC\n"
                             "       --max-width PERCENT --font NAME --font-size PX\n";
                return 0;
            }
            if (argument.rfind("--", 0) != 0 || i + 1 >= argc) throw Error("Неизвестный или неполный аргумент: " + argument);
            const auto key = argument.substr(2);
            if (!valid.count(key)) throw Error("Неизвестный аргумент: " + argument);
            if (args.count(key)) throw Error("Повторный аргумент: " + argument);
            args[key] = argv[++i];
        }
        WorkerSettings settings;
        if (args.count("vad-model")) settings.vadModel = fs::u8path(args.at("vad-model"));
        if (args.count("whisper-cli")) settings.whisperCli = fs::u8path(args.at("whisper-cli"));
        if (args.count("source-sha256")) settings.sourceSha256 = args.at("source-sha256");
        if (args.count("model-sha256")) settings.modelSha256 = args.at("model-sha256");
        settings.zoneStartMs = integer(args, "zone-start-ms", 0);
        settings.zoneEndMs = integer(args, "zone-end-ms", 0);
        settings.timeoutSeconds = static_cast<int>(integer(args, "timeout", settings.timeoutSeconds));
        const auto widthPercent = integer(args, "max-width", 60);
        if (args.count("font")) settings.render.font = args.at("font");
        if (settings.render.font.empty() || settings.render.font.find_first_of(",\r\n") != std::string::npos)
            throw Error("Неверное название шрифта.");
        settings.render.fontSize = static_cast<int>(integer(args, "font-size", settings.render.fontSize));
        if (widthPercent < 45 || widthPercent > 85 || settings.render.fontSize < 48 || settings.render.fontSize > 72)
            throw Error("Длина строки должна быть 45–85 %, размер шрифта 48–72 px.");
        // ASS uses a 1920-pixel design canvas. The width control is a percentage, not a character count.
        settings.layout.maxWidth = 1920.0 * static_cast<double>(widthPercent) / (100.0 * settings.render.fontSize * .65);
        settings.render.outline = static_cast<int>(integer(args, "outline", settings.render.outline));
        settings.render.shadow = static_cast<int>(integer(args, "shadow", settings.render.shadow));
        if (args.count("animation")) settings.render.animation = args.at("animation");
        if (settings.render.animation != "none" && settings.render.animation != "words"
            && settings.render.animation != "letters" && settings.render.animation != "pop"
            && settings.render.animation != "fade" && settings.render.animation != "rise"
            && settings.render.animation != "slide" && settings.render.animation != "reveal_words"
            && settings.render.animation != "accent")
            throw Error("Неизвестная анимация субтитров.");
        const auto bold = integer(args, "bold", 1);
        if (bold != 0 && bold != 1) throw Error("Неверная настройка жирного шрифта.");
        settings.render.bold = bold != 0;
        if (args.count("text-color")) settings.render.textColor = assColor(args.at("text-color"));
        if (args.count("outline-color")) settings.render.outlineColor = assColor(args.at("outline-color"));
        if (args.count("background-color")) settings.render.backgroundColor = assColor(args.at("background-color"));
        const int position = static_cast<int>(integer(args, "position", 0));
        if (position < 0 || position > 2) throw Error("Неверное положение субтитров.");
        settings.render.alignment = position == 1 ? 5 : position == 2 ? 8 : 2;
        settings.render.background = static_cast<int>(integer(args, "background", settings.render.background));
        if (settings.render.background < 0 || settings.render.background > 2
            || settings.render.outline < 0 || settings.render.outline > 20
            || settings.render.shadow < 0 || settings.render.shadow > 20)
            throw Error("Неверное оформление субтитров.");
        settings.render.marginV = static_cast<int>(integer(args, "safe-margin", 8)) * 11;
        if (settings.render.marginV < 0 || settings.render.marginV > 275) throw Error("Неверное безопасное поле.");
        settings.layout.minDurationMs = integer(args, "min-duration", settings.layout.minDurationMs);
        settings.layout.maxDurationMs = integer(args, "max-duration", settings.layout.maxDurationMs);
        settings.layout.maxReadingCps = integer(args, "max-reading-cps", 24);
        if (settings.layout.minDurationMs < 300 || settings.layout.minDurationMs > 1200
            || settings.layout.maxDurationMs < 1500 || settings.layout.maxDurationMs > 5000
            || settings.layout.minDurationMs >= settings.layout.maxDurationMs
            || settings.layout.maxReadingCps < 12 || settings.layout.maxReadingCps > 35)
            throw Error("Неверные параметры разбиения речи.");
        if (args.count("preview-text")) {
            const auto& text = args.at("preview-text");
            if (text.empty() || text.size() > 8192) throw Error("Текст предпросмотра должен содержать от 1 до 8192 байт.");
            std::istringstream input(text);
            std::vector<std::string> parts;
            for (std::string word; input >> word;) parts.push_back(word);
            if (parts.empty()) throw Error("Текст предпросмотра пуст.");
            Block block;
            block.startMs = 0;
            block.endMs = 4000;
            for (std::size_t i = 0; i < parts.size(); ++i)
                block.words.push_back({static_cast<std::int64_t>(4000 * i / parts.size()),
                                       static_cast<std::int64_t>(4000 * (i + 1) / parts.size()), parts[i], 1.0});
            std::cout << renderAss({block}, settings.render);
            return 0;
        }
        if (args.count("render-blocks-json")) {
            std::ifstream input(fs::u8path(required(args, "render-blocks-json")), std::ios::binary);
            if (!input) throw Error("Не удалось открыть блоки для оформления.");
            const std::string json(std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{});
            std::cout << renderAss(parseRenderBlocksJson(json), settings.render);
            return 0;
        }
        settings.audio = fs::u8path(required(args, "audio"));
        settings.model = fs::u8path(required(args, "model"));
        settings.outDir = fs::u8path(required(args, "out-dir"));
        auto progress = [](int value, const std::string& text) {
            std::cerr << "PROGRESS\t" << value << '\t' << text << '\n';
        };
        const auto result = runWorker(settings, cancelled, progress);
        std::cout << "{\"words\":\"" << result.wordsJson.string() << "\",\"blocks\":\""
                  << result.blocksJson.string() << "\",\"ass\":\"" << result.ass.string()
                  << "\",\"cache_hit\":" << (result.cacheHit ? "true" : "false") << "}\n";
        return 0;
    } catch (const Cancelled& error) {
        std::cerr << "CANCELLED\t" << error.what() << '\n';
        return 130;
    } catch (const std::exception& error) {
        std::cerr << "ERROR\t" << error.what() << '\n';
        return 1;
    }
}
