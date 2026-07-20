#include "toy_audio.h"

#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>

#define TEST_SAMPLES 4096
#define RENDER_FRAMES 128
#define START_COUNT 1000

static ToyMixer mixer;
static pthread_mutex_t mixer_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool render_running = true;

static void *render_thread(void *unused) {
    (void)unused;
    float mono[RENDER_FRAMES];
    float stereo[RENDER_FRAMES * 2];
    int pass = 0;
    while (atomic_load_explicit(&render_running, memory_order_relaxed)) {
        pthread_mutex_lock(&mixer_lock);
        if ((pass++ & 1) == 0) {
            toy_mixer_render(&mixer, mono, RENDER_FRAMES, false);
            for (int i = 0; i < RENDER_FRAMES; ++i) {
                assert(isfinite(mono[i]));
                assert(fabsf(mono[i]) <=
                       TOY_AUDIO_CLIP_THRESHOLD + 0.0001f);
            }
        } else {
            toy_mixer_render_stereo(&mixer, stereo, RENDER_FRAMES, false);
            for (int i = 0; i < RENDER_FRAMES * 2; ++i) {
                assert(isfinite(stereo[i]));
                assert(fabsf(stereo[i]) <=
                       TOY_AUDIO_CLIP_THRESHOLD + 0.0001f);
            }
        }
        pthread_mutex_unlock(&mixer_lock);
    }
    return NULL;
}

int main(void) {
    ToySamplePair pair = {0};
    assert(toy_sample_pair_alloc(&pair, TEST_SAMPLES));
    for (int i = 0; i < TEST_SAMPLES; ++i) {
        float phase = (float)i * 0.07f;
        pair.data1[i] = sinf(phase) * 0.25f;
        pair.data2[i] = sinf(phase * 0.97f) * 0.25f;
    }

    toy_mixer_init(&mixer, 1.0f);
    assert(toy_mixer_reserve(&mixer, TEST_SAMPLES));

    pthread_t thread;
    assert(pthread_create(&thread, NULL, render_thread, NULL) == 0);

    for (int i = 0; i < START_COUNT; ++i) {
        ToyVoiceClaim claim;
        pthread_mutex_lock(&mixer_lock);
        bool claimed =
            toy_mixer_claim_voice(&mixer, pair.length, &claim);
        pthread_mutex_unlock(&mixer_lock);
        assert(claimed);

        assert(toy_mixer_copy_claimed_voice(&mixer, claim, &pair));

        pthread_mutex_lock(&mixer_lock);
        assert(toy_mixer_commit_voice(
            &mixer, claim, pair.length, 0.5f, 0.8f, 0.6f));
        pthread_mutex_unlock(&mixer_lock);
    }

    ToyVoiceClaim stale;
    pthread_mutex_lock(&mixer_lock);
    assert(toy_mixer_claim_voice(&mixer, pair.length, &stale));
    toy_mixer_cancel_voice(&mixer, stale);
    assert(!toy_mixer_commit_voice(
        &mixer, stale, pair.length, 1.0f, 1.0f, 1.0f));
    atomic_store_explicit(&render_running, false, memory_order_relaxed);
    pthread_mutex_unlock(&mixer_lock);

    assert(pthread_join(thread, NULL) == 0);
    toy_sample_pair_free(&pair);
    toy_mixer_reset(&mixer);
    puts("mixer stress test passed");
    return 0;
}
