#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "lodepng.h"

#ifndef PI
#define PI 3.14159265358979323846
#endif

#define DEFAULT_TILT_DEGREES 15.0
#define LON_TILES 16
#define LAT_TILES 8

#define COLOR_LIGHT_R 131
#define COLOR_LIGHT_G 233
#define COLOR_LIGHT_B 255
#define COLOR_DARK_R 41
#define COLOR_DARK_G 173
#define COLOR_DARK_B 255

#define COLOR_BG_R 86
#define COLOR_BG_G 78
#define COLOR_BG_B 71

#define ADAPTIVE_SAFE_ZONE (66.0 / 108.0)

static const int icon_sizes[] = {32, 48, 64, 72, 96, 128, 144, 192, 256, 512, 1024};

static double axis_ax, axis_ay, axis_az;
static double axis_ux, axis_uy, axis_uz;
static double axis_vx, axis_vy, axis_vz;

static double clampd(double value, double min_val, double max_val) {
    if (value < min_val) {
        return min_val;
    }
    if (value > max_val) {
        return max_val;
    }
    return value;
}

static void compute_axis_vectors(double tilt_degrees) {
    double tilt_rad = tilt_degrees * PI / 180.0;
    double s = sin(tilt_rad);
    double c = cos(tilt_rad);

    axis_ax = s;
    axis_ay = c;
    axis_az = 0.0;

    double hx = 0.0;
    double hy = 0.0;
    double hz = 1.0;
    if (fabs(axis_ax * hx + axis_ay * hy + axis_az * hz) > 0.99) {
        hx = 1.0;
        hy = 0.0;
        hz = 0.0;
    }

    axis_ux = hy * axis_az - hz * axis_ay;
    axis_uy = hz * axis_ax - hx * axis_az;
    axis_uz = hx * axis_ay - hy * axis_ax;
    {
        double ulen = sqrt(axis_ux * axis_ux + axis_uy * axis_uy + axis_uz * axis_uz);
        axis_ux /= ulen;
        axis_uy /= ulen;
        axis_uz /= ulen;
    }

    axis_vx = axis_ay * axis_uz - axis_az * axis_uy;
    axis_vy = axis_az * axis_ux - axis_ax * axis_uz;
    axis_vz = axis_ax * axis_uy - axis_ay * axis_ux;
}

