#include "ghost_icon.h"
#include <stdlib.h>
#include <math.h>

static float smoothstep(float value) {
    value = fmaxf(0.0f, fminf(1.0f, value));
    return value * value * (3.0f - 2.0f * value);
}

static float hash_noise(int x, int y) {
    uint32_t value = (uint32_t)x * 0x8da6b343u ^ (uint32_t)y * 0xd8163841u;
    value ^= value >> 13;
    value *= 0x85ebca6bu;
    value ^= value >> 16;
    return (float)(value & 0x00ffffffu) / 16777215.0f;
}

/* Coherent value noise, used in a rotated coordinate system below so the
 * stone's grain has a deliberate, shared direction rather than static-like
 * speckles. */
static float stone_noise(float x, float y) {
    int x0 = (int)floorf(x);
    int y0 = (int)floorf(y);
    float tx = smoothstep(x - x0);
    float ty = smoothstep(y - y0);
    float a = hash_noise(x0, y0);
    float b = hash_noise(x0 + 1, y0);
    float c = hash_noise(x0, y0 + 1);
    float d = hash_noise(x0 + 1, y0 + 1);
    return (a + (b - a) * tx) * (1.0f - ty) + (c + (d - c) * tx) * ty;
}

static void soapstone_color(float grain_along, float grain_across,
                            float *red, float *green, float *blue) {
    float broad = stone_noise(grain_along * 0.028f, grain_across * 0.070f) - 0.5f;
    float fine = stone_noise(grain_along * 0.14f, grain_across * 0.32f) - 0.5f;
    float vein_phase = grain_across * 0.22f +
                       (stone_noise(grain_along * 0.050f, 9.0f) - 0.5f) * 2.2f;
    float vein = powf(fmaxf(0.0f, sinf(vein_phase)), 18.0f);
    float tone = broad * 16.0f + fine * 4.0f - vein * 11.0f;
    *red = 186.0f + tone;
    *green = 193.0f + tone;
    *blue = 187.0f + tone * 0.90f;
}

static void labradorite_color(float grain_along, float grain_across,
                              float *red, float *green, float *blue) {
    float broad = stone_noise(grain_along * 0.055f, grain_across * 0.13f) - 0.5f;
    float flash_line = powf(fmaxf(0.0f, sinf(grain_across * 0.24f +
                                              (stone_noise(grain_along * 0.065f, 31.0f) - 0.5f) * 2.4f)),
                            18.0f);
    float flash_gate = smoothstep((stone_noise(grain_along * 0.10f,
                                                grain_across * 0.045f) - 0.52f) * 2.8f);
    float flash = flash_line * flash_gate;
    *red = 37.0f + broad * 10.0f + flash * 14.0f;
    *green = 48.0f + broad * 13.0f + flash * 50.0f;
    *blue = 59.0f + broad * 16.0f + flash * 77.0f;
}

static void gold_color(float grain_along, float grain_across,
                       float *red, float *green, float *blue) {
    float brushed = stone_noise(grain_along * 0.18f, grain_across * 0.32f) - 0.5f;
    float glint = powf(fmaxf(0.0f, sinf(grain_along * 0.24f +
                                         grain_across * 0.04f)), 16.0f);
    float base = 179.0f + brushed * 22.0f + glint * 13.0f;
    *red = base + 24.0f;
    *green = base * 0.78f + 6.0f;
    *blue = base * 0.35f + 4.0f;
}

static void ghost_icon_material(float x, float y,
                                float *red, float *green, float *blue, float *alpha) {
    const float center = GHOST_ICON_SIZE / 2.0f;
    const float radius = GHOST_ICON_SIZE / 2.0f - 2.0f;
    const float inner_gold_radius = radius - 13.0f;
    const float labradorite_outer_radius = radius - 3.0f;
    float dx = x - center;
    float dy = y - center;
    float dist = sqrtf(dx * dx + dy * dy);

    *alpha = fmaxf(0.0f, fminf(1.0f, radius - dist + 0.5f));
    if (*alpha == 0.0f) {
        *red = *green = *blue = 0.0f;
        return;
    }

    /* All materials share this diagonal cut direction. */
    float grain_along = dx * 0.819152f + dy * 0.573576f;
    float grain_across = -dx * 0.573576f + dy * 0.819152f;
    if (dist < inner_gold_radius) {
        soapstone_color(grain_along, grain_across, red, green, blue);
    } else if (dist < inner_gold_radius + 2.0f || dist >= labradorite_outer_radius) {
        gold_color(grain_along, grain_across, red, green, blue);
    } else {
        labradorite_color(grain_along, grain_across, red, green, blue);
    }
}

