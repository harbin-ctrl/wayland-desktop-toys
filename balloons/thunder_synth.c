#include "thunder_synth.h"

#include <math.h>
#include <stddef.h>

#define THUNDER_PI 3.14159265358979323846f
#define THUNDER_MAX_ROLLS 8

typedef struct {
    float z;
    float coefficient;
} LowPass;

typedef struct {
    float b0, b1, b2;
    float a1, a2;
    float x1, x2;
    float y1, y2;
} Biquad;

typedef struct {
    float start;
    float attack;
    float decay;
    float gain;
    float brightness;
} ThunderRoll;

static uint32_t thunder_random(uint32_t *state) {
    *state = *state * 1664525u + 1013904223u;
    return *state;
}

static float thunder_random_unit(uint32_t *state) {
    return ((thunder_random(state) >> 8) & 0x00ffffffu) / 8388607.5f - 1.0f;
}

static float thunder_random_range(uint32_t *state, float low, float high) {
    float unit = ((thunder_random(state) >> 8) & 0x00ffffffu) / 16777216.0f;
    return low + (high - low) * unit;
}

static LowPass low_pass(float cutoff_hz, float sample_rate) {
    LowPass filter = {
        .z = 0.0f,
        .coefficient = 1.0f - expf(-2.0f * THUNDER_PI * cutoff_hz / sample_rate),
    };
    return filter;
}

static float low_pass_tick(LowPass *filter, float sample) {
    filter->z += filter->coefficient * (sample - filter->z);
    return filter->z;
}

static Biquad band_pass(float center_hz, float q, float sample_rate) {
    float omega = 2.0f * THUNDER_PI * center_hz / sample_rate;
    float alpha = sinf(omega) / (2.0f * q);
    float a0 = 1.0f + alpha;
    Biquad filter = {
        .b0 = alpha / a0,
        .b1 = 0.0f,
        .b2 = -alpha / a0,
        .a1 = -2.0f * cosf(omega) / a0,
        .a2 = (1.0f - alpha) / a0,
    };
    return filter;
}

static float biquad_tick(Biquad *filter, float sample) {
    float result = filter->b0 * sample + filter->b1 * filter->x1 +
                   filter->b2 * filter->x2 - filter->a1 * filter->y1 -
                   filter->a2 * filter->y2;
    filter->x2 = filter->x1;
    filter->x1 = sample;
    filter->y2 = filter->y1;
    filter->y1 = result;
    return result;
}

static float roll_envelope(const ThunderRoll *roll, float time) {
    float age = time - roll->start;
    if (age <= 0.0f) return 0.0f;
    float attack;
    if (age < roll->attack) {
        float phase = age / roll->attack;
        attack = 0.5f - 0.5f * cosf(THUNDER_PI * phase);
    } else {
        attack = 1.0f;
    }
    float decay_age = fmaxf(0.0f, age - roll->attack);
    return roll->gain * attack * expf(-decay_age / roll->decay);
}

static int make_rolls(ThunderRoll rolls[THUNDER_MAX_ROLLS], uint32_t *state,
                      int variation) {
    int count = 6 + variation % 3;
    rolls[0] = (ThunderRoll){
        .start = 0.04f + thunder_random_range(state, 0.0f, 0.12f),
        .attack = thunder_random_range(state, 0.48f, 0.88f),
        .decay = thunder_random_range(state, 2.6f, 4.2f),
        .gain = thunder_random_range(state, 0.78f, 1.0f),
        .brightness = thunder_random_range(state, 0.65f, 0.95f),
    };

    float cursor = thunder_random_range(state, 0.65f, 1.05f);
    for (int i = 1; i < count; i++) {
        float late = (float)i / (float)(count - 1);
        rolls[i] = (ThunderRoll){
            .start = cursor,
            .attack = thunder_random_range(state, 0.55f, 1.25f),
            .decay = thunder_random_range(state, 2.0f, 4.8f),
            .gain = thunder_random_range(state, 0.38f, 0.78f) * (1.0f - 0.20f * late),
            .brightness = thunder_random_range(state, 0.20f, 0.72f) * (1.0f - 0.45f * late),
        };
        cursor += thunder_random_range(state, 0.72f, 1.45f);
    }
    return count;
}

