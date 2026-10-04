// SPDX-License-Identifier: MIT
// Opt-in test probe only. Never install over the product's frei0r library.
// Forward the documented frei0r C ABI without interpreting metadata structs:
// https://dyne.org/frei0r/codedoc/html/frei0r_8h.html
// MLT loads this basename from a private FREI0R_PATH; the real renderer stays
// at SUNIMO_STAGE0_REAL_LIBRARY. No timing, parameters or pixels are changed.
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void *library;
static int initialized;
static int (*real_init)(void);
static void (*real_deinit)(void);
static void (*real_info)(void *);
static void (*real_param_info)(void *, int);
static void *(*real_construct)(unsigned, unsigned);
static void (*real_destruct)(void *);
static void (*real_set)(void *, void *, int);
static void (*real_get)(void *, void *, int);
static void (*real_update)(void *, double, const uint32_t *, uint32_t *);
typedef struct { void *real; unsigned width, height; } Probe;

int f0r_init(void);
static int fail(const char *message)
{
    fprintf(stderr, "SUNIMO_STAGE0_ERROR %s\n", message ? message : "unknown failure");
    if (library) dlclose(library);
    library = NULL;
    initialized = 0;
    return 0;
}

static int load_renderer(void)
{
    if (library) return 1;
    const char *path = getenv("SUNIMO_STAGE0_REAL_LIBRARY");
    if (!path || path[0] != '/') return fail("an explicit absolute real-library path is required");
    library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!library) return fail(dlerror());
#define LOAD(target, symbol) do { *(void **)(&(target)) = dlsym(library, symbol); \
    if (!(target)) return fail("missing required symbol: " symbol); } while (0)
    LOAD(real_init, "f0r_init");
    if (real_init == f0r_init) return fail("the probe cannot forward to itself");
    LOAD(real_deinit, "f0r_deinit");
    LOAD(real_info, "f0r_get_plugin_info");
    LOAD(real_param_info, "f0r_get_param_info");
    LOAD(real_construct, "f0r_construct");
    LOAD(real_destruct, "f0r_destruct");
    LOAD(real_set, "f0r_set_param_value");
    LOAD(real_get, "f0r_get_param_value");
    LOAD(real_update, "f0r_update");
#undef LOAD
    fprintf(stderr, "SUNIMO_STAGE0_LIBRARY %s\n", path);
    return 1;
}

int f0r_init(void)
{
    if (!load_renderer()) return 0;
    if (!real_init()) return 0;
    ++initialized;
    return 1;
}

void f0r_deinit(void)
{
    if (initialized) { real_deinit(); --initialized; }
    if (!initialized && library) { dlclose(library); library = NULL; }
}
// MLT discovery asks for metadata before f0r_init(). Loading symbols here
// must not invent an extra plugin initialization/deinitialization cycle.
void f0r_get_plugin_info(void *info)
{
    if (!info) return;
    if (load_renderer()) { real_info(info); return; }
    // Return an unsupported plugin type if the explicit renderer is missing.
    // Layout is the public frei0r f0r_plugin_info_t, not a private engine type.
    typedef struct { const char *name, *author; int type, color, abi, major, minor, count;
                     const char *explanation; } Info;
    *(Info *)info = (Info){"Unavailable diagnostic renderer", "SUNIMO", -1, 1, 1, 0, 0, 0,
                          "Set SUNIMO_STAGE0_REAL_LIBRARY to the installed renderer"};
}
void f0r_get_param_info(void *info, int index)
{
    if (info && load_renderer()) real_param_info(info, index);
}
void *f0r_construct(unsigned width, unsigned height)
{
    if (!initialized) return NULL;
    Probe *probe = calloc(1, sizeof(*probe));
    if (!probe) return NULL;
    probe->real = real_construct(width, height);
    if (!probe->real) { free(probe); return NULL; }
    probe->width = width;
    probe->height = height;
    return probe;
}
void f0r_destruct(void *instance)
{
    Probe *probe = instance;
    if (!probe) return;
    real_destruct(probe->real);
    free(probe);
}
void f0r_set_param_value(void *instance, void *value, int index)
{
    if (instance) real_set(((Probe *)instance)->real, value, index);
}
void f0r_get_param_value(void *instance, void *value, int index)
{
    if (instance) real_get(((Probe *)instance)->real, value, index);
}
void f0r_update(void *instance, double time, const uint32_t *input, uint32_t *output)
{
    Probe *probe = instance;
    if (!probe) return;
    fprintf(stderr, "SUNIMO_STAGE0_CLOCK instance=%p width=%u height=%u time=%.17g\n",
            instance, probe->width, probe->height, time);
    real_update(probe->real, time, input, output);
}
