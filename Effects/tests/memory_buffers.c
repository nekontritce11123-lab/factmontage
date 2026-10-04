/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/studiofx.c"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    int geometry = -1, glow = -1;
    for (int i = 0; i < SFX_RECIPE_COUNT; ++i) {
        if (is_geometry(sfx_recipes[i].algorithm)) geometry = i;
        if (sfx_recipes[i].algorithm == SFX_GLOW) glow = i;
    }
    assert(geometry >= 0 && glow >= 0);
    Instance *s = f0r_construct(96, 64);
    uint32_t input[96 * 64], first[96 * 64], restored[96 * 64];
    for (int i = 0; i < 96 * 64; ++i) input[i] = 0xff123456u + (uint32_t)i;
    double selected = (double)geometry / SFX_SLOT_SCALE;
    f0r_set_param_value(s, &selected, 0);
    f0r_update(s, 2, input, first);
    assert(s->mapx && s->mapy);
    selected = (double)glow / SFX_SLOT_SCALE;
    f0r_set_param_value(s, &selected, 0);
    assert(!s->mapx && !s->mapy && !s->cache_valid);
    f0r_update(s, 2, input, restored);
    assert(s->glow && s->temp);
    selected = (double)geometry / SFX_SLOT_SCALE;
    f0r_set_param_value(s, &selected, 0);
    assert(!s->glow && !s->temp);
    f0r_update(s, 2, input, restored);
    assert(memcmp(first, restored, sizeof first) == 0);
    f0r_destruct(s);
    puts("Effects scratch release/restore preserves RGBA PASS");
}
