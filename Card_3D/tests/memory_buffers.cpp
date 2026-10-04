// SPDX-License-Identifier: GPL-3.0-only
#include "../src/card_studio.cpp"
#include <cassert>
#include <iostream>

int main()
{
    card3d::Studio renderer(96, 64);
    std::vector<uint8_t> input(96 * 64 * 4, 255), first(input.size()), restored(input.size());
    double on = .5, off = 0;
    renderer.set_control(UI_SHADOW, &on);
    renderer.render(2, input.data(), first.data());
    assert(!renderer.shadow.empty() && !renderer.tmp.empty());
    renderer.set_control(UI_SHADOW, &off);
    assert(renderer.shadow.capacity() == 0 && renderer.tmp.capacity() == 0);
    renderer.set_control(UI_SHADOW, &on);
    renderer.render(2, input.data(), restored.data());
    assert(first == restored);
    std::cout << "Card shadow release/restore preserves RGBA PASS\n";
}
