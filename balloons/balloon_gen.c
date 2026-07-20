#include "balloon_gen.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

enum {
    FRAME_W = 186,
    FRAME_H = 329,
    FRAME_COUNT = 32,
    POP_COUNT = 6,
    SUPERSAMPLE = 4,
    HI_W = FRAME_W * SUPERSAMPLE,
    HI_H = FRAME_H * SUPERSAMPLE,
};

typedef struct { uint8_t r, g, b; } Color;
static const Color balloon_colors[] = {
    {220, 40, 50}, {240, 130, 30}, {235, 200, 40}, {60, 175, 70},
    {50, 110, 220}, {150, 70, 200}, {240, 120, 170}, {40, 180, 170},
};

static float clamp01(float value) { return fmaxf(0.0f, fminf(1.0f, value)); }

static void over(uint8_t *dst, Color color, float alpha) {
    alpha = clamp01(alpha);
    unsigned sa = (unsigned)lroundf(alpha * 255.0f);
    unsigned ia = 255 - sa;
    dst[0] = (uint8_t)((color.r * sa + dst[0] * ia + 127) / 255);
    dst[1] = (uint8_t)((color.g * sa + dst[1] * ia + 127) / 255);
    dst[2] = (uint8_t)((color.b * sa + dst[2] * ia + 127) / 255);
    dst[3] = (uint8_t)(sa + (dst[3] * ia + 127) / 255);
}

static Color shade(Color color, float amount) {
    Color result;
    if (amount <= 1.0f) {
        result.r = (uint8_t)(color.r * amount);
        result.g = (uint8_t)(color.g * amount);
        result.b = (uint8_t)(color.b * amount);
    } else {
        result.r = (uint8_t)(color.r + (255 - color.r) * (amount - 1.0f));
        result.g = (uint8_t)(color.g + (255 - color.g) * (amount - 1.0f));
        result.b = (uint8_t)(color.b + (255 - color.b) * (amount - 1.0f));
    }
    return result;
}

static bool allocate_anim(Anim *animation, int count) {
    memset(animation, 0, sizeof(*animation));
    animation->w = FRAME_W;
    animation->h = FRAME_H;
    animation->nframes = count;
    animation->frames = calloc((size_t)count, sizeof(*animation->frames));
    if (!animation->frames) return false;
    for (int i = 0; i < count; i++) {
        animation->frames[i].rgba = calloc((size_t)FRAME_W * FRAME_H, 4);
        animation->frames[i].delay_ms = 90;
        if (!animation->frames[i].rgba) return false;
    }
    return true;
}

void balloon_free_animation(Anim *animation) {
    if (!animation) return;
    for (int i = 0; i < animation->nframes; i++) free(animation->frames[i].rgba);
    free(animation->frames);
    memset(animation, 0, sizeof(*animation));
}

typedef struct { float x, y; } Point;

static void downsample(const uint8_t *high, uint8_t *pixels) {
    const unsigned samples = SUPERSAMPLE * SUPERSAMPLE;
    for (int y = 0; y < FRAME_H; y++) {
        for (int x = 0; x < FRAME_W; x++) {
            const uint8_t *row0 = high + ((size_t)y * SUPERSAMPLE * HI_W +
                                          (size_t)x * SUPERSAMPLE) * 4;
            const uint8_t *row1 = row0 + (size_t)HI_W * 4;
            const uint8_t *row2 = row1 + (size_t)HI_W * 4;
            const uint8_t *row3 = row2 + (size_t)HI_W * 4;
            const uint8_t *p[4] = { row0, row1, row2, row3 };
            unsigned sum[4] = {0, 0, 0, 0};
            for (int sy = 0; sy < 4; sy++) {
                const uint8_t *s = p[sy];
                for (int sx = 0; sx < 4; sx++, s += 4) {
                    sum[0] += s[0]; sum[1] += s[1];
                    sum[2] += s[2]; sum[3] += s[3];
                }
            }
            uint8_t *dst = pixels + ((size_t)y * FRAME_W + x) * 4;
            for (int c = 0; c < 4; c++) dst[c] = (uint8_t)((sum[c] + samples / 2) / samples);
        }
    }
}

