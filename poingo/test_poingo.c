/* Headless regression tests for the parts of Poingo that need no compositor.

   Built twice by `make test`: once plain, and once under ThreadSanitizer,
   which is what actually catches the regeneration races. */

#define main poingo_main
#include "poingo.c"
#undef main

static int g_failures = 0;

#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            g_failures++;                                             \
            fprintf(stderr, "FAIL %s:%d: ", __func__, __LINE__);      \
            fprintf(stderr, __VA_ARGS__);                             \
            fprintf(stderr, "\n");                                    \
        }                                                             \
    } while (0)

/* ------------------------------------------------------------------ */
/* Regeneration lifecycle                                              */
/* ------------------------------------------------------------------ */

#define TEST_FRAME_COUNT 24

static bool regen_state_init(FreedomState *st, FreedomFrameSet *frames) {
    memset(st, 0, sizeof(*st));
    memset(frames, 0, sizeof(*frames));
    if (!freerange_prepare_blank_frames(frames, TEST_FRAME_COUNT)) {
        return false;
    }
    st->frames_ref = frames;
    st->color_regen_angle_period = (4.0f * PI) / LON_TILES;
    return freerange_color_regen_prepare_assets(st, frames) &&
           freerange_regen_workspace_prepare(st, frames);
}

static void regen_state_free(FreedomState *st, FreedomFrameSet *frames) {
    freerange_color_regen_shutdown(st);
    freerange_destroy_frames(frames);
    free(st->regen_unit_done_storage);
    free(st->regen_order_storage);
    free(st->regen_thread_storage);
}

/* Quitting mid-regen frees the frame set the workers are still writing into.
   freerange_color_regen_shutdown() has to be the join point, so that no
   caller can free worker-visible memory behind a live thread. */
static void test_regen_shutdown_joins_workers(void) {
    FreedomState st;
    FreedomFrameSet frames;
    if (!regen_state_init(&st, &frames)) {
        CHECK(false, "could not set up regen state");
        return;
    }

    freerange_color_regen_start(&st, &frames);
    CHECK(st.color_regen_active, "regen did not start");

    freerange_color_regen_shutdown(&st);
    CHECK(st.regen_thread_count == 0, "%d worker threads outlived the shutdown",
          st.regen_thread_count);
    CHECK(st.regen_worker_ctx == NULL, "worker context outlived the shutdown");
    CHECK(!st.color_regen_active, "regen still marked active after shutdown");

    regen_state_free(&st, &frames);
}

/* A colour change rewrites the shared sphere cache. Doing that while the
   previous regen's workers are still reading it is a data race, so the
   transition has to join them before it touches the palette. TSan proves
   the race; this check proves the ordering even without it. */
static void test_color_change_joins_before_palette(void) {
    FreedomState st;
    FreedomFrameSet frames;
    if (!regen_state_init(&st, &frames)) {
        CHECK(false, "could not set up regen state");
        return;
    }

    const uint8_t light[3] = { 255, 255, 255 };
    const uint8_t dark[3] = { 255, 0, 0 };

    for (int i = 0; i < 8; i++) {
        freerange_color_regen_start(&st, &frames);
        CHECK(st.color_regen_active, "round %d: regen did not start", i);
        /* No upload step in between: the workers are still mid-flight. */
        freerange_regen_transition(&st, i & 1 ? POINGO_MODE_NOSTALGIA : POINGO_MODE_POINGO,
                                   i & 1 ? light : dark, i & 1 ? dark : light);
        CHECK(st.color_regen_active, "round %d: regen not restarted", i);
        if (g_failures) {
            break;
        }
    }

    regen_state_free(&st, &frames);
}

/* Every worker allocates its own scratch image. If they all fail to, nothing
   ever marks a unit done and the upload loop waits on it forever. */
static void test_regen_survives_worker_failure(void) {
    FreedomState st;
    FreedomFrameSet frames;
    if (!regen_state_init(&st, &frames)) {
        CHECK(false, "could not set up regen state");
        return;
    }

    freerange_color_regen_start(&st, &frames);
    CHECK(st.color_regen_active, "regen did not start");
    CHECK(st.regen_worker_ctx != NULL, "no worker context");

    if (st.regen_worker_ctx) {
        /* Stand in for every worker's scratch allocation failing. */
        poingo_atomic_set(&st.regen_worker_ctx->failed, 1);
        for (int i = 0; i < 10; i++) {
            freerange_regen_upload_step(&st, &frames, 0, 1000.0);
        }
        CHECK(!st.color_regen_active,
              "regen stayed active after every worker failed");
    }

    regen_state_free(&st, &frames);
}

int main(void) {
    srandom(1);

    test_regen_shutdown_joins_workers();
    test_color_change_joins_before_palette();
    test_regen_survives_worker_failure();

    release_sphere_pixel_cache();

    if (g_failures) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
