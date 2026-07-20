#include "lodepng.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static void write_icon(const char *directory, int size) {
    unsigned char *pixels = calloc((size_t)size * size, 4);
    if (!pixels) exit(1);
    const float cx = size * .5f, cy = size * .44f;
    const float rx = size * .34f, ry = size * .39f;
    for (int y = 0; y < size; y++) for (int x = 0; x < size; x++) {
        float dx = (x + .5f - cx) / rx, dy = (y + .5f - cy) / ry;
        float d = sqrtf(dx * dx + dy * dy);
        if (d > 1.02f) continue;
        float edge = fmaxf(0.0f, fminf(1.0f, (1.02f - d) * 30.0f));
        float light = fmaxf(0.0f, 1.0f - sqrtf((dx + .35f) * (dx + .35f) + (dy + .43f) * (dy + .43f)) * 2.6f);
        unsigned char *p = pixels + ((size_t)y * size + x) * 4;
        p[0] = (unsigned char)(185 + light * 65); p[1] = (unsigned char)(25 + light * 35);
        p[2] = (unsigned char)(38 + light * 30); p[3] = (unsigned char)(edge * 255);
    }
    char name[256];
    snprintf(name, sizeof(name), "%s/icon_%dx%d.png", directory, size, size);
    if (lodepng_encode32_file(name, pixels, size, size)) exit(1);
    free(pixels);
}

int main(int argc, char **argv) {
    const char *directory = argc > 1 ? argv[1] : ".";
    const int sizes[] = {16, 32, 48, 64, 128, 256, 512};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) write_icon(directory, sizes[i]);
    return 0;
}