uint32_t* ghost_icon_create_bg(bool premultiplied) {
    const int size = GHOST_ICON_SIZE;
    enum { supersample = 4 };
    uint32_t *buf = (uint32_t *)calloc(size * size, 4);
    if (!buf) return NULL;

    /* Supersampling is deliberately done before quantizing RGBA. Averaging
     * premultiplied samples preserves the sharp metal/obsidian boundaries
     * while eliminating the staircase edge visible at badge size. */
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            float pr = 0.0f, pg = 0.0f, pb = 0.0f, pa = 0.0f;
            for (int sy = 0; sy < supersample; sy++) {
                for (int sx = 0; sx < supersample; sx++) {
                    float r, g, b, a;
                    ghost_icon_material(x + (sx + 0.5f) / supersample,
                                        y + (sy + 0.5f) / supersample,
                                        &r, &g, &b, &a);
                    pr += r * a;
                    pg += g * a;
                    pb += b * a;
                    pa += a;
                }
            }
            pa /= supersample * supersample;
            pr /= supersample * supersample;
            pg /= supersample * supersample;
            pb /= supersample * supersample;
            if (pa == 0.0f) continue;

            uint32_t ar = (uint32_t)lroundf(premultiplied ? pr : pr / pa);
            uint32_t ag = (uint32_t)lroundf(premultiplied ? pg : pg / pa);
            uint32_t ab = (uint32_t)lroundf(premultiplied ? pb : pb / pa);
            uint32_t aa = (uint32_t)lroundf(pa * 255.0f);
            buf[y * size + x] = (aa << 24) | (ab << 16) | (ag << 8) | ar;
        }
    }
    return buf;
}

static uint32_t over_black(uint32_t destination, uint8_t alpha) {
    if (alpha == 0) return destination;

    uint32_t destination_alpha = destination >> 24;
    uint32_t inverse_alpha = 255 - alpha;
    uint32_t output_alpha = alpha + (destination_alpha * inverse_alpha + 127) / 255;
    if (output_alpha == 0) return 0;

    uint32_t output = output_alpha << 24;
    for (int shift = 0; shift <= 16; shift += 8) {
        uint32_t channel = (destination >> shift) & 0xff;
        channel = (channel * destination_alpha * inverse_alpha + output_alpha * 127) /
                  (output_alpha * 255);
        output |= channel << shift;
    }
    return output;
}

void ghost_icon_composite_shadow(uint32_t *destination, const uint32_t *source,
                                 int width, int height) {
    if (!destination || !source || width <= 0 || height <= 0) return;

    /* A 7x7 Gaussian-like kernel gives the same soft penumbra character as
     * Poingo's smooth radial falloff without turning a tool silhouette into
     * a hard duplicate.  The weights are normalized per pixel so a solid
     * part of an icon remains exactly 40% black at the shadow core. */
    enum { radius = 3 };
    const float sigma = 1.65f;
    const float two_sigma_squared = 2.0f * sigma * sigma;
    float weights[(radius * 2 + 1) * (radius * 2 + 1)];
    float weight_sum = 0.0f;
    int index = 0;
    for (int y = -radius; y <= radius; y++) {
        for (int x = -radius; x <= radius; x++) {
            float weight = expf(-(float)(x * x + y * y) / two_sigma_squared);
            weights[index++] = weight;
            weight_sum += weight;
        }
    }

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            float alpha = 0.0f;
            index = 0;
            for (int ky = -radius; ky <= radius; ky++) {
                int source_y = y - GHOST_ICON_SHADOW_OFFSET_Y + ky;
                for (int kx = -radius; kx <= radius; kx++, index++) {
                    int source_x = x - GHOST_ICON_SHADOW_OFFSET_X + kx;
                    if (source_x < 0 || source_x >= width ||
                        source_y < 0 || source_y >= height) continue;
                    alpha += ((source[(size_t)source_y * width + source_x] >> 24) / 255.0f) *
                             weights[index];
                }
            }
            alpha = fminf(alpha / weight_sum * GHOST_ICON_SHADOW_OPACITY,
                          GHOST_ICON_SHADOW_OPACITY);
            destination[(size_t)y * width + x] =
                over_black(destination[(size_t)y * width + x],
                           (uint8_t)lroundf(alpha * 255.0f));
        }
    }
}
