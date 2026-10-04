/* Frei0r 1.2 API declarations, transcribed verbatim from the official header.
 * Upstream: dyne/frei0r include/frei0r.h
 * Git blob b20487e9d34de76c7665b59378940317f63db2b4.
 * Documentation comments omitted; declarations are not a new local ABI.
 * scripts/check_official_header.sh compiles the ABI host against an untouched
 * upstream header for release validation. This transcription is for offline builds.
 */
#ifndef INCLUDED_FREI0R_H
#define INCLUDED_FREI0R_H
#include <inttypes.h>
#define FREI0R_MAJOR_VERSION 1
#define FREI0R_MINOR_VERSION 2
#define F0R_PLUGIN_TYPE_FILTER 0
#define F0R_PLUGIN_TYPE_SOURCE 1
#define F0R_PLUGIN_TYPE_MIXER2 2
#define F0R_PLUGIN_TYPE_MIXER3 3
#define F0R_COLOR_MODEL_BGRA8888 0
#define F0R_COLOR_MODEL_RGBA8888 1
#define F0R_COLOR_MODEL_PACKED32 2
#define F0R_PARAM_BOOL 0
#define F0R_PARAM_DOUBLE 1
#define F0R_PARAM_COLOR 2
#define F0R_PARAM_POSITION 3
#define F0R_PARAM_STRING 4
typedef struct f0r_plugin_info {
 const char* name; const char* author;
 int plugin_type; int color_model; int frei0r_version;
 int major_version; int minor_version; int num_params;
 const char* explanation;
} f0r_plugin_info_t;
typedef double f0r_param_bool;
typedef double f0r_param_double;
typedef struct f0r_param_color { float r; float g; float b; } f0r_param_color_t;
typedef struct f0r_param_position { double x; double y; } f0r_param_position_t;
typedef char* f0r_param_string;
typedef struct f0r_param_info { const char* name; int type; const char* explanation; } f0r_param_info_t;
typedef void* f0r_instance_t;
typedef void* f0r_param_t;
int f0r_init(void);
void f0r_deinit(void);
void f0r_get_plugin_info(f0r_plugin_info_t* info);
void f0r_get_param_info(f0r_param_info_t* info, int param_index);
f0r_instance_t f0r_construct(unsigned int width, unsigned int height);
void f0r_destruct(f0r_instance_t instance);
void f0r_set_param_value(f0r_instance_t instance, f0r_param_t param, int param_index);
void f0r_get_param_value(f0r_instance_t instance, f0r_param_t param, int param_index);
void f0r_update(f0r_instance_t instance, double time, const uint32_t* inframe, uint32_t* outframe);
void f0r_update2(f0r_instance_t instance, double time, const uint32_t* inframe1,
 const uint32_t* inframe2, const uint32_t* inframe3, uint32_t* outframe);
#endif