void thunder_synthesize(float *buffer, int sample_count, int sample_rate,
                        uint32_t seed, int variation, int layer) {
    if (!buffer || sample_count <= 0 || sample_rate <= 0) return;

    uint32_t state = seed ^ ((uint32_t)variation * 0x9e3779b9u) ^
                     ((uint32_t)layer * 0x85ebca6bu);
    ThunderRoll rolls[THUNDER_MAX_ROLLS];
    int roll_count = make_rolls(rolls, &state, variation);
    float layer_shift = layer ? 1.035f : 1.0f;

    LowPass deep_a = low_pass(145.0f * layer_shift, (float)sample_rate);
    LowPass deep_b = low_pass(145.0f * layer_shift, (float)sample_rate);
    LowPass deep_c = low_pass(145.0f * layer_shift, (float)sample_rate);
    LowPass deep_floor = low_pass(22.0f, (float)sample_rate);

    LowPass body_a = low_pass(430.0f * layer_shift, (float)sample_rate);
    LowPass body_b = low_pass(430.0f * layer_shift, (float)sample_rate);
    LowPass body_floor = low_pass(72.0f, (float)sample_rate);

    LowPass air_a = low_pass(1050.0f * layer_shift, (float)sample_rate);
    LowPass air_b = low_pass(1050.0f * layer_shift, (float)sample_rate);
    LowPass air_floor = low_pass(210.0f, (float)sample_rate);

    Biquad mode_52 = band_pass(52.0f * layer_shift, 0.62f, (float)sample_rate);
    Biquad mode_86 = band_pass(86.0f * layer_shift, 0.72f, (float)sample_rate);
    Biquad mode_138 = band_pass(138.0f * layer_shift, 0.80f, (float)sample_rate);
    LowPass output_a = low_pass(900.0f, (float)sample_rate);
    LowPass output_b = low_pass(900.0f, (float)sample_rate);

    float diffuse_a[8192] = {0};
    float diffuse_b[8192] = {0};
    float diffuse_c[8192] = {0};
    int delay_a = (int)(0.061f * sample_rate);
    int delay_b = (int)(0.097f * sample_rate);
    int delay_c = (int)(0.151f * sample_rate);
    if (delay_a > 8192) delay_a = 8192;
    if (delay_b > 8192) delay_b = 8192;
    if (delay_c > 8192) delay_c = 8192;
    if (delay_a < 1) delay_a = 1;
    if (delay_b < 1) delay_b = 1;
    if (delay_c < 1) delay_c = 1;
    int index_a = 0, index_b = 0, index_c = 0;
    float echo_low_a = 0.0f, echo_low_b = 0.0f, echo_low_c = 0.0f;

    float phase_a = thunder_random_range(&state, 0.0f, 2.0f * THUNDER_PI);
    float phase_b = thunder_random_range(&state, 0.0f, 2.0f * THUNDER_PI);
    float dc_input = 0.0f, dc_output = 0.0f;
    float peak = 0.0f;

    for (int sample = 0; sample < sample_count; sample++) {
        float time = (float)sample / (float)sample_rate;
        float deep_env = 0.0f, body_env = 0.0f, air_env = 0.0f;
        for (int i = 0; i < roll_count; i++) {
            float envelope = roll_envelope(&rolls[i], time);
            deep_env += envelope * (1.05f - 0.20f * rolls[i].brightness);
            body_env += envelope;
            air_env += envelope * rolls[i].brightness *
                       expf(-fmaxf(0.0f, time - rolls[i].start) / 2.2f);
        }

        float slow_motion = 0.79f +
            0.13f * sinf(2.0f * THUNDER_PI * 0.31f * time + phase_a) +
            0.08f * sinf(2.0f * THUNDER_PI * 0.73f * time + phase_b);

        float noise_deep = thunder_random_unit(&state);
        float noise_body = thunder_random_unit(&state);
        float noise_air = thunder_random_unit(&state);
        float noise_mode = thunder_random_unit(&state);

        float deep = low_pass_tick(&deep_a, noise_deep);
        deep = low_pass_tick(&deep_b, deep);
        deep = low_pass_tick(&deep_c, deep);
        deep -= low_pass_tick(&deep_floor, deep);

        float body = low_pass_tick(&body_a, noise_body);
        body = low_pass_tick(&body_b, body);
        body -= low_pass_tick(&body_floor, body);

        float air = low_pass_tick(&air_a, noise_air);
        air = low_pass_tick(&air_b, air);
        air -= low_pass_tick(&air_floor, air);

        float modes = 0.72f * biquad_tick(&mode_52, noise_mode) +
                      0.48f * biquad_tick(&mode_86, noise_mode) +
                      0.25f * biquad_tick(&mode_138, noise_mode);

        float mixed = slow_motion *
            (deep_env * (3.8f * deep + 0.62f * modes) +
             body_env * 2.15f * body +
             air_env * 0.52f * air);

        float echo_a = diffuse_a[index_a];
        float echo_b = diffuse_b[index_b];
        float echo_c = diffuse_c[index_c];
        echo_low_a += 0.075f * (echo_a - echo_low_a);
        echo_low_b += 0.060f * (echo_b - echo_low_b);
        echo_low_c += 0.050f * (echo_c - echo_low_c);
        diffuse_a[index_a] = mixed + echo_low_a * 0.34f;
        diffuse_b[index_b] = mixed + echo_low_b * 0.29f;
        diffuse_c[index_c] = mixed + echo_low_c * 0.24f;
        if (++index_a >= delay_a) index_a = 0;
        if (++index_b >= delay_b) index_b = 0;
        if (++index_c >= delay_c) index_c = 0;
        float diffused = mixed * 0.72f +
                         (echo_low_a + echo_low_b + echo_low_c) * 0.16f;

        float blocked = diffused - dc_input + 0.9965f * dc_output;
        dc_input = diffused;
        dc_output = blocked;

        float end_fade_start = sample_count / (float)sample_rate - 1.6f;
        float end_fade = time > end_fade_start
            ? fmaxf(0.0f, (sample_count / (float)sample_rate - time) / 1.6f)
            : 1.0f;
        float value = tanhf(blocked * 0.82f);
        value = low_pass_tick(&output_a, value);
        value = low_pass_tick(&output_b, value) * end_fade;
        buffer[sample] = value;
        peak = fmaxf(peak, fabsf(value));
    }

    if (peak > 0.0f) {
        float gain = 0.82f / peak;
        for (int sample = 0; sample < sample_count; sample++)
            buffer[sample] *= gain;
    }
}
