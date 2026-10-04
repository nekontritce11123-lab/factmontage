#include "transition.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error("FAIL line " + std::to_string(__LINE__) + ": " #condition); } while (false)

int main()
{
    try {
        constexpr int width = 159;
        constexpr int height = 97;
        std::vector<std::uint8_t> first(width * height * 4);
        std::vector<std::uint8_t> second(first.size());
        std::vector<std::uint8_t> output(first.size());
        for (std::size_t i = 0; i < first.size(); i += 4) {
            first[i] = std::uint8_t(i % 251); first[i + 3] = 255;
            second[i + 1] = std::uint8_t((i * 3) % 253); second[i + 3] = 255;
        }
        sunimo::TransitionRenderer renderer;
        // A moving edge must reveal the other clip, not a reflected copy of A.
        std::vector<std::uint8_t> rampA(first.size()), rampB(first.size());
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
            const auto i = (y * width + x) * 4;
            rampA[i] = rampB[i + 2] = std::uint8_t(40 + 200 * x / (width - 1));
            rampA[i + 3] = rampB[i + 3] = 255;
        }
        for (int style : {2, 3}) {
            sunimo::TransitionConfig config;
            config.style = style;
            for (double progress : {.25, .5, .75}) {
                renderer.render(width, height, rampA.data(), rampB.data(), output.data(), progress, config);
                const auto left = (height / 2 * width) * 4;
                const auto right = (height / 2 * width + width - 1) * 4;
                CHECK(output[left] > 30 && output[left + 2] == 0);
                CHECK(output[right] == 0 && output[right + 2] > 30);
            }
            auto edge = rampA;
            for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
                edge[(y * width + x) * 4] = x == width - 1 ? 255 : 0;
            config.softness = 1;
            renderer.render(width, height, edge.data(), rampB.data(), output.data(), .5, config);
            CHECK(output[(height / 2 * width + width / 2 + 6) * 4] > 10);
        }
        for (int style = 0; style < sunimo::parameters::styleCount; ++style) {
            for (int direction = 0; direction < 4; ++direction) {
                sunimo::TransitionConfig config;
                config.style = style;
                config.direction = direction;
                renderer.render(width, height, first.data(), second.data(), output.data(), 0, config);
                CHECK(output == first);
                renderer.render(width, height, first.data(), second.data(), output.data(), 1, config);
                CHECK(output == second);
                renderer.render(width, height, first.data(), second.data(), output.data(), .5, config);
                const auto reference = output;
                auto otherB = second;
                for (std::size_t i = 0; i < otherB.size(); i += 4) otherB[i + 1] = 255 - otherB[i + 1];
                renderer.render(width, height, first.data(), otherB.data(), output.data(), .5, config);
                CHECK(output != reference);
                renderer.render(width, height, first.data(), second.data(), output.data(), .2, config);
                renderer.render(width, height, first.data(), second.data(), output.data(), .5, config);
                CHECK(output == reference);
                auto inPlace = first;
                renderer.render(width, height, inPlace.data(), second.data(), inPlace.data(), .5, config);
                CHECK(inPlace == reference);
            }
        }
        std::vector<std::uint8_t> transparent(64 * 48 * 4), solid(transparent.size()), blend(transparent.size());
        for (std::size_t i = 0; i < transparent.size(); i += 4) {
            transparent[i] = 255;
            solid[i + 2] = 255;
            solid[i + 3] = 255;
        }
        sunimo::TransitionConfig dissolve;
        dissolve.style = 7;
        renderer.render(64, 48, transparent.data(), solid.data(), blend.data(), .5, dissolve);
        for (std::size_t i = 0; i < blend.size(); i += 4) {
            CHECK(blend[i] == 0 && blend[i + 2] == 255);
            CHECK(blend[i + 3] >= 126 && blend[i + 3] <= 129);
        }
        std::cout << "PASS 15 styles x 4 directions: endpoints, seek, in-place and alpha render\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
