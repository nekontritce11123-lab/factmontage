/* SPDX-License-Identifier: MIT
 * Deterministic virtual camera derived from SUNIMO Camera Studio 1.0.
 */
#include "camera.h"
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PI 3.14159265358979323846
#define MAX_WORKERS 3

typedef struct { struct StudioCamera *camera; int id; } Worker;
struct StudioCamera {
    unsigned width, height;
    uint8_t *scratch;
    const uint8_t *input;
    uint8_t *output;
    StudioCameraState state;
    pthread_mutex_t api, lock;
    pthread_cond_t ready, done;
    pthread_t threads[MAX_WORKERS];
    Worker workers[MAX_WORKERS];
    int worker_count, pending, quit;
    unsigned long generation;
};

static double clampd(double x, double low, double high) { return x < low ? low : x > high ? high : x; }
static uint32_t hash32(uint32_t x) { x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; return x ^ (x >> 16); }
static double random_value(int64_t i, uint32_t seed) { return ((double) hash32((uint32_t) i ^ seed) / 4294967295.0) * 2.0 - 1.0; }

/* Cubic B-spline value noise: bounded and C2 continuous. */
static double noise(double t, uint32_t seed)
{
    int64_t k = (int64_t) floor(t);
    double u = t - floor(t), u2 = u * u, u3 = u2 * u;
    return (random_value(k - 1, seed) * (1 - u) * (1 - u) * (1 - u)
            + random_value(k, seed) * (3 * u3 - 6 * u2 + 4)
            + random_value(k + 1, seed) * (-3 * u3 + 3 * u2 + 3 * u + 1)
            + random_value(k + 2, seed) * u3) / 6.0;
}

static double ease(double value)
{
    double u = clampd(value, 0, 1);
    return u * u * u * (20 + u * (-45 + u * (36 - 10 * u)));
}

static double zoom_mix(double from, double to, double progress)
{
    double u = ease(progress);
    return exp(log(from) * (1 - u) + log(to) * u);
}

static double soft_center(double value, double low, double high)
{
    if (high <= low) return .5;
    double edge = fmin(.006, (high - low) * .15);
    if (edge < 1e-9) return clampd(value, low, high);
    if (value < low + edge) { double u = clampd((value - low + edge) / (2 * edge), 0, 1); return low + edge * u * u; }
    if (value > high - edge) { double u = clampd((high + edge - value) / (2 * edge), 0, 1); return high - edge * u * u; }
    return value;
}

