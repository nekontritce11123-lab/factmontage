// SPDX-License-Identifier: GPL-3.0-or-later
// MLT 7 adapter. Requires a real SDK build and the integration tests described in TEST_REPORT_RU.md.
extern "C" {
#include <framework/mlt.h>
#include <framework/mlt_slices.h>
}
#include "studio_color/color.hpp"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <string>

namespace {
using namespace studio_color;
LutCache cache(8);
struct Slice {
    std::shared_ptr<const Lut> lut;
    std::uint8_t *pixels;
    int width, height;
    std::atomic_bool failed{false};
};
int process_slice(int, int index, int jobs, void *cookie) noexcept
{
    auto &s = *static_cast<Slice *>(cookie);
    int first = 0;
    const int rows = mlt_slices_size_slice(jobs, index, s.height, &first);
    try {
        auto *start = s.pixels + static_cast<std::ptrdiff_t>(first) * s.width * 4;
        s.lut->process(start, start, s.width, rows);
        return 0;
    } catch (...) {
        s.failed.store(true);
        return 1;
    }
}
int get_image(mlt_frame frame, uint8_t **image, mlt_image_format *format,
              int *width, int *height, int writable) noexcept
{
    auto filter = static_cast<mlt_filter>(mlt_frame_pop_service(frame));
    if (!filter) return 1;
    try {
        Params params;
        auto service = MLT_FILTER_SERVICE(filter);
        auto properties = MLT_FILTER_PROPERTIES(filter);
        // The host must certify the working input after inspecting actual colour metadata.
        // This flag is NOT an automatic HDR detector. Copy/retime/upstream changes invalidate it.
        mlt_service_lock(service);
        const bool validated = mlt_properties_get_int(properties, "studio_input_validated") == 1;
        const char *algorithm = mlt_properties_get(properties, "studio_color_algorithm");
        const bool compatible = algorithm && std::strcmp(algorithm, "sdr-primary-v1") == 0;
        for (const auto &spec : paramSpecs)
            if (mlt_properties_exists(properties, spec.id))
                params.*(spec.field) = mlt_properties_get_double(properties, spec.id);
        mlt_service_unlock(service);
        if (!validated || !compatible) {
            mlt_log_error(service, "Studio Color: input/algorithm not validated; refusing SDR processing.\n");
            return 1;
        }
        // Neutral state must not force an otherwise unnecessary RGBA conversion.
        if (isIdentity(params)) return mlt_frame_get_image(frame, image, format, width, height, writable);
        const auto lut = cache.get(params); // immutable; no analysis or disk access at playback
        *format = mlt_image_rgba;
        const int error = mlt_frame_get_image(frame, image, format, width, height, 1);
        if (error) return error;
        if (*format != mlt_image_rgba || !*image || *width <= 0 || *height <= 0 ||
            *width > 32768 || *height > 32768) return 1;
        Slice slice{lut, *image, *width, *height};
        // Reuse MLT's pool. No private worker pool per effect and no unbounded parallelism.
        const int threads = *height < 240 ? 1 : std::max(1, std::min(2, mlt_slices_count_normal()));
        if (threads == 1) process_slice(0, 0, 1, &slice);
        else mlt_slices_run_normal(threads, process_slice, &slice);
        return slice.failed.load() ? 1 : 0;
    } catch (const std::exception &e) {
        mlt_log_error(MLT_FILTER_SERVICE(filter), "Studio Color: %s\n", e.what());
        return 1;
    } catch (...) { return 1; }
}
mlt_frame process(mlt_filter filter, mlt_frame frame)
{
    mlt_frame_push_service(frame, filter);
    mlt_frame_push_get_image(frame, get_image);
    return frame;
}
void *create(mlt_profile, mlt_service_type, const char *, const void *) noexcept
{
    auto filter = mlt_filter_new();
    if (!filter) return nullptr;
    filter->process = process;
    const auto properties = MLT_FILTER_PROPERTIES(filter);
    for (const auto &spec : studio_color::paramSpecs)
        mlt_properties_set_double(properties, spec.id, spec.defaultValue);
    mlt_properties_set_int(properties, "studio_input_validated", 0);
    mlt_properties_set(properties, "studio_color_algorithm", "sdr-primary-v1");
    return filter;
}
mlt_properties metadata(mlt_service_type, const char *, void *) noexcept
{
    try {
        const std::string path = std::string(STUDIO_COLOR_DATA_DIR) + "/filter_studio_color.yml";
        return mlt_properties_parse_yaml(path.c_str());
    } catch (...) { return nullptr; }
}
}
extern "C" MLT_REPOSITORY
{
    MLT_REGISTER(mlt_service_filter_type, "studio.color", create);
    MLT_REGISTER_METADATA(mlt_service_filter_type, "studio.color", metadata, nullptr);
}
