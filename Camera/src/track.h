/* SPDX-License-Identifier: MIT */
#pragma once
#include <stddef.h>

typedef struct {
    double t, x, y, quality;
} StudioTrackKey;

typedef struct {
    StudioTrackKey *keys;
    size_t count;
    double source_aspect;
} StudioTrack;

void studio_track_clear(StudioTrack *track);
int studio_track_load(StudioTrack *track, const char *path);
StudioTrackKey studio_track_at(const StudioTrack *track, double time);

