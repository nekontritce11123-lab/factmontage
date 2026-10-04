/* SPDX-License-Identifier: MIT */
#include "camera.h"
#include "track.h"
#include <framework/mlt.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    StudioCamera *camera;
    StudioTrack track;
    char *track_path;
    int width, height;
} CameraFilter;

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = (char) (c | 32);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static int local_path(const char *source, char *result, size_t size)
{
    if (!source) source = "";
    int uri = !strncmp(source, "file://", 7);
    if (uri) source += 7;
    size_t used = 0;
    while (*source) {
        unsigned char value = (unsigned char) *source++;
        if (uri && value == '%' && source[0] && source[1]) {
            int high = hex_value(source[0]), low = hex_value(source[1]);
            if (high >= 0 && low >= 0) { value = (unsigned char) (high * 16 + low); source += 2; }
        }
        if (!value || value == '\n' || value == '\r' || used + 1 >= size) return 0;
        result[used++] = (char) value;
    }
    result[used] = 0;
    return 1;
}

static void load_track(CameraFilter *self, const char *path)
{
    path = path ? path : "";
    if (self->track_path && !strcmp(self->track_path, path)) return;
    free(self->track_path);
    self->track_path = strdup(path);
    studio_track_clear(&self->track);
    if (path[0] && !studio_track_load(&self->track, path))
        fprintf(stderr, "Studio Camera: cannot read head track '%s'; using the chosen camera point.\n", path);
}

static StudioCameraConfig config_from(CameraFilter *self, mlt_filter filter, mlt_frame frame)
{
    mlt_properties p = MLT_FILTER_PROPERTIES(filter);
    mlt_position position = mlt_filter_get_position(filter, frame);
    mlt_position length = mlt_filter_get_length2(filter, frame);
    double origin = mlt_properties_get_double(p, "studio_time_origin");
    double span = mlt_properties_get_double(p, "studio_time_span");
    if (span <= 0) span = length > 1 ? length - 1 : 1;
    double fps = mlt_profile_fps(mlt_service_profile(MLT_FILTER_SERVICE(filter)));
    double duration = mlt_properties_get_double(p, "duration");
    double denominator = mlt_properties_get_int(p, "timing") == 0 ? span : fmax(1, duration * fps);
    double clock = origin + position;
    StudioCameraConfig config = {
        .mode = mlt_properties_get_int(p, "mode"), .live = mlt_properties_get_int(p, "live"),
        .zoom = mlt_properties_get_double(p, "zoom") / 100.0,
        .start_x = mlt_properties_get_double(p, "start_x") / 100.0,
        .start_y = mlt_properties_get_double(p, "start_y") / 100.0,
        .end_x = mlt_properties_get_double(p, "end_x") / 100.0,
        .end_y = mlt_properties_get_double(p, "end_y") / 100.0,
        .progress = clock / denominator, .time = clock / fps,
        .cycle_period = mlt_properties_get_double(p, "cycle_period"),
        .variant = mlt_properties_get_int(p, "variant"),
        .tracking = mlt_properties_get_int(p, "tracking")
    };
    if (config.tracking) {
        char path[PATH_MAX];
        load_track(self, local_path(mlt_properties_get(p, "track_path"), path, sizeof(path)) ? path : "");
        if (self->track.count) {
            StudioTrackKey key = studio_track_at(&self->track, config.time + mlt_properties_get_double(p, "track_offset"));
            config.track_x = key.x;
            config.track_y = key.y;
            config.track_quality = key.quality;
            double frame_aspect = self->height > 0 ? (double) self->width / self->height : 0;
            if (self->track.source_aspect > 0 && frame_aspect > 0) {
                if (self->track.source_aspect > frame_aspect)
                    config.track_y = .5 + (config.track_y - .5) * frame_aspect / self->track.source_aspect;
                else
                    config.track_x = .5 + (config.track_x - .5) * self->track.source_aspect / frame_aspect;
            }
        } else {
            config.tracking = 0;
        }
    }
    return config;
}

static int get_image(mlt_frame frame, uint8_t **image, mlt_image_format *format,
                     int *width, int *height, int writable)
{
    (void) writable;
    mlt_filter filter = (mlt_filter) mlt_frame_pop_service(frame);
    CameraFilter *self = filter->child;
    *format = mlt_image_rgba;
    int error = mlt_frame_get_image(frame, image, format, width, height, 1);
    if (error || !*image) return error;
    mlt_service_lock(MLT_FILTER_SERVICE(filter));
    if (!self->camera || self->width != *width || self->height != *height) {
        studio_camera_destroy(self->camera);
        self->camera = studio_camera_create(*width, *height);
        self->width = *width; self->height = *height;
    }
    StudioCameraConfig config = config_from(self, filter, frame);
    if (!self->camera || !studio_camera_render(self->camera, &config, *image, *image)) error = 1;
    mlt_service_unlock(MLT_FILTER_SERVICE(filter));
    return error;
}

static mlt_frame process(mlt_filter filter, mlt_frame frame)
{
    mlt_frame_push_service(frame, filter);
    mlt_frame_push_get_image(frame, get_image);
    return frame;
}

static void close_filter(mlt_filter filter)
{
    CameraFilter *self = filter->child;
    if (self) {
        studio_camera_destroy(self->camera);
        studio_track_clear(&self->track);
        free(self->track_path);
        free(self);
    }
    filter->child = NULL; filter->close = NULL; filter->parent.close = NULL;
    mlt_service_close(&filter->parent);
}

static mlt_filter camera_init(mlt_profile profile, mlt_service_type type, const char *id, const void *arg)
{
    (void) profile; (void) type; (void) id; (void) arg;
    mlt_filter filter = mlt_filter_new();
    CameraFilter *self = calloc(1, sizeof(*self));
    if (!filter || !self) { if (filter) mlt_filter_close(filter); free(self); return NULL; }
    mlt_properties p = MLT_FILTER_PROPERTIES(filter);
    mlt_properties_set_int(p, "mode", 0); mlt_properties_set_int(p, "live", 1);
    mlt_properties_set_double(p, "zoom", 120); mlt_properties_set_double(p, "start_x", 35);
    mlt_properties_set_double(p, "start_y", 50); mlt_properties_set_double(p, "end_x", 65);
    mlt_properties_set_double(p, "end_y", 50); mlt_properties_set_int(p, "timing", 0);
    mlt_properties_set_double(p, "duration", .65); mlt_properties_set_double(p, "cycle_period", 6);
    mlt_properties_set_int(p, "tracking", 0); mlt_properties_set(p, "track_path", "");
    mlt_properties_set_double(p, "track_offset", 0);
    mlt_properties_set_int(p, "variant", 0);
    mlt_properties_set_double(p, "studio_time_origin", 0); mlt_properties_set_double(p, "studio_time_span", 0);
    filter->child = self; filter->process = process; filter->close = close_filter;
    return filter;
}

static mlt_properties metadata(mlt_service_type type, const char *id, void *data)
{
    (void) type; (void) id; (void) data;
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/studio/filter_camera.yml", mlt_environment("MLT_DATA"));
    return mlt_properties_parse_yaml(path);
}

MLT_REPOSITORY
{
    MLT_REGISTER(mlt_service_filter_type, "studio.camera", camera_init);
    MLT_REGISTER_METADATA(mlt_service_filter_type, "studio.camera", metadata, NULL);
}
