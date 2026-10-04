// SPDX-License-Identifier: MIT
#include "engine.hpp"
#include <cassert>
#include <iostream>
using namespace sunimo;
int main(int argc, char **argv)
{
    assert(argc == 2);
    auto scene = Scene::file(argv[1]);
    scene->c.v[13] = 2;
    scene->c.v[22] = .5;
    Renderer renderer;
    renderer.set(scene);
    std::vector<uint8_t> first(96 * 64 * 4), restored(first.size());
    renderer.render(.5, 96, 64, nullptr, first.data());
    assert(!renderer.temp.empty() && !renderer.accum.empty());
    scene->c.v[22] = 0;
    renderer.set(scene);
    assert(renderer.temp.capacity() == 0 && renderer.accum.capacity() == 0);
    scene->c.v[22] = .5;
    renderer.set(scene);
    renderer.render(.5, 96, 64, nullptr, restored.data());
    assert(first == restored);
    renderer.set(nullptr);
    assert(renderer.overlay.capacity() == 0 && renderer.temp.capacity() == 0 && renderer.accum.capacity() == 0);
    std::cout << "Text scratch release/restore preserves RGBA PASS\n";
}
