#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t *rgba;
    int delay_ms;
} AnimFrame;

typedef struct {
    int w, h;
    int nframes;
    AnimFrame *frames;
} Anim;

bool balloon_generate_assets(Anim **balloons, int *balloon_count, Anim *string,
                             Anim **pops, int *pop_count);
void balloon_free_animation(Anim *animation);