static void radial_ellipse(uint8_t *pixels, float cx, float cy, float rx, float ry,
                           Color color, float alpha_center, float alpha_edge) {
    int x0 = (int)floorf(cx - rx), x1 = (int)ceilf(cx + rx);
    int y0 = (int)floorf(cy - ry), y1 = (int)ceilf(cy + ry);
    for (int y = y0; y <= y1; y++) {
        if (y < 0 || y >= HI_H) continue;
        for (int x = x0; x <= x1; x++) {
            if (x < 0 || x >= HI_W) continue;
            float dx = (x + .5f - cx) / rx;
            float dy = (y + .5f - cy) / ry;
            float r2 = dx * dx + dy * dy;
            if (r2 <= 1.0f) {
                float alpha = (alpha_center + (alpha_edge - alpha_center) * r2) / 255.0f;
                over(pixels + ((size_t)y * HI_W + x) * 4, color, alpha);
            }
        }
    }
}

static void ellipse_outline(uint8_t *pixels, float cx, float cy, float rx, float ry,
                            float width, Color color) {
    int x0 = (int)floorf(cx - rx), x1 = (int)ceilf(cx + rx);
    int y0 = (int)floorf(cy - ry), y1 = (int)ceilf(cy + ry);
    float inner_rx = rx - width, inner_ry = ry - width;
    for (int y = y0; y <= y1; y++) {
        if (y < 0 || y >= HI_H) continue;
        for (int x = x0; x <= x1; x++) {
            if (x < 0 || x >= HI_W) continue;
            float dx = x + .5f - cx, dy = y + .5f - cy;
            float outer = dx * dx / (rx * rx) + dy * dy / (ry * ry);
            float inner = dx * dx / (inner_rx * inner_rx) + dy * dy / (inner_ry * inner_ry);
            if (outer <= 1.0f && inner >= 1.0f)
                over(pixels + ((size_t)y * HI_W + x) * 4, color, 1.0f);
        }
    }
}

static bool point_in_polygon(float x, float y, const Point *points, int count) {
    bool inside = false;
    for (int i = 0, j = count - 1; i < count; j = i++) {
        if (((points[i].y > y) != (points[j].y > y)) &&
            x < (points[j].x - points[i].x) * (y - points[i].y) /
                    (points[j].y - points[i].y) + points[i].x)
            inside = !inside;
    }
    return inside;
}

static float point_segment_distance(float px, float py, float ax, float ay, float bx, float by) {
    float vx = bx - ax, vy = by - ay;
    float denom = vx * vx + vy * vy;
    float t = denom > 0.0f ? ((px - ax) * vx + (py - ay) * vy) / denom : 0.0f;
    t = clamp01(t);
    float dx = px - (ax + t * vx), dy = py - (ay + t * vy);
    return sqrtf(dx * dx + dy * dy);
}

static void fill_polygon(uint8_t *pixels, const Point *points, int count,
                         Color color, float alpha) {
    float min_x = points[0].x, max_x = points[0].x;
    float min_y = points[0].y, max_y = points[0].y;
    for (int i = 1; i < count; i++) {
        min_x = fminf(min_x, points[i].x); max_x = fmaxf(max_x, points[i].x);
        min_y = fminf(min_y, points[i].y); max_y = fmaxf(max_y, points[i].y);
    }
    for (int y = (int)floorf(min_y); y <= (int)ceilf(max_y); y++) {
        if (y < 0 || y >= HI_H) continue;
        for (int x = (int)floorf(min_x); x <= (int)ceilf(max_x); x++) {
            if (x >= 0 && x < HI_W && point_in_polygon(x + .5f, y + .5f, points, count))
                over(pixels + ((size_t)y * HI_W + x) * 4, color, alpha);
        }
    }
}

static void draw_line(uint8_t *pixels, Point a, Point b, float width,
                      Color color, float alpha) {
    int x0 = (int)floorf(fminf(a.x, b.x) - width);
    int x1 = (int)ceilf(fmaxf(a.x, b.x) + width);
    int y0 = (int)floorf(fminf(a.y, b.y) - width);
    int y1 = (int)ceilf(fmaxf(a.y, b.y) + width);
    float radius = width * .5f;
    for (int y = y0; y <= y1; y++) {
        if (y < 0 || y >= HI_H) continue;
        for (int x = x0; x <= x1; x++) {
            if (x >= 0 && x < HI_W &&
                point_segment_distance(x + .5f, y + .5f, a.x, a.y, b.x, b.y) <= radius)
                over(pixels + ((size_t)y * HI_W + x) * 4, color, alpha);
        }
    }
}

