#pragma once

#include <stdint.h>

void thunder_synthesize(float *buffer, int sample_count, int sample_rate,
                        uint32_t seed, int variation, int layer);