static void render_ball_rgba(unsigned char *rgba, unsigned width, unsigned height) {
    unsigned x;
    unsigned y;
    double radius = (double)(width < height ? width : height) / 2.0;
    double cx = (double)width / 2.0 - 0.5;
    double cy = (double)height / 2.0 - 0.5;
    double light_x = -0.3;
    double light_y = -0.5;
    double light_z = 1.0;
    double light_len = sqrt(light_x * light_x + light_y * light_y + light_z * light_z);
    double ambient = 0.3;
    double diffuse = 0.7;
    double half_pi = PI * 0.5;
    double two_pi = PI * 2.0;

    light_x /= light_len;
    light_y /= light_len;
    light_z /= light_len;

    memset(rgba, 0, (size_t)width * (size_t)height * 4u);

    for (y = 0; y < height; ++y) {
        double dy = (double)y - cy;
        double rem = radius * radius - dy * dy;
        if (rem < 0.0) {
            continue;
        }

        double dx = sqrt(rem);
        int xl = (int)ceil(cx - dx);
        int xr = (int)floor(cx + dx);
        if (xl < 0) {
            xl = 0;
        }
        if (xr >= (int)width) {
            xr = (int)width - 1;
        }

        for (x = (unsigned)xl; x <= (unsigned)xr; ++x) {
            double vx = ((double)x - cx) / radius;
            double vy = dy / radius;
            double r2 = vx * vx + vy * vy;
            if (r2 > 1.0) {
                continue;
            }

            double nz = sqrt(1.0 - r2);
            double nx = vx;
            double ny = vy;
            double dot_product = nx * light_x + ny * light_y + nz * light_z;
            double brightness = ambient + diffuse * fmax(0.0, dot_product);
            double na;
            double nu;
            double nv;
            double lat;
            double lon;
            int v;
            int u;
            bool is_light;
            int r;
            int g;
            int b;
            size_t idx;

            brightness = fmin(1.0, brightness);
            brightness = fmax(0.35, brightness);

            na = nx * axis_ax + ny * axis_ay + nz * axis_az;
            nu = nx * axis_ux + ny * axis_uy + nz * axis_uz;
            nv = nx * axis_vx + ny * axis_vy + nz * axis_vz;

            lat = asin(clampd(na, -1.0, 1.0));
            lon = atan2(nv, nu);

            v = (int)floor(((lat + half_pi) / PI) * LAT_TILES);
            if (v < 0) {
                v = 0;
            }
            if (v >= LAT_TILES) {
                v = LAT_TILES - 1;
            }

            u = (int)floor(((lon + PI) / two_pi) * LON_TILES);
            u %= LON_TILES;
            if (u < 0) {
                u += LON_TILES;
            }

            is_light = ((u + v) & 1) == 0;

            if (is_light) {
                r = (int)(COLOR_LIGHT_R * brightness);
                g = (int)(COLOR_LIGHT_G * brightness);
                b = (int)(COLOR_LIGHT_B * brightness);
            } else {
                r = (int)(COLOR_DARK_R * brightness);
                g = (int)(COLOR_DARK_G * brightness);
                b = (int)(COLOR_DARK_B * brightness);
            }

            idx = ((size_t)y * (size_t)width + (size_t)x) * 4u;
            rgba[idx + 0] = (unsigned char)clampd((double)r, 0.0, 255.0);
            rgba[idx + 1] = (unsigned char)clampd((double)g, 0.0, 255.0);
            rgba[idx + 2] = (unsigned char)clampd((double)b, 0.0, 255.0);
            rgba[idx + 3] = 255;
        }
    }
}

static void render_ball_full_rgba(unsigned char *rgba, unsigned size) {
    double radius = (double)size / 2.0;
    double cx = (double)size / 2.0 - 0.5;
    double cy = (double)size / 2.0 - 0.5;
    unsigned x;
    unsigned y;

    render_ball_rgba(rgba, size, size);

    for (y = 0; y < size; ++y) {
        for (x = 0; x < size; ++x) {
            double dx = (double)x - cx;
            double dy = (double)y - cy;
            double r = sqrt(dx * dx + dy * dy);
            double alpha;
            size_t idx;
            if (r < radius - 1.5) {
                continue;
            }
            alpha = clampd(radius + 0.5 - r, 0.0, 1.0);
            idx = ((size_t)y * (size_t)size + (size_t)x) * 4u;
            rgba[idx + 3] = (unsigned char)(alpha * 255.0 + 0.5);
        }
    }
}

static int write_icon_png(unsigned size, const char *filename) {
    unsigned char *rgba;
    unsigned error;

    if (!filename || size == 0) {
        return 1;
    }

    rgba = (unsigned char *)malloc((size_t)size * (size_t)size * 4u);
    if (!rgba) {
        fprintf(stderr, "Failed to allocate icon buffer for %u\n", size);
        return 1;
    }

    render_ball_full_rgba(rgba, size);
    error = lodepng_encode32_file(filename, rgba, size, size);
    free(rgba);

    if (error != 0) {
        fprintf(stderr, "Failed to write %s: %u: %s\n",
                filename, error, lodepng_error_text(error));
        return 1;
    }

    return 0;
}

