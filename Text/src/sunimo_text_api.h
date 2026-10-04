/* SPDX-License-Identifier: MIT
 * Native C ABI for a future host integration. All strings UTF-8, pixels straight RGBA.
 * Create independent instances for workers; the implementation also serializes each instance.
 * smt_load/render: 1 success, 0 failure. Failed load preserves the last valid scene.
 * Error string belongs to the instance and is valid until the next operation.
 */
#ifndef SUNIMO_TEXT_API_H
#define SUNIMO_TEXT_API_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void *smt_create(void);
void smt_destroy(void *instance);
int smt_load(void *instance, const void *stxt_bytes, size_t byte_count);
int smt_render(void *instance, double seconds, unsigned width, unsigned height,
               const void *input_rgba_or_null, void *output_rgba);
const char *smt_last_error(void *instance);
#ifdef __cplusplus
}
#endif
#endif