static StudioCameraState calculate(const StudioCamera *camera, const StudioCameraConfig *source)
{
    StudioCameraConfig c = *source;
    c.mode = c.mode < 0 ? 0 : c.mode > 5 ? 5 : c.mode;
    c.live = c.live < 0 ? 0 : c.live > 2 ? 2 : c.live;
    c.zoom = clampd(c.zoom, 1, 2);
    c.progress = clampd(isfinite(c.progress) ? c.progress : 0, 0, 1);
    c.time = clampd(isfinite(c.time) ? c.time : 0, -1e12, 1e12);
    c.cycle_period = clampd(isfinite(c.cycle_period) ? c.cycle_period : 6, 2, 12);
    c.start_x = clampd(c.start_x, 0, 1); c.start_y = clampd(c.start_y, 0, 1);
    c.end_x = clampd(c.end_x, 0, 1); c.end_y = clampd(c.end_y, 0, 1);
    c.track_x = clampd(isfinite(c.track_x) ? c.track_x : .5, 0, 1);
    c.track_y = clampd(isfinite(c.track_y) ? c.track_y : .5, 0, 1);
    c.track_quality = clampd(isfinite(c.track_quality) ? c.track_quality : 0, 0, 1);

    double p = c.progress, zoom = c.zoom;
    if (c.mode == 1) zoom = zoom_mix(1, c.zoom, p);
    else if (c.mode == 2) zoom = zoom_mix(c.zoom, 1, p);
    else if (c.mode == 3) {
        if (p < .25) zoom = zoom_mix(1, c.zoom, p * 4);
        else if (p > .75) zoom = zoom_mix(c.zoom, 1, (p - .75) * 4);
    } else if (c.mode == 5) {
        double phase = fmod(c.time, c.cycle_period) / c.cycle_period;
        if (phase < 0) phase += 1;
        zoom = zoom_mix(1, c.zoom, .5 - .5 * cos(2 * PI * phase));
    }

    double point_x = c.end_x, point_y = c.end_y;
    if (c.mode == 4) {
        double u = ease(p);
        point_x = c.start_x + (c.end_x - c.start_x) * u;
        point_y = c.start_y + (c.end_y - c.start_y) * u;
    }

    double amount = c.live == 1 ? .35 : c.live == 2 ? .65 : 0;
    double speed = c.live == 1 ? .35 : .50;
    double period = 12 - 8.5 * speed;
    uint32_t seed = hash32(0x58b14U + (uint32_t) c.variant);
    double tick = c.time / period;
    tick += .28 * sin(tick * .43 + 1.3) + .17 * sin(tick * .29 + 2.4);
    double dx = .024 * amount * noise(tick + 3.73, seed);
    double dy = .018 * amount * noise(tick * .87 + 9.17, seed + 71);
    double max_rotation = .18 * PI / 180 * amount;
    double rotation = max_rotation * noise(tick * .71 + 27, seed + 997);
    double aspect = (double) camera->width / camera->height;
    double rot = fmax(cos(max_rotation) + sin(max_rotation) / aspect,
                      cos(max_rotation) + sin(max_rotation) * aspect);
    double overscan = amount > 0 ? rot / (1 - 2 * .024 * amount) + .002 * amount : 1;
    double scale = zoom * overscan;
    double cs = cos(rotation), sn = sin(rotation);
    double cx = .5 + (1 - 1 / scale) * (point_x - .5);
    double cy = .5 + (1 - 1 / scale) * (point_y - .5);
    if (c.tracking) {
        /* Place the tracked head at x=50%, y=38%; crop bounds still win. */
        double tracked_x = c.track_x - (-.12 * sn / aspect) / scale;
        double tracked_y = c.track_y - (-.12 * cs) / scale;
        cx += (tracked_x - cx) * c.track_quality;
        cy += (tracked_y - cy) * c.track_quality;
    }
    cx += dx / scale;
    cy += dy / scale;
    double ex = (fabs(cs) + fabs(sn) / aspect) / (2 * scale);
    double ey = (fabs(cs) + fabs(sn) * aspect) / (2 * scale);
    cx = soft_center(cx, ex, 1 - ex); cy = soft_center(cy, ey, 1 - ey);
    return (StudioCameraState) {scale, cx, cy, rotation, zoom, overscan};
}

static void render_rows(StudioCamera *camera, int first, int last)
{
    unsigned w = camera->width, h = camera->height;
    StudioCameraState s = camera->state;
    double cs = cos(s.rotation) / s.scale, sn = sin(s.rotation) / s.scale;
    double bx = s.center_x * w - .5, by = s.center_y * h - .5;
    for (int y = first; y < last; ++y) {
        double v = y + .5 - h * .5;
        double fx = bx + cs * (.5 - w * .5) + sn * v, fy = by - sn * (.5 - w * .5) + cs * v;
        uint8_t *out = camera->output + (size_t) y * w * 4;
        for (unsigned x = 0; x < w; ++x, fx += cs, fy -= sn, out += 4) {
            double xx = clampd(fx, 0, w - 1), yy = clampd(fy, 0, h - 1);
            unsigned ix = (unsigned) xx, iy = (unsigned) yy, jx = ix + 1 < w ? ix + 1 : ix, jy = iy + 1 < h ? iy + 1 : iy;
            float u = (float) (xx - ix), q = (float) (yy - iy);
            const uint8_t *p0 = camera->input + ((size_t) iy*w+ix)*4, *p1 = camera->input + ((size_t) iy*w+jx)*4;
            const uint8_t *p2 = camera->input + ((size_t) jy*w+ix)*4, *p3 = camera->input + ((size_t) jy*w+jx)*4;
            float q0=(1-u)*(1-q), q1=u*(1-q), q2=(1-u)*q, q3=u*q;
            if ((p0[3]&p1[3]&p2[3]&p3[3]) == 255) {
                for (int j=0;j<3;++j) out[j]=(uint8_t)(p0[j]*q0+p1[j]*q1+p2[j]*q2+p3[j]*q3+.5f);
                out[3]=255;
            } else {
                float a0=q0*p0[3],a1=q1*p1[3],a2=q2*p2[3],a3=q3*p3[3],alpha=a0+a1+a2+a3;
                out[3]=(uint8_t)(alpha+.5f);
                if(alpha>.0001f) for(int j=0;j<3;++j) out[j]=(uint8_t)clampd((p0[j]*a0+p1[j]*a1+p2[j]*a2+p3[j]*a3)/alpha+.5f,0,255);
                else out[0]=out[1]=out[2]=0;
            }
        }
    }
}