static int write_foreground_png(unsigned size, const char *filename) {
    unsigned char *rgba;
    unsigned char *ball;
    unsigned ball_size;
    unsigned offset;
    unsigned y;
    unsigned error;

    if (!filename || size == 0) {
        return 1;
    }

    ball_size = (unsigned)((double)size * ADAPTIVE_SAFE_ZONE + 0.5);
    if (ball_size == 0) {
        ball_size = 1;
    }
    offset = (size - ball_size) / 2;

    rgba = (unsigned char *)calloc((size_t)size * (size_t)size, 4u);
    ball = (unsigned char *)malloc((size_t)ball_size * (size_t)ball_size * 4u);
    if (!rgba || !ball) {
        fprintf(stderr, "Failed to allocate icon buffer for %u\n", size);
        free(rgba);
        free(ball);
        return 1;
    }

    render_ball_full_rgba(ball, ball_size);
    for (y = 0; y < ball_size; ++y) {
        memcpy(rgba + (((size_t)(y + offset) * size + offset) * 4u),
               ball + ((size_t)y * ball_size * 4u),
               (size_t)ball_size * 4u);
    }
    free(ball);

    error = lodepng_encode32_file(filename, rgba, size, size);
    free(rgba);

    if (error != 0) {
        fprintf(stderr, "Failed to write %s: %u: %s\n",
                filename, error, lodepng_error_text(error));
        return 1;
    }

    return 0;
}

static int write_background_png(unsigned size, const char *filename) {
    unsigned char *rgba;
    size_t i;
    size_t count;
    unsigned error;

    if (!filename || size == 0) {
        return 1;
    }

    count = (size_t)size * (size_t)size;
    rgba = (unsigned char *)malloc(count * 4u);
    if (!rgba) {
        fprintf(stderr, "Failed to allocate icon buffer for %u\n", size);
        return 1;
    }

    for (i = 0; i < count; ++i) {
        rgba[i * 4u + 0] = COLOR_BG_R;
        rgba[i * 4u + 1] = COLOR_BG_G;
        rgba[i * 4u + 2] = COLOR_BG_B;
        rgba[i * 4u + 3] = 255;
    }

    error = lodepng_encode32_file(filename, rgba, size, size);
    free(rgba);

    if (error != 0) {
        fprintf(stderr, "Failed to write %s: %u: %s\n",
                filename, error, lodepng_error_text(error));
        return 1;
    }

    return 0;
}

static int generate_default_icon_set(void) {
    size_t i;
    for (i = 0; i < sizeof(icon_sizes) / sizeof(icon_sizes[0]); ++i) {
        char filename[64];
        snprintf(filename, sizeof(filename), "icon_%d.png", icon_sizes[i]);
        if (write_icon_png((unsigned)icon_sizes[i], filename) != 0) {
            return 1;
        }
        printf("Saved %s\n", filename);
    }
    return 0;
}

static void print_usage(const char *argv0) {
    fprintf(stderr,
            "Usage: %s [SIZE OUTPUT.png [MODE]]\n"
            "Renders the ball icon (full size, transparent background).\n"
            "MODE selects an Android adaptive-icon layer instead:\n"
            "  foreground   ball scaled to the adaptive safe zone, transparent elsewhere\n"
            "  background   solid backdrop in the app's background color\n"
            "Without arguments, writes the default PNG icon set in the current directory.\n",
            argv0 ? argv0 : "icon_maker");
}

int main(int argc, char *argv[]) {
    compute_axis_vectors(DEFAULT_TILT_DEGREES);

    if (argc == 1) {
        return generate_default_icon_set();
    }

    if (argc != 3 && argc != 4) {
        print_usage(argv[0]);
        return 1;
    }

    {
        char *endptr = NULL;
        long requested = strtol(argv[1], &endptr, 10);
        if (!endptr || *endptr != '\0' || requested <= 0 || requested > 4096) {
            fprintf(stderr, "Invalid icon size: %s\n", argv[1]);
            return 1;
        }
        if (argc == 4) {
            if (strcmp(argv[3], "foreground") == 0) {
                return write_foreground_png((unsigned)requested, argv[2]);
            }
            if (strcmp(argv[3], "background") == 0) {
                return write_background_png((unsigned)requested, argv[2]);
            }
            fprintf(stderr, "Invalid mode: %s\n", argv[3]);
            print_usage(argv[0]);
            return 1;
        }
        return write_icon_png((unsigned)requested, argv[2]);
    }
}
