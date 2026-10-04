/* Minimal declarations of the frei0r 1.x C ABI used by this filter.
 * Independently assembled from the public interface specification:
 * https://github.com/dyne/frei0r/blob/master/include/frei0r.h
 * Not a copy of the upstream implementation or documentation.
 */
#ifndef CARD3D_FREI0R_MINIMAL_H
#define CARD3D_FREI0R_MINIMAL_H
#include <stdint.h>
#if defined(_WIN32)
# define F0R_API __declspec(dllexport)
#else
# define F0R_API __attribute__((visibility("default")))
#endif
#ifdef __cplusplus
extern "C" {
#endif
#define F0R_PLUGIN_TYPE_FILTER 0
#define F0R_COLOR_MODEL_RGBA8888 1
#define F0R_PARAM_BOOL 0
#define F0R_PARAM_DOUBLE 1
#define F0R_PARAM_COLOR 2
typedef struct { float r,g,b; } f0r_param_color_t;
typedef void *f0r_instance_t;
typedef void *f0r_param_t;
typedef struct {
    const char *name;
    const char *author;
    int plugin_type;
    int color_model;
    int frei0r_version;
    int major_version;
    int minor_version;
    int num_params;
    const char *explanation;
} f0r_plugin_info_t;
typedef struct {
    const char *name;
    int type;
    const char *explanation;
} f0r_param_info_t;
F0R_API int f0r_init(void);
F0R_API void f0r_deinit(void);
F0R_API void f0r_get_plugin_info(f0r_plugin_info_t *info);
F0R_API void f0r_get_param_info(f0r_param_info_t *info, int index);
F0R_API f0r_instance_t f0r_construct(unsigned int width, unsigned int height);
F0R_API void f0r_destruct(f0r_instance_t instance);
F0R_API void f0r_set_param_value(f0r_instance_t instance, f0r_param_t value, int index);
F0R_API void f0r_get_param_value(f0r_instance_t instance, f0r_param_t value, int index);
F0R_API void f0r_update(f0r_instance_t instance, double time,
                       const uint32_t *input, uint32_t *output);
#ifdef __cplusplus
}
#endif
#endif
