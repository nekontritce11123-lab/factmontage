/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>

typedef struct StudioCamera StudioCamera;

typedef struct {
    int mode;       /* 0 static, 1 in, 2 out, 3 in/hold/out, 4 pan, 5 loop */
    int live;       /* 0 off, 1 calm, 2 visible */
    double zoom;    /* 1.0 .. 2.0 */
    double start_x, start_y, end_x, end_y;
    double progress; /* 0 .. 1 */
    double time;     /* seconds; continuous across clip cuts */
    double cycle_period; /* 2 .. 12 seconds */
    int variant;
    int tracking;   /* 0 off, 1 follow a pre-analysed head track */
    double track_x, track_y, track_quality;
} StudioCameraConfig;

typedef struct {
    double scale, center_x, center_y, rotation, user_zoom, overscan;
} StudioCameraState;

StudioCamera *studio_camera_create(unsigned width, unsigned height);
void studio_camera_destroy(StudioCamera *camera);
StudioCameraState studio_camera_state(StudioCamera *camera, const StudioCameraConfig *config);
int studio_camera_render(StudioCamera *camera, const StudioCameraConfig *config,
                         const uint8_t *input, uint8_t *output);
