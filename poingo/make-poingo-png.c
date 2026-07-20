
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

#define APNG_FPS 60
#define ROT_PERIOD 0.5
#define ROT_FRAMES ((int)(APNG_FPS * ROT_PERIOD * (LON_TILES / 2) * (2.0 / LON_TILES) + 0.5))
#define ROT_ANGLE_PERIOD (4.0 * PI / LON_TILES)

#define GRAVITY 0.00014

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

static void render_ball_rgba(unsigned char *rgba, unsigned width, unsigned height,
                             double cx, double cy, double radius,
                             double phase_offset) {
    unsigned x;
    unsigned y;
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
        if (xl > xr) {
            continue;
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
            lon = atan2(nv, nu) + phase_offset;

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

    for (y = 0; y < height; ++y) {
        double dy = (double)y - cy;
        for (x = 0; x < width; ++x) {
            double dx2 = (double)x - cx;
            double r = sqrt(dx2 * dx2 + dy * dy);
            double alpha;
            size_t idx;
            if (r < radius - 1.5 || r > radius + 1.5) {
                continue;
            }
            alpha = clampd(radius + 0.5 - r, 0.0, 1.0);
            idx = ((size_t)y * (size_t)width + (size_t)x) * 4u;
            rgba[idx + 3] = (unsigned char)((double)rgba[idx + 3] * alpha + 0.5);
        }
    }
}



typedef struct {
    unsigned char *data;
    size_t size;
    size_t cap;
} byte_buf;

static int buf_append(byte_buf *buf, const void *bytes, size_t len) {
    if (buf->size + len > buf->cap) {
        size_t new_cap = buf->cap ? buf->cap * 2 : 65536;
        unsigned char *grown;
        while (new_cap < buf->size + len) {
            new_cap *= 2;
        }
        grown = (unsigned char *)realloc(buf->data, new_cap);
        if (!grown) {
            return 1;
        }
        buf->data = grown;
        buf->cap = new_cap;
    }
    memcpy(buf->data + buf->size, bytes, len);
    buf->size += len;
    return 0;
}

static void put_be32(unsigned char *out, uint32_t value) {
    out[0] = (unsigned char)(value >> 24);
    out[1] = (unsigned char)(value >> 16);
    out[2] = (unsigned char)(value >> 8);
    out[3] = (unsigned char)value;
}

static void put_be16(unsigned char *out, uint16_t value) {
    out[0] = (unsigned char)(value >> 8);
    out[1] = (unsigned char)value;
}

static int buf_append_chunk(byte_buf *buf, const char type[4],
                            const unsigned char *data, size_t len) {
    unsigned char head[8];
    unsigned char crc_bytes[4];
    unsigned crc;

    put_be32(head, (uint32_t)len);
    memcpy(head + 4, type, 4);
    if (buf_append(buf, head, 8) != 0) {
        return 1;
    }

    {
        unsigned char *crc_input = (unsigned char *)malloc(len + 4);
        if (!crc_input) {
            return 1;
        }
        memcpy(crc_input, type, 4);
        if (len > 0) {
            memcpy(crc_input + 4, data, len);
        }
        crc = lodepng_crc32(crc_input, len + 4);
        free(crc_input);
    }

    if (len > 0 && buf_append(buf, data, len) != 0) {
        return 1;
    }
    put_be32(crc_bytes, crc);
    return buf_append(buf, crc_bytes, 4);
}

static int png_extract(const unsigned char *png, size_t png_size,
                       unsigned char ihdr_out[13],
                       unsigned char **idat_out, size_t *idat_size_out) {
    size_t pos = 8; 
    byte_buf idat = {NULL, 0, 0};
    bool have_ihdr = false;

    while (pos + 12 <= png_size) {
        uint32_t len = ((uint32_t)png[pos] << 24) | ((uint32_t)png[pos + 1] << 16) |
                       ((uint32_t)png[pos + 2] << 8) | (uint32_t)png[pos + 3];
        const unsigned char *type = png + pos + 4;
        const unsigned char *data = png + pos + 8;

        if (pos + 12 + (size_t)len > png_size) {
            break;
        }

        if (memcmp(type, "IHDR", 4) == 0 && len == 13) {
            if (ihdr_out) {
                memcpy(ihdr_out, data, 13);
            }
            have_ihdr = true;
        } else if (memcmp(type, "IDAT", 4) == 0) {
            if (buf_append(&idat, data, len) != 0) {
                free(idat.data);
                return 1;
            }
        } else if (memcmp(type, "IEND", 4) == 0) {
            break;
        }

        pos += 12 + (size_t)len;
    }

    if (!have_ihdr || idat.size == 0) {
        free(idat.data);
        return 1;
    }

    *idat_out = idat.data;
    *idat_size_out = idat.size;
    return 0;
}

typedef struct {
    byte_buf out;
    unsigned width;
    unsigned height;
    uint16_t delay_num;
    uint16_t delay_den;
    int total_frames;
    int frames_written;
    uint32_t seq;
} ApngWriter;

static unsigned encode_frame_png(unsigned char **png, size_t *png_size,
                                 const unsigned char *rgba,
                                 unsigned width, unsigned height) {
    LodePNGState state;
    unsigned error;

    lodepng_state_init(&state);
    state.info_raw.colortype = LCT_RGBA;
    state.info_raw.bitdepth = 8;
    state.info_png.color.colortype = LCT_RGBA;
    state.info_png.color.bitdepth = 8;
    state.encoder.auto_convert = 0;
    error = lodepng_encode(png, png_size, rgba, width, height, &state);
    lodepng_state_cleanup(&state);
    return error;
}

static int apng_begin(ApngWriter *wr, unsigned width, unsigned height,
                      int total_frames, uint16_t delay_num, uint16_t delay_den) {
    static const unsigned char png_sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    unsigned char ihdr[13];
    unsigned char actl[8];

    memset(wr, 0, sizeof(*wr));
    wr->width = width;
    wr->height = height;
    wr->delay_num = delay_num;
    wr->delay_den = delay_den;
    wr->total_frames = total_frames;

    put_be32(ihdr, width);
    put_be32(ihdr + 4, height);
    ihdr[8] = 8;  
    ihdr[9] = 6;  
    ihdr[10] = 0; 
    ihdr[11] = 0; 
    ihdr[12] = 0; 

    put_be32(actl, (uint32_t)total_frames);
    put_be32(actl + 4, 0); 

    if (buf_append(&wr->out, png_sig, 8) != 0 ||
        buf_append_chunk(&wr->out, "IHDR", ihdr, 13) != 0 ||
        buf_append_chunk(&wr->out, "acTL", actl, 8) != 0) {
        free(wr->out.data);
        return 1;
    }
    return 0;
}

#define APNG_DISPOSE_OP_NONE 0
#define APNG_DISPOSE_OP_BACKGROUND 1

static int apng_add_frame_region(ApngWriter *wr, const unsigned char *rgba,
                                 unsigned x, unsigned y, unsigned w, unsigned h,
                                 unsigned char dispose_op) {
    unsigned char *png = NULL;
    size_t png_size = 0;
    unsigned char *idat = NULL;
    size_t idat_size = 0;
    unsigned char fctl[26];
    unsigned error;
    int failed = 0;

    if (x + w > wr->width || y + h > wr->height || w == 0 || h == 0) {
        fprintf(stderr, "Frame %d region %ux%u+%u+%u exceeds canvas %ux%u\n",
                wr->frames_written, w, h, x, y, wr->width, wr->height);
        return 1;
    }
    if (wr->frames_written == 0 && (x != 0 || y != 0 || w != wr->width || h != wr->height)) {
        fprintf(stderr, "First frame must cover the full canvas\n");
        return 1;
    }

    error = encode_frame_png(&png, &png_size, rgba, w, h);
    if (error != 0) {
        fprintf(stderr, "Failed to encode frame %d: %u: %s\n",
                wr->frames_written, error, lodepng_error_text(error));
        return 1;
    }

    if (png_extract(png, png_size, NULL, &idat, &idat_size) != 0) {
        fprintf(stderr, "Failed to parse encoded frame %d\n", wr->frames_written);
        free(png);
        return 1;
    }
    free(png);

    put_be32(fctl, wr->seq++);
    put_be32(fctl + 4, w);
    put_be32(fctl + 8, h);
    put_be32(fctl + 12, x);
    put_be32(fctl + 16, y);
    put_be16(fctl + 20, wr->delay_num);
    put_be16(fctl + 22, wr->delay_den);
    fctl[24] = dispose_op;
    fctl[25] = 0; 
    if (buf_append_chunk(&wr->out, "fcTL", fctl, 26) != 0) {
        free(idat);
        return 1;
    }

    if (wr->frames_written == 0) {
        failed = buf_append_chunk(&wr->out, "IDAT", idat, idat_size);
    } else {
        unsigned char *fdat = (unsigned char *)malloc(idat_size + 4);
        if (!fdat) {
            free(idat);
            return 1;
        }
        put_be32(fdat, wr->seq++);
        memcpy(fdat + 4, idat, idat_size);
        failed = buf_append_chunk(&wr->out, "fdAT", fdat, idat_size + 4);
        free(fdat);
    }
    free(idat);
    if (failed) {
        return 1;
    }

    wr->frames_written++;
    return 0;
}

static int apng_add_frame(ApngWriter *wr, const unsigned char *rgba) {
    return apng_add_frame_region(wr, rgba, 0, 0, wr->width, wr->height,
                                 APNG_DISPOSE_OP_NONE);
}

static int apng_finish(ApngWriter *wr, const char *filename) {
    FILE *fp;
    int result = 1;

    if (wr->frames_written != wr->total_frames) {
        fprintf(stderr, "Frame count mismatch: wrote %d of %d\n",
                wr->frames_written, wr->total_frames);
        goto done;
    }
    if (buf_append_chunk(&wr->out, "IEND", NULL, 0) != 0) {
        goto done;
    }

    fp = fopen(filename, "wb");
    if (!fp) {
        fprintf(stderr, "Failed to open %s for writing\n", filename);
        goto done;
    }
    if (fwrite(wr->out.data, 1, wr->out.size, fp) == wr->out.size) {
        result = 0;
    } else {
        fprintf(stderr, "Failed to write %s\n", filename);
    }
    fclose(fp);

done:
    free(wr->out.data);
    memset(wr, 0, sizeof(*wr));
    return result;
}


static int make_rotating(const char *filename, unsigned width, unsigned height) {
    int frame_count = ROT_FRAMES;
    size_t frame_bytes = (size_t)width * (size_t)height * 4u;
    unsigned char *frame_rgba = (unsigned char *)malloc(frame_bytes);
    double radius = (double)(width < height ? width : height) / 2.0;
    double cx = (double)width / 2.0 - 0.5;
    double cy = (double)height / 2.0 - 0.5;
    ApngWriter wr;
    int frame;
    int result;

    if (!frame_rgba) {
        fprintf(stderr, "Failed to allocate %ux%u frame buffer\n", width, height);
        return 1;
    }
    if (apng_begin(&wr, width, height, frame_count, 1, APNG_FPS) != 0) {
        free(frame_rgba);
        return 1;
    }

    for (frame = 0; frame < frame_count; ++frame) {
        double phase = ((double)frame / (double)frame_count) * ROT_ANGLE_PERIOD;
        memset(frame_rgba, 0, frame_bytes);
        render_ball_rgba(frame_rgba, width, height, cx, cy, radius, phase);
        if (apng_add_frame(&wr, frame_rgba) != 0) {
            free(wr.out.data);
            free(frame_rgba);
            return 1;
        }
    }
    free(frame_rgba);

    result = apng_finish(&wr, filename);
    if (result == 0) {
        printf("Saved %s (%d frames, %ux%u, %.2fs loop)\n",
               filename, frame_count, width, height,
               (double)frame_count / (double)APNG_FPS);
    }
    return result;
}

static int make_bouncing(const char *filename, unsigned width, unsigned height,
                         double peak_norm) {
    size_t frame_bytes = (size_t)width * (size_t)height * 4u;
    unsigned char *frame_rgba;
    double radius = (double)width / 2.0;
    double cx = (double)width / 2.0 - 0.5;
    double floor_norm;  
    double travel_norm; 
    double natural_period;
    double half;
    double gravity_adj;
    double launch_speed;
    ApngWriter wr;
    int frame_count;
    int frame;
    int result;

    floor_norm = (double)(height - width) / (double)height;
    travel_norm = floor_norm - peak_norm;

    if (travel_norm <= 0.0) {
        fprintf(stderr,
                "Error: invalid dimensions %ux%u for peak_norm %f (floor_norm %f)\n",
                width, height, peak_norm, floor_norm);
        return 1;
    }
    natural_period = 2.0 * sqrt(2.0 * travel_norm / GRAVITY);
    frame_count = (int)(natural_period / ROT_FRAMES + 0.5) * ROT_FRAMES;
    if (frame_count < ROT_FRAMES) {
        frame_count = ROT_FRAMES;
    }
    half = (double)frame_count / 2.0;
    gravity_adj = 2.0 * travel_norm / (half * half);
    launch_speed = gravity_adj * half;

    frame_rgba = (unsigned char *)malloc(frame_bytes);
    if (!frame_rgba) {
        fprintf(stderr, "Failed to allocate %ux%u frame buffer\n", width, height);
        return 1;
    }
    if (apng_begin(&wr, width, height, frame_count, 1, APNG_FPS) != 0) {
        free(frame_rgba);
        return 1;
    }

    for (frame = 0; frame < frame_count; ++frame) {
        double t = (double)frame;
        double y_norm = floor_norm - launch_speed * t + 0.5 * gravity_adj * t * t;
        double top_px = y_norm * (double)height;
        double cy = top_px + radius - 0.5;
        double phase = ((double)(frame % ROT_FRAMES) / (double)ROT_FRAMES) * ROT_ANGLE_PERIOD;

        memset(frame_rgba, 0, frame_bytes);
        render_ball_rgba(frame_rgba, width, height, cx, cy, radius, phase);
        if (apng_add_frame(&wr, frame_rgba) != 0) {
            free(wr.out.data);
            free(frame_rgba);
            return 1;
        }
    }
    free(frame_rgba);

    result = apng_finish(&wr, filename);
    if (result == 0) {
        printf("Saved %s (%d frames, %ux%u, %.2fs loop, gravity %.6f vs app %.6f)\n",
               filename, frame_count, width, height,
               (double)frame_count / (double)APNG_FPS, gravity_adj, GRAVITY);
    }
    return result;
}

static long gcd_long(long a, long b) {
    while (b != 0) {
        long t = a % b;
        a = b;
        b = t;
    }
    return a;
}

static int period_candidates(double natural, long *out, int max_out) {
    long lo = (long)ceil(natural * 0.85 / ROT_FRAMES);
    long hi = (long)floor(natural * 1.15 / ROT_FRAMES);
    int n = 0;
    long k;

    if (lo < 1) {
        lo = 1;
    }
    if (hi < lo) {
        lo = hi = (long)(natural / ROT_FRAMES + 0.5);
        if (lo < 1) {
            lo = hi = 1;
        }
    }
    for (k = lo; k <= hi && n < max_out; ++k) {
        out[n++] = k * ROT_FRAMES;
    }
    return n;
}

static int make_sweep(const char *filename, unsigned width, unsigned height,
                      unsigned size, double peak_norm) {
    double radius = (double)size / 2.0;
    double travel_x = (double)(width - size);              
    double floor_norm = (double)(height - size) / (double)height;
    double travel_y_norm = floor_norm - peak_norm;
    double vx_natural = (double)width / 273.0;             
    double tx_natural = 2.0 * travel_x / vx_natural;
    double ty_natural = 2.0 * sqrt(2.0 * travel_y_norm / GRAVITY);
    long tx_cands[64];
    long ty_cands[64];
    int tx_n = period_candidates(tx_natural, tx_cands, 64);
    int ty_n = period_candidates(ty_natural, ty_cands, 64);
    long tx = 0;
    long ty = 0;
    long frame_count = 0;
    double best_score = 0.0;
    double gravity_adj;
    double launch_speed;
    double vx_adj;
    double ty_offset;
    size_t canvas_bytes = (size_t)width * (size_t)height * 4u;
    unsigned char *canvas_rgba = NULL;
    unsigned char *region_rgba = NULL;
    unsigned region_max = size + 6; 
    ApngWriter wr;
    long frame;
    int i;
    int j;
    int result;

    for (i = 0; i < tx_n; ++i) {
        for (j = 0; j < ty_n; ++j) {
            long g = gcd_long(tx_cands[i], ty_cands[j]);
            long n = tx_cands[i] / g * ty_cands[j];
            double dev_x = fabs((double)tx_cands[i] - tx_natural) / tx_natural;
            double dev_y = fabs((double)ty_cands[j] - ty_natural) / ty_natural;
            double score = (double)n * (1.0 + dev_x + dev_y);
            if (frame_count == 0 || score < best_score) {
                best_score = score;
                tx = tx_cands[i];
                ty = ty_cands[j];
                frame_count = n;
            }
        }
    }

    gravity_adj = 2.0 * travel_y_norm / ((ty / 2.0) * (ty / 2.0));
    launch_speed = gravity_adj * (ty / 2.0);
    vx_adj = 2.0 * travel_x / (double)tx;

    ty_offset = (ty / 2.0) * (1.0 - 1.0 / sqrt(2.0));

    canvas_rgba = (unsigned char *)malloc(canvas_bytes);
    region_rgba = (unsigned char *)malloc((size_t)region_max * region_max * 4u);
    if (!canvas_rgba || !region_rgba) {
        fprintf(stderr, "Failed to allocate frame buffers for %ux%u\n", width, height);
        free(canvas_rgba);
        free(region_rgba);
        return 1;
    }
    if (apng_begin(&wr, width, height, (int)frame_count, 1, APNG_FPS) != 0) {
        free(canvas_rgba);
        free(region_rgba);
        return 1;
    }

    for (frame = 0; frame < frame_count; ++frame) {
        double ux = (double)(frame % tx);
        double uy = fmod((double)frame + ty_offset, (double)ty);
        double x_px = (ux <= tx / 2.0) ? vx_adj * ux : vx_adj * ((double)tx - ux);
        double y_norm = floor_norm - launch_speed * uy + 0.5 * gravity_adj * uy * uy;
        double y_px = y_norm * (double)height;
        double cx = x_px + radius - 0.5;
        double cy = y_px + radius - 0.5;
        double phase = ((double)(frame % ROT_FRAMES) / (double)ROT_FRAMES) * ROT_ANGLE_PERIOD;
        int failed;

        if (frame == 0) {
            memset(canvas_rgba, 0, canvas_bytes);
            render_ball_rgba(canvas_rgba, width, height, cx, cy, radius, phase);
            failed = apng_add_frame_region(&wr, canvas_rgba, 0, 0, width, height,
                                           APNG_DISPOSE_OP_BACKGROUND);
        } else {
            long rx = (long)floor(x_px) - 2;
            long ry = (long)floor(y_px) - 2;
            long rx2 = (long)ceil(x_px) + (long)size + 2;
            long ry2 = (long)ceil(y_px) + (long)size + 2;
            unsigned rw;
            unsigned rh;
            if (rx < 0) rx = 0;
            if (ry < 0) ry = 0;
            if (rx2 > (long)width) rx2 = (long)width;
            if (ry2 > (long)height) ry2 = (long)height;
            rw = (unsigned)(rx2 - rx);
            rh = (unsigned)(ry2 - ry);
            memset(region_rgba, 0, (size_t)rw * rh * 4u);
            render_ball_rgba(region_rgba, rw, rh,
                             cx - (double)rx, cy - (double)ry, radius, phase);
            failed = apng_add_frame_region(&wr, region_rgba,
                                           (unsigned)rx, (unsigned)ry, rw, rh,
                                           APNG_DISPOSE_OP_BACKGROUND);
        }
        if (failed) {
            free(wr.out.data);
            free(canvas_rgba);
            free(region_rgba);
            return 1;
        }
    }
    free(canvas_rgba);
    free(region_rgba);

    result = apng_finish(&wr, filename);
    if (result == 0) {
        printf("Saved %s (%ld frames, %ux%u canvas, %u ball, %.2fs loop)\n",
               filename, frame_count, width, height, size,
               (double)frame_count / (double)APNG_FPS);
        printf("  horizontal: round trip %ld frames, speed %.3f px/tick vs natural %.3f\n",
               tx, vx_adj, vx_natural);
        printf("  vertical:   bounce %ld frames, gravity %.6f vs app %.6f\n",
               ty, gravity_adj, GRAVITY);
    }
    return result;
}

static void print_usage(const char *argv0) {
    fprintf(stderr,
            "Usage: %s KIND WIDTH HEIGHT [OUTPUT.png]\n"
            "       %s sweep|halfsweep WIDTH HEIGHT BALLSIZE [OUTPUT.png]\n"
            "Renders a looping Poingo ball animation as an APNG.\n"
            "KIND:\n"
            "  rotating   full-size ball spinning in place, transparent background\n"
            "             (requires WIDTH == HEIGHT)\n"
            "  bounce     WIDTHxWIDTH ball bouncing vertically over the full height\n"
            "             (requires HEIGHT > WIDTH); one full bounce per loop\n"
            "  halfbounce like bounce, but the ball only rises until its top touches the\n"
            "             halfway line of the image, so the bounce lives in the lower half\n"
            "             (requires HEIGHT > 2*WIDTH); one full bounce per loop\n"
            "  sweep      BALLSIZExBALLSIZE ball bouncing around the whole WIDTHxHEIGHT\n"
            "             canvas like the desktop demo (requires WIDTH and HEIGHT each\n"
            "             at least 1.25x BALLSIZE so the ball has room to move)\n"
            "  halfsweep  like sweep, but the ball only rises until its top touches the\n"
            "             halfway line of the canvas (requires HEIGHT at least 2.5x\n"
            "             BALLSIZE)\n"
            "Without OUTPUT, writes poingo_KIND_WIDTHxHEIGHT.png (sweep/halfsweep:\n"
            "poingo_KIND_WIDTHxHEIGHT_bBALLSIZE.png) in the current directory.\n",
            argv0 ? argv0 : "make-poingo-png",
            argv0 ? argv0 : "make-poingo-png");
}

int main(int argc, char *argv[]) {
    const char *kind;
    char default_name[128];
    const char *filename;
    long width;
    long height;
    long ball_size = 0;
    bool is_sweep;
    bool is_halfsweep;
    int fixed_args;
    char *endptr;

    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    kind = argv[1];
    is_halfsweep = strcmp(kind, "halfsweep") == 0;
    is_sweep = is_halfsweep || strcmp(kind, "sweep") == 0;
    fixed_args = is_sweep ? 5 : 4; 

    if (argc != fixed_args && argc != fixed_args + 1) {
        print_usage(argv[0]);
        return 1;
    }

    endptr = NULL;
    width = strtol(argv[2], &endptr, 10);
    if (!endptr || *endptr != '\0' || width <= 0 || width > 4096) {
        fprintf(stderr, "Invalid width: %s (must be 1..4096)\n", argv[2]);
        return 1;
    }
    endptr = NULL;
    height = strtol(argv[3], &endptr, 10);
    if (!endptr || *endptr != '\0' || height <= 0 || height > 4096) {
        fprintf(stderr, "Invalid height: %s (must be 1..4096)\n", argv[3]);
        return 1;
    }
    if (is_sweep) {
        endptr = NULL;
        ball_size = strtol(argv[4], &endptr, 10);
        if (!endptr || *endptr != '\0' || ball_size < 8 || ball_size > 4096) {
            fprintf(stderr, "Invalid ball size: %s (must be 8..4096)\n", argv[4]);
            return 1;
        }
    }

    if (argc == fixed_args + 1) {
        filename = argv[fixed_args];
    } else if (is_sweep) {
        snprintf(default_name, sizeof(default_name), "poingo_%s_%ldx%ld_b%ld.png",
                 kind, width, height, ball_size);
        filename = default_name;
    } else {
        snprintf(default_name, sizeof(default_name), "poingo_%s_%ldx%ld.png",
                 kind, width, height);
        filename = default_name;
    }

    compute_axis_vectors(DEFAULT_TILT_DEGREES);

    if (strcmp(kind, "rotating") == 0) {
        if (width != height) {
            fprintf(stderr, "Error: 'rotating' requires width to be equal to height (got %ldx%ld)\n", width, height);
            return 1;
        }
        return make_rotating(filename, (unsigned)width, (unsigned)height);
    }
    if (strcmp(kind, "bounce") == 0) {
        if (height <= width) {
            fprintf(stderr, "Error: 'bounce' requires height to be greater than width (the ball is %ldx%ld, and needs room to bounce in %ldx%ld)\n", width, width, width, height);
            return 1;
        }
        return make_bouncing(filename, (unsigned)width, (unsigned)height, 0.0);
    }
    if (strcmp(kind, "halfbounce") == 0) {
        if (height <= 2 * width) {
            fprintf(stderr, "Error: 'halfbounce' requires height to be strictly greater than 2x width (got %ldx%ld; ball is %ldx%ld and must bounce strictly below the halfway line %ldpx)\n",
                    width, height, width, width, height / 2);
            return 1;
        }
        return make_bouncing(filename, (unsigned)width, (unsigned)height, 0.5);
    }
    if (is_sweep) {
        long min_dim = ball_size + ball_size / 4;
        if (width < min_dim || height < min_dim) {
            fprintf(stderr,
                    "Error: '%s' needs room around the %ldx%ld ball: canvas width and "
                    "height must each be at least 1.25x the ball (%ld), got %ldx%ld.\n"
                    "For a purely vertical bounce use 'bounce' instead.\n",
                    kind, ball_size, ball_size, min_dim, width, height);
            return 1;
        }
        if (is_halfsweep && height < 2 * ball_size + ball_size / 2) {
            fprintf(stderr,
                    "Error: 'halfsweep' bounces with the ball's top touching the halfway "
                    "line %ldpx, so the canvas must be well over 2x as tall as the "
                    "%ldx%ld ball: height must be at least 2.5x the ball (%ld), got %ld.\n",
                    height / 2, ball_size, ball_size,
                    2 * ball_size + ball_size / 2, height);
            return 1;
        }
        return make_sweep(filename, (unsigned)width, (unsigned)height,
                          (unsigned)ball_size, is_halfsweep ? 0.5 : 0.0);
    }

    fprintf(stderr, "Unknown animation kind: %s\n", kind);
    print_usage(argv[0]);
    return 1;
}
