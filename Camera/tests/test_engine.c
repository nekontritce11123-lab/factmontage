#include "../src/camera.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static StudioCameraConfig config(void){return(StudioCameraConfig){.mode=0,.live=0,.zoom=1.2,.start_x=.35,.start_y=.5,.end_x=.65,.end_y=.5,.cycle_period=6};}
int main(void)
{
    StudioCamera *camera=studio_camera_create(160,90);assert(camera);StudioCameraConfig c=config();c.zoom=1;c.end_x=c.end_y=.5;
    size_t bytes=160*90*4;uint8_t *input=malloc(bytes),*output=malloc(bytes);for(size_t i=0;i<bytes;++i)input[i]=(uint8_t)(i*17);
    assert(studio_camera_render(camera,&c,input,output));assert(!memcmp(input,output,bytes));
    c=config();c.mode=1;c.progress=0;StudioCameraState a=studio_camera_state(camera,&c);c.progress=1;StudioCameraState b=studio_camera_state(camera,&c);assert(fabs(a.user_zoom-1)<1e-12);assert(fabs(b.user_zoom-1.2)<1e-12);
    c.mode=4;c.progress=0;a=studio_camera_state(camera,&c);c.progress=1;b=studio_camera_state(camera,&c);assert(a.center_x<b.center_x);
    c.mode=3;c.progress=.25;a=studio_camera_state(camera,&c);c.progress=.75;b=studio_camera_state(camera,&c);assert(fabs(a.user_zoom-b.user_zoom)<1e-12);
    c.live=1;c.time=4.25;a=studio_camera_state(camera,&c);b=studio_camera_state(camera,&c);assert(!memcmp(&a,&b,sizeof(a)));
    c.mode=4;c.progress=.4;c.time=4;a=studio_camera_state(camera,&c);c.progress=(120+80)/500.;c.time=(120+80)/50.;b=studio_camera_state(camera,&c);assert(!memcmp(&a,&b,sizeof(a)));
    c=config();c.mode=5;c.live=0;c.cycle_period=6;c.time=0;a=studio_camera_state(camera,&c);
    c.time=3;b=studio_camera_state(camera,&c);assert(fabs(a.user_zoom-1)<1e-12);assert(fabs(b.user_zoom-1.2)<1e-12);
    c.time=6;b=studio_camera_state(camera,&c);assert(fabs(a.user_zoom-b.user_zoom)<1e-12);
    c.time=86400+1.5;a=studio_camera_state(camera,&c);c.time=1.5;b=studio_camera_state(camera,&c);assert(fabs(a.user_zoom-b.user_zoom)<1e-12);
    c=config();c.live=0;c.tracking=1;c.track_x=.3;c.track_y=.25;c.track_quality=1;
    a=studio_camera_state(camera,&c);assert(a.center_x<.5&&a.center_y<.5);
    c.track_x=.7;c.track_y=.75;b=studio_camera_state(camera,&c);assert(b.center_x>a.center_x&&b.center_y>a.center_y);
    c.track_quality=0;a=studio_camera_state(camera,&c);c.tracking=0;b=studio_camera_state(camera,&c);assert(fabs(a.center_x-b.center_x)<1e-12&&fabs(a.center_y-b.center_y)<1e-12);
    studio_camera_destroy(camera);free(input);free(output);return 0;
}
