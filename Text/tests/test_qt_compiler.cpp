// SPDX-License-Identifier: MIT
#include "scenecompiler.hpp"
#include "styles.hpp"
#include "engine.hpp"
#include <QGuiApplication>
#include <QDir>
#include <QTemporaryDir>
#include <cassert>
#include <iostream>
int main(int argc,char **argv)
{
    QGuiApplication app(argc,argv);
    const QJsonObject settings{{"text",QString::fromUtf8("Привет, мир!\nБуквы АВ fi й е́")},{"size",28},{"duration",4},{"inPreset",2},{"lifeAmount",.4}};
    const auto bytes=SunimoTextQt::compile(settings,{640,360});
    const auto scene=sunimo::Scene::load(bytes.constData(),bytes.size());
    assert(scene->sprites.size()==1 && scene->rw==640 && scene->rh==360);
    assert(scene->metadata.find("native_editor_version")!=std::string::npos);
    assert(SunimoTextQt::compile(settings,{640,360})==bytes);
    QJsonObject moved = settings;
    moved.insert("inPreset", 8);
    moved.insert("inDuration", 1.2);
    moved.insert("lifeAmount", .9);
    moved.insert("lifeSpeed", 1.5);
    moved.insert("styleId", 7);
    const auto reused = SunimoTextQt::retime(bytes, moved);
    assert(reused == SunimoTextQt::compile(moved, {640,360}));
    assert(SunimoTextQt::rasterSettings(settings) == SunimoTextQt::rasterSettings(moved));
    sunimo::Renderer renderer;renderer.set(scene);
    std::vector<uint8_t> middle(640*360*4),again(middle.size()),early(middle.size());
    renderer.render(2.,640,360,nullptr,middle.data());
    renderer.render(.1,640,360,nullptr,early.data());
    renderer.render(2.,640,360,nullptr,again.data());
    assert(middle==again && middle!=early);
    int visible=0,transparent=0;
    for(size_t i=3;i<middle.size();i+=4){visible+=middle[i]>0;transparent+=middle[i]==0;}
    assert(visible>100 && transparent>1000);
    QTemporaryDir folder;assert(folder.isValid());
    QString error;const auto path=folder.filePath(QString::fromUtf8("Надпись с пробелом.stxt"));
    assert(SunimoTextQt::save(path,bytes,&error));
    assert(SunimoTextQt::metadata(path).value("native_editor_version").toInt()==1);
    assert(SunimoTextQt::metadata(path).value("text").toString()==settings.value("text").toString().normalized(QString::NormalizationForm_C));
    assert(SunimoTextQt::metadata(folder.filePath("missing.stxt")).isEmpty());
    QJsonObject outlined = settings;
    outlined.insert("color", "#ffff0000");
    outlined.insert("outlineEnabled", true);
    outlined.insert("outlineColor", "#ff00ff00");
    for (int width : {0, 1, 3, 10, 20}) {
        outlined.insert("outlineWidth", width);
        const auto outlinedBytes = SunimoTextQt::compile(outlined,{640,360});
        const auto outlinedScene = sunimo::Scene::load(outlinedBytes.constData(), outlinedBytes.size());
        const auto &sprite = outlinedScene->sprites[0];
        int red = 0, green = 0;
        for (size_t i = 0; i < sprite.pixels.size(); i += 4) {
            const auto &pixels = sprite.pixels;
            red += pixels[i] > 80 && pixels[i] > pixels[i+1] * 2 && pixels[i+3] > 80;
            green += pixels[i+1] > 80 && pixels[i+1] > pixels[i] * 2 && pixels[i+3] > 80;
        }
        assert(red > 100);
        assert(width == 0 ? green == 0 : green > 100);
        for (uint32_t x = 0; x < sprite.pw; ++x) {
            assert(sprite.pixels[x*4+3] == 0);
            assert(sprite.pixels[(size_t(sprite.ph-1)*sprite.pw+x)*4+3] == 0);
        }
    }
    outlined.insert("outlineWidth", 3);
    outlined.insert("color", "#80ffffff");
    outlined.insert("outlineColor", "#8000ff00");
    outlined.insert("backgroundEnabled", true);
    outlined.insert("backgroundColor", "#800000ff");
    const auto translucentBytes = SunimoTextQt::compile(outlined,{640,360});
    const auto translucentScene = sunimo::Scene::load(translucentBytes.constData(), translucentBytes.size());
    int translucentEdge = 0, translucentBox = 0;
    const auto &translucentPixels = translucentScene->sprites[0].pixels;
    for (size_t i = 0; i < translucentPixels.size(); i += 4) {
        translucentEdge += translucentPixels[i+1] > translucentPixels[i] * 2 && translucentPixels[i+3] > 20 && translucentPixels[i+3] < 200;
        translucentBox += translucentPixels[i+2] > translucentPixels[i] * 2 && translucentPixels[i+3] > 20 && translucentPixels[i+3] < 200;
    }
    assert(translucentEdge > 100 && translucentBox > 100);
    const auto restoredPath = folder.filePath(QString::fromUtf8("Настройки оформления.stxt"));
    assert(SunimoTextQt::save(restoredPath, translucentBytes, &error));
    assert(SunimoTextQt::compile(SunimoTextQt::metadata(restoredPath),{640,360}) == translucentBytes);
    auto rendered = [](const QJsonObject &value) {
        const auto bytes = SunimoTextQt::compile(value,{640,360});
        return sunimo::Scene::load(bytes.constData(), bytes.size())->sprites[0];
    };
    QJsonObject plain = settings;
    plain.insert("color", "#ffff0000");
    const auto plainSprite = rendered(plain);
    QJsonObject disabled = plain;
    disabled.insert("outlineEnabled", false);
    disabled.insert("outlineWidth", 20);
    disabled.insert("outlineColor", "#ff00ff00");
    assert(rendered(disabled).pixels == plainSprite.pixels);
    QJsonObject decorated = plain;
    decorated.insert("opacity", 50);
    const auto translucent = rendered(decorated);
    int peakAlpha = 0;
    for (size_t i = 3; i < translucent.pixels.size(); i += 4) peakAlpha = std::max(peakAlpha, int(translucent.pixels[i]));
    assert(peakAlpha >= 120 && peakAlpha <= 128);
    decorated = plain;
    decorated.insert("backgroundEnabled", true);
    decorated.insert("backgroundColor", "#ff0000ff");
    const auto boxed = rendered(decorated);
    int blue = 0;
    for (size_t i = 0; i < boxed.pixels.size(); i += 4)
        blue += boxed.pixels[i+2] > 80 && boxed.pixels[i+2] > boxed.pixels[i] * 2 && boxed.pixels[i+3] > 80;
    assert(blue > 100);
    for (uint32_t y = 0; y < boxed.ph; ++y) {
        assert(boxed.pixels[(size_t(y) * boxed.pw) * 4 + 3] == 0);
        assert(boxed.pixels[(size_t(y) * boxed.pw + boxed.pw - 1) * 4 + 3] == 0);
    }
    for (const auto &change : {QJsonObject{{"italic",true}}, QJsonObject{{"letterSpacing",4}},
                               QJsonObject{{"wordSpacing",12}}, QJsonObject{{"alignment",0}}}) {
        decorated = plain;
        for (auto it = change.begin(); it != change.end(); ++it) decorated.insert(it.key(), it.value());
        assert(rendered(decorated).pixels != plainSprite.pixels);
    }
    decorated = plain;
    decorated.insert("lineSpacing", 150);
    assert(rendered(decorated).ph > plainSprite.ph);
    decorated = plain;
    decorated.insert("shadow", 8);
    assert(rendered(decorated).pixels != plainSprite.pixels);
    QJsonObject clustered = settings;
    clustered.insert("text", QString::fromUtf8("АБ ВГ\nДЕ ЖЗ"));
    clustered.insert("group", 0);
    clustered.insert("inPreset", 0);
    clustered.insert("outPreset", 0);
    clustered.insert("lifeSource", 72);
    clustered.insert("lifeAmount", 1);
    clustered.insert("lifeSpeed", 1);
    clustered.insert("duration", 8);
    const auto clusteredBytes = SunimoTextQt::compile(clustered, {640, 360});
    const auto clusteredScene = sunimo::Scene::load(clusteredBytes.constData(), clusteredBytes.size());
    assert(clusteredScene->sprites.size() >= 8);
    assert(clusteredScene->c.group() == 0);
    assert(clusteredScene->sprites[0].word == clusteredScene->sprites[1].word);
    assert(clusteredScene->sprites[0].word != clusteredScene->sprites[2].word);
    assert(clusteredScene->sprites[0].line != clusteredScene->sprites[4].line);
    QJsonObject shapedText = clustered;
    shapedText.insert("text", QString::fromUtf8("а́ б"));
    shapedText.insert("lifeAmount", 0);
    const auto shapedData = SunimoTextQt::compile(shapedText, {640, 360});
    const auto shapedScene = sunimo::Scene::load(shapedData.constData(), shapedData.size());
    assert(shapedScene->sprites.size() == 2);
    QJsonObject blockText = shapedText;
    blockText.insert("group", 3);
    const auto blockData = SunimoTextQt::compile(blockText, {640, 360});
    sunimo::Renderer splitText, wholeText;
    splitText.set(shapedScene);
    wholeText.set(sunimo::Scene::load(blockData.constData(), blockData.size()));
    std::vector<uint8_t> splitFrame(640*360*4), wholeFrame(splitFrame.size());
    splitText.render(2, 640, 360, nullptr, splitFrame.data());
    wholeText.render(2, 640, 360, nullptr, wholeFrame.data());
    double sharedAlpha = 0, totalAlpha = 0;
    for (size_t i = 3; i < splitFrame.size(); i += 4) {
        sharedAlpha += std::min(splitFrame[i], wholeFrame[i]);
        totalAlpha += splitFrame[i] + wholeFrame[i];
    }
    assert(totalAlpha > 10000 && 2 * sharedAlpha / totalAlpha > .8);
    auto poseDifference = [](const sunimo::Pose &a, const sunimo::Pose &b) {
        return std::abs(a.x-b.x) + std::abs(a.y-b.y) + std::abs(a.rz-b.rz)
             + std::abs(a.sx-b.sx) + std::abs(a.sy-b.sy);
    };
    for (int mode = 0; mode < 4; ++mode) {
        clustered.insert("group", mode);
        const auto data = SunimoTextQt::compile(clustered, {640, 360});
        const auto shaped = sunimo::Scene::load(data.constData(), data.size());
        assert(shaped->c.group() == mode);
        assert(mode == 3 ? shaped->sprites.size() == 1 : shaped->sprites.size() >= 8);
        sunimo::Renderer motion; motion.set(shaped);
        std::vector<uint8_t> frame(640*360*4), repeat(frame.size()), later(frame.size());
        motion.render(2.3, 640, 360, nullptr, frame.data());
        motion.render(3.1, 640, 360, nullptr, later.data());
        motion.render(2.3, 640, 360, nullptr, repeat.data());
        assert(frame == repeat && frame != later);
        assert(std::count_if(frame.begin()+3, frame.end(), [](uint8_t value) { return value > 0; }) > 100);
        if (mode == 0) assert(poseDifference(motion.state(0, 2.3, 8).primary, motion.state(1, 2.3, 8).primary) > 1e-5);
        if (mode == 1) {
            assert(poseDifference(motion.state(0, 2.3, 8).primary, motion.state(1, 2.3, 8).primary) < 1e-9);
            assert(poseDifference(motion.state(0, 2.3, 8).primary, motion.state(2, 2.3, 8).primary) > 1e-5);
        }
        if (mode == 2) {
            assert(poseDifference(motion.state(0, 2.3, 8).primary, motion.state(1, 2.3, 8).primary) < 1e-9);
            assert(poseDifference(motion.state(0, 2.3, 8).primary, motion.state(4, 2.3, 8).primary) > 1e-5);
        }
    }
    clustered.insert("group", 0);
    clustered.insert("lifeAmount", 0);
    const auto stillData = SunimoTextQt::compile(clustered, {640, 360});
    sunimo::Renderer still; still.set(sunimo::Scene::load(stillData.constData(), stillData.size()));
    std::vector<uint8_t> stillA(640*360*4), stillB(stillA.size());
    still.render(2, 640, 360, nullptr, stillA.data());
    still.render(3, 640, 360, nullptr, stillB.data());
    assert(stillA == stillB);
    clustered.insert("lifeSpeed", 2);
    const auto stillFastData = SunimoTextQt::compile(clustered, {640, 360});
    still.set(sunimo::Scene::load(stillFastData.constData(), stillFastData.size()));
    still.render(2, 640, 360, nullptr, stillB.data());
    assert(stillA == stillB);
    clustered.insert("lifeAmount", 1);
    const auto movingFastData = SunimoTextQt::compile(clustered, {640, 360});
    still.set(sunimo::Scene::load(movingFastData.constData(), movingFastData.size()));
    still.render(2, 640, 360, nullptr, stillB.data());
    assert(stillA != stillB);
    for (int life : {21, 72}) {
        QJsonObject strong{{"text", QString::fromUtf8("И")}, {"size", 120}, {"group", 0},
                           {"inPreset", 0}, {"outPreset", 0}, {"lifeSource", life},
                           {"lifeAmount", 1}, {"duration", 8}};
        const auto data = SunimoTextQt::compile(strong, {1920, 1080});
        sunimo::Renderer motion;
        motion.set(sunimo::Scene::load(data.constData(), data.size()));
        double top = 1e9, bottom = -1e9, topTime = 0, bottomTime = 0;
        for (int frame = 60; frame < 420; ++frame) {
            const double t = frame / 60.;
            const auto state = motion.state(0, t, 8);
            const auto point = sunimo::project(motion.matrix(0, state, 480, 270),
                                                motion.scene->sprites[0].pw / 2., motion.scene->sprites[0].ph / 2.);
            if (point[1] < top) { top = point[1]; topTime = t; }
            if (point[1] > bottom) { bottom = point[1]; bottomTime = t; }
        }
        auto centroid = [&](double t) {
            std::vector<uint8_t> frame(480 * 270 * 4);
            motion.render(t, 480, 270, nullptr, frame.data());
            double mass = 0, weighted = 0;
            for (int y = 0; y < 270; ++y) for (int x = 0; x < 480; ++x) {
                const double alpha = frame[(size_t(y) * 480 + x) * 4 + 3];
                mass += alpha; weighted += y * alpha;
            }
            assert(mass > 100);
            return weighted / mass;
        };
        assert(std::abs(centroid(topTime) - centroid(bottomTime)) >= 2);
    }
    QJsonObject tooMany = clustered;
    tooMany.insert("text", QString(520, QLatin1Char('i')));
    tooMany.insert("size", 12);
    bool rejectedClusters = false;
    try { SunimoTextQt::compile(tooMany, {4096, 4096}); }
    catch (const std::exception &e) { rejectedClusters = QByteArray(e.what()).contains("512"); }
    assert(rejectedClusters);
    assert(SunimoTextQt::styles.size() == 18);
    std::vector<QString> styleNames;
    for (const auto &style : SunimoTextQt::styles) {
        const QString name = QString::fromUtf8(style.name);
        assert(!name.isEmpty() && std::find(styleNames.begin(), styleNames.end(), name) == styleNames.end());
        styleNames.push_back(name);
        auto values = SunimoTextQt::applyStyle(QJsonObject{{"text", QString::fromUtf8("Тест два слова")},
                                                           {"size", 48}, {"duration", 4}}, style);
        const auto data = SunimoTextQt::compile(values, {640, 360});
        const QString demos = qEnvironmentVariable("SUNIMO_TEXT_DEMOS");
        if (!demos.isEmpty()) {
            assert(QDir().mkpath(demos));
            QString error;
            assert(SunimoTextQt::save(QDir(demos).filePath(QStringLiteral("%1-%2.stxt")
                       .arg(style.id, 2, 10, QLatin1Char('0')).arg(name)), data, &error));
        }
        sunimo::Renderer motion;
        motion.set(sunimo::Scene::load(data.constData(), data.size()));
        std::vector<uint8_t> entrance(640*360*4), middle(entrance.size()), middleLater(entrance.size()), exit(entrance.size());
        motion.render(.2, 640, 360, nullptr, entrance.data());
        motion.render(2, 640, 360, nullptr, middle.data());
        motion.render(2.5, 640, 360, nullptr, middleLater.data());
        motion.render(3.8, 640, 360, nullptr, exit.data());
        assert(entrance != middle && exit != middle);
        assert(middle != middleLater);
        for (double edge : {style.inDuration, 4 - style.outDuration}) {
            for (size_t i = 0; i < motion.scene->sprites.size(); ++i) {
                const auto before = motion.state(i, edge - 1e-5, 4);
                const auto after = motion.state(i, edge + 1e-5, 4);
                const auto &sprite = motion.scene->sprites[i];
                const auto p = sunimo::project(motion.matrix(i, before, 640, 360), sprite.pw / 2., sprite.ph / 2.);
                const auto q = sunimo::project(motion.matrix(i, after, 640, 360), sprite.pw / 2., sprite.ph / 2.);
                assert(std::hypot(p[0]-q[0], p[1]-q[1]) < .1);
                assert(std::abs(before.primary.alpha-after.primary.alpha) < .01);
            }
        }
        values.insert("outPreset", 54);
        const auto differentExit = SunimoTextQt::compile(values, {640, 360});
        motion.set(sunimo::Scene::load(differentExit.constData(), differentExit.size()));
        std::vector<uint8_t> changedMiddle(middle.size());
        motion.render(2, 640, 360, nullptr, changedMiddle.data());
        assert(changedMiddle == middle);
    }
    auto onlyMotion = SunimoTextQt::applyStyle(QJsonObject{{"text", "Только движение"}, {"duration", 4}},
                                               SunimoTextQt::styles[6]);
    onlyMotion.insert("inPreset", 0);
    onlyMotion.insert("outPreset", 0);
    const auto onlyData = SunimoTextQt::compile(onlyMotion, {640, 360});
    sunimo::Renderer onlyRenderer;
    onlyRenderer.set(sunimo::Scene::load(onlyData.constData(), onlyData.size()));
    std::vector<uint8_t> onlyA(640*360*4), onlyB(onlyA.size());
    onlyRenderer.render(1.5, 640, 360, nullptr, onlyA.data());
    onlyRenderer.render(2.5, 640, 360, nullptr, onlyB.data());
    assert(onlyA != onlyB);
    auto shortStyle = SunimoTextQt::applyStyle(QJsonObject{{"text", "Короткий клип"}, {"duration", .2}},
                                               SunimoTextQt::styles[3]);
    const auto shortData = SunimoTextQt::compile(shortStyle, {640, 360});
    onlyRenderer.set(sunimo::Scene::load(shortData.constData(), shortData.size()));
    onlyRenderer.render(.1, 640, 360, nullptr, onlyA.data());
    assert(std::any_of(onlyA.begin(), onlyA.end(), [](uint8_t pixel) { return pixel != 0; }));
    for(const auto &invalid : {QJsonObject{{"text",""}},QJsonObject{{"text","x"},{"size",10000}},QJsonObject{{"text","x"},{"inPreset",101}}}){
        bool rejected=false;try{SunimoTextQt::compile(invalid,{640,360});}catch(const std::exception&){rejected=true;}assert(rejected);
    }
    std::cout << "Qt shaping -> STXT -> production renderer, deterministic seek, UTF-8 assets: PASS\n";
}