static void outline_polygon(uint8_t *pixels, const Point *points, int count,
                            float width, Color color) {
    for (int i = 0; i < count; i++)
        draw_line(pixels, points[i], points[(i + 1) % count], width, color, 1.0f);
}

static void draw_balloon_base(uint8_t *pixels, uint8_t *high, Color base) {
    memset(high, 0, (size_t)HI_W * HI_H * 4);
    const float sx = HI_W / 72.0f, sy = HI_H / 128.0f;
    const float bx = 36.0f * sx;
    const float by = 34.0f * sy;
    const Color outline = shade(base, 0.62f);

    radial_ellipse(high, bx, by, 24.0f * sx, 30.0f * sy, base, 200.0f, 252.0f);
    radial_ellipse(high, bx - 9.5f * sx, by - 13.0f * sy,
                   5.5f * sx, 7.0f * sy, shade(base, 1.65f), 175.0f, 0.0f);
    ellipse_outline(high, bx, by, 24.0f * sx, 30.0f * sy,
                    1.2f * (sx + sy) * .5f, outline);

    Point knot[] = {
        {bx - 4.0f * sx, by + 29.0f * sy},
        {bx + 4.0f * sx, by + 29.0f * sy},
        {bx, by + 34.0f * sy},
    };
    Point nub[] = {
        {bx, by + 33.0f * sy},
        {bx - 2.4f * sx, by + 37.5f * sy},
        {bx + 2.4f * sx, by + 37.5f * sy},
    };
    fill_polygon(high, knot, 3, shade(base, 0.8f), 205.0f / 255.0f);
    fill_polygon(high, nub, 3, outline, 1.0f);
    downsample(high, pixels);
}

static void shift_balloon_frame(const uint8_t *base, uint8_t *pixels, int frame) {
    float phase = (float)frame * 2.0f * (float)M_PI / FRAME_COUNT;
    float shift = sinf(phase) * 3.0f * FRAME_H / 128.0f;
    for (int y = 0; y < FRAME_H; y++) {
        float source_y = y - shift;
        int y0 = (int)floorf(source_y);
        float fraction = source_y - y0;
        for (int x = 0; x < FRAME_W; x++) {
            uint8_t *dst = pixels + ((size_t)y * FRAME_W + x) * 4;
            for (int c = 0; c < 4; c++) {
                unsigned a = y0 >= 0 && y0 < FRAME_H
                    ? base[((size_t)y0 * FRAME_W + x) * 4 + c] : 0;
                unsigned b = y0 + 1 >= 0 && y0 + 1 < FRAME_H
                    ? base[((size_t)(y0 + 1) * FRAME_W + x) * 4 + c] : 0;
                float value = a + ((float)b - a) * fraction;
                dst[c] = (uint8_t)(value < 0.0f ? 0.0f : value > 255.0f ? 255.0f : value + 0.5f);
            }
        }
    }
}

static void draw_string_frame(uint8_t *pixels, uint8_t *high, int frame) {
    memset(high, 0, (size_t)HI_W * HI_H * 4);
    const float sx = HI_W / 72.0f, sy = HI_H / 128.0f;
    const float phase = (float)frame * 2.0f * (float)M_PI / FRAME_COUNT;
    const float bx = 36.0f * sx;
    Point current = {bx, (34.0f + sinf(phase) * 3.0f + 37.5f) * sy};
    Color thread = {90, 90, 100};
    for (int segment = 0; segment < 14; segment++) {
        Point next = {
            bx + sinf(phase + segment * .7f) * (2.0f + segment * .5f) * sx,
            current.y + 6.0f * sy,
        };
        draw_line(high, current, next, 2.0f * (sx + sy) * .5f, thread, 1.0f);
        current = next;
    }
    downsample(high, pixels);
}

static uint32_t next_random(uint32_t *state) {
    *state = *state * 1664525u + 1013904223u;
    return *state;
}

