// Deterministic organic motion and projective math, adapted from CARD3D 1.0 (MIT).
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <algorithm>
using std::isfinite;
#define NT 256
#define NC 5
#define PI 3.1415926535897932384626433832795
#define RAD (PI/180.0)
#define CAMERA 3.6
struct Pose { double yaw,pitch,roll; };
struct Organic { unsigned w=1,h=1; double p[6]{}; double noise[NC][NT+1]{}; double integral[NC][NT+1]{}; };
static double clampd(double x, double lo, double hi) {
    return x < lo ? lo : x > hi ? hi : x;
}
static uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= UINT32_C(0x7feb352d);
    x ^= x >> 15; x *= UINT32_C(0x846ca68b);
    return x ^ (x >> 16);
}
static void init_noise(Organic *c) {
    /* Mean-zero periodic knots, each joined with a C2 quintic interpolation.
       The wrap has matching value, first and second derivatives. */
    for (int k = 0; k < NC; ++k) {
        double mean = 0.0, peak = 0.0;
        for (int n = 0; n < NT; ++n) {
            uint32_t h = hash32((uint32_t)n + (uint32_t)(k+1)*UINT32_C(0x9e3779b9));
            c->noise[k][n] = 2.0 * (double)h / 4294967295.0 - 1.0;
            mean += c->noise[k][n] / NT;
        }
        for (int n = 0; n < NT; ++n) {
            c->noise[k][n] -= mean;
            if (fabs(c->noise[k][n]) > peak) peak = fabs(c->noise[k][n]);
        }
        for (int n = 0; n < NT; ++n) c->noise[k][n] /= peak > 0 ? peak : 1;
        c->noise[k][NT] = c->noise[k][0];
        c->integral[k][0] = 0;
        for (int n = 0; n < NT; ++n)
            c->integral[k][n+1] = c->integral[k][n]
                + 0.5*(c->noise[k][n]+c->noise[k][n+1]);
    }
}
static double noise_at(const Organic *c, int channel, double x) {
    double cycle = floor(x / NT);
    double q = x - cycle * NT;
    if (q < 0) q = 0;
    if (q >= NT) q = 0;
    unsigned n = (unsigned)q;
    double f = q - n;
    double s = f*f*f*(f*(6*f-15)+10);
    return c->noise[channel][n]
        + (c->noise[channel][n+1]-c->noise[channel][n])*s;
}
static double integral_at(const Organic *c, int channel, double x) {
    double cycle = floor(x / NT);
    double q = x - cycle * NT;
    if (q < 0) q = 0;
    if (q >= NT) q = 0;
    unsigned n = (unsigned)q;
    double f = q - n, f2 = f*f, f4 = f2*f2;
    double si = f4*(f2-3*f+2.5); /* integral of 6f^5-15f^4+10f^3 */
    return cycle*c->integral[channel][NT] + c->integral[channel][n]
        + c->noise[channel][n]*f
        + (c->noise[channel][n+1]-c->noise[channel][n])*si;
}
static Pose pose_at(const Organic *c, double time) {
    double a = 30*c->p[0], j = c->p[2];
    double period = clampd(20*c->p[1], 2, 20);
    double t = isfinite(time) ? clampd(time, -1e9, 1e9) : 0;
    double u = t / period;
    /* Integrate bounded velocity noise instead of randomly changing frame offsets.
       d(phase)/du / (2*pi) is always between 1-.42*j and 1+.42*j. */
    double ph = 2*PI*(u + j*(
        .28*(integral_at(c,0,.55*u+.37)-integral_at(c,0,.37))/.55
       +.14*(integral_at(c,1,1.31*u+1.1)-integral_at(c,1,1.1))/1.31)) - .8;
    double envelope = 1 - .15*j + .15*j*noise_at(c,2,.33*u+2);
    Pose p;
    p.yaw = (70*c->p[3]-35) + a*envelope*sin(ph);
    p.pitch = (40*c->p[4]-20)
        + a*j*(.25*sin(.73*ph+1.2) + .07*noise_at(c,3,.93*u+8));
    p.roll = a*j*(.045*sin(.47*ph-.8) + .025*noise_at(c,4,1.09*u+.1));
    return p;
}
static void rotation(Pose p, double r[9]) {
    /* Rz(roll) * Ry(yaw) * Rx(pitch), in a coordinate system with y down.
       Positive z is towards the camera. Angles in the public UI are degrees. */
    double cy=cos(p.yaw*RAD), sy=sin(p.yaw*RAD);
    double cx=cos(p.pitch*RAD), sx=sin(p.pitch*RAD);
    double cz=cos(p.roll*RAD), sz=sin(p.roll*RAD);
    r[0]=cz*cy; r[1]=cz*sy*sx-sz*cx; r[2]=cz*sy*cx+sz*sx;
    r[3]=sz*cy; r[4]=sz*sy*sx+cz*cx; r[5]=sz*sy*cx-cz*sx;
    r[6]=-sy;   r[7]=cy*sx;             r[8]=cy*cx;
}
static int intervals(double radius) {
    return radius <= 1e-12 ? 0 : (int)ceil(2*radius); /* <= 1 degree per step */
}
static double safe_fit(Organic *c) {
    /* Compute one fixed scale for the entire possible pose envelope.
       Each unsampled pose is within the sum of half-grid angular steps of
       a sampled pose. r*delta bounds its 3D displacement (radians).
       Enlarging X,Y,Z by that amount conservatively bounds projection too.
       Thus no changing auto-fit is needed while playing. */
    double L = c->w > c->h ? c->w : c->h;
    double ax=c->w/L, ay=c->h/L, radius=sqrt(ax*ax+ay*ay);
    double a=30*c->p[0], b=.32*a*c->p[2], g=.07*a*c->p[2];
    int na=intervals(a), nb=intervals(b), ng=intervals(g);
    double da=na ? 2*a/na : 0, db=nb ? 2*b/nb : 0, dg=ng ? 2*g/ng : 0;
    double eps=radius*(da+db+dg)*.5*RAD;
    double maxx=ax, maxy=ay;
    for (int ia=0; ia<=na; ++ia)
    for (int ib=0; ib<=nb; ++ib)
    for (int ig=0; ig<=ng; ++ig) {
        Pose p = {(70*c->p[3]-35)-a+da*ia,
                  (40*c->p[4]-20)-b+db*ib, -g+dg*ig};
        double r[9]; rotation(p,r);
        for (int k=0;k<4;++k) {
            double x=(k&1)?ax:-ax, y=(k&2)?ay:-ay;
            double X=r[0]*x+r[1]*y, Y=r[3]*x+r[4]*y, Z=r[6]*x+r[7]*y;
            double denom=CAMERA-Z-eps;
            if (denom < .1) denom=.1;
            double xp=CAMERA*(fabs(X)+eps)/denom;
            double yp=CAMERA*(fabs(Y)+eps)/denom;
            if (xp>maxx) maxx=xp;
            if (yp>maxy) maxy=yp;
        }
    }
    double f=fmin(ax/maxx,ay/maxy);
    return clampd(f, .02, 1);
}
static int inverse3(const double m[9], double inv[9]) {
    inv[0]=m[4]*m[8]-m[5]*m[7]; inv[1]=m[2]*m[7]-m[1]*m[8];
    inv[2]=m[1]*m[5]-m[2]*m[4]; inv[3]=m[5]*m[6]-m[3]*m[8];
    inv[4]=m[0]*m[8]-m[2]*m[6]; inv[5]=m[2]*m[3]-m[0]*m[5];
    inv[6]=m[3]*m[7]-m[4]*m[6]; inv[7]=m[1]*m[6]-m[0]*m[7];
    inv[8]=m[0]*m[4]-m[1]*m[3];
    double d=m[0]*inv[0]+m[1]*inv[3]+m[2]*inv[6];
    if (!isfinite(d) || fabs(d)<1e-14) return 0;
    for (int i=0;i<9;++i) inv[i]/=d;
    return 1;
}
