
#ifndef SPRAYCAN_H
#define SPRAYCAN_H

#include <math.h>

#ifndef PI
#define PI 3.14159265358979323846
#endif

#define SPRAYCAN_AXIS_X 0.70710678
#define SPRAYCAN_AXIS_Y 0.70710678

#define SPRAYCAN_T_MIN (-3.0)
#define SPRAYCAN_T_MAX 87.0
#define SPRAYCAN_S_MAX 17.0

#define SPRAYCAN_BODY_HW 13.0   // body half-width

#define SPRAYCAN_LIGHT_X 0.94
#define SPRAYCAN_LIGHT_Y 0.34

static inline double spraycan_clamp01_(double v) {
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

static inline double spraycan_hash_(double x, double y) {
    double v = sin(x * 127.1 + y * 311.7) * 43758.5453;
    return v - floor(v);
}

static inline double spraycan_rrect_(double t, double s,
                                     double t0, double t1,
                                     double hw, double rad) {
    double ct = (t0 + t1) * 0.5;
    double ht = (t1 - t0) * 0.5 - rad;
    double hs = hw - rad;
    double dt = fabs(t - ct) - ht;
    double ds = fabs(s) - hs;
    double ax = dt > 0.0 ? dt : 0.0;
    double ay = ds > 0.0 ? ds : 0.0;
    double inner = dt > ds ? dt : ds;
    if (inner > 0.0) inner = 0.0;
    return sqrt(ax * ax + ay * ay) + inner - rad;
}

#define SPRAYCAN_NOZ_T0 13.0
#define SPRAYCAN_NOZ_T1 23.0
#define SPRAYCAN_CAP_T0 20.0
#define SPRAYCAN_CAP_T1 34.0
#define SPRAYCAN_BODY_T0 33.0
#define SPRAYCAN_BODY_T1 84.0
#define SPRAYCAN_DROP_TIP 40.0   // teardrop decal: pointed end (toward cap)
#define SPRAYCAN_DROP_BASE 51.0  // compact decal leaves breathing room for the Pi stamp
#define SPRAYCAN_DROP_R 8.0

static inline double spraycan_cap_hw_(double t) {
    double u = (t - SPRAYCAN_CAP_T0) / (SPRAYCAN_CAP_T1 - SPRAYCAN_CAP_T0);
    if (u < 0.0) u = 0.0;
    if (u > 1.0) u = 1.0;
    return SPRAYCAN_BODY_HW * pow(sin(u * (PI / 2.0)), 0.85);
}

static inline double spraycan_drop_sdf_(double t, double s) {
    if (t < SPRAYCAN_DROP_TIP) {
        double dt = t - SPRAYCAN_DROP_TIP;
        return sqrt(dt * dt + s * s);
    }
    if (t > SPRAYCAN_DROP_BASE) {
        double dt = t - SPRAYCAN_DROP_BASE;
        return sqrt(dt * dt + s * s) - SPRAYCAN_DROP_R;
    }
    double u = (t - SPRAYCAN_DROP_TIP) / (SPRAYCAN_DROP_BASE - SPRAYCAN_DROP_TIP);
    double hw = SPRAYCAN_DROP_R * pow(sin(u * (PI / 2.0)), 0.75);
    return fabs(s) - hw;
}

static inline void spraycan_sample(double t, double s,
                                   double paint_r, double paint_g, double paint_b,
                                   int with_mist,
                                   double *out_r, double *out_g, double *out_b,
                                   double *out_a) {
    double d_noz = spraycan_rrect_(t, s, SPRAYCAN_NOZ_T0, SPRAYCAN_NOZ_T1, 3.6, 1.5);
    double d_body = spraycan_rrect_(t, s, SPRAYCAN_BODY_T0, SPRAYCAN_BODY_T1,
                                    SPRAYCAN_BODY_HW, 2.8);
    double d_cap = 1e9;
    if (t > SPRAYCAN_CAP_T0 && t < SPRAYCAN_CAP_T1 + 1.0) {
        d_cap = fabs(s) - spraycan_cap_hw_(t);
    }

    double d_solid = d_noz;
    int region = 0;                            
    if (d_cap < d_solid) { d_solid = d_cap; region = 1; }
    if (d_body < d_solid) { d_solid = d_body; region = 2; }

    double cov = spraycan_clamp01_(0.5 - d_solid);   

    double fr = 0.0, fg = 0.0, fb = 0.0;
    if (cov > 0.0) {
        if (region == 0) {
            fr = 150.0; fg = 152.0; fb = 158.0;
            if (t < 15.4) { fr = 96.0; fg = 98.0; fb = 104.0; }
            if (t > 19.4 && t < 20.6) { fr *= 0.8; fg *= 0.8; fb *= 0.8; }
        } else if (region == 1) {
            double hw = spraycan_cap_hw_(t);
            double k = hw > 0.01 ? fabs(s) / hw : 1.0;
            double shade = 1.0 - 0.22 * k * k;   
            fr = paint_r * shade;
            fg = paint_g * shade;
            fb = paint_b * shade;
            double gx = t - 27.0, gy = s + 4.6;
            double gl = spraycan_clamp01_((2.7 - sqrt(gx * gx + gy * gy)) / 1.2);
            fr += (255.0 - fr) * 0.85 * gl;
            fg += (255.0 - fg) * 0.85 * gl;
            fb += (255.0 - fb) * 0.85 * gl;
        } else {
            double k = fabs(s) / SPRAYCAN_BODY_HW;
            double shade = 1.0 - 0.28 * k * k * k;
            fr = 206.0 * shade; fg = 208.0 * shade; fb = 213.0 * shade;
            double streak = exp(-((s + 6.0) * (s + 6.0)) / (2.6 * 2.6)) * 0.45;
            fr += (255.0 - fr) * streak;
            fg += (255.0 - fg) * streak;
            fb += (255.0 - fb) * streak;

            if (t < 36.0) {
                fr *= 0.86; fg *= 0.86; fb *= 0.86;
                if (t > 33.6 && t < 34.8 && s < 0.0) {
                    fr += (255.0 - fr) * 0.35;
                    fg += (255.0 - fg) * 0.35;
                    fb += (255.0 - fb) * 0.35;
                }
            } else if (t > 80.0) {
                fr *= 0.84; fg *= 0.84; fb *= 0.84;
            } else if (t > 78.5) {
                fr *= 0.94; fg *= 0.94; fb *= 0.94;
            }

            double dd = spraycan_drop_sdf_(t, s);
            if (dd < 0.8) {
                double inside = spraycan_clamp01_((0.0 - dd) / 0.7);
                if (inside > 0.0) {
                    double edgek = spraycan_clamp01_(1.0 + dd / 3.0);
                    double m = 1.0 - 0.15 * edgek;
                    double dr = paint_r * m, dg = paint_g * m, db = paint_b * m;
                    double gx = t - 63.0, gy = s + 3.2;
                    double gl = spraycan_clamp01_((2.4 - sqrt(gx * gx + gy * gy)) / 1.1);
                    dr += (255.0 - dr) * 0.85 * gl;
                    dg += (255.0 - dg) * 0.85 * gl;
                    db += (255.0 - db) * 0.85 * gl;
                    fr = fr * (1.0 - inside) + dr * inside;
                    fg = fg * (1.0 - inside) + dg * inside;
                    fb = fb * (1.0 - inside) + db * inside;
                }
                double oa = spraycan_clamp01_((0.8 - fabs(dd)) / 0.6) * 0.85;
                fr = fr * (1.0 - oa) + 52.0 * oa;
                fg = fg * (1.0 - oa) + 50.0 * oa;
                fb = fb * (1.0 - oa) + 56.0 * oa;
            }
        }

        double ot = spraycan_clamp01_((d_solid + 1.4) / 0.9);
        fr = fr * (1.0 - ot) + 42.0 * ot;
        fg = fg * (1.0 - ot) + 40.0 * ot;
        fb = fb * (1.0 - ot) + 46.0 * ot;
    }

    double ma = 0.0;
    if (with_mist && cov < 1.0 && t > -2.0 && t < 16.0) {
        double h = t >= 0.0 ? 8.0 - t * 0.42     
                            : 8.0 + t * 3.0;     
        if (h > 0.0) {
            double edge = spraycan_clamp01_((h - fabs(s)) / 2.2);
            double dens = 0.35 + 0.65 * spraycan_clamp01_(t / 16.0);
            double grain = 0.55 + 0.75 * spraycan_hash_(t * 1.7, s * 1.7);
            ma = 0.34 * edge * dens * grain;
            if (ma > 1.0) ma = 1.0;
        }
    }

    double a = cov + ma * (1.0 - cov);
    if (a > 0.0) {
        double mw = ma * (1.0 - cov);
        *out_r = (fr * cov + paint_r * mw) / a;
        *out_g = (fg * cov + paint_g * mw) / a;
        *out_b = (fb * cov + paint_b * mw) / a;
    } else {
        *out_r = *out_g = *out_b = 0.0;
    }
    *out_a = a;
}

#endif // SPRAYCAN_H