static float random_unit(uint32_t *state) {
    return (float)(next_random(state) >> 8) / 16777216.0f;
}

static float random_range(uint32_t *state, float low, float high) {
    return low + (high - low) * random_unit(state);
}

static float edge_distance(float cx, float cy, float angle, float margin) {
    float dx = cosf(angle), dy = sinf(angle), distance = INFINITY;
    if (dx > 0.0f) distance = fminf(distance, (HI_W - 1 - margin - cx) / dx);
    else if (dx < 0.0f) distance = fminf(distance, (margin - cx) / dx);
    if (dy > 0.0f) distance = fminf(distance, (HI_H - 1 - margin - cy) / dy);
    else if (dy < 0.0f) distance = fminf(distance, (margin - cy) / dy);
    return fmaxf(distance, 0.0f);
}

static void draw_disc(uint8_t *pixels, Point center, float radius,
                      Color fill, Color outline, float outline_width) {
    int x0 = (int)floorf(center.x - radius - outline_width);
    int x1 = (int)ceilf(center.x + radius + outline_width);
    int y0 = (int)floorf(center.y - radius - outline_width);
    int y1 = (int)ceilf(center.y + radius + outline_width);
    for (int y = y0; y <= y1; y++) {
        if (y < 0 || y >= HI_H) continue;
        for (int x = x0; x <= x1; x++) {
            if (x < 0 || x >= HI_W) continue;
            float d = hypotf(x + .5f - center.x, y + .5f - center.y);
            uint8_t *dst = pixels + ((size_t)y * HI_W + x) * 4;
            if (d <= radius) over(dst, fill, 1.0f);
            if (d >= radius - outline_width && d <= radius) over(dst, outline, 1.0f);
        }
    }
}

static void draw_pop(uint8_t *pixels, uint8_t *high, int variant) {
    memset(high, 0, (size_t)HI_W * HI_H * 4);
    uint32_t state = 0x6d2b79f5u + (uint32_t)variant * 0x9e3779b9u;
    const float cx = HI_W * .5f, cy = HI_H * .5f;
    const float scale = SUPERSAMPLE;
    const float margin = 5.0f * scale;
    const float outline_width = 2.5f * scale;
    const Color fill = {255, 255, 230}, white = {255, 255, 255};
    const Color dot = {255, 255, 240}, edge = {0, 0, 0};
    Point burst[14] = {0};
    int points = (5 + (int)(next_random(&state) % 3)) * 2;
    for (int i = 0; i < points; i++) {
        float angle = i * 2.0f * (float)M_PI / points + random_range(&state, -.15f, .15f);
        float radius = (i % 2 == 0)
            ? random_range(&state, 50.0f * scale, 95.0f * scale)
            : random_range(&state, 15.0f * scale, 30.0f * scale);
        radius = fminf(radius, edge_distance(cx, cy, angle, margin));
        burst[i] = (Point){cx + cosf(angle) * radius, cy + sinf(angle) * radius};
    }
    fill_polygon(high, burst, points, fill, 1.0f);
    outline_polygon(high, burst, points, outline_width, edge);

    int shard_count = 6 + (int)(next_random(&state) % 5);
    int made = 0;
    for (int attempt = 0; made < shard_count && attempt < 100; attempt++) {
        float angle = random_range(&state, 0.0f, 2.0f * (float)M_PI);
        float length = random_range(&state, 10.0f * scale, 25.0f * scale);
        float width = random_range(&state, 3.0f * scale, 8.0f * scale);
        float distance = random_range(&state, 60.0f * scale, 110.0f * scale);
        Point shard[3];
        shard[0] = (Point){cx + cosf(angle) * distance, cy + sinf(angle) * distance};
        float shard_angle = angle + random_range(&state, -.5f, .5f);
        shard[1] = (Point){shard[0].x + cosf(shard_angle - .2f) * length,
                           shard[0].y + sinf(shard_angle - .2f) * length};
        shard[2] = (Point){shard[0].x + cosf(shard_angle + .2f) * width,
                           shard[0].y + sinf(shard_angle + .2f) * width};
        bool valid = true;
        for (int i = 0; i < 3; i++)
            valid &= shard[i].x >= margin && shard[i].x <= HI_W - 1 - margin &&
                     shard[i].y >= margin && shard[i].y <= HI_H - 1 - margin;
        if (!valid) continue;
        fill_polygon(high, shard, 3, white, 1.0f);
        outline_polygon(high, shard, 3, outline_width, edge);
        made++;
    }

    for (int i = 0; i < 8; i++) {
        float angle = random_range(&state, 0.0f, 2.0f * (float)M_PI);
        float radius = random_range(&state, 1.0f * scale, 3.0f * scale);
        float distance = random_range(&state, 30.0f * scale, 100.0f * scale);
        distance = fminf(distance, edge_distance(cx, cy, angle, margin) - radius);
        Point center = {cx + cosf(angle) * distance, cy + sinf(angle) * distance};
        draw_disc(high, center, radius, dot, edge, outline_width);
    }

    downsample(high, pixels);
    float max_radius = 0.0f;
    for (int y = 0; y < FRAME_H; y++) for (int x = 0; x < FRAME_W; x++) {
        uint8_t *pixel = pixels + ((size_t)y * FRAME_W + x) * 4;
        if (pixel[3] > 0)
            max_radius = fmaxf(max_radius, hypotf(x - FRAME_W * .5f, y - FRAME_H * .5f));
    }
    if (max_radius > 0.0f) {
        for (int y = 0; y < FRAME_H; y++) for (int x = 0; x < FRAME_W; x++) {
            uint8_t *pixel = pixels + ((size_t)y * FRAME_W + x) * 4;
            if (pixel[3] == 0) continue;
            float t = hypotf(x - FRAME_W * .5f, y - FRAME_H * .5f) / max_radius;
            float alpha_scale = .70f + .20f * t;
            for (int c = 0; c < 4; c++) pixel[c] = (uint8_t)lroundf(pixel[c] * alpha_scale);
        }
    }
}

