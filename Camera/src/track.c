/* SPDX-License-Identifier: MIT
 * Strict reader for the existing SUNIMO_CAMERA_TRACK_V1 format.
 */
#define _GNU_SOURCE
#include "track.h"
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TRACK_KEYS 2000000U
#define MAX_TRACK_FILE (64L * 1024 * 1024)

void studio_track_clear(StudioTrack *track)
{
    if (!track) return;
    free(track->keys);
    *track = (StudioTrack) {0};
}

static int fail(StudioTrack *track, FILE *file)
{
    if (file) fclose(file);
    studio_track_clear(track);
    return 0;
}

int studio_track_load(StudioTrack *track, const char *path)
{
    if (!track || !path || !path[0]) return 0;
    StudioTrack parsed = {0};
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    if (fseek(file, 0, SEEK_END) || ftell(file) < 0 || ftell(file) > MAX_TRACK_FILE) return fail(&parsed, file);
    rewind(file);
    char line[2048];
    if (!fgets(line, sizeof(line), file)
        || strncmp(line, "SUNIMO_CAMERA_TRACK_V1", 22)
        || (line[22] != '\n' && line[22] != '\r' && line[22] != 0)) return fail(&parsed, file);
    locale_t locale = newlocale(LC_NUMERIC_MASK, "C", NULL);
    if (!locale) return fail(&parsed, file);
    size_t capacity = 0;
    double previous = -1;
    while (fgets(line, sizeof(line), file)) {
        if (!strchr(line, '\n') && !feof(file)) {
            freelocale(locale);
            return fail(&parsed, file);
        }
        if (line[0] == '#') {
            if (!strncmp(line, "# aspect=", 9)) {
                char *end = NULL;
                double aspect = strtod_l(line + 9, &end, locale);
                if (end != line + 9 && isfinite(aspect) && aspect > .02 && aspect < 50) parsed.source_aspect = aspect;
            }
            continue;
        }
        if (line[0] == '\n' || line[0] == '\r') continue;
        double values[4];
        char *cursor = line;
        for (int i = 0; i < 4; ++i) {
            char *end = NULL;
            values[i] = strtod_l(cursor, &end, locale);
            if (end == cursor || !isfinite(values[i]) || (i < 3 && *end != ',')) {
                freelocale(locale);
                return fail(&parsed, file);
            }
            cursor = end + (i < 3);
        }
        while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') ++cursor;
        if (*cursor || values[0] < 0 || values[0] <= previous || values[0] > 86400
            || values[1] < 0 || values[1] > 1 || values[2] < 0 || values[2] > 1
            || values[3] < 0 || values[3] > 1 || parsed.count >= MAX_TRACK_KEYS) {
            freelocale(locale);
            return fail(&parsed, file);
        }
        if (parsed.count == capacity) {
            size_t next = capacity ? capacity * 2 : 512;
            if (next > MAX_TRACK_KEYS) next = MAX_TRACK_KEYS;
            StudioTrackKey *keys = realloc(parsed.keys, next * sizeof(*keys));
            if (!keys) {
                freelocale(locale);
                return fail(&parsed, file);
            }
            parsed.keys = keys;
            capacity = next;
        }
        parsed.keys[parsed.count++] = (StudioTrackKey) {values[0], values[1], values[2], values[3]};
        previous = values[0];
    }
    int error = ferror(file);
    fclose(file);
    freelocale(locale);
    if (error || parsed.count < 2) return fail(&parsed, NULL);
    studio_track_clear(track);
    *track = parsed;
    return 1;
}

StudioTrackKey studio_track_at(const StudioTrack *track, double time)
{
    if (!track || track->count == 0) return (StudioTrackKey) {0, .5, .5, 0};
    if (time <= track->keys[0].t) return track->keys[0];
    if (time >= track->keys[track->count - 1].t) return track->keys[track->count - 1];
    size_t left = 0, right = track->count - 1;
    while (right - left > 1) {
        size_t middle = (left + right) / 2;
        if (track->keys[middle].t > time) right = middle; else left = middle;
    }
    StudioTrackKey a = track->keys[left], b = track->keys[right];
    double u = (time - a.t) / (b.t - a.t);
    return (StudioTrackKey) {time, a.x + (b.x - a.x) * u, a.y + (b.y - a.y) * u,
                            a.quality + (b.quality - a.quality) * u};
}

