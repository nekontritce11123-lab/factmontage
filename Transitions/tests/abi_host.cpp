// This host intentionally compiles against the official SDK header.
#include <frei0r.h>

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

int main(int argc, char **argv)
{
    assert(argc == 2);
#ifdef _WIN32
    const auto library = LoadLibraryA(argv[1]);
    assert(library);
    const auto symbol = [&](const char *name) { auto value = GetProcAddress(library, name); assert(value); return value; };
#else
    const auto library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) { std::cerr << dlerror() << '\n'; return 1; }
    const auto symbol = [&](const char *name) { auto value = dlsym(library, name); assert(value); return value; };
#endif
    const auto init = reinterpret_cast<decltype(&f0r_init)>(symbol("f0r_init"));
    const auto deinit = reinterpret_cast<decltype(&f0r_deinit)>(symbol("f0r_deinit"));
    const auto getInfo = reinterpret_cast<decltype(&f0r_get_plugin_info)>(symbol("f0r_get_plugin_info"));
    const auto getParameterInfo = reinterpret_cast<decltype(&f0r_get_param_info)>(symbol("f0r_get_param_info"));
    const auto construct = reinterpret_cast<decltype(&f0r_construct)>(symbol("f0r_construct"));
    const auto destruct = reinterpret_cast<decltype(&f0r_destruct)>(symbol("f0r_destruct"));
    const auto set = reinterpret_cast<decltype(&f0r_set_param_value)>(symbol("f0r_set_param_value"));
    const auto get = reinterpret_cast<decltype(&f0r_get_param_value)>(symbol("f0r_get_param_value"));
    const auto update = reinterpret_cast<decltype(&f0r_update2)>(symbol("f0r_update2"));
    assert(init());
    f0r_plugin_info_t info{};
    getInfo(&info);
    assert(info.plugin_type == F0R_PLUGIN_TYPE_MIXER2 && info.color_model == F0R_COLOR_MODEL_RGBA8888);
    assert(info.num_params == 7 && std::strcmp(info.name, "SUNIMO Smooth Transitions") == 0);
    const char *names[] = {"Progress", "Style", "Strength", "Direction", "Softness", "Center", "Quality"};
    for (int index = 0; index < 7; ++index) {
        f0r_param_info_t parameter{};
        getParameterInfo(&parameter, index);
        assert(std::strcmp(parameter.name, names[index]) == 0);
        assert(parameter.type == (index == 5 ? F0R_PARAM_POSITION : F0R_PARAM_DOUBLE));
    }
    auto instance = construct(64, 48);
    assert(instance);
    for (int style = 0; style < 15; ++style) {
        double code = style < 12 ? style / 11.0 : (2 * (style - 12) + 1) / 22.0;
        set(instance, &code, 1);
        double stored = -1;
        get(instance, &stored, 1);
        assert(std::abs(stored - code) < 1e-12);
    }
    double progress = 0;
    set(instance, &progress, 0);
    get(instance, &progress, 0);
    assert(progress == 0);
    std::vector<std::uint32_t> first(64 * 48, 0xff2040e0), second(64 * 48, 0xffe08020), output(first.size());
    update(instance, 0, first.data(), second.data(), nullptr, output.data());
    assert(output == first);
    progress = 1;
    set(instance, &progress, 0);
    update(instance, 0, first.data(), second.data(), nullptr, output.data());
    assert(output == second);
    destruct(instance);
    deinit();
#ifdef _WIN32
    FreeLibrary(library);
#else
    dlclose(library);
#endif
    std::cout << "PASS official frei0r ABI, 7 parameters and exact endpoints\n";
}