static void *worker(void *data)
{
    Worker *w=data; StudioCamera *c=w->camera; unsigned long last=0;
    pthread_mutex_lock(&c->lock);
    for (;;) {
        while(!c->quit && c->generation==last) pthread_cond_wait(&c->ready,&c->lock);
        if(c->quit) break;
        last=c->generation; int count=c->worker_count+1,id=w->id; pthread_mutex_unlock(&c->lock);
        render_rows(c,(int)c->height*id/count,(int)c->height*(id+1)/count);
        pthread_mutex_lock(&c->lock); if(--c->pending==0) pthread_cond_signal(&c->done);
    }
    pthread_mutex_unlock(&c->lock); return NULL;
}

StudioCamera *studio_camera_create(unsigned width, unsigned height)
{
    if(!width||!height||width>16384||height>16384||(size_t)width*height>67108864) return NULL;
    StudioCamera *c=calloc(1,sizeof(*c)); if(!c) return NULL; c->width=width;c->height=height;
    if(pthread_mutex_init(&c->api,NULL)||pthread_mutex_init(&c->lock,NULL)||pthread_cond_init(&c->ready,NULL)||pthread_cond_init(&c->done,NULL)){free(c);return NULL;}
    long cpus=sysconf(_SC_NPROCESSORS_ONLN);int count=(int)clampd(cpus-1,0,MAX_WORKERS);if((size_t)width*height<320*180)count=0;
    for(int i=0;i<count;++i){c->workers[i]=(Worker){c,i};if(pthread_create(c->threads+i,NULL,worker,c->workers+i))break;c->worker_count++;}
    return c;
}

void studio_camera_destroy(StudioCamera *c)
{
    if(!c)return;
    pthread_mutex_lock(&c->api);pthread_mutex_lock(&c->lock);c->quit=1;pthread_cond_broadcast(&c->ready);pthread_mutex_unlock(&c->lock);
    for(int i=0;i<c->worker_count;++i)pthread_join(c->threads[i],NULL);
    free(c->scratch);pthread_mutex_unlock(&c->api);
    pthread_cond_destroy(&c->ready);pthread_cond_destroy(&c->done);pthread_mutex_destroy(&c->lock);pthread_mutex_destroy(&c->api);free(c);
}

StudioCameraState studio_camera_state(StudioCamera *c,const StudioCameraConfig *config)
{ if(!c||!config)return(StudioCameraState){0};pthread_mutex_lock(&c->api);StudioCameraState result=calculate(c,config);pthread_mutex_unlock(&c->api);return result; }

int studio_camera_render(StudioCamera *c,const StudioCameraConfig *config,const uint8_t *input,uint8_t *output)
{
    if(!c||!config||!input||!output)return 0;
    pthread_mutex_lock(&c->api);size_t bytes=(size_t)c->width*c->height*4;c->state=calculate(c,config);
    if(c->state.scale==1&&c->state.center_x==.5&&c->state.center_y==.5&&c->state.rotation==0){if(input!=output)memcpy(output,input,bytes);pthread_mutex_unlock(&c->api);return 1;}
    if(input==output){if(!c->scratch)c->scratch=malloc(bytes);if(!c->scratch){pthread_mutex_unlock(&c->api);return 0;}memcpy(c->scratch,input,bytes);input=c->scratch;}
    c->input=input;c->output=output;
    if(c->worker_count){pthread_mutex_lock(&c->lock);c->pending=c->worker_count;c->generation++;pthread_cond_broadcast(&c->ready);pthread_mutex_unlock(&c->lock);render_rows(c,(int)c->height*c->worker_count/(c->worker_count+1),(int)c->height);pthread_mutex_lock(&c->lock);while(c->pending)pthread_cond_wait(&c->done,&c->lock);pthread_mutex_unlock(&c->lock);}else render_rows(c,0,(int)c->height);
    pthread_mutex_unlock(&c->api);return 1;
}
