#include "../src/track.h"
#include <assert.h>
#include <math.h>

int main(int argc, char **argv)
{
    assert(argc == 2);
    StudioTrack track = {0};
    assert(studio_track_load(&track, argv[1]));
    assert(track.count == 3);
    assert(fabs(track.source_aspect - 16.0 / 9.0) < 1e-9);
    StudioTrackKey key = studio_track_at(&track, .5);
    assert(fabs(key.x - .3) < 1e-12 && fabs(key.y - .4) < 1e-12 && fabs(key.quality - .75) < 1e-12);
    studio_track_clear(&track);
    assert(!studio_track_load(&track, "/definitely/missing.scam"));
    return 0;
}
