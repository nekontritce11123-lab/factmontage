// SPDX-License-Identifier: MIT
#include "subtitle.hpp"

#include <cassert>
#include <iostream>

int main() {
    using namespace studio_subtitles;
    const std::string json = R"({"transcription":[{"timestamps":{"from":"00:00:01,000","to":"00:00:03,000"},"text":" Привет мир","tokens":[{"text":" Привет","offsets":{"from":1000,"to":1800},"p":0.98},{"text":" мир","offsets":{"from":1800,"to":2500},"p":0.97}]}]})";
    const auto words = parseWhisperJson(json);
    assert(words.size() == 2);
    assert(words[0].text == "Привет");
    assert(words[0].startMs == 1000 && words[1].endMs == 2500);
    const auto fallback = parseWhisperJson(R"({"transcription":[{"timestamps":{"from":"00:00:02,000","to":"00:00:03,000"},"text":" один два"}]})");
    assert(fallback.size() == 2);
    assert(fallback[0].startMs == 2000 && fallback[1].endMs == 3000);
    const auto realTokens = parseWhisperJson(R"({"transcription":[{"timestamps":{"from":"00:00:00,000","to":"00:00:03,000"},"text":" Кстати, если вам интересен контент","tokens":[{"text":" Кстати","offsets":{"from":200,"to":520}},{"text":",","offsets":{"from":620,"to":730}},{"text":" если","offsets":{"from":730,"to":1100}},{"text":" вам","offsets":{"from":1100,"to":1210}},{"text":" интерес","offsets":{"from":1580,"to":2020}},{"text":"ен","offsets":{"from":2020,"to":2200}},{"text":" конт","offsets":{"from":2470,"to":2770}},{"text":"ент","offsets":{"from":2860,"to":3080}}]}]})");
    assert(realTokens.size() == 5);
    assert(realTokens[0].text == "Кстати," && realTokens[3].text == "интересен" && realTokens[4].text == "контент");
    const std::string vadJson = R"({"vad_segments":[{"offsets":{"from":3000,"to":6000}},{"offsets":{"from":11000,"to":15000}}],"transcription":[{"offsets":{"from":3000,"to":15000},"tokens":[{"text":" Привет","offsets":{"from":5500,"to":5900}},{"text":" я","offsets":{"from":5900,"to":11300}},{"text":" шум","offsets":{"from":7000,"to":7500}},{"text":" здесь","offsets":{"from":11300,"to":12000}}]}]})";
    const auto vadWords = parseWhisperJson(vadJson);
    assert(vadWords.size() == 3 && vadWords[1].text == "я");
    assert(vadWords[1].startMs == 11000 && vadWords[1].endMs == 11300);
    const auto shiftedVadWords = parseWhisperJson(vadJson, 2000);
    assert(shiftedVadWords[0].startMs == 7500 && shiftedVadWords[1].startMs == 13000);
    bool invalidVadRejected = false;
    try { parseWhisperJson(R"({"vad_segments":[{"offsets":{"from":3000,"to":2000}}],"transcription":[]})"); }
    catch (const Error&) { invalidVadRejected = true; }
    assert(invalidVadRejected);
    const auto realBlocks = layoutWords(realTokens);
    assert(realBlocks.size() < realTokens.size());
    for (std::size_t i = 1; i < realBlocks.size(); ++i) assert(realBlocks[i - 1].endMs <= realBlocks[i].startMs);
    const auto sentenceBlocks = layoutWords({{1000, 1300, "Первое.", 1.0}, {1600, 1900, "Второе", 1.0}});
    assert(sentenceBlocks.size() == 2 && sentenceBlocks[1].startMs == 1600);
    assert(sentenceBlocks[0].endMs == 1600 && sentenceBlocks[1].endMs == 2250);
    const auto overlapping = layoutWords({{1000, 2000, "Первое.", 1.0}, {1500, 2500, "Второе", 1.0}});
    assert(overlapping.size() == 2);
    assert(overlapping[0].speechEndMs == 2000 && overlapping[0].endMs == 2000);
    assert(overlapping[0].endMs <= overlapping[1].startMs);
    assert(overlapping[0].timingConflict && overlapping[1].timingConflict);
    assert(renderBlocksJson(overlapping).find("\"speech_end_ms\":2000,\"timing_conflict\":true") != std::string::npos);
    const auto overlapAss = renderAss(overlapping);
    assert(overlapAss.find("Dialogue: 0,0:00:01.00,0:00:02.00") != std::string::npos);
    assert(overlapAss.find("Dialogue: 0,0:00:02.00,") != std::string::npos);
    assert(overlapAss.find("\\N") == std::string::npos);
    RenderOptions overlapHighlight;
    overlapHighlight.animation = "words";
    assert(renderAss(overlapping, overlapHighlight).find("{\\k75}Второе") != std::string::npos);
    const auto simultaneous = layoutWords({{6340, 6341, "Там", 1.0}, {6340, 6341, "я", 1.0},
                                           {6450, 6970, "делюсь", 1.0}, {6970, 7560, "идеями", 1.0}});
    assert(!simultaneous.empty());
    assert(simultaneous[0].line1.find("Там я") != std::string::npos);
    for (const auto& block : simultaneous) assert(block.endMs > block.startMs);
    LayoutOptions balanced;
    balanced.maxWidth = 28;
    const auto balancedBlocks = layoutWords({{8350, 9900, "брилочков,", 1.0}, {9900, 10400, "какими-то", 1.0},
                                              {10400, 10970, "интересными", 1.0}, {11030, 11550, "моментами", 1.0}}, balanced);
    assert(balancedBlocks.size() == 2);
    assert(balancedBlocks[0].line1 == "брилочков, какими-то");
    assert(balancedBlocks[1].line1 == "интересными моментами");

    LayoutOptions options;
    options.maxWidth = 8;
    options.measure = [](std::string_view text) { return defaultMeasure(text); };
    auto blocks = layoutWords(words, options);
    assert(blocks.size() == 2);
    assert(blocks[0].line1 == "Привет");
    assert(blocks[1].line1 == "мир");
    assert(blocks[0].id != 0);
    assert(mergeBlocks(blocks, 0, LayoutOptions{}));
    assert(blocks.size() == 1);
    assert(splitBlock(blocks, 0, 1, options));
    assert(blocks.size() == 2);
    const auto firstId = blocks[0].id;
    assert(firstId != 0 && blocks[1].id != 0 && firstId != blocks[1].id);
    assert(mergeBlocks(blocks, 0, LayoutOptions{}));
    assert(blocks.size() == 1 && blocks[0].id == firstId);
    assert(blocks[0].endMs >= 1650);

    const auto ass = renderAss(blocks);
    assert(ass.find("[Events]") != std::string::npos);
    assert(ass.find("Style: Studio-") != std::string::npos);
    assert(ass.find("Style: Default,") == std::string::npos);
    const auto styleStart = ass.find("Style: ") + 7;
    const auto styleEnd = ass.find(',', styleStart);
    const auto styleName = ass.substr(styleStart, styleEnd - styleStart);
    assert(ass.find("," + styleName + ",,0,0,0,SUNIMO1:") != std::string::npos);
    assert(ass.find("Style: " + styleName + "-Box,") != std::string::npos);
    assert(ass.find("Dialogue: 0,0:00:01.00,") != std::string::npos);
    assert(ass.find("Dialogue: 1,0:00:01.00,") != std::string::npos);
    RenderOptions noBox;
    noBox.background = 0;
    assert(renderAss(blocks, noBox).find("-Box,") == std::string::npos);
    RenderOptions alternateStyle;
    alternateStyle.fontSize += 1;
    const auto alternateAss = renderAss(blocks, alternateStyle);
    assert(alternateAss.find("Style: " + styleName + ",") == std::string::npos);
    assert(ass.find("Привет") != std::string::npos);
    assert(ass.find("\\N") == std::string::npos);
    assert(renderAss(layoutWords({{1000, 1600, "строка\\Nдве", 1.0}})).find("\\N") == std::string::npos);
    assert(ass.find("\\k") == std::string::npos);
    RenderOptions highlighted;
    highlighted.animation = "words";
    const auto highlightedAss = renderAss(blocks, highlighted);
    assert(highlightedAss.find("\\k") != std::string::npos);
    const auto gapAss = renderAss(layoutWords({{1000, 1300, "Один", 1.0}, {1600, 1900, "два", 1.0}}), highlighted);
    assert(gapAss.find("{\\k60}Один") != std::string::npos);
    RenderOptions letters;
    letters.animation = "letters";
    const auto letterAss = renderAss(blocks, letters);
    assert(letterAss.find("{\\1a&HFF&\\3a&HFF&\\4a&HFF&\\t(") != std::string::npos);
    assert(letterAss.find("{\\k") == std::string::npos);
    RenderOptions pop;
    pop.animation = "pop";
    assert(renderAss(blocks, pop).find("\\fscx108\\fscy108\\t(0,120,\\fscx100\\fscy100)") != std::string::npos);

    const auto timed = layoutWords({{1000, 1300, "Первое", 1.0}, {1600, 1900, "второе", 1.0}});
    assert(timed.size() == 1);
    RenderOptions byWord;
    byWord.animation = "reveal_words";
    const auto timedAss = renderAss(timed, byWord);
    assert(timedAss.find("{\\1a&HFF&\\3a&HFF&\\4a&HFF&\\t(600,601,\\1a&H00&\\3a&H00&\\4a&H66&)}второе") != std::string::npos);
    assert(timedAss.find(",SUNIMO1:") != std::string::npos);
    assert(timedAss.find(":1000-1300;1600-1900,") != std::string::npos);
    byWord.textColor = 0x7FFFFFFF;
    byWord.outlineColor = 0xBFFFFFFF;
    assert(renderAss(timed, byWord).find("\\t(600,601,\\1a&H7F&\\3a&HBF&\\4a&H66&)") != std::string::npos);
    RenderOptions fade;
    fade.animation = "fade";
    assert(renderAss(timed, fade).find("\\fad(160,0)") != std::string::npos);
    RenderOptions rise;
    rise.animation = "rise";
    assert(renderAss(timed, rise).find("\\move(960,1060,960,1020,0,180)") != std::string::npos);
    RenderOptions slide;
    slide.animation = "slide";
    assert(renderAss(timed, slide).find("\\move(840,1020,960,1020,0,200)") != std::string::npos);
    RenderOptions accent;
    accent.animation = "accent";
    assert(renderAss(timed, accent).find("\\fscx105\\fscy105\\t(0,150,\\fscx100\\fscy100)") != std::string::npos);

    const auto saved = parseRenderBlocksJson(R"({"schema":3,"blocks":[{"id":"42","start_ms":1000,"end_ms":2200,"text":"Первое второе","words":[{"start_ms":1000,"end_ms":1300,"text":"Первое"},{"start_ms":1600,"end_ms":1900,"text":"второе"}]}]})");
    assert(saved.size() == 1 && saved[0].id == 42 && saved[0].timedWords);
    assert(renderAss(saved, byWord).find("\\t(600,601,") != std::string::npos);
    const auto shortWithPause = parseRenderBlocksJson(R"({"schema":3,"blocks":[{"id":"43","start_ms":1000,"end_ms":3000,"text":"AB Да","words":[{"start_ms":1000,"end_ms":1080,"text":"AB"},{"start_ms":1600,"end_ms":1680,"text":"Да"}]}]})");
    const auto typedPauseAss = renderAss(shortWithPause, letters);
    assert(typedPauseAss.find("\\t(40,41,\\1a&H00&\\3a&H00&\\4a&H66&)}B") != std::string::npos);
    assert(typedPauseAss.find("\\t(600,601,\\1a&H00&\\3a&H00&\\4a&H66&)}Д") != std::string::npos);
    assert(typedPauseAss.find("\\t(640,641,\\1a&H00&\\3a&H00&\\4a&H66&)}а") != std::string::npos);
    const auto corrected = parseRenderBlocksJson(R"({"schema":3,"blocks":[{"id":"42","start_ms":1000,"end_ms":2200,"text":"Исправлено вручную","words":[]}]})");
    assert(corrected.size() == 1 && !corrected[0].timedWords);
    const auto correctedAss = renderAss(corrected, byWord);
    assert(correctedAss.find("Исправлено вручную") != std::string::npos);
    assert(correctedAss.find("\\fad(160,0)") != std::string::npos);
    assert(correctedAss.find(":,") != std::string::npos);
    assert(correctedAss.find("\\t(600,601,") == std::string::npos);
    bool rejected = false;
    try { parseRenderBlocksJson(R"({"schema":3,"blocks":[{"id":"42","start_ms":1000,"end_ms":2200,"text":"Другой текст","words":[{"start_ms":1000,"end_ms":1300,"text":"Первое"}]}]})"); }
    catch (const Error&) { rejected = true; }
    assert(rejected);

    std::cout << "StudioSubtitles unit: PASS\n";
    return 0;
}