bool balloon_generate_assets(Anim **balloons, int *balloon_count, Anim *string,
                             Anim **pops, int *pop_count) {
    if (!balloons || !balloon_count || !string || !pops || !pop_count) return false;
    *balloons = NULL;
    *pops = NULL;
    *balloon_count = 0;
    *pop_count = 0;
    *balloons = calloc(sizeof(balloon_colors) / sizeof(balloon_colors[0]), sizeof(**balloons));
    *pops = calloc(POP_COUNT, sizeof(**pops));
    if (!*balloons || !*pops || !allocate_anim(string, FRAME_COUNT)) goto fail;
    *balloon_count = (int)(sizeof(balloon_colors) / sizeof(balloon_colors[0]));
    *pop_count = POP_COUNT;
    uint8_t *high = calloc((size_t)HI_W * HI_H, 4);
    uint8_t *base = malloc((size_t)FRAME_W * FRAME_H * 4);
    if (!high || !base) {
        free(high);
        free(base);
        goto fail;
    }
    for (int b = 0; b < *balloon_count; b++) {
        if (!allocate_anim(&(*balloons)[b], FRAME_COUNT)) {
            free(high);
            free(base);
            goto fail;
        }
        draw_balloon_base(base, high, balloon_colors[b]);
        for (int f = 0; f < FRAME_COUNT; f++)
            shift_balloon_frame(base, (*balloons)[b].frames[f].rgba, f);
    }
    for (int f = 0; f < FRAME_COUNT; f++) draw_string_frame(string->frames[f].rgba, high, f);
    for (int p = 0; p < POP_COUNT; p++) {
        if (!allocate_anim(&(*pops)[p], 1)) {
            free(high);
            free(base);
            goto fail;
        }
        draw_pop((*pops)[p].frames[0].rgba, high, p);
    }
    free(high);
    free(base);
    return true;

fail:
    balloon_free_animation(string);
    if (*balloons) {
        for (int b = 0; b < (int)(sizeof(balloon_colors) / sizeof(balloon_colors[0])); b++)
            balloon_free_animation(&(*balloons)[b]);
        free(*balloons);
        *balloons = NULL;
    }
    if (*pops) {
        for (int p = 0; p < POP_COUNT; p++) balloon_free_animation(&(*pops)[p]);
        free(*pops);
        *pops = NULL;
    }
    return false;
}
