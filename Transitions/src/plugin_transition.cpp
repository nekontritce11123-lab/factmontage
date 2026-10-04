// SPDX-License-Identifier: MIT
#if defined(TRANSITIONS_SYSTEM_FREI0R)
extern "C" {
#include <frei0r.h>
}
#else
#include "frei0r_minimal.h"
#endif

#include "generated_parameters.hpp"
#include "transition.hpp"
#include <cmath>
#include <cstring>

namespace tr = sunimo;

struct Mixer
{
    int width;
    int height;
    double progress = 0;
    tr::TransitionConfig config;
    tr::TransitionRenderer renderer;
    Mixer(int w, int h) : width(w), height(h) {}
};

#if defined(_WIN32)
#define F0R_EXPORT extern "C" __declspec(dllexport)
#else
#define F0R_EXPORT extern "C" __attribute__((visibility("default")))
#endif

F0R_EXPORT int f0r_init() { return 1; }
F0R_EXPORT void f0r_deinit() {}

F0R_EXPORT void f0r_get_plugin_info(f0r_plugin_info_t *info)
{
    if (info) {
        *info = {"FactMontage Transitions", "SUNIMO Motion contributors", F0R_PLUGIN_TYPE_MIXER2,
                 F0R_COLOR_MODEL_RGBA8888, FREI0R_MAJOR_VERSION, 1, 0, 7,
                 "Fifteen bounded SDR/RGBA8888 CPU transitions."};
    }
}

F0R_EXPORT void f0r_get_param_info(f0r_param_info_t *info, int index)
{
    static const char *names[] = {"Progress", "Style", "Strength", "Direction", "Softness", "Center", "Quality"};
    static const char *help[] = {"Animated 0 to 1 by host", "Style code from generated_parameters.hpp", "Motion strength 0..1",
                                 "Direction 0..3 divided by 3", "Feather or defocus amount", "Normalized center",
                                 "0 Lite, 1 extra blur"};
    if (!info) return;
    if (index < 0 || index >= tr::parameters::parameterCount) {
        *info = {"", F0R_PARAM_DOUBLE, ""};
        return;
    }
    *info = {names[index], index == tr::parameters::center ? F0R_PARAM_POSITION : F0R_PARAM_DOUBLE, help[index]};
}

F0R_EXPORT f0r_instance_t f0r_construct(unsigned width, unsigned height)
{
    if (width < 2 || height < 2 || width > 8192 || height > 8192 || std::size_t(width) * height > 33554432) return nullptr;
    try {
        return new Mixer(int(width), int(height));
    } catch (...) {
        return nullptr;
    }
}

F0R_EXPORT void f0r_destruct(f0r_instance_t instance) { delete static_cast<Mixer *>(instance); }

F0R_EXPORT void f0r_set_param_value(f0r_instance_t instance, f0r_param_t parameter, int index)
{
    if (!instance || !parameter) return;
    auto *mixer = static_cast<Mixer *>(instance);
    if (index == tr::parameters::center) {
        const auto *value = static_cast<f0r_param_position_t *>(parameter);
        mixer->config.cx = tr::clamp(value->x);
        mixer->config.cy = tr::clamp(value->y);
        return;
    }
    const double value = tr::clamp(*static_cast<double *>(parameter));
    switch (index) {
    case tr::parameters::progress: mixer->progress = value; break;
    case tr::parameters::style: {
        int nearest = 0;
        for (int i = 1; i < tr::parameters::styleCount; ++i)
            if (std::abs(value - tr::parameters::styleCodes[i]) < std::abs(value - tr::parameters::styleCodes[nearest])) nearest = i;
        mixer->config.style = nearest;
        break;
    }
    case tr::parameters::strength: mixer->config.strength = value; break;
    case tr::parameters::direction: mixer->config.direction = int(std::round(value * (tr::parameters::directionCount - 1))); break;
    case tr::parameters::softness: mixer->config.softness = value; break;
    case tr::parameters::quality: mixer->config.quality = value >= .5; break;
    default: break;
    }
}

F0R_EXPORT void f0r_get_param_value(f0r_instance_t instance, f0r_param_t parameter, int index)
{
    if (!instance || !parameter) return;
    auto *mixer = static_cast<Mixer *>(instance);
    if (index == tr::parameters::center) {
        *static_cast<f0r_param_position_t *>(parameter) = {mixer->config.cx, mixer->config.cy};
        return;
    }
    double value = 0;
    switch (index) {
    case tr::parameters::progress: value = mixer->progress; break;
    case tr::parameters::style: value = tr::parameters::styleCodes[std::clamp(mixer->config.style, 0, tr::parameters::styleCount - 1)]; break;
    case tr::parameters::strength: value = mixer->config.strength; break;
    case tr::parameters::direction: value = mixer->config.direction / double(tr::parameters::directionCount - 1); break;
    case tr::parameters::softness: value = mixer->config.softness; break;
    case tr::parameters::quality: value = mixer->config.quality; break;
    default: break;
    }
    *static_cast<double *>(parameter) = value;
}

F0R_EXPORT void f0r_update2(f0r_instance_t instance, double, const std::uint32_t *first,
                            const std::uint32_t *second, const std::uint32_t *, std::uint32_t *output)
{
    if (!instance || !output) return;
    auto *mixer = static_cast<Mixer *>(instance);
    try {
        mixer->renderer.render(mixer->width, mixer->height, reinterpret_cast<const std::uint8_t *>(first),
                               reinterpret_cast<const std::uint8_t *>(second), reinterpret_cast<std::uint8_t *>(output),
                               mixer->progress, mixer->config);
    } catch (...) {
        const auto bytes = std::size_t(mixer->width) * mixer->height * 4;
        if (first) std::memmove(output, first, bytes); else std::memset(output, 0, bytes);
    }
}

F0R_EXPORT void f0r_update(f0r_instance_t instance, double time, const std::uint32_t *input, std::uint32_t *output)
{
    f0r_update2(instance, time, input, input, nullptr, output);
}
