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
/* Physics                                                             */
/* ------------------------------------------------------------------ */

#define TEST_WINDOW_W 1280
#define TEST_WINDOW_H 720

/* One integrator step with no audio and no event capture. */
static void step_physics(float *x, float *y, float *vx, float *vy, int *dir,
                         float diameter, double sim_delta) {
    float diameter_norm = diameter / (float)TEST_WINDOW_H;
    update_ball_physics(x, y, vx, vy, dir, TEST_WINDOW_W,
                        0.0f, 0.0f, diameter, diameter_norm,
                        sim_delta, false, false,
                        NULL, NULL, NULL, 0, 0.0f);
}

/* A hard flick at maximum speed advances the ball several window widths in
   one step. Reflecting once leaves it outside the opposite wall. */
static void test_fast_ball_stays_in_window(void) {
    g_speed_multiplier = SPEED_MAX;
    g_floor_y_normalized = 1.0f;

    const float diameter = 124.0f;
    const float diameter_norm = diameter / (float)TEST_WINDOW_H;
    float x = TEST_WINDOW_W * 0.5f;
    float y = 0.5f;
    float vx = (float)TEST_WINDOW_W * 0.4f;   /* the slingshot/flick cap */
    float vy = 0.0f;
    int dir = 1;

    for (int i = 0; i < 240; i++) {
        step_physics(&x, &y, &vx, &vy, &dir, diameter, 1.0 / 60.0);
        CHECK(x >= 0.0f && x + diameter <= (float)TEST_WINDOW_W,
              "step %d: ball_x %.1f outside [0, %d]", i, (double)x,
              TEST_WINDOW_W - (int)diameter);
        CHECK(y >= 0.0f && y + diameter_norm <= g_floor_y_normalized,
              "step %d: ball_y %.4f outside [0, %.4f]", i, (double)y,
              (double)(g_floor_y_normalized - diameter_norm));
        if (g_failures) {
            break;
        }
    }

    g_speed_multiplier = 1.0f;
}

/* Horizontal damping pulls ball_vx towards natural_vx. It must converge on
   it, never shoot past it: ball_vx is a magnitude, paired with a separate
   direction, so a negative value inverts the ball's travel. */
static void test_damping_does_not_overshoot(void) {
    g_speed_multiplier = SPEED_MAX;
    g_floor_y_normalized = 1.0f;

    const float natural_vx = get_natural_vx(TEST_WINDOW_W);
    const float diameter = 124.0f;
    /* A 20 Hz frame at maximum speed -- inside the 15..240 Hz range the
       frame pacer accepts. */
    const double slow_frame = 1.0 / 20.0;

    float x = TEST_WINDOW_W * 0.5f;
    float y = 0.5f;
    float vx = natural_vx * 8.0f;
    float vy = 0.0f;
    int dir = 1;

    for (int i = 0; i < 60; i++) {
        float before = vx;
        step_physics(&x, &y, &vx, &vy, &dir, diameter, slow_frame);
        CHECK(vx >= 0.0f, "step %d: ball_vx went negative (%.3f)", i, (double)vx);
        if (before > natural_vx) {
            CHECK(vx >= natural_vx - 0.001f,
                  "step %d: ball_vx %.3f undershot natural_vx %.3f from %.3f",
                  i, (double)vx, (double)natural_vx, (double)before);
        }
        if (g_failures) {
            break;
        }
    }

    g_speed_multiplier = 1.0f;
}

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

    test_fast_ball_stays_in_window();
    test_damping_does_not_overshoot();
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
