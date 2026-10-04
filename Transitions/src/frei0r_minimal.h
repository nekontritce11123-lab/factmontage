/* Minimal declaration-only subset of the public frei0r 1.x ABI.
 * Release ABI checks compile against the official SDK frei0r.h. */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define FREI0R_MAJOR_VERSION 1
#define F0R_PLUGIN_TYPE_MIXER2 2
#define F0R_COLOR_MODEL_RGBA8888 1
#define F0R_PARAM_DOUBLE 1
#define F0R_PARAM_POSITION 3
typedef struct { const char *name,*author; int plugin_type,color_model,frei0r_version,major_version,minor_version,num_params; const char *explanation; } f0r_plugin_info_t;
typedef struct { const char *name; int type; const char *explanation; } f0r_param_info_t;
typedef void *f0r_instance_t;
typedef void *f0r_param_t;
typedef struct { double x,y; } f0r_param_position_t;
#ifdef __cplusplus
}
#endif
