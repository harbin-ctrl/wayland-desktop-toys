
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "lodepng.h"

#ifndef PI
#define PI 3.14159265358979323846
#endif

#define REF_SIZE 96.0
#define BRUSH_LEN 34.0      /* bristle head length, tip to round base */
#define BRUSH_W 10.0        /* head max half-width */
#define HANDLE_LEN (1.5 * BRUSH_LEN)
#define HANDLE_W 8.0        /* handle half-width */
#define PAINT_LEN 16.0      /* how far up the head the paint dip reaches */
#define TIP 14.5            /* tip position (x == y), brush centered in canvas */
#define AXIS_X 0.70710678   /* brush axis unit vector, tip -> handle */
#define AXIS_Y 0.70710678
#define LIGHT_X 0.94
#define LIGHT_Y 0.34
#define GLOSS_FACE_LO 0.25
#define GLOSS_FACE_HI 0.55

#define PAINT_R 225.0
#define PAINT_G 30.0
#define PAINT_B 35.0

static const int icon_sizes[] = {32, 48, 64, 128, 192, 256, 512, 1024};
#define NUM_SIZES ((int)(sizeof(icon_sizes) / sizeof(icon_sizes[0])))

static double clamp01(double v) {
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

static double bristle_sdf(double t, double s) {
    if (t < 0.0) return sqrt(t * t + s * s);
    if (t > BRUSH_LEN) {
        double dt = t - BRUSH_LEN;
        return sqrt(dt * dt + s * s) - BRUSH_W;
    }
    double h = BRUSH_W * pow(sin((t / BRUSH_LEN) * (PI / 2.0)), 0.75);
    return fabs(s) - h;
}

static double handle_sdf(double t, double s) {
    double a = HANDLE_LEN / 2.0;
    double dt = (t - (BRUSH_LEN + a - 3.0)) / a;
    double ds = s / HANDLE_W;
    return (sqrt(dt * dt + ds * ds) - 1.0) * HANDLE_W;
}

static void sample_brush(double rx, double ry,
                         double *out_r, double *out_g, double *out_b,
                         double *out_a) {
    rx -= TIP;
    ry -= TIP;
    double t = rx * AXIS_X + ry * AXIS_Y;
    double s = -rx * AXIS_Y + ry * AXIS_X;

    double d_b = bristle_sdf(t, s);
    double d_h = handle_sdf(t, s);
    double d = d_b < d_h ? d_b : d_h;

    double cov = clamp01(0.5 - d);
    *out_a = cov;
    if (cov <= 0.0) {
        *out_r = *out_g = *out_b = 0.0;
        return;
    }

    double fr, fg, fb;
    double d_sel;

    if (d_b < 0.0) {
        d_sel = d_b;

        double strand = 0.94 + 0.06 * sin(s * 1.9 + t * 0.25);
        fr = 182.0 * strand;
        fg = 182.0 * strand;
        fb = 188.0 * strand;

        double dip_edge = PAINT_LEN + 2.5 * (s / BRUSH_W) * (s / BRUSH_W);
        double pm = clamp01(dip_edge - t);

        if (pm > 0.0) {
            double gt = bristle_sdf(t + 0.5, s) - bristle_sdf(t - 0.5, s);
            double gs = bristle_sdf(t, s + 0.5) - bristle_sdf(t, s - 0.5);
            double gx = gt * AXIS_X - gs * AXIS_Y;
            double gy = gt * AXIS_Y + gs * AXIS_X;
            double glen = sqrt(gx * gx + gy * gy);
            double facing = glen > 1e-9
                ? (gx / glen) * LIGHT_X + (gy / glen) * LIGHT_Y : 0.0;

            double depth = -d_b;
            double band_in = clamp01((depth - 1.3) / 0.8);
            double band_out = clamp01((4.0 - depth) / 0.8);
            double lit = clamp01((facing - GLOSS_FACE_LO) /
                                 (GLOSS_FACE_HI - GLOSS_FACE_LO));
            double gloss = band_in * band_out * lit;

            double gt2 = t - 11.5, gs2 = s - 2.6;
            double ax = 4.0, ay = 0.8;
            double seg = clamp01((gt2 * ax + gs2 * ay) / (ax * ax + ay * ay));
            double qx = gt2 - seg * ax, qy = gs2 - seg * ay;
            double glint = clamp01((1.1 - sqrt(qx * qx + qy * qy)) / 0.7);
            gloss += (1.0 - gloss) * glint * 0.9;

            double pr = PAINT_R + (255.0 - PAINT_R) * gloss;
            double pg = PAINT_G + (255.0 - PAINT_G) * gloss;
            double pb = PAINT_B + (255.0 - PAINT_B) * gloss;
            fr = pr * pm + fr * (1.0 - pm);
            fg = pg * pm + fg * (1.0 - pm);
            fb = pb * pm + fb * (1.0 - pm);
        }
    } else {
        d_sel = d_h;

        fr = 196.0; fg = 120.0; fb = 58.0;

        double gt = handle_sdf(t + 0.5, s) - handle_sdf(t - 0.5, s);
        double gs = handle_sdf(t, s + 0.5) - handle_sdf(t, s - 0.5);
        double gx = gt * AXIS_X - gs * AXIS_Y;
        double gy = gt * AXIS_Y + gs * AXIS_X;
        double glen = sqrt(gx * gx + gy * gy);
        double facing = glen > 1e-9
            ? (gx / glen) * LIGHT_X + (gy / glen) * LIGHT_Y : 0.0;

        double depth = -d_h;
        double band_in = clamp01((depth - 1.0) / 0.8);
        double band_out = clamp01((3.2 - depth) / 0.8);
        double lit = clamp01((facing - GLOSS_FACE_LO) /
                             (GLOSS_FACE_HI - GLOSS_FACE_LO));
        double rim = band_in * band_out * lit * 0.5;

        fr += (255.0 - fr) * rim;
        fg += (255.0 - fg) * rim;
        fb += (255.0 - fb) * rim;
    }

    double ot = clamp01((d_sel + 1.2) / 0.8);
    if (d_b > 0.0 && d_h < 0.0) {
        double jt = clamp01((1.2 - d_b) / 0.8);
        if (jt > ot) ot = jt;
    }
    fr = fr * (1.0 - ot) + 42.0 * ot;
    fg = fg * (1.0 - ot) + 40.0 * ot;
    fb = fb * (1.0 - ot) + 46.0 * ot;

    *out_r = fr;
    *out_g = fg;
    *out_b = fb;
}

static int write_icon(int size) {
    unsigned char *img = malloc((size_t)size * size * 4);
    if (!img) return 0;

    double scale = REF_SIZE / size;   
    int ss = size < (int)REF_SIZE ? 4 : 2; 

    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            double ar = 0, ag = 0, ab = 0, aa = 0;
            for (int sy = 0; sy < ss; sy++) {
                for (int sx = 0; sx < ss; sx++) {
                    double u = (x + (sx + 0.5) / ss) * scale;
                    double v = (y + (sy + 0.5) / ss) * scale;
                    double r, g, b, a;
                    sample_brush(u, v, &r, &g, &b, &a);
                    ar += r * a; ag += g * a; ab += b * a; aa += a;
                }
            }
            double n = (double)ss * ss;
            unsigned char *p = img + ((size_t)y * size + x) * 4;
            if (aa > 0.0) {
                p[0] = (unsigned char)(ar / aa + 0.5);
                p[1] = (unsigned char)(ag / aa + 0.5);
                p[2] = (unsigned char)(ab / aa + 0.5);
                p[3] = (unsigned char)(aa / n * 255.0 + 0.5);
            } else {
                p[0] = p[1] = p[2] = p[3] = 0;
            }
        }
    }

    char name[64];
    snprintf(name, sizeof(name), "icon_%d.png", size);
    unsigned err = lodepng_encode32_file(name, img, size, size);
    free(img);
    if (err) {
        fprintf(stderr, "icon_maker: %s: %s\n", name, lodepng_error_text(err));
        return 0;
    }
    printf("wrote %s\n", name);
    return 1;
}

int main(void) {
    int ok = 1;
    for (int i = 0; i < NUM_SIZES; i++) {
        ok &= write_icon(icon_sizes[i]);
    }
    return ok ? 0 : 1;
}
