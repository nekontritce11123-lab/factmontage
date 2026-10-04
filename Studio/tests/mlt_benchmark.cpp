// SPDX-License-Identifier: GPL-3.0-only
// Synchronous MLT render cost; deliberately not a GUI playback/drop counter.
extern "C" {
#include <framework/mlt.h>
}
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <sys/resource.h>
#include <vector>

int main(int argc, char **argv)
{
    if (argc != 5) return 2;
    const int width = std::atoi(argv[2]), height = std::atoi(argv[3]), count = std::atoi(argv[4]);
    if (width < 1 || height < 1 || count < 2) return 2;
    mlt_factory_init(nullptr);
    auto profile = mlt_profile_init(nullptr);
    profile->width = width; profile->height = height;
    profile->frame_rate_num = 60; profile->frame_rate_den = 1;
    profile->progressive = 1; profile->colorspace = 709;
    profile->sample_aspect_num = profile->sample_aspect_den = 1;
    profile->display_aspect_num = 16; profile->display_aspect_den = 9;
    profile->is_explicit = 1;
    auto producer = mlt_factory_producer(profile, "xml", argv[1]);
    if (!producer) return 3;
    std::vector<double> samples;
    unsigned long long checksum = 0;
    for (int i = 0; i < count + 12; ++i) {
        const auto start = std::chrono::steady_clock::now();
        mlt_producer_seek(producer, i);
        mlt_frame frame = nullptr;
        if (mlt_service_get_frame(MLT_PRODUCER_SERVICE(producer), &frame, 0) || !frame) return 4;
        uint8_t *pixels = nullptr;
        auto format = mlt_image_rgba;
        int w = width, h = height;
        if (mlt_frame_get_image(frame, &pixels, &format, &w, &h, 0) || !pixels || w != width || h != height) return 5;
        const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (i >= 12) samples.push_back(elapsed);
        for (int offset = 0; offset < w * h * 4; offset += 4096) checksum = checksum * 33 + pixels[offset];
        mlt_frame_close(frame);
    }
    mlt_producer_close(producer);
    mlt_profile_close(profile);
    mlt_factory_close();
    std::sort(samples.begin(), samples.end());
    struct rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    const auto late = std::count_if(samples.begin(), samples.end(), [](double ms) { return ms > 1000.0 / 60; });
    std::printf("{\"median_ms\":%.3f,\"p95_ms\":%.3f,\"over_16_67_ms\":%ld,\"frames\":%d,\"warmup\":12,\"peak_rss_kib\":%ld,\"checksum\":\"%llu\"}\n",
        (samples[(samples.size()-1)/2] + samples[samples.size()/2])/2,
        samples[(samples.size()*95+99)/100-1], long(late), count, usage.ru_maxrss, checksum);
}
