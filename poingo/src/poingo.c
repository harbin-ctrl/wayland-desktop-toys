
#include <stdio.h>
#include <stdbool.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <signal.h>
#include <time.h>
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#if defined(__unix__)
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>
#if defined(__linux__)
#include <sys/syscall.h>
#include <linux/memfd.h>
#endif
#include <pthread.h>
#endif
#include <stdint.h>


#if defined(HAVE_CUBEB)
#include <cubeb/cubeb.h>
#endif

#include "ringmenu.h"
#include "toy_audio.h"
#include "lodepng.h"

#include <stdatomic.h>


static inline uint64_t poingo_perf_freq(void) { return 1000000000ULL; }
static inline uint64_t poingo_perf_counter(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
static inline uint32_t poingo_ticks_ms(void) {
    return (uint32_t)(poingo_perf_counter() / 1000000ULL);
}

static inline int poingo_cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

typedef struct { atomic_int value; } PoingoAtomic;
static inline int poingo_atomic_set(PoingoAtomic *a, int v) {
    return atomic_exchange(&a->value, v);
}
static inline int poingo_atomic_get(PoingoAtomic *a) {
    return atomic_load(&a->value);
}
static inline int poingo_atomic_add(PoingoAtomic *a, int v) {
    return atomic_fetch_add(&a->value, v);
}

typedef int (*PoingoThreadFn)(void *);
typedef struct { PoingoThreadFn fn; void *data; } PoingoThreadTramp;
static void *poingo_thread_trampoline(void *arg) {
    PoingoThreadTramp *tr = arg;
    tr->fn(tr->data);
    free(tr);
    return NULL;
}
static pthread_t *poingo_thread_create(PoingoThreadFn fn, const char *name,
                                       void *data) {
    (void)name;
    PoingoThreadTramp *tr = malloc(sizeof(*tr));
    if (!tr) return NULL;
    tr->fn = fn;
    tr->data = data;
    pthread_t *t = malloc(sizeof(pthread_t));
    if (!t) { free(tr); return NULL; }
    if (pthread_create(t, NULL, poingo_thread_trampoline, tr) != 0) {
        free(t);
        free(tr);
        return NULL;
    }
    return t;
}
static void poingo_thread_wait(pthread_t *t, int *status) {
    (void)status;
    if (!t) return;
    pthread_join(*t, NULL);
    free(t);
}

#define POINGO_MENU_ITEMS 8
#define POINGO_MENU_DISC 36
enum {
    POINGO_MENU_LIGHT = 1,
    POINGO_MENU_DARK = 2,
    POINGO_MENU_NOSTALGIA = 3,
    POINGO_MENU_POINGO = 4,
    POINGO_MENU_NEWCOLOR = 5,
    POINGO_MENU_MUTE = 6,
    POINGO_MENU_GHOST = 7,
    POINGO_MENU_QUIT = 8,
};
static RingMenu *g_menu = NULL;
static uint32_t *g_menu_scratch = NULL;
static size_t g_menu_scratch_cap = 0;
static int g_menu_rect[4] = {0, 0, -1, -1};
static unsigned int g_menu_tex = 0;
static bool g_menu_was_open = false;
static int g_picker_slot = -1;      
static bool g_picker_locked = false;
static uint8_t g_picker_original[3];
static uint8_t g_picker_color[3];

#include <wayland-client.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include "xdg-shell-client-protocol.h"
#include <linux/input.h>
#include <linux/input-event-codes.h>
#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "lodepng.h"
#include "ringmenu.h"
#include "ghost_icon.h"

#define WINDOW_WIDTH 640
#define WINDOW_HEIGHT 480

#define GRAVITY 0.00014f
#define CANVAS_WIDTH 480.0f
#define CANVAS_HEIGHT 270.0f
#define FLOOR_Y_NORMALIZED 0.9f
#define TARGET_PEAK_Y 0.1f


#define FPS 60
#define ROT_PERIOD 0.5f  // Half second per half rotation

static int g_ball_w = 512;
static int g_ball_h = 512;
#define BALL_W g_ball_w
#define BALL_H g_ball_h
#define BALL_R (g_ball_w / 2.0f)

#define FREERANGE_REGEN_BLOCK_SIZE 8

#define REGEN_TIME_BUDGET_MS       5.0
#define REGEN_TIMER_CHECK_INTERVAL 4
#define FREERANGE_REGEN_TARGET_MS  2.0
#define FREERANGE_REGEN_COLD_START 4
#define FREERANGE_REGEN_TARGET_TICKS 120.0f

#ifndef PI
#define PI 3.14159265358979323846f
#endif

#define LON_TILES 16
#define LAT_TILES 8

#define COLOR_LIGHT_R 131
#define COLOR_LIGHT_G 233
#define COLOR_LIGHT_B 255
#define COLOR_DARK_R 41
#define COLOR_DARK_G 173
#define COLOR_DARK_B 255

#define BG_COLOR_R 86
#define BG_COLOR_G 78
#define BG_COLOR_B 71
#define GRID_COLOR_R 99
#define GRID_COLOR_G 95
#define GRID_COLOR_B 97
#define GRID_A_COLOR_R 101
#define GRID_A_COLOR_G 91
#define GRID_A_COLOR_B 134
#define FRAMES_N (FPS * ROT_PERIOD * (LON_TILES / 2))
#define PERIOD_FRAMES ((int)(FRAMES_N * (2.0f / LON_TILES) + 0.5f))
#define FRAME_COUNT PERIOD_FRAMES
#define HALF_PERIOD ((FRAME_COUNT / 2))
#define ADV_PER_TICK (PERIOD_FRAMES / (ROT_PERIOD * FPS))

#define SAMPLE_RATE 48000
#define AUDIO_BUFFER_SIZE 2048

#define MASTER_VOLUME_MIN 0.0f
#define MASTER_VOLUME_MAX 1.5f
#define MASTER_VOLUME_STEP 0.05f
#define MASTER_VOLUME_FINE_STEP (MASTER_VOLUME_STEP * 0.2f)
#define VOLUME_HUD_HOLD_TIME 1.5f

#define MIN_BOUNCE_SOUND_SPEED_RATIO 0.4f
#define BOUNCE_DEBOUNCE_SECONDS 0.095f  // Legacy upper bound for predictive bounce dedupe
#define RECENT_BOUNCE_PLAYBACK_COUNT 12
#define AUDIO_SHUTDOWN_CHECK_MS 100u
#define AUDIO_SHUTDOWN_MAX_MS 2000u
#define AUDIO_PREDICT_SECONDS 2.0f
#define AUDIO_PREDICT_DELAY_SECONDS 0.015f
#define AUDIO_PREDICT_HORIZON_MIN_SECONDS 0.25f
#define AUDIO_PREDICT_HORIZON_MARGIN_SECONDS 0.10f
#define AUDIO_PREDICT_HORIZON_DELAY_MULTIPLIER 2.0f
#define AUDIO_PREDICT_HORIZON_MAX_SECONDS 0.75f
#define AUDIO_PREDICT_ONSET_COMPENSATION_SECONDS 0.0125f
#define AUDIO_PREDICT_MAX_EVENTS 128
#define AUDIO_PREDICT_FIXED_STEP_SECONDS (1.0f / 240.0f)
#define AUDIO_PREDICT_QUEUE_MAX 128
#define AUDIO_PREDICT_EVENT_STALE_SECONDS 0.150f
#define AUDIO_PREDICT_PLAYED_RETENTION_SECONDS 0.250f

static float AUDIO_NORMALIZED_MIN = 0.0f;
static const float AUDIO_NORMALIZED_MAX = 10.0f;
static const float AUDIO_RESCALE_MIN = 0.1f;
static const float AUDIO_RESCALE_MAX = 1.0f;
#define VOLUME_HUD_FADE_TIME 0.5f

typedef enum {
    DECOR_KIND_CLOSE = 0,
    DECOR_KIND_FULLSCREEN,
    DECOR_KIND_RESIZE
} DecorKind;

typedef enum {
    INTERACTION_IDLE = 0,
    INTERACTION_BALL_DRAG,
    INTERACTION_WINDOW_DRAG,
    INTERACTION_RESIZE_DRAG,
    INTERACTION_DECOR_PRESS
} InteractionMode;

typedef struct {
    InteractionMode mode;
    DecorKind decor_kind;  
    int pointer_x;
    int pointer_y;
    bool button_down;
} InteractionState;


static bool g_freerange_trace = false;
static volatile sig_atomic_t g_freerange_quit_requested = 0;
static float g_floor_y_normalized = FLOOR_Y_NORMALIZED;
static float g_target_peak_y = TARGET_PEAK_Y;

#define SPEED_MIN 0.1f
#ifdef SPEED_MAX
#undef SPEED_MAX
#endif
#define SPEED_MAX 9.9f
#define SPEED_STEP 0.1f
#define SPEED_FINE_STEP (SPEED_STEP * 0.2f)
#define SPEED_HUD_HOLD_TIME 1.5f
#define SPEED_HUD_FADE_TIME 0.5f
#define FREEDOM_BALL_SCALE_MIN 0.25f
#define FREEDOM_BALL_SCALE_MAX 1.5f
#define FREEDOM_BALL_SCALE_STEP 0.05f

static uint8_t g_color_light_rgb[3] = { COLOR_LIGHT_R, COLOR_LIGHT_G, COLOR_LIGHT_B };
static uint8_t g_color_dark_rgb[3] = { COLOR_DARK_R, COLOR_DARK_G, COLOR_DARK_B };
static uint8_t g_grid_color_rgb[3] = { GRID_COLOR_R, GRID_COLOR_G, GRID_COLOR_B };
static uint32_t g_color_drop_light[POINGO_MENU_DISC * POINGO_MENU_DISC];
static uint32_t g_color_drop_dark[POINGO_MENU_DISC * POINGO_MENU_DISC];

static void poingo_menu_set_drop(int slot, const uint8_t rgb[3]) {
    if (!g_menu) return;
    uint32_t *img = slot == 0 ? g_color_drop_light : g_color_drop_dark;
    if (ringmenu_color_drop_into(img, POINGO_MENU_DISC * POINGO_MENU_DISC,
                                 rgb[0], rgb[1], rgb[2], slot,
                                 POINGO_MENU_ITEMS, POINGO_MENU_DISC))
        ringmenu_update_image(g_menu, slot, img);
}

static void poingo_menu_sync_drops(void) {
    poingo_menu_set_drop(0, g_color_light_rgb);
    poingo_menu_set_drop(1, g_color_dark_rgb);
}
static bool g_a_mode = false;
static float g_ball_tilt_deg = 15.0f;
static bool g_background_needs_refresh = false;

static uint32_t gcd_u32(uint32_t a, uint32_t b);
static void regen_make_shuffled_order(uint32_t *order, uint32_t total);
static inline float clamp_freerange_ball_scale(float value);
static float remap_normalized_scale_to_sound(float normalized_scale);
void mark_sounds_dirty(float size_scale);
static inline void window_coords_to_render(int logical_x, int logical_y,
                                           int logical_w, int logical_h,
                                           int render_w, int render_h,
                                           int *out_x, int *out_y);


#define KEY_REPEAT_INITIAL_DELAY 0.50f   // Half second before first repeat
#define KEY_REPEAT_INTERVAL      0.12f   // ~8 repeats per second when holding

#define MIN_BRIGHTNESS 0.35f
#define AMBIENT_LIGHT 0.3f
#define DIFFUSE_LIGHT 0.7f

static int g_frame_texture_w = 0;
static int g_frame_texture_h = 0;
static int g_ball_texture_offset_x = 0;
static int g_ball_texture_offset_y = 0;

static float axis_ax, axis_ay, axis_az;
static float axis_ux, axis_uy, axis_uz;
static float axis_vx, axis_vy, axis_vz;

static InteractionState g_interaction_state;
static float get_audio_predict_effective_delay_seconds(void);
static float get_audio_predict_horizon_seconds(void);
static void update_bounce_sound_style_for_mode(void);

#if defined(HAVE_CUBEB)
typedef struct {
    uint32_t channels;
} CubebProbeState;

typedef struct {
    cubeb *ctx;
    cubeb_stream *stream;
    uint32_t rate;
    uint32_t latency_frames;
    uint32_t channels;
    uint32_t start_ticks;
    uint32_t last_update_ticks;
    bool active;
    char backend_id[24];
} CubebProbeRuntime;

static CubebProbeRuntime g_cubeb_probe = {0};
static PoingoAtomic g_cubeb_probe_restart_requested;
#endif

static inline bool interaction_is_ball_drag(void) {
    return g_interaction_state.mode == INTERACTION_BALL_DRAG;
}

static inline void interaction_begin_ball_drag(int pointer_x, int pointer_y) {
    g_interaction_state.mode = INTERACTION_BALL_DRAG;
    g_interaction_state.pointer_x = pointer_x;
    g_interaction_state.pointer_y = pointer_y;
    g_interaction_state.button_down = true;
}

static inline void interaction_end_ball_drag(void) {
    if (g_interaction_state.mode == INTERACTION_BALL_DRAG) {
        g_interaction_state.mode = INTERACTION_IDLE;
        g_interaction_state.button_down = false;
    }
}



typedef struct {
    float lon_norm;
    uint8_t light_rgb[3];
    uint8_t dark_rgb[3];
    uint8_t lat_index;
    uint8_t valid;
    uint8_t padding[2];
} SpherePixelCache;

static SpherePixelCache *sphere_cache = NULL;
static int sphere_cache_size = 0;
static int sphere_cache_capacity_size = 0;

static void invalidate_sphere_pixel_cache(void) {
    /* Keep the storage; color/mode changes only require recomputation. */
    sphere_cache_size = 0;
}

static void release_sphere_pixel_cache(void) {
    free(sphere_cache);
    sphere_cache = NULL;
    sphere_cache_size = 0;
    sphere_cache_capacity_size = 0;
}

static inline void compute_axis_vectors(void) {
    float tilt_rad = g_ball_tilt_deg * PI / 180.0f;
    float s = sinf(tilt_rad);
    float c = cosf(tilt_rad);

    axis_ax = s;
    axis_ay = c;
    axis_az = 0.0f;

    float hx = 0.0f, hy = 0.0f, hz = 1.0f;
    if (fabsf(axis_ax * hx + axis_ay * hy + axis_az * hz) > 0.99f) {
        hx = 1.0f;
        hy = 0.0f;
        hz = 0.0f;
    }

    axis_ux = hy * axis_az - hz * axis_ay;
    axis_uy = hz * axis_ax - hx * axis_az;
    axis_uz = hx * axis_ay - hy * axis_ax;
    float ulen = sqrtf(axis_ux * axis_ux + axis_uy * axis_uy + axis_uz * axis_uz);
    axis_ux /= ulen;
    axis_uy /= ulen;
    axis_uz /= ulen;

    axis_vx = axis_ay * axis_uz - axis_az * axis_uy;
    axis_vy = axis_az * axis_ux - axis_ax * axis_uz;
    axis_vz = axis_ax * axis_uy - axis_ay * axis_ux;
}

static bool set_a_mode_enabled(bool enabled) {
    if (g_a_mode == enabled) {
        return false;
    }
    g_a_mode = enabled;
    if (g_a_mode) {
        g_grid_color_rgb[0] = GRID_A_COLOR_R;
        g_grid_color_rgb[1] = GRID_A_COLOR_G;
        g_grid_color_rgb[2] = GRID_A_COLOR_B;
        g_ball_tilt_deg = -12.5f;
    } else {
        g_grid_color_rgb[0] = GRID_COLOR_R;
        g_grid_color_rgb[1] = GRID_COLOR_G;
        g_grid_color_rgb[2] = GRID_COLOR_B;
        g_ball_tilt_deg = 15.0f;
    }
    compute_axis_vectors();
    update_bounce_sound_style_for_mode();
    invalidate_sphere_pixel_cache();
    g_background_needs_refresh = true;
    return true;
}

static inline float clampf(float value, float min_val, float max_val) {
    if (value < min_val) return min_val;
    if (value > max_val) return max_val;
    return value;
}

static void pick_palette_colors(float light_value,
                                uint8_t out_light[3], uint8_t out_dark[3],
                                bool allow_white);
static bool ensure_sphere_pixel_cache(int size);
static double get_perf_seconds(uint64_t start_counter, uint64_t end_counter);
static bool g_debug_mode;
static void audio_predict_reset(float now_time);


static void pick_palette_colors(float light_value,
                                uint8_t out_light[3], uint8_t out_dark[3],
                                bool allow_white) {
    (void)allow_white;

    typedef struct {
        uint8_t light[3];
        uint8_t dark[3];
    } BallPalettePair;

    static const BallPalettePair palette_pairs[] = {
        { {255, 196,  54}, {164,  51,  87} },  
        { {255, 142,  52}, { 84,  48, 156} },  
        { {255, 224,  74}, {181,  72, 142} },  
        { {255, 120, 176}, { 64,  92, 198} },  
        { {169, 245,  92}, {  0, 150, 124} }   
    };

    int palette_count = (int)(sizeof(palette_pairs) / sizeof(palette_pairs[0]));
    if (light_value < 0.0f) light_value = 0.0f;
    if (light_value > 1.0f) light_value = 1.0f;

    int current_pair = -1;
    for (int i = 0; i < palette_count; ++i) {
        if (memcmp(palette_pairs[i].light, g_color_light_rgb, 3) == 0 &&
            memcmp(palette_pairs[i].dark, g_color_dark_rgb, 3) == 0) {
            current_pair = i;
            break;
        }
    }

    int pair_index = random() % palette_count;
    if (palette_count > 1) {
        while (pair_index == current_pair) {
            pair_index = random() % palette_count;
        }
    }

    float light_target = light_value * 255.0f;
    float dark_target = light_target * 0.78f;
    const BallPalettePair *pair = &palette_pairs[pair_index];

    float base_light_max = fmaxf(pair->light[0], fmaxf(pair->light[1], pair->light[2]));
    float base_dark_max = fmaxf(pair->dark[0], fmaxf(pair->dark[1], pair->dark[2]));
    float light_scale = (base_light_max > 0.0f) ? (light_target / base_light_max) : 0.0f;
    float dark_scale = (base_dark_max > 0.0f) ? (dark_target / base_dark_max) : 0.0f;

    for (int c = 0; c < 3; ++c) {
        out_light[c] = (uint8_t)clampf(pair->light[c] * light_scale, 0.0f, 255.0f);
        out_dark[c] = (uint8_t)clampf(pair->dark[c] * dark_scale, 0.0f, 255.0f);
    }
}




static bool ensure_sphere_pixel_cache(int size) {
    if (sphere_cache && sphere_cache_size == size) {
        return true;
    }

    if (sphere_cache && sphere_cache_capacity_size != size) {
        /* A different cache size would require a runtime allocation. All
         * callers use the startup-selected ball texture size. */
        return false;
    }

    size_t total_pixels = (size_t)size * (size_t)size;
    SpherePixelCache *cache = sphere_cache;
    if (!cache) {
        cache = (SpherePixelCache *)calloc(total_pixels, sizeof(SpherePixelCache));
        if (!cache) return false;
    }

    float light_x = -0.3f;
    float light_y = -0.5f;
    float light_z = 1.0f;
    float light_len = sqrtf(light_x * light_x + light_y * light_y + light_z * light_z);
    float inv_light_len = (light_len != 0.0f) ? (1.0f / light_len) : 1.0f;
    light_x *= inv_light_len;
    light_y *= inv_light_len;
    light_z *= inv_light_len;

    float radius = size / 2.0f;
    float cx = radius - 0.5f;
    float cy = radius - 0.5f;
    float radius_sq = radius * radius;
    float inv_radius = (radius != 0.0f) ? (1.0f / radius) : 0.0f;

    float half_pi = PI * 0.5f;
    float lat_scale = (float)LAT_TILES / PI;
    float inv_two_pi = 1.0f / (2.0f * PI);

    for (int y = 0; y < size; y++) {
        float dy = (float)y - cy;
        float rem = radius_sq - dy * dy;
        if (rem < 0.0f) {
            continue;
        }

        float dx = sqrtf(rem);
        int xl = (int)ceilf(cx - dx);
        int xr = (int)floorf(cx + dx);
        if (xl < 0) xl = 0;
        if (xr >= size) xr = size - 1;

        float vy = dy * inv_radius;
        float vy_sq = vy * vy;

        for (int x = xl; x <= xr; x++) {
            float vx = ((float)x - cx) * inv_radius;
            float r2 = vx * vx + vy_sq;
            if (r2 > 1.0f) {
                continue;
            }

            float nz = sqrtf(1.0f - r2);
            float nx = vx;
            float ny = vy;

            float dot_product = nx * light_x + ny * light_y + nz * light_z;
            float brightness = AMBIENT_LIGHT + DIFFUSE_LIGHT * fmaxf(0.0f, dot_product);
            brightness = clampf(brightness, MIN_BRIGHTNESS, 1.0f);

            float na = nx * axis_ax + ny * axis_ay + nz * axis_az;
            float nu = nx * axis_ux + ny * axis_uy + nz * axis_uz;
            float nv = nx * axis_vx + ny * axis_vy + nz * axis_vz;

            float lat = asinf(clampf(na, -1.0f, 1.0f));
            float lon = atan2f(nv, nu);

            float lat_scaled = (lat + half_pi) * lat_scale;
            int v = (int)floorf(lat_scaled);
            if (v < 0) v = 0;
            if (v >= LAT_TILES) v = LAT_TILES - 1;

            float lon_norm = lon * inv_two_pi;
            lon_norm -= floorf(lon_norm);  

            int idx = y * size + x;
            SpherePixelCache *c = &cache[idx];
            c->valid = 1;
            c->lat_index = (uint8_t)v;
            c->lon_norm = lon_norm;
            c->light_rgb[0] = (uint8_t)clampf(g_color_light_rgb[0] * brightness, 0.0f, 255.0f);
            c->light_rgb[1] = (uint8_t)clampf(g_color_light_rgb[1] * brightness, 0.0f, 255.0f);
            c->light_rgb[2] = (uint8_t)clampf(g_color_light_rgb[2] * brightness, 0.0f, 255.0f);
            c->dark_rgb[0] = (uint8_t)clampf(g_color_dark_rgb[0] * brightness, 0.0f, 255.0f);
            c->dark_rgb[1] = (uint8_t)clampf(g_color_dark_rgb[1] * brightness, 0.0f, 255.0f);
            c->dark_rgb[2] = (uint8_t)clampf(g_color_dark_rgb[2] * brightness, 0.0f, 255.0f);
        }
    }

    sphere_cache = cache;
    sphere_cache_size = size;
    sphere_cache_capacity_size = size;
    return true;
}

static double get_perf_seconds(uint64_t start_counter, uint64_t end_counter) {
    uint64_t freq = poingo_perf_freq();
    if (freq == 0 || end_counter < start_counter) {
        return 0.0;
    }
    return (double)(end_counter - start_counter) / (double)freq;
}



__attribute__((hot))
static void generate_sphere_pixels(uint8_t * restrict pixels, const int size, const float phase_offset) {
    if (!pixels) return;

    if (!sphere_cache || sphere_cache_size != size) {
        return;
    }

    memset(pixels, 0, (size_t)size * size * 4);

    float phase_norm = phase_offset * (1.0f / (2.0f * PI));
    size_t total_pixels = (size_t)size * (size_t)size;

    for (size_t i = 0; i < total_pixels; i++) {
        const SpherePixelCache *c = &sphere_cache[i];
        if (!c->valid) {
            continue;
        }

        float lon_norm = c->lon_norm + phase_norm;
        lon_norm -= floorf(lon_norm);

        int u = (int)(lon_norm * (float)LON_TILES);
        if (u >= LON_TILES) u = LON_TILES - 1;

        bool is_light = ((u + c->lat_index) & 1) == 0;
        const uint8_t *src = is_light ? c->light_rgb : c->dark_rgb;

        size_t idx = i * 4;
        pixels[idx + 0] = src[0];
        pixels[idx + 1] = src[1];
        pixels[idx + 2] = src[2];
        pixels[idx + 3] = 255;
    }
}

__attribute__((hot))
static void generate_sphere_pixels_band(uint8_t * restrict pixels,
                                        const int size,
                                        const float phase_offset,
                                        const int y0,
                                        const int y1) {
    if (!pixels || size <= 0) return;
    if (!sphere_cache || sphere_cache_size != size) return;

    int start_y = y0;
    int end_y = y1;
    if (start_y < 0) start_y = 0;
    if (end_y > size) end_y = size;
    if (start_y >= end_y) return;

    float phase_norm = phase_offset * (1.0f / (2.0f * PI));

    for (int y = start_y; y < end_y; y++) {
        uint8_t *row = pixels + (size_t)y * (size_t)size * 4;
        memset(row, 0, (size_t)size * 4);

        size_t row_base = (size_t)y * (size_t)size;
        for (int x = 0; x < size; x++) {
            const SpherePixelCache *c = &sphere_cache[row_base + (size_t)x];
            if (!c->valid) {
                continue;
            }

            float lon_norm = c->lon_norm + phase_norm;
            lon_norm -= floorf(lon_norm);

            int u = (int)(lon_norm * (float)LON_TILES);
            if (u >= LON_TILES) u = LON_TILES - 1;

            bool is_light = ((u + c->lat_index) & 1) == 0;
            const uint8_t *src = is_light ? c->light_rgb : c->dark_rgb;

            size_t idx = (size_t)x * 4;
            row[idx + 0] = src[0];
            row[idx + 1] = src[1];
            row[idx + 2] = src[2];
            row[idx + 3] = 255;
        }
    }
}

__attribute__((hot))
static uint8_t* generate_shadow_pixels(int shadow_size, int * restrict out_width, int * restrict out_height) {
    int size = shadow_size;
    uint8_t *pixels = NULL;
    if (posix_memalign((void**)&pixels, 32, (size_t)size * size * 4) != 0) {
        return NULL;
    }

    float cx = size / 2.0f;
    float cy = size / 2.0f;
    float shadow_radius = size / 2.0f;
    const float max_alpha = 0.4f;

    const float inner_threshold = shadow_radius * 0.7f;
    const float inner_threshold_sq = inner_threshold * inner_threshold;
    const float outer_threshold_sq = shadow_radius * shadow_radius;
    const float falloff_range = shadow_radius * 0.3f;

    int total_pixels = size * size;
    #pragma GCC ivdep
    for (int i = 0; i < total_pixels; i++) {
        int y = i / size;
        int x = i % size;

        float dx = x - cx;
        float dy = y - cy;
        float dist_sq = dx * dx + dy * dy;

        float alpha = 0.0f;

        if (dist_sq < inner_threshold_sq) {
            alpha = max_alpha;
        } else if (dist_sq < outer_threshold_sq) {
            float dist = sqrtf(dist_sq);
            float t = (dist - inner_threshold) / falloff_range;

            t = t * t * (3.0f - 2.0f * t);
            alpha = (1.0f - t) * max_alpha;
        }

        int idx = i * 4;
        pixels[idx + 0] = 0;
        pixels[idx + 1] = 0;
        pixels[idx + 2] = 0;
        pixels[idx + 3] = (uint8_t)(alpha * 255.0f);
    }

    *out_width = size;
    *out_height = size;
    return pixels;
}

__attribute__((hot))
static void composite_ball_and_shadow(uint8_t * restrict result,
                                      const uint8_t * restrict ball_pixels, const int ball_w, const int ball_h,
                                      const uint8_t * restrict shadow_pixels, const int shadow_w, const int shadow_h,
                                      const int composite_w, const int composite_h,
                                      const int ball_x, const int ball_y,
                                      const int shadow_x, const int shadow_y) {
    size_t alloc_size = (size_t)composite_w * (size_t)composite_h * 4;
    memset(result, 0, alloc_size);

    for (int y = 0; y < shadow_h; y++) {
        int dst_y = shadow_y + y;
        if (dst_y < 0 || dst_y >= composite_h) continue;

        for (int x = 0; x < shadow_w; x++) {
            int dst_x = shadow_x + x;
            if (dst_x < 0 || dst_x >= composite_w) continue;

            int src_idx = (y * shadow_w + x) * 4;
            int dst_idx = (dst_y * composite_w + dst_x) * 4;

            result[dst_idx + 0] = shadow_pixels[src_idx + 0];
            result[dst_idx + 1] = shadow_pixels[src_idx + 1];
            result[dst_idx + 2] = shadow_pixels[src_idx + 2];
            result[dst_idx + 3] = shadow_pixels[src_idx + 3];
        }
    }

    for (int y = 0; y < ball_h; y++) {
        int dst_y = ball_y + y;
        if (dst_y < 0 || dst_y >= composite_h) continue;

        for (int x = 0; x < ball_w; x++) {
            int dst_x = ball_x + x;
            if (dst_x < 0 || dst_x >= composite_w) continue;

            int src_idx = (y * ball_w + x) * 4;
            int dst_idx = (dst_y * composite_w + dst_x) * 4;

            uint8_t src_a = ball_pixels[src_idx + 3];
            if (src_a == 0) continue;

            uint8_t dst_a = result[dst_idx + 3];

            if (src_a == 255) {
                result[dst_idx + 0] = ball_pixels[src_idx + 0];
                result[dst_idx + 1] = ball_pixels[src_idx + 1];
                result[dst_idx + 2] = ball_pixels[src_idx + 2];
                result[dst_idx + 3] = 255;
            } else {
                unsigned int inv_src_a = 255 - src_a;
                unsigned int out_a = src_a + ((dst_a * inv_src_a) / 255);

                if (out_a > 0) {
                    for (int c = 0; c < 3; c++) {
                        unsigned int src_c = ball_pixels[src_idx + c];
                        unsigned int dst_c = result[dst_idx + c];
                        unsigned int out_c = (src_c * src_a + dst_c * dst_a * inv_src_a / 255) / out_a;
                        result[dst_idx + c] = (uint8_t)out_c;
                    }
                    result[dst_idx + 3] = (uint8_t)out_a;
                }
            }
        }
    }
}

typedef struct {
    int frame_count;
    float angle_period;
    int sphere_texture_size;
    const uint8_t *shadow_pixels;
    int shadow_w;
    int shadow_h;
    int composite_w;
    int composite_h;
    int ball_offset_x;
    int ball_offset_y;
    int shadow_offset_x;
    int shadow_offset_y;
    uint8_t *frame_buffer;
    size_t frame_size;
    PoingoAtomic next_index;
    PoingoAtomic failure;
    PoingoAtomic completed;
} BallFrameJobContext;


static int generate_ball_frame_worker(void *userdata) {
    BallFrameJobContext *ctx = (BallFrameJobContext *)userdata;

    size_t frame_size = ctx->frame_size;
    size_t ball_size = (size_t)ctx->sphere_texture_size * (size_t)ctx->sphere_texture_size * 4;
    uint8_t *ball_pixels = NULL;
    if (posix_memalign((void**)&ball_pixels, 32, ball_size) != 0) {
        poingo_atomic_set(&ctx->failure, 1);
        return 0;
    }

    for (;;) {
        int index = poingo_atomic_add(&ctx->next_index, 1);
        if (index >= ctx->frame_count) {
            break;
        }

        if (poingo_atomic_get(&ctx->failure)) {
            break;
        }

        float phase_offset = ((float)index / (float)ctx->frame_count) * ctx->angle_period;
        generate_sphere_pixels(ball_pixels, ctx->sphere_texture_size, phase_offset);

        uint8_t * restrict frame_dst =
            (uint8_t * restrict)__builtin_assume_aligned(ctx->frame_buffer + (size_t)index * frame_size, 32);

        composite_ball_and_shadow(
            frame_dst,
            ball_pixels, ctx->sphere_texture_size, ctx->sphere_texture_size,
            ctx->shadow_pixels, ctx->shadow_w, ctx->shadow_h,
            ctx->composite_w, ctx->composite_h,
            ctx->ball_offset_x, ctx->ball_offset_y,
            ctx->shadow_offset_x, ctx->shadow_offset_y);

        poingo_atomic_add(&ctx->completed, 1);
    }

    free(ball_pixels);
    return 0;
}



enum {
    BALL_REGEN_FILL = 0,
    BALL_REGEN_CLEAR = 1
};










typedef enum {
    BOUNCE_SOUND_POINGO = 0,
    BOUNCE_SOUND_NOSTALGIA = 1
} BounceSoundStyle;

static BounceSoundStyle g_bounce_sound_style = BOUNCE_SOUND_POINGO;

static ToyBounceStyle toy_style(void) {
    return g_bounce_sound_style == BOUNCE_SOUND_NOSTALGIA
               ? TOY_BOUNCE_NOSTALGIA : TOY_BOUNCE_POINGO;
}

typedef struct {
    ToySamplePair bounce;
    float pending_size_scale;
    float current_size_scale;
} AudioState;

static ToyMixer g_mixer;
static bool g_ghost_mute = false;

typedef struct {
    float time;
    int volume;
    int surface;
    float pan;      
} BounceEvent;

typedef struct {
    BounceEvent events[AUDIO_PREDICT_MAX_EVENTS];
    int count;
} AudioPredictBuffer;

typedef struct {
    BounceEvent event;
    float last_seen_time;
    bool active;
    bool played;
} AudioPredictQueueEntry;

typedef struct {
    float time;
    int volume;
    int surface;
    bool valid;
} RecentBouncePlayback;

typedef struct {
    unsigned long long actual_collisions_total;
    unsigned long long actual_audible_total;
    unsigned long long actual_soft_total;
    unsigned long long predict_window_total;
    unsigned long long predict_soft_total;
    unsigned long long predict_deduped_total;
    unsigned long long predict_played_total;
    unsigned long long actual_collisions_window;
    unsigned long long actual_audible_window;
    unsigned long long actual_soft_window;
    unsigned long long predict_window_events_window;
    unsigned long long predict_soft_window;
    unsigned long long predict_deduped_window;
    unsigned long long predict_played_window;
    float window_start_time;
    float last_log_time;
    bool overlay_enabled;
} AudioDebugStats;

typedef struct {
    uint32_t current_buttons;
    uint8_t last_button;
    uint8_t last_state;
} MouseDebugState;

AudioState audio_state = {
    .bounce = { NULL, NULL, 0, true },
    .pending_size_scale = 1.0f,
    .current_size_scale = 1.0f,
};

static bool audio_device_open = false;

static pthread_mutex_t g_audio_lock = PTHREAD_MUTEX_INITIALIZER;
static void audio_lock(void)   { pthread_mutex_lock(&g_audio_lock); }
static void audio_unlock(void) { pthread_mutex_unlock(&g_audio_lock); }

static void update_bounce_sound_style_for_mode(void) {
    BounceSoundStyle target_style = g_a_mode ? BOUNCE_SOUND_NOSTALGIA : BOUNCE_SOUND_POINGO;
    if (g_bounce_sound_style == target_style) {
        return;
    }
    g_bounce_sound_style = target_style;
    float size_scale = audio_state.bounce.dirty ? audio_state.pending_size_scale : audio_state.current_size_scale;
    mark_sounds_dirty(fmaxf(size_scale, 0.01f));
}

static AudioPredictBuffer g_audio_predict = {0};
static AudioPredictQueueEntry g_audio_predict_queue[AUDIO_PREDICT_QUEUE_MAX] = {{0}};
static float g_audio_sim_time = 0.0f;
static float g_audio_predict_delay_seconds = AUDIO_PREDICT_DELAY_SECONDS;
static float g_audio_predict_step_seconds = 1.0f / (float)FPS;
enum {
    BOUNCE_SURFACE_FLOOR = 0,
    BOUNCE_SURFACE_CEILING = 1,
    BOUNCE_SURFACE_LEFT = 2,
    BOUNCE_SURFACE_RIGHT = 3,
    BOUNCE_SURFACE_COUNT = 4
};
static RecentBouncePlayback g_recent_bounce_playbacks[RECENT_BOUNCE_PLAYBACK_COUNT] = {{0}};
static int g_recent_bounce_playback_next = 0;
static AudioDebugStats g_audio_debug = {
    .window_start_time = -1000000.0f,
    .last_log_time = -1000000.0f,
    .overlay_enabled = false
};
static bool g_predictive_audio_enabled = true;
static bool g_audio_trace_enabled = false;


static bool audio_is_perceptually_quiet(void) {
    if (!audio_device_open) {
        return true;
    }

    audio_lock();
    bool quiet = toy_mixer_is_quiet(&g_mixer);
    audio_unlock();
    return quiet;
}


static void notify_volume_changed(void);
static void adjust_master_volume(float delta);
static void set_master_mute(bool mute);
static void toggle_master_mute(void);
static void update_volume_hud(float delta_seconds);
static void update_fps_hud(float delta_seconds);
static bool should_block_input(void);

static inline void window_coords_to_render(int logical_x, int logical_y,
                                           int logical_w, int logical_h,
                                           int render_w, int render_h,
                                           int *out_x, int *out_y) {
    float scale_x = (logical_w > 0) ? ((float)render_w / (float)logical_w) : 1.0f;
    float scale_y = (logical_h > 0) ? ((float)render_h / (float)logical_h) : 1.0f;
    if (out_x) {
        *out_x = (int)lroundf((float)logical_x * scale_x);
    }
    if (out_y) {
        *out_y = (int)lroundf((float)logical_y * scale_y);
    }
}

static inline void render_coords_to_window(int render_x, int render_y,
                                           int logical_w, int logical_h,
                                           int render_w, int render_h,
                                           int *out_x, int *out_y) {
    float inv_scale_x = (render_w > 0) ? ((float)logical_w / (float)render_w) : 1.0f;
    float inv_scale_y = (render_h > 0) ? ((float)logical_h / (float)render_h) : 1.0f;
    if (out_x) {
        *out_x = (int)lroundf((float)render_x * inv_scale_x);
    }
    if (out_y) {
        *out_y = (int)lroundf((float)render_y * inv_scale_y);
    }
}

static float g_volume_hud_time = 0.0f;
static float g_volume_hud_alpha = 0.0f;
static float g_volume_hud_value = 1.2f;
static bool g_volume_muted = false;
static float g_speed_multiplier = 1.0f;
static float g_speed_hud_time = 0.0f;
static float g_speed_hud_alpha = 0.0f;
static bool g_suppress_hud = false;
static float g_freerange_ball_scale = 1.0f;
static bool g_debug_mode = false;  

typedef enum {
    BANNER_SHOWING,      
    BANNER_HOLDING,      
    BANNER_FADING,       
    BANNER_HIDDEN        
} BannerState;

typedef struct {
    BannerState state;
    float alpha;              
    float timer;              
    bool block_input;         
    float pulse_timer;        
    bool user_requested;      
} LoadingBanner;

static LoadingBanner g_loading_banner = {
    .state = BANNER_HIDDEN,
    .alpha = 0.0f,
    .timer = 0.0f,
    .block_input = false,
    .pulse_timer = 0.0f,
    .user_requested = false
};

static void android_append_downloads_trace_line(const char *line);

#define BANNER_MINIMUM_TIME 8.0f   // Minimum time banner stays visible (seconds)
#define BANNER_FADE_TIME 0.5f       // Fade out duration (seconds)




static void android_append_downloads_trace_line(const char *line) {
    (void)line;
}


static bool show_fps_hud = false;
#define FPS_HUD_FADE_TIME 0.3f
#define FPS_SAMPLE_COUNT 60
static float fps_hud_alpha = 0.0f;
static float fps_hud_target_alpha = 0.0f;




// NOTE FOR FUTURE POINGO WORK:


typedef struct {
    bool is_pressed;
    float hold_time;
    float repeat_timer;
} KeyRepeatState;

static KeyRepeatState g_vol_up_key = {false, 0.0f, 0.0f};
static KeyRepeatState g_vol_down_key = {false, 0.0f, 0.0f};
static KeyRepeatState g_speed_up_key = {false, 0.0f, 0.0f};
static KeyRepeatState g_speed_down_key = {false, 0.0f, 0.0f};

#if defined(__unix__)

#endif
enum {
    CONSOLE_ESC_NONE = 0,
    CONSOLE_ESC_ESC,
    CONSOLE_ESC_ESC_BRACKET
};

#if defined(HAVE_CUBEB)
static cubeb *g_audio_ctx = NULL;
static cubeb_stream *g_audio_stream = NULL;

__attribute__((hot))
static long audio_data_cb(cubeb_stream *stream, void *user,
                          const void *in, void *out, long nframes) {
    (void)stream; (void)user; (void)in;
    if (!out || nframes <= 0) {
        return nframes;
    }
    audio_lock();
    toy_mixer_render_stereo(&g_mixer, (float *)out, (int)nframes,
                            should_block_input());
    audio_unlock();
    return nframes;
}

static void audio_output_state_cb(cubeb_stream *stream, void *user,
                                  cubeb_state state) {
    (void)stream; (void)user;
    if (state == CUBEB_STATE_ERROR) {
        fprintf(stderr, "poingo: audio output stream error\n");
    }
}
#endif

__attribute__((cold))
static void shutdown_audio(void) {
    if (!audio_device_open) {
        return;
    }

#if defined(HAVE_CUBEB)
    if (g_audio_stream) {
        cubeb_stream_stop(g_audio_stream);
        cubeb_stream_destroy(g_audio_stream);
        g_audio_stream = NULL;
    }
    if (g_audio_ctx) {
        cubeb_destroy(g_audio_ctx);
        g_audio_ctx = NULL;
    }
#endif
    audio_device_open = false;

    toy_sample_pair_free(&audio_state.bounce);
    toy_mixer_reset(&g_mixer);
    toy_audio_release_scratch();
    audio_state.pending_size_scale = 1.0f;
    audio_state.current_size_scale = 1.0f;
    g_volume_muted = false;
    g_volume_hud_alpha = 0.0f;
    g_volume_hud_time = 0.0f;
}

static bool init_audio(bool start_muted) {
#if !defined(HAVE_CUBEB)
    (void)start_muted;
    fprintf(stderr, "poingo: built without cubeb; running silent\n");
    return false;
#else
    if (cubeb_init(&g_audio_ctx, "poingo", NULL) != CUBEB_OK || !g_audio_ctx) {
        fprintf(stderr, "poingo: failed to init cubeb; running silent\n");
        g_audio_ctx = NULL;
        return false;
    }

    cubeb_stream_params params;
    memset(&params, 0, sizeof(params));
    params.format = CUBEB_SAMPLE_FLOAT32NE;
    params.rate = SAMPLE_RATE;
    params.channels = 2;   
    params.layout = CUBEB_LAYOUT_STEREO;
    params.prefs = CUBEB_STREAM_PREF_NONE;

    uint32_t latency_frames = 0;
    if (cubeb_get_min_latency(g_audio_ctx, &params, &latency_frames) != CUBEB_OK ||
        latency_frames == 0) {
        latency_frames = AUDIO_BUFFER_SIZE;
    }

    if (cubeb_stream_init(g_audio_ctx, &g_audio_stream, "poingo",
                          NULL, NULL, NULL, &params, latency_frames,
                          audio_data_cb, audio_output_state_cb, NULL) != CUBEB_OK ||
        !g_audio_stream) {
        fprintf(stderr, "poingo: failed to open cubeb stream; running silent\n");
        cubeb_destroy(g_audio_ctx);
        g_audio_ctx = NULL;
        return false;
    }
    audio_device_open = true;

    toy_mixer_init(&g_mixer, TOY_AUDIO_DEFAULT_VOLUME);
    if (start_muted) {
        g_mixer.muted = true;
        g_mixer.master_volume = 0.0f;
        notify_volume_changed();
    }

    if (!toy_sample_pair_alloc(&audio_state.bounce,
                               toy_audio_bounce_pair_length())) {
        fprintf(stderr, "Failed to allocate audio sample buffers\n");
        shutdown_audio();
        return false;
    }
    if (!toy_mixer_reserve(&g_mixer, audio_state.bounce.length) ||
        !toy_audio_reserve_scratch(audio_state.bounce.length)) {
        fprintf(stderr, "Failed to allocate audio mixer workspace\n");
        shutdown_audio();
        return false;
    }

    if (cubeb_stream_start(g_audio_stream) != CUBEB_OK) {
        fprintf(stderr, "poingo: failed to start cubeb stream; running silent\n");
        shutdown_audio();
        return false;
    }
    return true;
#endif
}

void play_bounce_sound(int volume, float pan) {
    audio_lock();  

    float size_scale = fmaxf(audio_state.current_size_scale, 0.05f);

    if (audio_state.bounce.dirty) {
        size_scale = fmaxf(audio_state.pending_size_scale, 0.05f);
        toy_audio_generate_bounce_pair(&audio_state.bounce, size_scale,
                                       toy_style());
        audio_state.current_size_scale = size_scale;
    }

    float size_factor = 0.7f + 0.3f * fminf(size_scale, 1.0f);

    float velocity_factor = fminf(volume / 32.0f, 1.0f);
    velocity_factor = fmaxf(velocity_factor, 0.6f);  
    if (g_ghost_mute) velocity_factor *= 0.05f;

    if (pan < 0.0f) pan = 0.0f;
    if (pan > 1.0f) pan = 1.0f;
    float pan_shaped = g_a_mode ? pan : (0.08f + 0.84f * pan);
    float l_gain, r_gain;
    toy_audio_equal_power_pan(pan_shaped, &l_gain, &r_gain);

    toy_mixer_start_voice_panned(&g_mixer, &audio_state.bounce,
                                 size_factor * velocity_factor,
                                 l_gain, r_gain);

    audio_unlock();
}

static void adjust_master_volume(float delta) {
    if (delta == 0.0f) {
        return;
    }

    audio_lock();
    toy_mixer_adjust_volume(&g_mixer, delta);
    audio_unlock();

    notify_volume_changed();
}

static void notify_volume_changed(void) {
    g_volume_muted = g_mixer.muted;
    g_volume_hud_value = g_mixer.muted && g_mixer.master_volume <= MASTER_VOLUME_MIN
        ? g_mixer.master_volume_before_mute
        : g_mixer.master_volume;
    g_volume_hud_value = fmaxf(MASTER_VOLUME_MIN, fminf(MASTER_VOLUME_MAX, g_volume_hud_value));
    if (g_suppress_hud) {
        return;
    }
    g_volume_hud_time = VOLUME_HUD_HOLD_TIME + VOLUME_HUD_FADE_TIME;
    g_volume_hud_alpha = 1.0f;
}

static inline float clamp_speed(float value) {
    if (value < SPEED_MIN) return SPEED_MIN;
    if (value > SPEED_MAX) return SPEED_MAX;
    return value;
}

static inline float get_natural_vx(int window_w) {
    return (float)window_w / 273.0f;
}

static inline float clamp_freerange_ball_scale(float value) {
    if (value < FREEDOM_BALL_SCALE_MIN) return FREEDOM_BALL_SCALE_MIN;
    if (value > FREEDOM_BALL_SCALE_MAX) return FREEDOM_BALL_SCALE_MAX;
    return value;
}

static bool parse_color_arg(const char *value, uint8_t out_rgb[3]) {
    if (!value || !out_rgb) {
        return false;
    }

    const char *hex = value;
    if (hex[0] == '#') {
        hex++;
    } else if (hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
        hex += 2;
    }

    if (strlen(hex) == 6) {
        for (int i = 0; i < 6; i++) {
            if (!isxdigit((unsigned char)hex[i])) {
                return false;
            }
        }
        char buf[3] = {0};
        buf[0] = hex[0];
        buf[1] = hex[1];
        out_rgb[0] = (uint8_t)strtoul(buf, NULL, 16);
        buf[0] = hex[2];
        buf[1] = hex[3];
        out_rgb[1] = (uint8_t)strtoul(buf, NULL, 16);
        buf[0] = hex[4];
        buf[1] = hex[5];
        out_rgb[2] = (uint8_t)strtoul(buf, NULL, 16);
        return true;
    }

    int r = -1, g = -1, b = -1;
    if (sscanf(value, "%d,%d,%d", &r, &g, &b) == 3) {
        if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255) {
            return false;
        }
        out_rgb[0] = (uint8_t)r;
        out_rgb[1] = (uint8_t)g;
        out_rgb[2] = (uint8_t)b;
        return true;
    }

    return false;
}

static void notify_speed_changed(void) {
    if (g_suppress_hud) {
        return;
    }
    g_speed_hud_time = SPEED_HUD_HOLD_TIME + SPEED_HUD_FADE_TIME;
    g_speed_hud_alpha = 1.0f;
}

static void set_speed_multiplier(float value) {
    float clamped = clamp_speed(value);
    g_speed_multiplier = clamped;
    notify_speed_changed();
}

static void adjust_speed(float delta) {
    if (delta == 0.0f) {
        return;
    }
    set_speed_multiplier(g_speed_multiplier + delta);
}

static void set_master_mute(bool mute) {
    audio_lock();
    toy_mixer_set_mute(&g_mixer, mute);
    audio_unlock();
    notify_volume_changed();
}

static void toggle_master_mute(void) {
    set_master_mute(!g_mixer.muted);
}

static int process_key_repeat(KeyRepeatState *state, bool currently_pressed, float delta_seconds) {
    if (currently_pressed) {
        if (!state->is_pressed) {
            state->is_pressed = true;
            state->hold_time = 0.0f;
            state->repeat_timer = 0.0f;
            return 1;
        } else {
            state->hold_time += delta_seconds;
            if (state->hold_time >= KEY_REPEAT_INITIAL_DELAY) {
                state->repeat_timer += delta_seconds;
                int count = 0;
                while (state->repeat_timer >= KEY_REPEAT_INTERVAL) {
                    count++;
                    state->repeat_timer -= KEY_REPEAT_INTERVAL;
                }
                return count;
            }
        }
    } else {
        state->is_pressed = false;
        state->hold_time = 0.0f;
        state->repeat_timer = 0.0f;
    }
    return 0;
}

static inline void update_volume_hud(float delta_seconds) {
    if (g_volume_hud_time > 0.0f) {
        if (delta_seconds > 0.0f) {
            g_volume_hud_time = fmaxf(0.0f, g_volume_hud_time - delta_seconds);
        }
        if (g_volume_hud_time > VOLUME_HUD_FADE_TIME) {
            g_volume_hud_alpha = 1.0f;
        } else if (VOLUME_HUD_FADE_TIME > 0.0f) {
            g_volume_hud_alpha = g_volume_hud_time / VOLUME_HUD_FADE_TIME;
        } else {
            g_volume_hud_alpha = 0.0f;
        }
    } else {
        g_volume_hud_alpha = 0.0f;
    }
}

static inline void update_speed_hud(float delta_seconds) {
    if (g_speed_hud_time > 0.0f) {
        if (delta_seconds > 0.0f) {
            g_speed_hud_time = fmaxf(0.0f, g_speed_hud_time - delta_seconds);
        }
        if (g_speed_hud_time > SPEED_HUD_FADE_TIME) {
            g_speed_hud_alpha = 1.0f;
        } else if (SPEED_HUD_FADE_TIME > 0.0f) {
            g_speed_hud_alpha = g_speed_hud_time / SPEED_HUD_FADE_TIME;
        } else {
            g_speed_hud_alpha = 0.0f;
        }
    } else {
        g_speed_hud_alpha = 0.0f;
    }
}

#define GLYPH_ROWS 5
#define GLYPH_COLS 3

typedef struct {
    char ch;
    const char *rows[GLYPH_ROWS];
} GlyphDef;

static const GlyphDef g_volume_glyphs[] = {
    {' ', {"...", "...", "...", "...", "..."}},
    {'0', {"XXX", "X.X", "X.X", "X.X", "XXX"}},
    {'1', {".X.", "XX.", ".X.", ".X.", "XXX"}},
    {'2', {"XXX", "..X", "XXX", "X..", "XXX"}},
    {'3', {"XXX", "..X", "XXX", "..X", "XXX"}},
    {'4', {"X.X", "X.X", "XXX", "..X", "..X"}},
    {'5', {"XXX", "X..", "XXX", "..X", "XXX"}},
    {'6', {"XXX", "X..", "XXX", "X.X", "XXX"}},
    {'7', {"XXX", "..X", "..X", "..X", "..X"}},
    {'8', {"XXX", "X.X", "XXX", "X.X", "XXX"}},
    {'9', {"XXX", "X.X", "XXX", "..X", "XXX"}},
    {'V', {"X.X", "X.X", "X.X", "X.X", ".X."}},
    {'O', {"XXX", "X.X", "X.X", "X.X", "XXX"}},
    {'L', {"X..", "X..", "X..", "X..", "XXX"}},
    {'U', {"X.X", "X.X", "X.X", "X.X", "XXX"}},
    {'M', {"XXX", "XXX", "X.X", "X.X", "X.X"}},
    {'E', {"XXX", "X..", "XXX", "X..", "XXX"}},
    {'T', {"XXX", ".X.", ".X.", ".X.", ".X."}},
    {'D', {"XX.", "X.X", "X.X", "X.X", "XX."}},
    {'P', {"XXX", "X.X", "XXX", "X..", "X.."}},
    {'S', {".XX", "X..", "XXX", "..X", "XX."}},
    {'.', {"...", "...", ".X.", "...", "..."}},
    {'%', {"X..", "..X", ".X.", "X..", "..X"}},
    {'^', {".X.", "XXX", ".X.", ".X.", ".X."}},
    {'v', {".X.", ".X.", ".X.", "XXX", ".X."}},
    {'<', {".X.", "X..", "XXX", "X..", ".X."}},
    {'>', {".X.", "..X", "XXX", "..X", ".X."}},
    {'R', {"XX.", "X.X", "XX.", "X.X", "X.X"}},
    {'A', {".X.", "X.X", "XXX", "X.X", "X.X"}},
    {'B', {"XX.", "X.X", "XX.", "X.X", "XX."}},
    {'C', {".XX", "X..", "X..", "X..", ".XX"}},
    {'F', {"XXX", "X..", "XX.", "X..", "X.."}},
    {'G', {".XX", "X..", "X.X", "X.X", ".XX"}},
    {'H', {"X.X", "X.X", "XXX", "X.X", "X.X"}},
    {'I', {"XXX", ".X.", ".X.", ".X.", "XXX"}},
    {'N', {"X.X", "XXX", "XXX", "XXX", "X.X"}},
    {'Q', {"XXX", "X.X", "X.X", "XXX", "..X"}},
    {'W', {"X.X", "X.X", "X.X", "XXX", "XXX"}},
    {'X', {"X.X", ".X.", "X.X", ".X.", "X.X"}},
    {'Y', {"X.X", "X.X", ".X.", ".X.", ".X."}},
    {'a', {"...", ".XX", "X.X", "X.X", ".XX"}},
    {'b', {"X..", "X..", "XX.", "X.X", "XX."}},
    {'c', {"...", ".XX", "X..", "X..", ".XX"}},
    {'d', {"..X", "..X", ".XX", "X.X", ".XX"}},
    {'e', {"...", ".XX", "XXX", "X..", ".XX"}},
    {'f', {".XX", ".X.", "XXX", ".X.", ".X."}},
    {'g', {"...", ".XX", "X.X", ".XX", "XX."}},
    {'h', {"X..", "X..", "XX.", "X.X", "X.X"}},
    {'i', {".X.", "...", ".X.", ".X.", ".X."}},
    {'k', {"X..", "X.X", "XX.", "X.X", "X.X"}},
    {'l', {"XX.", ".X.", ".X.", ".X.", "XXX"}},
    {'n', {"...", "XX.", "X.X", "X.X", "X.X"}},
    {'o', {"...", ".X.", "X.X", "X.X", ".X."}},
    {'q', {"...", ".XX", "X.X", ".XX", "..X"}},
    {'r', {"...", ".XX", "X..", "X..", "X.."}},
    {'s', {"...", ".XX", "XX.", ".XX", "XX."}},
    {'t', {".X.", "XXX", ".X.", ".X.", ".XX"}},
    {'u', {"...", "X.X", "X.X", "X.X", ".XX"}},
    {'w', {"...", "X.X", "X.X", "XXX", "XXX"}},
    {'y', {"...", "X.X", "X.X", ".XX", "XX."}},
    {'z', {"...", "XXX", ".X.", "X..", "XXX"}},
    {'!', {".X.", ".X.", ".X.", "...", ".X."}},
    {'-', {"...", "...", "XXX", "...", "..."}},
    {'=', {"...", "XXX", "...", "XXX", "..."}},
    {'/', {"..X", "..X", ".X.", "X..", "X.."}},
    {'[', {"XX.", "X..", "X..", "X..", "XX."}},
    {']', {".XX", "..X", "..X", "..X", ".XX"}},
    {0, {NULL, NULL, NULL, NULL, NULL}}
};

static const GlyphDef *lookup_volume_glyph(char c) {
    for (size_t i = 0; g_volume_glyphs[i].ch != 0; i++) {
        if (g_volume_glyphs[i].ch == c) {
            return &g_volume_glyphs[i];
        }
    }
    if (c >= 'a' && c <= 'z') {
        char upper = (char)(c - 'a' + 'A');
        for (size_t i = 0; g_volume_glyphs[i].ch != 0; i++) {
            if (g_volume_glyphs[i].ch == upper) {
                return &g_volume_glyphs[i];
            }
        }
    }
    return NULL;
}

static float measure_text(const char *text, float scale) {
    if (!text || *text == '\0') {
        return 0.0f;
    }
    float width = 0.0f;
    size_t len = strlen(text);
    for (size_t i = 0; i < len; i++) {
        width += GLYPH_COLS * scale;
        if (i + 1 < len) {
            width += scale;
        }
    }
    return width;
}



static inline void update_fps_hud(float delta_seconds) {
    fps_hud_target_alpha = show_fps_hud ? 1.0f : 0.0f;

    if (fps_hud_alpha < fps_hud_target_alpha) {
        fps_hud_alpha = fminf(fps_hud_target_alpha, fps_hud_alpha + delta_seconds / FPS_HUD_FADE_TIME);
    } else if (fps_hud_alpha > fps_hud_target_alpha) {
        fps_hud_alpha = fmaxf(fps_hud_target_alpha, fps_hud_alpha - delta_seconds / FPS_HUD_FADE_TIME);
    }
}








static bool should_block_input(void) {
    return g_loading_banner.state == BANNER_SHOWING ||
           g_loading_banner.state == BANNER_HOLDING ||
           g_loading_banner.state == BANNER_FADING;
}



static bool is_mouse_over_ball(int mouse_x, int mouse_y, float ball_x, float ball_y,
                                int window_w, int window_h);











static float remap_normalized_scale_to_sound(float normalized_scale) {
    float clamped_norm = fmaxf(AUDIO_NORMALIZED_MIN, fminf(AUDIO_NORMALIZED_MAX, normalized_scale));
    float t = (clamped_norm - AUDIO_NORMALIZED_MIN) / (AUDIO_NORMALIZED_MAX - AUDIO_NORMALIZED_MIN);
    float rescaled = AUDIO_RESCALE_MIN + t * (AUDIO_RESCALE_MAX - AUDIO_RESCALE_MIN);
    if (rescaled < AUDIO_RESCALE_MIN) rescaled = AUDIO_RESCALE_MIN;
    if (rescaled > AUDIO_RESCALE_MAX) rescaled = AUDIO_RESCALE_MAX;
    return rescaled;
}

void mark_sounds_dirty(float size_scale) {
    audio_state.bounce.dirty = true;
    audio_state.pending_size_scale = fmaxf(size_scale, 0.01f);
}

static bool is_mouse_over_ball(int mouse_x, int mouse_y, float ball_x, float ball_y,
                                int window_w, int window_h) {
    float prop_x = window_w / CANVAS_WIDTH;
    float prop_y = window_h / CANVAS_HEIGHT;
    float total_prop = fminf(prop_x, prop_y);
    float ball_diameter = 124.0f * total_prop;
    ball_diameter *= g_freerange_ball_scale;

    float ball_y_pixels = ball_y * window_h;

    float ball_center_x = ball_x + ball_diameter / 2.0f;
    float ball_center_y = ball_y_pixels + ball_diameter / 2.0f;
    float ball_radius = ball_diameter / 2.0f;

    float dx = mouse_x - ball_center_x;
    float dy = mouse_y - ball_center_y;
    float distance = sqrtf(dx * dx + dy * dy);

    return distance <= ball_radius;
}

void calculate_equilibrium_state(int window_w, int window_h,
                                 float gravity, float floor_y_norm,
                                 float target_peak_y,
                                 float *out_ball_y, float *out_vy,
                                 float *out_bounce_speed) {
    float prop_x = window_w / CANVAS_WIDTH;
    float prop_y = window_h / CANVAS_HEIGHT;
    float total_prop = fminf(prop_x, prop_y);
    float ball_diameter = 124.0f * total_prop;
    ball_diameter *= g_freerange_ball_scale;
    float ball_diameter_normalized = ball_diameter / (float)window_h;

    float collision_y = floor_y_norm - ball_diameter_normalized;

    float height_to_climb = collision_y - target_peak_y;

    float time_scale = 1.0f;
    float g_eff = gravity * time_scale;

    float bounce_speed = sqrtf(2.0f * g_eff * height_to_climb);

    *out_ball_y = collision_y;
    *out_vy = -bounce_speed;  
    *out_bounce_speed = bounce_speed;
}

static float get_ideal_bounce_vy(int window_h, float ball_diameter_normalized, float gravity) {
    float bounce_height = g_floor_y_normalized - g_target_peak_y - ball_diameter_normalized;
    if (bounce_height < 0.0001f) {
        bounce_height = 0.0001f;
    }
    float gravity_eff = gravity;
    return sqrtf(2.0f * gravity_eff * bounce_height);
}

static float get_min_bounce_speed_ratio(void) {
    return MIN_BOUNCE_SOUND_SPEED_RATIO;
}

static bool impact_speed_is_audible(float impact_speed, float baseline_speed) {
    if (baseline_speed <= 0.0f) {
        return false;
    }
    return (impact_speed / baseline_speed) >= get_min_bounce_speed_ratio();
}

static void update_ball_physics(float *ball_x, float *ball_y,
                                float *ball_vx, float *ball_vy,
                                int *ball_vx_direction,
                                int window_w, int window_h,
                                float border_inset, float border_inset_norm,
                                float ball_diameter, float ball_diameter_normalized,
                                double sim_delta,
                                bool ball_grabbed, bool make_noise,
                                BounceEvent *events, int *event_count, int max_events,
                                float event_time);

static int volume_from_impact_ratio(float ratio) {
    if (ratio < 0.0f) {
        ratio = 0.0f;
    }
    float clamped = fminf(ratio, 3.0f);
    return (int)lroundf(8.0f + clamped * 8.0f);
}

static float compute_bounce_pan(float ball_x, float ball_diameter, int window_w) {
    if (window_w <= 0) {
        return 0.5f;
    }
    float pan = (ball_x + ball_diameter * 0.5f) / (float)window_w;
    if (pan < 0.0f) pan = 0.0f;
    if (pan > 1.0f) pan = 1.0f;
    return pan;
}

static void maybe_play_actual_bounce_sound(bool make_noise, float impact_speed, float baseline_speed, float pan) {
    if (!make_noise) {
        return;
    }
    if (!impact_speed_is_audible(impact_speed, baseline_speed)) {
        return;
    }
    float ratio = impact_speed / baseline_speed;
    play_bounce_sound(volume_from_impact_ratio(ratio), pan);
}

static void audio_trace_event(const char *stage,
                              int surface,
                              float event_time,
                              float impact_speed,
                              float baseline_speed,
                              int volume);
static void audio_debug_note_predict_soft_reject(float event_time);
static void audio_predict_reset(float now_time);

#if defined(HAVE_CUBEB)
static long cubeb_silence_cb(cubeb_stream *stream, void *user, const void *in,
                             void *out, long nframes) {
    (void)stream;
    (void)in;
    CubebProbeRuntime *state = (CubebProbeRuntime *)user;
    if (!out || !state || state->channels == 0) {
        return nframes;
    }
    size_t samples = (size_t)nframes * (size_t)state->channels;
    memset(out, 0, samples * sizeof(float));
    return nframes;
}

static void cubeb_state_cb(cubeb_stream *stream, void *user, cubeb_state state) {
    (void)stream;
    (void)user;
    (void)state;
}

static void probe_cubeb_stop(void) {
    if (g_cubeb_probe.stream) {
        cubeb_stream_stop(g_cubeb_probe.stream);
        cubeb_stream_destroy(g_cubeb_probe.stream);
        g_cubeb_probe.stream = NULL;
    }
    if (g_cubeb_probe.ctx) {
        cubeb_destroy(g_cubeb_probe.ctx);
        g_cubeb_probe.ctx = NULL;
    }
    g_cubeb_probe.rate = 0;
    g_cubeb_probe.latency_frames = 0;
    g_cubeb_probe.channels = 0;
    g_cubeb_probe.active = false;
    snprintf(g_cubeb_probe.backend_id, sizeof(g_cubeb_probe.backend_id), "unknown");
}

static void probe_cubeb_start(void) {
    if (g_cubeb_probe.active) {
        return;
    }

    cubeb *ctx = NULL;
    if (cubeb_init(&ctx, "poingo-latency-probe", NULL) != CUBEB_OK || !ctx) {
        return;
    }

    uint32_t rate = SAMPLE_RATE;
    if (cubeb_get_preferred_sample_rate(ctx, &rate) != CUBEB_OK || rate == 0) {
        rate = SAMPLE_RATE;
    }

    cubeb_stream_params params;
    memset(&params, 0, sizeof(params));
    params.format = CUBEB_SAMPLE_FLOAT32NE;
    params.rate = rate;
    params.channels = 1;
    params.layout = CUBEB_LAYOUT_MONO;

    uint32_t latency_frames = 0;
    if (cubeb_get_min_latency(ctx, &params, &latency_frames) != CUBEB_OK || latency_frames == 0) {
        latency_frames = rate / 50;  
    }

    cubeb_stream *stream = NULL;
    if (cubeb_stream_init(ctx, &stream, "poingo-probe",
                          NULL, NULL,
                          NULL, &params,
                          latency_frames,
                          cubeb_silence_cb, cubeb_state_cb,
                          &g_cubeb_probe) != CUBEB_OK || !stream) {
        cubeb_destroy(ctx);
        return;
    }

    if (cubeb_stream_start(stream) != CUBEB_OK) {
        cubeb_stream_destroy(stream);
        cubeb_destroy(ctx);
        return;
    }

    g_cubeb_probe.ctx = ctx;
    g_cubeb_probe.stream = stream;
    g_cubeb_probe.rate = rate;
    g_cubeb_probe.latency_frames = latency_frames;
    g_cubeb_probe.channels = params.channels;
    g_cubeb_probe.start_ticks = poingo_ticks_ms();
    g_cubeb_probe.last_update_ticks = g_cubeb_probe.start_ticks;
    const char *backend_id = cubeb_get_backend_id(ctx);
    if (backend_id) {
        snprintf(g_cubeb_probe.backend_id, sizeof(g_cubeb_probe.backend_id), "%s", backend_id);
    } else {
        snprintf(g_cubeb_probe.backend_id, sizeof(g_cubeb_probe.backend_id), "unknown");
    }
    g_cubeb_probe.active = true;
}

static void probe_cubeb_handle_restart(void) {
    if (poingo_atomic_set(&g_cubeb_probe_restart_requested, 0) == 0) {
        return;
    }
    probe_cubeb_stop();
    g_audio_predict_delay_seconds = AUDIO_PREDICT_DELAY_SECONDS;
    g_cubeb_probe.start_ticks = 0;
    g_cubeb_probe.last_update_ticks = 0;
    probe_cubeb_start();
}

static void probe_cubeb_update(void) {
    probe_cubeb_handle_restart();
    if (!g_cubeb_probe.active) {
        return;
    }
    uint32_t now_ticks = poingo_ticks_ms();
    uint32_t elapsed_since_start = now_ticks - g_cubeb_probe.start_ticks;
    uint32_t update_interval_ms = (elapsed_since_start < 5000) ? 250u : 2000u;
    if (now_ticks - g_cubeb_probe.last_update_ticks < update_interval_ms) {
        return;
    }
    g_cubeb_probe.last_update_ticks = now_ticks;

    uint32_t measured_latency = 0;
    if (cubeb_stream_get_latency(g_cubeb_probe.stream, &measured_latency) == CUBEB_OK &&
        measured_latency > 0) {
        g_audio_predict_delay_seconds = (float)measured_latency / (float)g_cubeb_probe.rate;
    } else {
        g_audio_predict_delay_seconds = (float)g_cubeb_probe.latency_frames / (float)g_cubeb_probe.rate;
    }
    if (g_audio_predict_delay_seconds < 0.0f) {
        g_audio_predict_delay_seconds = 0.0f;
    } else if (g_audio_predict_delay_seconds > AUDIO_PREDICT_SECONDS) {
        g_audio_predict_delay_seconds = AUDIO_PREDICT_SECONDS;
    }
}
#else
static void probe_cubeb_start(void) {
}
static void probe_cubeb_update(void) {
}
#endif

static inline void append_bounce_event(BounceEvent *events, int *event_count, int max_events,
                                       float event_time, float impact_speed, float baseline_speed,
                                       int surface, float pan) {
    if (!events || !event_count || *event_count >= max_events) {
        return;
    }
    if (!impact_speed_is_audible(impact_speed, baseline_speed)) {
        audio_trace_event("PRED_SOFT", surface, event_time, impact_speed, baseline_speed, -1);
        audio_debug_note_predict_soft_reject(event_time);
        return;
    }
    float ratio = impact_speed / baseline_speed;
    int volume = volume_from_impact_ratio(ratio);
    events[*event_count].time = event_time;
    events[*event_count].volume = volume;
    events[*event_count].surface = surface;
    events[*event_count].pan = pan;
    audio_trace_event("PRED_ADD", surface, event_time, impact_speed, baseline_speed, volume);
    (*event_count)++;
}

static float get_bounce_dedupe_window_seconds(void) {
    float dedupe_window = g_audio_predict_step_seconds * 1.5f;
    if (dedupe_window < 0.020f) {
        dedupe_window = 0.020f;
    }
    if (dedupe_window > BOUNCE_DEBOUNCE_SECONDS) {
        dedupe_window = BOUNCE_DEBOUNCE_SECONDS;
    }
    return dedupe_window;
}

static const char *bounce_surface_name(int surface) {
    switch (surface) {
        case BOUNCE_SURFACE_FLOOR: return "FLOOR";
        case BOUNCE_SURFACE_CEILING: return "CEIL";
        case BOUNCE_SURFACE_LEFT: return "LEFT";
        case BOUNCE_SURFACE_RIGHT: return "RIGHT";
        default: return "UNK";
    }
}

static void audio_trace_event(const char *stage,
                              int surface,
                              float event_time,
                              float impact_speed,
                              float baseline_speed,
                              int volume) {
    if (!g_audio_trace_enabled || !stage) {
        return;
    }
    float ratio = 0.0f;
    char line[256];
    if (baseline_speed > 0.0f) {
        ratio = impact_speed / baseline_speed;
    }
    snprintf(line, sizeof(line),
             "AUDIOTRACE %s MODE=%s SURF=%s T=%.4f IMP=%.6f BASE=%.6f R=%.4f VOL=%d",
             stage,
             g_predictive_audio_enabled ? "PRED" : "REAL",
             bounce_surface_name(surface),
             event_time,
             impact_speed,
             baseline_speed,
             ratio,
             volume);
    fprintf(stderr, "%s\n", line);
    android_append_downloads_trace_line(line);
}

static void audio_debug_ensure_window(float now_time) {
    if (g_audio_debug.window_start_time < -999999.0f) {
        g_audio_debug.window_start_time = now_time;
    }
    if (g_audio_debug.last_log_time < -999999.0f) {
        g_audio_debug.last_log_time = now_time;
    }
}

static void audio_debug_note_actual_collision(float event_time, float impact_speed, float baseline_speed) {
    audio_debug_ensure_window(event_time);
    g_audio_debug.actual_collisions_total++;
    g_audio_debug.actual_collisions_window++;
    if (impact_speed_is_audible(impact_speed, baseline_speed)) {
        g_audio_debug.actual_audible_total++;
        g_audio_debug.actual_audible_window++;
    } else {
        g_audio_debug.actual_soft_total++;
        g_audio_debug.actual_soft_window++;
    }
}

static void audio_debug_note_predict_soft_reject(float event_time) {
    audio_debug_ensure_window(event_time);
    g_audio_debug.predict_soft_total++;
    g_audio_debug.predict_soft_window++;
}

static void audio_debug_note_predict_window_event(float event_time) {
    audio_debug_ensure_window(event_time);
    g_audio_debug.predict_window_total++;
    g_audio_debug.predict_window_events_window++;
}

static void audio_debug_note_predict_deduped(float event_time) {
    audio_debug_ensure_window(event_time);
    g_audio_debug.predict_deduped_total++;
    g_audio_debug.predict_deduped_window++;
}

static void audio_debug_note_predict_played(float event_time) {
    audio_debug_ensure_window(event_time);
    g_audio_debug.predict_played_total++;
    g_audio_debug.predict_played_window++;
}

static void audio_debug_maybe_log(float now_time) {
    audio_debug_ensure_window(now_time);
    if (!g_audio_trace_enabled) {
        return;
    }
    if ((now_time - g_audio_debug.last_log_time) < 1.0f) {
        return;
    }

    unsigned long long miss_window = 0;
    if (g_audio_debug.actual_audible_window > g_audio_debug.predict_played_window) {
        miss_window = g_audio_debug.actual_audible_window - g_audio_debug.predict_played_window;
    }

    {
        char line[256];
        snprintf(line, sizeof(line),
                 "AUDIODBG W A=%llu AU=%llu AS=%llu PW=%llu PS=%llu PD=%llu PP=%llu MISS=%llu | T A=%llu AU=%llu AS=%llu PW=%llu PS=%llu PD=%llu PP=%llu",
                 g_audio_debug.actual_collisions_window,
                 g_audio_debug.actual_audible_window,
                 g_audio_debug.actual_soft_window,
                 g_audio_debug.predict_window_events_window,
                 g_audio_debug.predict_soft_window,
                 g_audio_debug.predict_deduped_window,
                 g_audio_debug.predict_played_window,
                 miss_window,
                 g_audio_debug.actual_collisions_total,
                 g_audio_debug.actual_audible_total,
                 g_audio_debug.actual_soft_total,
                 g_audio_debug.predict_window_total,
                 g_audio_debug.predict_soft_total,
                 g_audio_debug.predict_deduped_total,
                 g_audio_debug.predict_played_total);
        fprintf(stderr, "%s\n", line);
        android_append_downloads_trace_line(line);
    }

    g_audio_debug.actual_collisions_window = 0;
    g_audio_debug.actual_audible_window = 0;
    g_audio_debug.actual_soft_window = 0;
    g_audio_debug.predict_window_events_window = 0;
    g_audio_debug.predict_soft_window = 0;
    g_audio_debug.predict_deduped_window = 0;
    g_audio_debug.predict_played_window = 0;
    g_audio_debug.window_start_time = now_time;
    g_audio_debug.last_log_time = now_time;
}

static bool recent_bounce_playback_matches(const BounceEvent *ev) {
    if (!ev) {
        return false;
    }
    float dedupe_window = get_bounce_dedupe_window_seconds();
    for (int i = 0; i < RECENT_BOUNCE_PLAYBACK_COUNT; ++i) {
        const RecentBouncePlayback *recent = &g_recent_bounce_playbacks[i];
        if (!recent->valid) {
            continue;
        }
        if (recent->surface != ev->surface) {
            continue;
        }
        if (fabsf(recent->time - ev->time) > dedupe_window) {
            continue;
        }
        if (abs(recent->volume - ev->volume) > 2) {
            continue;
        }
        return true;
    }
    return false;
}

static void clear_audio_predict_queue(void) {
    for (int i = 0; i < AUDIO_PREDICT_QUEUE_MAX; ++i) {
        g_audio_predict_queue[i].active = false;
        g_audio_predict_queue[i].played = false;
        g_audio_predict_queue[i].last_seen_time = -1000000.0f;
    }
}

static float get_audio_predict_match_window_seconds(void) {
    float match_window = g_audio_predict_step_seconds * 2.5f;
    if (match_window < 0.010f) {
        match_window = 0.010f;
    }
    if (match_window > 0.050f) {
        match_window = 0.050f;
    }
    return match_window;
}

static int find_audio_predict_queue_match(const BounceEvent *ev) {
    if (!ev) {
        return -1;
    }
    float match_window = get_audio_predict_match_window_seconds();
    float best_diff = match_window + 1.0f;
    int best_index = -1;
    for (int i = 0; i < AUDIO_PREDICT_QUEUE_MAX; ++i) {
        const AudioPredictQueueEntry *entry = &g_audio_predict_queue[i];
        if (!entry->active || entry->played) {
            continue;
        }
        if (entry->event.surface != ev->surface) {
            continue;
        }
        if (abs(entry->event.volume - ev->volume) > 2) {
            continue;
        }
        float diff = fabsf(entry->event.time - ev->time);
        if (diff > match_window) {
            continue;
        }
        if (diff < best_diff) {
            best_diff = diff;
            best_index = i;
        }
    }
    return best_index;
}

static int allocate_audio_predict_queue_slot(void) {
    for (int i = 0; i < AUDIO_PREDICT_QUEUE_MAX; ++i) {
        if (!g_audio_predict_queue[i].active) {
            return i;
        }
    }

    int best_index = 0;
    float best_score = 1000000.0f;
    for (int i = 0; i < AUDIO_PREDICT_QUEUE_MAX; ++i) {
        const AudioPredictQueueEntry *entry = &g_audio_predict_queue[i];
        float score = entry->played ? entry->event.time - 1000.0f : entry->event.time;
        if (score < best_score) {
            best_score = score;
            best_index = i;
        }
    }
    return best_index;
}

static void reconcile_audio_predict_queue(const AudioPredictBuffer *buffer, float now_time) {
    if (!buffer) {
        return;
    }

    for (int i = 0; i < buffer->count; ++i) {
        const BounceEvent *ev = &buffer->events[i];
        int match_index = find_audio_predict_queue_match(ev);
        if (match_index >= 0) {
            g_audio_predict_queue[match_index].event = *ev;
            g_audio_predict_queue[match_index].last_seen_time = now_time;
        } else {
            int slot = allocate_audio_predict_queue_slot();
            g_audio_predict_queue[slot].event = *ev;
            g_audio_predict_queue[slot].last_seen_time = now_time;
            g_audio_predict_queue[slot].active = true;
            g_audio_predict_queue[slot].played = false;
        }
    }
}

static void cleanup_audio_predict_queue(float now_time, float effective_delay) {
    for (int i = 0; i < AUDIO_PREDICT_QUEUE_MAX; ++i) {
        AudioPredictQueueEntry *entry = &g_audio_predict_queue[i];
        if (!entry->active) {
            continue;
        }

        if (entry->played) {
            if ((now_time - entry->event.time) > AUDIO_PREDICT_PLAYED_RETENTION_SECONDS) {
                entry->active = false;
            }
            continue;
        }

        if ((now_time - entry->last_seen_time) > AUDIO_PREDICT_EVENT_STALE_SECONDS) {
            entry->active = false;
            continue;
        }

        if ((entry->event.time + effective_delay) < (now_time - AUDIO_PREDICT_EVENT_STALE_SECONDS)) {
            entry->active = false;
        }
    }
}

static void record_recent_bounce_playback(const BounceEvent *ev) {
    if (!ev) {
        return;
    }
    g_recent_bounce_playbacks[g_recent_bounce_playback_next].time = ev->time;
    g_recent_bounce_playbacks[g_recent_bounce_playback_next].volume = ev->volume;
    g_recent_bounce_playbacks[g_recent_bounce_playback_next].surface = ev->surface;
    g_recent_bounce_playbacks[g_recent_bounce_playback_next].valid = true;
    g_recent_bounce_playback_next++;
    if (g_recent_bounce_playback_next >= RECENT_BOUNCE_PLAYBACK_COUNT) {
        g_recent_bounce_playback_next = 0;
    }
}

static void audio_predict_reset(float now_time) {
    (void)now_time;
    clear_audio_predict_queue();
    for (int i = 0; i < RECENT_BOUNCE_PLAYBACK_COUNT; ++i) {
        g_recent_bounce_playbacks[i].valid = false;
    }
    g_recent_bounce_playback_next = 0;
    audio_debug_ensure_window(now_time);
}

static float get_audio_predict_effective_delay_seconds(void) {
    float effective_delay = g_audio_predict_delay_seconds - AUDIO_PREDICT_ONSET_COMPENSATION_SECONDS;
    if (effective_delay < 0.0f) {
        effective_delay = 0.0f;
    } else if (effective_delay > AUDIO_PREDICT_SECONDS) {
        effective_delay = AUDIO_PREDICT_SECONDS;
    }
    return effective_delay;
}

static float get_audio_predict_horizon_seconds(void) {
    float effective_delay = get_audio_predict_effective_delay_seconds();
    float horizon = effective_delay * AUDIO_PREDICT_HORIZON_DELAY_MULTIPLIER +
                    AUDIO_PREDICT_HORIZON_MARGIN_SECONDS;
    if (horizon < AUDIO_PREDICT_HORIZON_MIN_SECONDS) {
        horizon = AUDIO_PREDICT_HORIZON_MIN_SECONDS;
    }
    if (horizon > AUDIO_PREDICT_HORIZON_MAX_SECONDS) {
        horizon = AUDIO_PREDICT_HORIZON_MAX_SECONDS;
    }
    if (horizon > AUDIO_PREDICT_SECONDS) {
        horizon = AUDIO_PREDICT_SECONDS;
    }
    return horizon;
}


static void build_audio_predict_buffer(AudioPredictBuffer *buffer,
                                       float base_time,
                                       float ball_x, float ball_y,
                                       float ball_vx, float ball_vy,
                                       int ball_vx_direction,
                                       int window_w, int window_h,
                                       float border_inset, float border_inset_norm,
                                       float ball_diameter, float ball_diameter_normalized,
                                       float step_seconds) {
    if (!buffer) {
        return;
    }
    buffer->count = 0;
    float sim_x = ball_x;
    float sim_y = ball_y;
    float sim_vx = ball_vx;
    float sim_vy = ball_vy;
    int sim_dir = ball_vx_direction;
    float step = AUDIO_PREDICT_FIXED_STEP_SECONDS;
    if (step_seconds > 0.0f && step_seconds < step) {
        step = step_seconds;
    }
    if (step <= 0.0001f) {
        step = AUDIO_PREDICT_FIXED_STEP_SECONDS;
    }
    g_audio_predict_step_seconds = step;
    float horizon = get_audio_predict_horizon_seconds();
    float t = 0.0f;
    while (t < horizon && buffer->count < AUDIO_PREDICT_MAX_EVENTS) {
        update_ball_physics(&sim_x, &sim_y,
                            &sim_vx, &sim_vy,
                            &sim_dir,
                            window_w, window_h,
                            border_inset, border_inset_norm,
                            ball_diameter, ball_diameter_normalized,
                            step,
                            false, true,
                            buffer->events, &buffer->count, AUDIO_PREDICT_MAX_EVENTS,
                            base_time + t + step);
        t += step;
    }
}

static void trigger_predictive_audio_with_timing(const AudioPredictBuffer *buffer,
                                                 float now_time,
                                                 bool make_noise,
                                                 float *timing_error_out,
                                                 bool *timing_valid_out) {
    audio_debug_maybe_log(now_time);

    if (timing_error_out) {
        *timing_error_out = 0.0f;
    }
    if (timing_valid_out) {
        *timing_valid_out = false;
    }

    if (!buffer || !make_noise) {
        return;
    }

    reconcile_audio_predict_queue(buffer, now_time);

    float effective_delay = get_audio_predict_effective_delay_seconds();
    if (effective_delay < g_audio_predict_step_seconds) {
        effective_delay = g_audio_predict_step_seconds;
    }
    float target_time = now_time + effective_delay;
    cleanup_audio_predict_queue(now_time, effective_delay);
    int timing_samples = 0;
    float timing_accum = 0.0f;

    for (int i = 0; i < AUDIO_PREDICT_QUEUE_MAX; ++i) {
        AudioPredictQueueEntry *entry = &g_audio_predict_queue[i];
        if (!entry->active || entry->played) {
            continue;
        }
        const BounceEvent *ev = &entry->event;
        float event_time = ev->time;
        if (event_time > target_time) {
            continue;
        }

        audio_debug_note_predict_window_event(event_time);
        audio_trace_event("PRED_WIN", ev->surface, event_time, 0.0f, 0.0f, ev->volume);

        if (recent_bounce_playback_matches(ev)) {
            audio_trace_event("PRED_DEDUP", ev->surface, event_time, 0.0f, 0.0f, ev->volume);
            audio_debug_note_predict_deduped(event_time);
            entry->played = true;
            continue;
        }

        play_bounce_sound(ev->volume, ev->pan);
        audio_trace_event("PRED_PLAY", ev->surface, event_time, 0.0f, 0.0f, ev->volume);
        record_recent_bounce_playback(ev);
        audio_debug_note_predict_played(event_time);
        entry->played = true;

        float expected_play_time = event_time + effective_delay;
        float timing_error = now_time - expected_play_time;
        timing_accum += timing_error;
        timing_samples++;
    }

    if (timing_samples > 0) {
        if (timing_error_out) {
            *timing_error_out = timing_accum / (float)timing_samples;
        }
        if (timing_valid_out) {
            *timing_valid_out = true;
        }
    }
}

static void trigger_predictive_audio(const AudioPredictBuffer *buffer, float now_time, bool make_noise) {
    trigger_predictive_audio_with_timing(buffer, now_time, make_noise, NULL, NULL);
}

static void update_ball_physics(float *ball_x, float *ball_y,
                                float *ball_vx, float *ball_vy,
                                int *ball_vx_direction,
                                int window_w, int window_h,
                                float border_inset, float border_inset_norm,
                                float ball_diameter, float ball_diameter_normalized,
                                double sim_delta,
                                bool ball_grabbed, bool make_noise,
                                BounceEvent *events, int *event_count, int max_events,
                                float event_time) {
    if (ball_grabbed) {
        return;
    }

    bool predictive_pass = make_noise && events && event_count && max_events > 0;
    bool immediate_audio_pass = make_noise && !predictive_pass;

    float time_scale = (float)(sim_delta * 60.0f * g_speed_multiplier);
    float time_scale_x = time_scale;
    float time_scale_y = time_scale;
    float gravity = GRAVITY;

    *ball_vy += gravity * time_scale_y;
    *ball_x += (*ball_vx * (float)(*ball_vx_direction)) * time_scale_x;
    *ball_y += *ball_vy * time_scale_y;

    float natural_vx = get_natural_vx(window_w);
    float vx_difference = *ball_vx - natural_vx;
    if (fabsf(vx_difference) > 0.01f) {
        float damping_factor = 0.049f * time_scale_x;
        *ball_vx -= vx_difference * damping_factor;
    } else {
        *ball_vx = natural_vx;
    }

    if (*ball_x < border_inset) {
        *ball_x = border_inset + (border_inset - *ball_x);
        *ball_vx_direction = -*ball_vx_direction;
        float pan = compute_bounce_pan(*ball_x, ball_diameter, window_w);
        if (predictive_pass) {
            append_bounce_event(events, event_count, max_events, event_time,
                                fabsf(*ball_vx), natural_vx,
                                BOUNCE_SURFACE_LEFT, pan);
        } else {
            audio_debug_note_actual_collision(event_time, fabsf(*ball_vx), natural_vx);
            audio_trace_event("ACTUAL_HIT", BOUNCE_SURFACE_LEFT, event_time, fabsf(*ball_vx), natural_vx, -1);
            if (immediate_audio_pass && impact_speed_is_audible(fabsf(*ball_vx), natural_vx)) {
                audio_trace_event("REAL_PLAY", BOUNCE_SURFACE_LEFT, event_time, fabsf(*ball_vx), natural_vx,
                                  volume_from_impact_ratio(fabsf(*ball_vx) / natural_vx));
            }
            maybe_play_actual_bounce_sound(immediate_audio_pass, fabsf(*ball_vx), natural_vx, pan);
        }
    }
    else if (*ball_x + ball_diameter > window_w - border_inset) {
        float right_edge = window_w - border_inset;
        *ball_x = (right_edge - ball_diameter) - ((*ball_x + ball_diameter) - right_edge);
        *ball_vx_direction = -*ball_vx_direction;
        float pan = compute_bounce_pan(*ball_x, ball_diameter, window_w);
        if (predictive_pass) {
            append_bounce_event(events, event_count, max_events, event_time,
                                fabsf(*ball_vx), natural_vx,
                                BOUNCE_SURFACE_RIGHT, pan);
        } else {
            audio_debug_note_actual_collision(event_time, fabsf(*ball_vx), natural_vx);
            audio_trace_event("ACTUAL_HIT", BOUNCE_SURFACE_RIGHT, event_time, fabsf(*ball_vx), natural_vx, -1);
            if (immediate_audio_pass && impact_speed_is_audible(fabsf(*ball_vx), natural_vx)) {
                audio_trace_event("REAL_PLAY", BOUNCE_SURFACE_RIGHT, event_time, fabsf(*ball_vx), natural_vx,
                                  volume_from_impact_ratio(fabsf(*ball_vx) / natural_vx));
            }
            maybe_play_actual_bounce_sound(immediate_audio_pass, fabsf(*ball_vx), natural_vx, pan);
        }
    }

    if (*ball_y < border_inset_norm && *ball_vy < 0.0f) {
        *ball_y = border_inset_norm + (border_inset_norm - *ball_y);
        *ball_vy = -*ball_vy;
        float impact_vy = fabsf(*ball_vy);
        float ideal_vy = get_ideal_bounce_vy(window_h, ball_diameter_normalized, gravity);
        float pan = compute_bounce_pan(*ball_x, ball_diameter, window_w);
        if (predictive_pass) {
            append_bounce_event(events, event_count, max_events, event_time,
                                impact_vy, ideal_vy,
                                BOUNCE_SURFACE_CEILING, pan);
        } else {
            audio_debug_note_actual_collision(event_time, impact_vy, ideal_vy);
            audio_trace_event("ACTUAL_HIT", BOUNCE_SURFACE_CEILING, event_time, impact_vy, ideal_vy, -1);
            if (immediate_audio_pass && impact_speed_is_audible(impact_vy, ideal_vy)) {
                audio_trace_event("REAL_PLAY", BOUNCE_SURFACE_CEILING, event_time, impact_vy, ideal_vy,
                                  volume_from_impact_ratio(impact_vy / ideal_vy));
            }
            maybe_play_actual_bounce_sound(immediate_audio_pass, impact_vy, ideal_vy, pan);
        }
    }

    if ((*ball_y + ball_diameter_normalized >= g_floor_y_normalized) && (*ball_vy >= 0.0f)) {
        float impact_vy = *ball_vy;
        float penetration = (*ball_y + ball_diameter_normalized) - g_floor_y_normalized;
        *ball_y = (g_floor_y_normalized - ball_diameter_normalized) - penetration;

        *ball_vy = -*ball_vy;

        float ideal_vy = get_ideal_bounce_vy(window_h, ball_diameter_normalized, gravity);

        float vy_difference = fabsf(*ball_vy) - ideal_vy;
        if (fabsf(vy_difference) > 0.0001f) {
            float adjustment = vy_difference * 0.507f;
            *ball_vy = -(fabsf(*ball_vy) - adjustment);
        } else {
            *ball_vy = -ideal_vy;
        }

        float pan = compute_bounce_pan(*ball_x, ball_diameter, window_w);
        if (predictive_pass) {
            append_bounce_event(events, event_count, max_events, event_time,
                                fabsf(impact_vy), ideal_vy,
                                BOUNCE_SURFACE_FLOOR, pan);
        } else {
            audio_debug_note_actual_collision(event_time, fabsf(impact_vy), ideal_vy);
            audio_trace_event("ACTUAL_HIT", BOUNCE_SURFACE_FLOOR, event_time, fabsf(impact_vy), ideal_vy, -1);
            if (immediate_audio_pass && impact_speed_is_audible(fabsf(impact_vy), ideal_vy)) {
                audio_trace_event("REAL_PLAY", BOUNCE_SURFACE_FLOOR, event_time, fabsf(impact_vy), ideal_vy,
                                  volume_from_impact_ratio(fabsf(impact_vy) / ideal_vy));
            }
            maybe_play_actual_bounce_sound(immediate_audio_pass, fabsf(impact_vy), ideal_vy, pan);
        }
    }
}


static bool is_mouse_over_ball(int mouse_x, int mouse_y, float ball_x, float ball_y,
                                int window_w, int window_h);














static uint32_t gcd_u32(uint32_t a, uint32_t b) {
    while (b != 0) {
        uint32_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

static void regen_make_shuffled_order(uint32_t *order, uint32_t total) {
    if (!order || total == 0) return;
    for (uint32_t i = 0; i < total; i++) {
        order[i] = i;
    }
    for (uint32_t i = total - 1; i > 0; i--) {
        uint32_t j = (uint32_t)(random() % (long)(i + 1));
        uint32_t tmp = order[i];
        order[i] = order[j];
        order[j] = tmp;
    }
}

static const char *freerange_vert_shader_text =
    "attribute vec2 pos;\n"
    "attribute vec2 uv_in;\n"
    "varying vec2 uv;\n"
    "void main() {\n"
    "    gl_Position = vec4(pos, 0.0, 1.0);\n"
    "    uv = uv_in;\n"
    "}\n";

static const char *freerange_frag_shader_text =
    "precision mediump float;\n"
    "varying vec2 uv;\n"
    "uniform sampler2D tex;\n"
    "uniform float alpha;\n"
    "void main() {\n"
    "    gl_FragColor = texture2D(tex, uv) * alpha;\n"
    "}\n";

typedef struct FreedomFrameSet FreedomFrameSet;

typedef struct {
    FreedomFrameSet *frames;
    int block_size;
    int num_blocks;
    uint32_t regen_start;
    uint32_t regen_stride;
    uint32_t regen_total;
    uint32_t *regen_order;
    int regen_mode;
    float angle_period;
    const uint8_t *shadow_pixels;
    int shadow_w;
    int shadow_h;
    int shadow_offset_x;
    int shadow_offset_y;
    int ball_offset_x;
    int ball_offset_y;
    PoingoAtomic next_seq;
    PoingoAtomic stop;
    PoingoAtomic *unit_done;
} RegenWorkerCtx;

typedef struct {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;
    struct wl_output *output;
    struct xdg_wm_base *wm_base;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;

    EGLDisplay egl_display;
    EGLContext egl_context;
    EGLSurface egl_surface;
    struct wl_egl_window *egl_window;
    GLuint gl_program;
    GLint gl_pos_loc;
    GLint gl_uv_loc;
    GLint gl_tex_loc;
    GLint gl_alpha_loc;
    GLuint *gl_textures;
    int gl_texture_count;
    bool gl_ready;
    GLuint gl_hud_texture;
    int gl_hud_w;
    int gl_hud_h;
    
    GLuint gl_ghost_bg_texture;
    float hud_x;
    float hud_y;
    uint8_t *hud_pixels;
    size_t hud_pixels_size;

    int width;
    int height;
    int pending_width;
    int pending_height;
    int refresh_mhz;
    bool configured;
    bool resize_pending;
    bool running;
    bool ghost_mode;

    bool pointer_down;
    int pointer_x;
    int pointer_y;
    bool key_vol_up_pressed;
    bool key_vol_down_pressed;
    bool key_speed_up_pressed;
    bool key_speed_down_pressed;

    bool ball_grabbed;
    float grab_u;
    float grab_v;
    float slingshot_pull_x;
    float slingshot_pull_y;

    FreedomFrameSet *frames_ref;
    bool color_regen_active;
    uint32_t color_regen_start;
    uint32_t color_regen_stride;
    uint32_t color_regen_cursor;
    uint32_t color_regen_units_done;
    uint32_t color_regen_units_total;
    float color_regen_angle_period;
    uint8_t *color_regen_shadow_pixels;
    int color_regen_shadow_w;
    int color_regen_shadow_h;
    int color_regen_shadow_offset_x;
    int color_regen_shadow_offset_y;
    int color_regen_ball_offset_x;
    int color_regen_ball_offset_y;
    uint8_t *color_regen_ball_pixels;
    int color_regen_mode;
    int color_regen_block_size;
    float regen_ms_per_block;
    RegenWorkerCtx *regen_worker_ctx;
    RegenWorkerCtx regen_ctx_storage;
    PoingoAtomic *regen_unit_done_storage;
    uint32_t *regen_order_storage;
    pthread_t **regen_thread_storage;
    uint32_t regen_workspace_units;
    int regen_workspace_threads;
    pthread_t    **regen_threads;
    int             regen_thread_count;
    int             regen_units_uploaded;
    bool shutdown_pending;
    bool ball_cleared;
    uint32_t shutdown_last_check_ticks;
    uint32_t shutdown_start_ticks;

    #define FREEDOM_MOUSE_HISTORY_SIZE 10
    struct {
        int x;
        int y;
        uint32_t time;
    } mouse_history[FREEDOM_MOUSE_HISTORY_SIZE];
    int mouse_history_index;

    float ball_x;
    float ball_y;
    float ball_vx;
    float ball_vy;
    int ball_vx_direction;
    float ball_diameter;
    float ball_diameter_norm;

    float phase_f;
    int phase_i;

    uint64_t last_counter;
    uint64_t performance_frequency;
    bool has_last_counter;
    bool make_noise;
    float last_total_prop;

} FreedomState;

typedef struct FreedomFrameSet {
    uint8_t *frames;
    size_t frame_size;
    int frame_count;
    int frame_w;
    int frame_h;
} FreedomFrameSet;

static bool freerange_gl_init(FreedomState *st, const FreedomFrameSet *frames);
static void freerange_gl_shutdown(FreedomState *st);
static void freerange_gl_draw_frame(FreedomState *st, const FreedomFrameSet *frames, float extrap_dt);
static bool freerange_gl_update_hud(FreedomState *st);
static void freerange_gl_draw_hud(FreedomState *st);
static void freerange_regen_workers_shutdown(FreedomState *st);
static void freerange_regen_update_scanline(uint8_t *restrict dst_frame, int composite_w, int composite_h, int y, const uint8_t *restrict ball_pixels, int ball_w, int ball_h, const uint8_t *restrict shadow_pixels, int shadow_w, int shadow_h, int ball_x, int ball_y, int shadow_x, int shadow_y);

static void freerange_color_regen_shutdown(FreedomState *st) {
    if (!st) {
        return;
    }
    free(st->color_regen_shadow_pixels);
    st->color_regen_shadow_pixels = NULL;
    st->color_regen_shadow_w = 0;
    st->color_regen_shadow_h = 0;
    free(st->color_regen_ball_pixels);
    st->color_regen_ball_pixels = NULL;
}

static int freerange_regen_worker(void *userdata) {
    RegenWorkerCtx *ctx = (RegenWorkerCtx *)userdata;

    size_t ball_size = (size_t)BALL_W * (size_t)BALL_H * 4;
    uint8_t *ball_pixels = NULL;
    if (posix_memalign((void **)&ball_pixels, 32, ball_size) != 0) {
        return 0;
    }
    memset(ball_pixels, 0, ball_size);

    for (;;) {
        int seq = poingo_atomic_add(&ctx->next_seq, 1);
        if (seq >= (int)ctx->regen_total || poingo_atomic_get(&ctx->stop)) {
            break;
        }

        uint32_t unit;
        if (ctx->regen_order) {
            unit = ctx->regen_order[seq];
        } else {
            unit =
                (ctx->regen_start +
                 (uint32_t)((uint64_t)seq * (uint64_t)ctx->regen_stride % ctx->regen_total)) %
                ctx->regen_total;
        }

        int frame_index = (int)(unit / (uint32_t)ctx->num_blocks);
        int block_index = (int)(unit % (uint32_t)ctx->num_blocks);

        if (frame_index >= 0 && frame_index < ctx->frames->frame_count) {
            int y_start = block_index * ctx->block_size;
            int y_end = y_start + ctx->block_size;
            if (y_end > ctx->frames->frame_h) y_end = ctx->frames->frame_h;

            uint8_t *frame_dst = ctx->frames->frames +
                                 (size_t)frame_index * ctx->frames->frame_size;
            float phase_offset = ((float)frame_index / (float)ctx->frames->frame_count)
                                 * ctx->angle_period;

            for (int y = y_start; y < y_end; y++) {
                if (ctx->regen_mode == BALL_REGEN_CLEAR) {
                    uint8_t *row = frame_dst + (size_t)y * (size_t)ctx->frames->frame_w * 4;
                    memset(row, 0, (size_t)ctx->frames->frame_w * 4);
                } else {
                    int src_y = y - ctx->ball_offset_y;
                    if (src_y >= 0 && src_y < BALL_H) {
                        generate_sphere_pixels_band(ball_pixels, BALL_W, phase_offset,
                                                    src_y, src_y + 1);
                    }
                    freerange_regen_update_scanline(frame_dst,
                                                   ctx->frames->frame_w, ctx->frames->frame_h,
                                                   y,
                                                   ball_pixels, BALL_W, BALL_H,
                                                   ctx->shadow_pixels,
                                                   ctx->shadow_w, ctx->shadow_h,
                                                   ctx->ball_offset_x, ctx->ball_offset_y,
                                                   ctx->shadow_offset_x, ctx->shadow_offset_y);
                }
            }
        }

        poingo_atomic_set(&ctx->unit_done[seq], 1);
    }

    free(ball_pixels);
    return 0;
}

static void freerange_regen_workers_shutdown(FreedomState *st) {
    if (!st) return;
    if (st->regen_worker_ctx) {
        poingo_atomic_set(&st->regen_worker_ctx->stop, 1);
    }
    if (st->regen_threads) {
        for (int t = 0; t < st->regen_thread_count; t++) {
            if (st->regen_threads[t]) {
                poingo_thread_wait(st->regen_threads[t], NULL);
            }
        }
        st->regen_threads = NULL;
        st->regen_thread_count = 0;
    }
    if (st->regen_worker_ctx) {
        st->regen_worker_ctx = NULL;
    }
    st->regen_units_uploaded = 0;
}

static bool freerange_color_regen_prepare_assets(FreedomState *st, const FreedomFrameSet *frames) {
    if (!st || !frames) {
        return false;
    }
    if (!frames->frames || frames->frame_w <= 0 || frames->frame_h <= 0) {
        return false;
    }

    st->color_regen_ball_offset_x = g_ball_texture_offset_x;
    st->color_regen_ball_offset_y = g_ball_texture_offset_y;
    st->color_regen_mode = BALL_REGEN_FILL;

    if (!st->color_regen_ball_pixels) {
        size_t ball_size = (size_t)BALL_W * (size_t)BALL_H * 4;
        if (posix_memalign((void**)&st->color_regen_ball_pixels, 32, ball_size) != 0) {
            st->color_regen_ball_pixels = NULL;
            return false;
        }
        memset(st->color_regen_ball_pixels, 0, ball_size);
    }

    if (!st->color_regen_shadow_pixels) {
        const int sphere_texture_size = BALL_W;
        const float canonical_total_prop = 1.0f;
        const float canonical_ball_diameter = 124.0f * canonical_total_prop;
        const float canonical_ball_radius = canonical_ball_diameter * 0.5f;
        const float shadow_offset_x_screen = canonical_total_prop * 8.0f * 0.6f;
        const float shadow_offset_y_screen = canonical_total_prop * 8.0f * 1.0f;
        const float shadow_size_screen = canonical_ball_diameter * 1.1f + 20.0f * canonical_total_prop;

        const float scale_to_texture = (float)sphere_texture_size / canonical_ball_diameter;
        float desired_shadow_width_pixels = shadow_size_screen * scale_to_texture;
        int desired_shadow_size = (int)ceilf(desired_shadow_width_pixels);

        int shadow_w = 0, shadow_h = 0;
        uint8_t *shadow_pixels = generate_shadow_pixels(desired_shadow_size, &shadow_w, &shadow_h);
        if (!shadow_pixels) {
            return false;
        }

        const float shadow_x_screen = (canonical_ball_radius + shadow_offset_x_screen) - shadow_size_screen * 0.5f;
        const float shadow_y_screen = (canonical_ball_radius + shadow_offset_y_screen) - shadow_size_screen * 0.5f;
        const float shadow_x_tex = shadow_x_screen * scale_to_texture;
        const float shadow_y_tex = shadow_y_screen * scale_to_texture;

        st->color_regen_shadow_offset_x = (int)lroundf(shadow_x_tex + (float)st->color_regen_ball_offset_x);
        st->color_regen_shadow_offset_y = (int)lroundf(shadow_y_tex + (float)st->color_regen_ball_offset_y);
        st->color_regen_shadow_pixels = shadow_pixels;
        st->color_regen_shadow_w = shadow_w;
        st->color_regen_shadow_h = shadow_h;
    }

    return true;
}

static void freerange_regen_update_scanline(uint8_t * restrict dst_frame,
                                          int composite_w,
                                          int composite_h,
                                          int y,
                                          const uint8_t * restrict ball_pixels,
                                          int ball_w,
                                          int ball_h,
                                          const uint8_t * restrict shadow_pixels,
                                          int shadow_w,
                                          int shadow_h,
                                          int ball_x,
                                          int ball_y,
                                          int shadow_x,
                                          int shadow_y) {
    if (!dst_frame || composite_w <= 0 || composite_h <= 0 || y < 0 || y >= composite_h) {
        return;
    }

    uint8_t *row = dst_frame + (size_t)y * (size_t)composite_w * 4;
    memset(row, 0, (size_t)composite_w * 4);

    int shadow_src_y = y - shadow_y;
    if (shadow_pixels && shadow_src_y >= 0 && shadow_src_y < shadow_h) {
        int dst_x0 = shadow_x;
        int src_x0 = 0;
        int copy_w = shadow_w;
        if (dst_x0 < 0) { src_x0 = -dst_x0; copy_w -= src_x0; dst_x0 = 0; }
        if (dst_x0 + copy_w > composite_w) { copy_w = composite_w - dst_x0; }
        if (copy_w > 0) {
            const uint8_t *src = shadow_pixels + ((size_t)shadow_src_y * (size_t)shadow_w + (size_t)src_x0) * 4;
            uint8_t *dst = row + (size_t)dst_x0 * 4;
            memcpy(dst, src, (size_t)copy_w * 4);
        }
    }

    int ball_src_y = y - ball_y;
    if (ball_pixels && ball_src_y >= 0 && ball_src_y < ball_h) {
        int dst_x0 = ball_x;
        int src_x0 = 0;
        int copy_w = ball_w;
        if (dst_x0 < 0) { src_x0 = -dst_x0; copy_w -= src_x0; dst_x0 = 0; }
        if (dst_x0 + copy_w > composite_w) { copy_w = composite_w - dst_x0; }
        if (copy_w > 0) {
            const uint8_t *src = ball_pixels + ((size_t)ball_src_y * (size_t)ball_w + (size_t)src_x0) * 4;
            uint8_t *dst = row + (size_t)dst_x0 * 4;
            for (int x = 0; x < copy_w; x++) {
                const uint8_t *sp = src + (size_t)x * 4;
                if (sp[3] == 0) {
                    continue;
                }
                uint8_t *dp = dst + (size_t)x * 4;
                dp[0] = sp[0];
                dp[1] = sp[1];
                dp[2] = sp[2];
                dp[3] = 255;
            }
        }
    }
}

static int freerange_regen_ideal_block_size(float ms_per_block, int frame_count, int frame_h) {
    if (ms_per_block <= 0.0f || frame_count <= 0 || frame_h <= 0) {
        return FREERANGE_REGEN_BLOCK_SIZE;
    }
    float ideal_total = FREERANGE_REGEN_TARGET_TICKS * (FREERANGE_REGEN_TARGET_MS / ms_per_block);
    float ideal_block = ((float)frame_count * (float)frame_h) / ideal_total;
    int rounded = (int)(ideal_block + 0.5f);
    if (rounded < FREERANGE_REGEN_BLOCK_SIZE) rounded = FREERANGE_REGEN_BLOCK_SIZE;
    if (rounded > frame_h)                    rounded = frame_h;
    return rounded;
}

static bool freerange_regen_workspace_prepare(FreedomState *st,
                                               const FreedomFrameSet *frames) {
    if (!st || !frames || frames->frame_count <= 0 || frames->frame_h <= 0) return false;
    if (st->regen_workspace_units > 0) return true;
    uint32_t units = (uint32_t)frames->frame_count *
                     (uint32_t)((frames->frame_h + FREERANGE_REGEN_BLOCK_SIZE - 1) /
                                FREERANGE_REGEN_BLOCK_SIZE);
    int threads = poingo_cpu_count() - 1;
    if (threads < 1) threads = 1;
    if (units == 0) return false;
    st->regen_unit_done_storage = calloc(units, sizeof(*st->regen_unit_done_storage));
    st->regen_order_storage = malloc((size_t)units * sizeof(*st->regen_order_storage));
    st->regen_thread_storage = calloc((size_t)threads, sizeof(*st->regen_thread_storage));
    if (!st->regen_unit_done_storage || !st->regen_order_storage ||
        !st->regen_thread_storage) {
        free(st->regen_unit_done_storage);
        free(st->regen_order_storage);
        free(st->regen_thread_storage);
        st->regen_unit_done_storage = NULL;
        st->regen_order_storage = NULL;
        st->regen_thread_storage = NULL;
        return false;
    }
    st->regen_workspace_units = units;
    st->regen_workspace_threads = threads;
    return true;
}

static void freerange_color_regen_start(FreedomState *st, const FreedomFrameSet *frames) {
    if (!st || !frames || !frames->frames || frames->frame_count <= 0 || frames->frame_h <= 0) {
        return;
    }

    int block_size = freerange_regen_ideal_block_size(st->regen_ms_per_block,
                                                      frames->frame_count, frames->frame_h);
    st->color_regen_block_size = block_size;

    int regen_num_blocks = (frames->frame_h + block_size - 1) / block_size;
    uint32_t total_units = (uint32_t)frames->frame_count * (uint32_t)regen_num_blocks;
    if (total_units == 0) {
        return;
    }

    uint32_t start = (uint32_t)(random() % (long)total_units);
    uint32_t stride = 1;
    if (total_units > 1) {
        for (int attempt = 0; attempt < 1000; attempt++) {
            stride = (uint32_t)(random() % (long)(total_units - 1)) + 1;
            if (gcd_u32(stride, total_units) == 1) {
                break;
            }
        }
    }

    st->color_regen_start = start;
    st->color_regen_stride = stride;
    st->color_regen_cursor = 0;
    st->color_regen_units_done = 0;
    st->color_regen_units_total = total_units;

    freerange_regen_workers_shutdown(st);

    if (!st->regen_unit_done_storage || !st->regen_order_storage ||
        !st->regen_thread_storage || total_units > st->regen_workspace_units) return;
    RegenWorkerCtx *ctx = &st->regen_ctx_storage;
    memset(ctx, 0, sizeof(*ctx));
    memset(st->regen_unit_done_storage, 0,
           (size_t)total_units * sizeof(*st->regen_unit_done_storage));
    ctx->unit_done = st->regen_unit_done_storage;

    ctx->frames          = (FreedomFrameSet *)frames;
    ctx->block_size      = block_size;
    ctx->num_blocks      = regen_num_blocks;
    ctx->regen_start     = start;
    ctx->regen_stride    = stride;
    ctx->regen_total     = total_units;
    regen_make_shuffled_order(st->regen_order_storage, total_units);
    ctx->regen_order     = st->regen_order_storage;
    ctx->regen_mode      = st->color_regen_mode;
    ctx->angle_period    = st->color_regen_angle_period;
    ctx->shadow_pixels   = st->color_regen_shadow_pixels;
    ctx->shadow_w        = st->color_regen_shadow_w;
    ctx->shadow_h        = st->color_regen_shadow_h;
    ctx->shadow_offset_x = st->color_regen_shadow_offset_x;
    ctx->shadow_offset_y = st->color_regen_shadow_offset_y;
    ctx->ball_offset_x   = st->color_regen_ball_offset_x;
    ctx->ball_offset_y   = st->color_regen_ball_offset_y;
    poingo_atomic_set(&ctx->next_seq, 0);
    poingo_atomic_set(&ctx->stop, 0);

    st->regen_worker_ctx   = ctx;
    st->regen_units_uploaded = 0;

    int thread_count = poingo_cpu_count() - 1;
    if (thread_count < 1) thread_count = 1;
    if (thread_count > (int)total_units) thread_count = (int)total_units;

    st->regen_threads = st->regen_thread_storage;
    memset(st->regen_threads, 0,
           (size_t)thread_count * sizeof(*st->regen_threads));

    int spawned = 0;
    for (int t = 0; t < thread_count; t++) {
        st->regen_threads[t] = poingo_thread_create(freerange_regen_worker, "regen_worker", ctx);
        if (!st->regen_threads[t]) {
            break;
        }
        spawned++;
    }
    st->regen_thread_count = spawned;

    if (spawned == 0) {
        freerange_regen_workers_shutdown(st);
        return;
    }

    st->color_regen_active = true;
}

static void freerange_regen_upload_step(FreedomState *st, FreedomFrameSet *frames,
                                        int max_units, double target_ms) {
    if (!st || !frames || !st->color_regen_active || !st->regen_worker_ctx || target_ms <= 0.0) {
        return;
    }

    RegenWorkerCtx *ctx = st->regen_worker_ctx;
    const int total        = (int)ctx->regen_total;
    const int block_size   = ctx->block_size;
    const int num_blocks   = ctx->num_blocks;

    uint64_t upload_start = poingo_perf_counter();
    int uploaded_this_tick = 0;

    while (st->regen_units_uploaded < total) {
        if (max_units > 0 && uploaded_this_tick >= max_units) break;
        if (get_perf_seconds(upload_start, poingo_perf_counter()) * 1000.0 >= target_ms) {
            break;
        }

        int seq = st->regen_units_uploaded;
        if (!poingo_atomic_get(&ctx->unit_done[seq])) {
            break;  
        }

        uint32_t unit;
        if (ctx->regen_order) {
            unit = ctx->regen_order[seq];
        } else {
            unit =
                (ctx->regen_start +
                 (uint32_t)((uint64_t)seq * (uint64_t)ctx->regen_stride % ctx->regen_total)) %
                ctx->regen_total;
        }
        int frame_index = (int)(unit / (uint32_t)num_blocks);
        int block_index = (int)(unit % (uint32_t)num_blocks);

        if (frame_index >= 0 && frame_index < frames->frame_count &&
            st->gl_ready && st->gl_textures && frame_index < st->gl_texture_count) {
            int y_start = block_index * block_size;
            int y_end   = y_start + block_size;
            if (y_end > frames->frame_h) y_end = frames->frame_h;
            const uint8_t *src = frames->frames
                                 + (size_t)frame_index * frames->frame_size
                                 + (size_t)y_start * (size_t)frames->frame_w * 4;
            glBindTexture(GL_TEXTURE_2D, st->gl_textures[frame_index]);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y_start,
                            frames->frame_w, y_end - y_start,
                            GL_RGBA, GL_UNSIGNED_BYTE, src);
        }

        st->regen_units_uploaded++;
        uploaded_this_tick++;
    }

    if (st->regen_units_uploaded >= total) {
        freerange_regen_workers_shutdown(st);
        st->color_regen_active = false;
        return;
    }

    if (g_debug_mode && uploaded_this_tick > 0) {
        static int regen_log_countdown = 0;
        if (--regen_log_countdown <= 0) {
            regen_log_countdown = 60;
            int workers_at = poingo_atomic_get(&ctx->next_seq);
            fprintf(stderr, "[regen] uploaded %d  workers at %d/%d  total uploaded %d/%d\n",
                    uploaded_this_tick, workers_at, total,
                    st->regen_units_uploaded, total);
        }
    }
}


#define CURSOR_BUF 128            /* cursor buffer: blade + hilt */
#define CURSOR_BALL_SIZE 72       /* hilt ball diameter */
#define CURSOR_BALL_FRAMES 20
#define CURSOR_TIP 2              /* streak point = hotspot (x = y) */
#define STREAK_GAP 4.0f           /* float clearance between streak and ball */
#define STREAK_BORDER 2.2f        /* dark rim width just inside the edge */

static struct {
    struct wl_surface *surface;
    struct wl_buffer *buffers[CURSOR_BALL_FRAMES];
    uint8_t *map;
    size_t map_size;
    uint32_t *blade;   
    int current;
    bool ready;
} g_ball_cursor;

static struct { uint8_t *m; int w, h; } g_etch;

static uint8_t *cursor_read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *buf = NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        long sz = ftell(f);
        if (sz > 0 && fseek(f, 0, SEEK_SET) == 0) {
            buf = malloc((size_t)sz);
            if (buf && fread(buf, 1, (size_t)sz, f) == (size_t)sz) {
                *out_len = (size_t)sz;
            } else {
                free(buf);
                buf = NULL;
            }
        }
    }
    fclose(f);
    return buf;
}

static void etch_mask_load(void) {
    static const char *paths[] = {
        "/usr/share/piwiz/raspberry-pi-logo.png",
        "/usr/share/raspberrypi-artwork/raspberry-pi-logo-small.png",
        "/usr/share/raspberrypi-artwork/raspberry-pi-logo.png",
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        size_t len = 0;
        uint8_t *buf = cursor_read_file(paths[i], &len);
        if (!buf) continue;
        uint8_t *rgba = NULL;
        unsigned w = 0, h = 0;
        unsigned err = lodepng_decode32(&rgba, &w, &h, buf, len);
        free(buf);
        if (err || !rgba) continue;
        uint8_t *m = malloc((size_t)w * h);
        if (!m) { free(rgba); return; }
        for (size_t p = 0; p < (size_t)w * h; p++) {
            const uint8_t *s = rgba + p * 4;
            unsigned v = s[0];               
            if (s[1] > v) v = s[1];
            if (s[2] > v) v = s[2];
            m[p] = (uint8_t)((s[3] * (255u - v)) / 255u);
        }
        free(rgba);
        g_etch.m = m;
        g_etch.w = (int)w;
        g_etch.h = (int)h;
        return;
    }
}

static float etch_sample(float x, float y) {
    if (!g_etch.m) return 0.0f;
    x -= 0.5f; y -= 0.5f;
    int x0 = (int)floorf(x), y0 = (int)floorf(y);
    float fx = x - x0, fy = y - y0;
    float acc = 0.0f;
    for (int j = 0; j <= 1; j++) {
        int yy = y0 + j;
        if (yy < 0 || yy >= g_etch.h) continue;
        float wy = j ? fy : 1.0f - fy;
        for (int i = 0; i <= 1; i++) {
            int xx = x0 + i;
            if (xx < 0 || xx >= g_etch.w) continue;
            float wx = i ? fx : 1.0f - fx;
            acc += wx * wy * (float)g_etch.m[yy * g_etch.w + xx];
        }
    }
    return acc / 255.0f;
}

static void blade_render(void) {
    uint32_t *px = g_ball_cursor.blade;
    if (!px) return;
    memset(px, 0, (size_t)CURSOR_BUF * CURSOR_BUF * 4);

    const float s2 = 0.70710678f;
    const float tipx = CURSOR_TIP + 0.5f, tipy = CURSOR_TIP + 0.5f;
    const float bc = (float)CURSOR_BUF - CURSOR_BALL_SIZE / 2.0f - 0.5f;
    const float len = (bc - tipx) / s2;    
    const float wend = CURSOR_BALL_SIZE / 2.0f - 1.0f;  

    float lx3 = -0.3f, ly3 = -0.5f, lz3 = 1.0f;
    float il = 1.0f / sqrtf(lx3 * lx3 + ly3 * ly3 + lz3 * lz3);
    lx3 *= il; ly3 *= il; lz3 *= il;
    const float vdotl = s2 * lx3 - s2 * ly3;

    float eh = 30.0f, escale = 1.0f, et0 = 0.0f;
    if (g_etch.m) {
        escale = eh / (float)g_etch.h;
        et0 = 0.45f * len - eh * 0.5f;
    }

    for (int y = 0; y < CURSOR_BUF; y++) {
        for (int x = 0; x < CURSOR_BUF; x++) {
            float rx = (float)x - tipx, ry = (float)y - tipy;
            float t = (rx + ry) * s2;      
            float s = (rx - ry) * s2;      
            if (t < -0.8f || t > len) continue;
            float tt = t < 0.0f ? 0.0f : t;
            float w = wend * powf(tt / len, 0.55f);

            float bdx = (float)x - bc, bdy = (float)y - bc;
            float e = w - fabsf(s);
            float eb = sqrtf(bdx * bdx + bdy * bdy) - (wend + STREAK_GAP);
            if (eb < e) e = eb;
            float cov = e + 0.5f;
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;

            float q = w > 0.01f ? clampf(s / w, -1.0f, 1.0f) : 0.0f;
            float nz = sqrtf(1.0f - 0.85f * q * q);
            float dot = q * vdotl + nz * lz3;
            float bright = 0.58f + 0.52f * fmaxf(0.0f, dot);
            bright *= 1.0f + 0.05f * (1.0f - tt / len);

            float r = 214.0f * bright, g = 218.0f * bright, b = 226.0f * bright;

            if (g_etch.m) {
                float lx = s / escale + (float)g_etch.w * 0.5f;
                float ly = (t - et0) / escale;
                float m = etch_sample(lx, ly);
                const float dpx = 1.1f;
                float up = etch_sample(lx + 0.243f * dpx, ly - 0.970f * dpx);
                float dn = etch_sample(lx - 0.243f * dpx, ly + 0.970f * dpx);
                float relief = up - dn;      
                float k = 1.0f - 0.16f * m;
                float lift = 85.0f * relief;
                r = r * k + lift;
                g = g * k + lift;
                b = b * k + lift * 1.04f;
            }

            if (e < STREAK_BORDER) {
                float ol = (STREAK_BORDER - e) / STREAK_BORDER;
                if (ol > 1.0f) ol = 1.0f;
                ol *= 0.8f;
                r += (92.0f - r) * ol;
                g += (97.0f - g) * ol;
                b += (107.0f - b) * ol;
            }

            if (r < 0.0f) r = 0.0f;
            if (g < 0.0f) g = 0.0f;
            if (b < 0.0f) b = 0.0f;
            if (r > 255.0f) r = 255.0f;
            if (g > 255.0f) g = 255.0f;
            if (b > 255.0f) b = 255.0f;
            uint32_t a8 = (uint32_t)(cov * 255.0f + 0.5f);
            uint32_t r8 = (uint32_t)(r * cov + 0.5f);
            uint32_t g8 = (uint32_t)(g * cov + 0.5f);
            uint32_t b8 = (uint32_t)(b * cov + 0.5f);
            px[y * CURSOR_BUF + x] = (a8 << 24) | (r8 << 16) | (g8 << 8) | b8;
        }
    }
}

static void ball_cursor_render_frames(void) {
    if (!g_ball_cursor.map) return;

    const int size = CURSOR_BUF;
    const float radius = CURSOR_BALL_SIZE / 2.0f - 1.0f;
    const float cx = (float)CURSOR_BUF - CURSOR_BALL_SIZE / 2.0f - 0.5f;
    const float cy = cx;
    const float inv_radius = 1.0f / radius;
    const float angle_period = (4.0f * PI) / LON_TILES;

    int bx0 = (int)(cx - radius) - 1, bx1 = (int)(cx + radius) + 2;
    int by0 = bx0, by1 = bx1;
    if (bx0 < 0) bx0 = 0;
    if (by0 < 0) by0 = 0;
    if (bx1 > size) bx1 = size;
    if (by1 > size) by1 = size;

    float light_x = -0.3f, light_y = -0.5f, light_z = 1.0f;
    float inv_len = 1.0f / sqrtf(light_x * light_x + light_y * light_y +
                                 light_z * light_z);
    light_x *= inv_len; light_y *= inv_len; light_z *= inv_len;

    for (int f = 0; f < CURSOR_BALL_FRAMES; f++) {
        uint32_t *px = (uint32_t *)(g_ball_cursor.map +
                                    (size_t)f * size * size * 4);
        memcpy(px, g_ball_cursor.blade, (size_t)size * size * 4);
        float phase_norm = ((float)f / (float)CURSOR_BALL_FRAMES) *
                           angle_period / (2.0f * PI);
        for (int y = by0; y < by1; y++) {
            for (int x = bx0; x < bx1; x++) {
                float dx = (float)x - cx;
                float dy = (float)y - cy;
                float dist = sqrtf(dx * dx + dy * dy);
                float cov = radius - dist + 0.5f;
                if (cov <= 0.0f) continue;
                if (cov > 1.0f) cov = 1.0f;

                float vx = dx * inv_radius;
                float vy = dy * inv_radius;
                float r2 = vx * vx + vy * vy;
                if (r2 > 1.0f) r2 = 1.0f;
                float nz = sqrtf(1.0f - r2);

                float dot = vx * light_x + vy * light_y + nz * light_z;
                float brightness = AMBIENT_LIGHT +
                                   DIFFUSE_LIGHT * fmaxf(0.0f, dot);
                brightness = clampf(brightness, MIN_BRIGHTNESS, 1.0f);

                float na = vx * axis_ax + vy * axis_ay + nz * axis_az;
                float nu = vx * axis_ux + vy * axis_uy + nz * axis_uz;
                float nv = vx * axis_vx + vy * axis_vy + nz * axis_vz;

                float lat = asinf(clampf(na, -1.0f, 1.0f));
                int v = (int)floorf((lat + PI * 0.5f) *
                                    ((float)LAT_TILES / PI));
                if (v < 0) v = 0;
                if (v >= LAT_TILES) v = LAT_TILES - 1;

                float lon_norm = atan2f(nv, nu) / (2.0f * PI) + phase_norm;
                lon_norm -= floorf(lon_norm);
                int u = (int)(lon_norm * (float)LON_TILES);
                if (u >= LON_TILES) u = LON_TILES - 1;

                const uint8_t *rgb = (((u + v) & 1) == 0) ? g_color_light_rgb
                                                          : g_color_dark_rgb;
                uint32_t under = px[y * size + x];
                float inv = 1.0f - cov;
                uint32_t a8 = (uint32_t)(cov * 255.0f +
                                         inv * ((under >> 24) & 0xFF) + 0.5f);
                uint32_t r8 = (uint32_t)(rgb[0] * brightness * cov +
                                         inv * ((under >> 16) & 0xFF) + 0.5f);
                uint32_t g8 = (uint32_t)(rgb[1] * brightness * cov +
                                         inv * ((under >> 8) & 0xFF) + 0.5f);
                uint32_t b8 = (uint32_t)(rgb[2] * brightness * cov +
                                         inv * (under & 0xFF) + 0.5f);
                px[y * size + x] = (a8 << 24) | (r8 << 16) | (g8 << 8) | b8;
            }
        }
    }
}

static bool ball_cursor_create(struct wl_shm *shm, struct wl_compositor *compositor) {
    if (!shm || !compositor) return false;

    int stride = CURSOR_BUF * 4;
    size_t frame_bytes = (size_t)stride * CURSOR_BUF;
    size_t total = frame_bytes * CURSOR_BALL_FRAMES;

    int fd = memfd_create("poingo-cursor", MFD_CLOEXEC);
    if (fd < 0) return false;
    if (ftruncate(fd, (off_t)total) < 0) { close(fd); return false; }
    void *data = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) { close(fd); return false; }

    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)total);
    for (int f = 0; f < CURSOR_BALL_FRAMES; f++) {
        g_ball_cursor.buffers[f] = wl_shm_pool_create_buffer(
            pool, (int32_t)(f * frame_bytes),
            CURSOR_BUF, CURSOR_BUF, stride, WL_SHM_FORMAT_ARGB8888);
    }
    wl_shm_pool_destroy(pool);
    close(fd);

    g_ball_cursor.map = data;
    g_ball_cursor.map_size = total;

    g_ball_cursor.blade = malloc((size_t)CURSOR_BUF * CURSOR_BUF * 4);
    if (!g_ball_cursor.blade) return false;
    etch_mask_load();   
    blade_render();

    compute_axis_vectors();
    ball_cursor_render_frames();

    g_ball_cursor.surface = wl_compositor_create_surface(compositor);
    if (!g_ball_cursor.surface) return false;
    wl_surface_attach(g_ball_cursor.surface, g_ball_cursor.buffers[0], 0, 0);
    wl_surface_damage(g_ball_cursor.surface, 0, 0, CURSOR_BUF, CURSOR_BUF);
    wl_surface_commit(g_ball_cursor.surface);
    g_ball_cursor.current = 0;
    g_ball_cursor.ready = true;
    return true;
}

static void ball_cursor_set(struct wl_pointer *pointer, uint32_t serial) {
    if (!g_ball_cursor.ready) return;
    wl_pointer_set_cursor(pointer, serial, g_ball_cursor.surface,
                          CURSOR_TIP, CURSOR_TIP);
}

static void ball_cursor_animate(int phase_i, int frame_count) {
    if (!g_ball_cursor.ready || frame_count <= 0) return;
    int f = (int)(((long)phase_i * CURSOR_BALL_FRAMES) / frame_count);
    f %= CURSOR_BALL_FRAMES;
    if (f < 0) f += CURSOR_BALL_FRAMES;
    if (f == g_ball_cursor.current) return;
    g_ball_cursor.current = f;
    wl_surface_attach(g_ball_cursor.surface, g_ball_cursor.buffers[f], 0, 0);
    wl_surface_damage(g_ball_cursor.surface, 0, 0, CURSOR_BUF, CURSOR_BUF);
    wl_surface_commit(g_ball_cursor.surface);
}

static void ball_cursor_destroy(void) {
    for (int f = 0; f < CURSOR_BALL_FRAMES; f++) {
        if (g_ball_cursor.buffers[f]) wl_buffer_destroy(g_ball_cursor.buffers[f]);
        g_ball_cursor.buffers[f] = NULL;
    }
    if (g_ball_cursor.surface) wl_surface_destroy(g_ball_cursor.surface);
    if (g_ball_cursor.map) munmap(g_ball_cursor.map, g_ball_cursor.map_size);
    free(g_ball_cursor.blade);
    memset(&g_ball_cursor, 0, sizeof(g_ball_cursor));
    free(g_etch.m);
    memset(&g_etch, 0, sizeof(g_etch));
}

static void freerange_color_randomize_and_regen(FreedomState *st) {
    if (!st || !st->frames_ref) {
        return;
    }

    float light_value = fmaxf(g_color_light_rgb[0],
                              fmaxf(g_color_light_rgb[1], g_color_light_rgb[2])) / 255.0f;
    pick_palette_colors(light_value,
                        g_color_light_rgb, g_color_dark_rgb,
                        false);

    invalidate_sphere_pixel_cache();
    if (!ensure_sphere_pixel_cache(BALL_W)) {
        return;
    }

    if (!freerange_color_regen_prepare_assets(st, st->frames_ref)) {
        return;
    }
    freerange_color_regen_start(st, st->frames_ref);
}

static void freerange_clear_ball_and_start_regen(FreedomState *st) {
    if (!st || !st->frames_ref) {
        return;
    }
    if (!freerange_color_regen_prepare_assets(st, st->frames_ref)) {
        return;
    }
    st->color_regen_mode = BALL_REGEN_CLEAR;
    freerange_color_regen_start(st, st->frames_ref);
}

static void freerange_request_graceful_shutdown(FreedomState *st) {
    if (!st) {
        return;
    }
    if (st->ball_cleared) {
        return;
    }
    if (st->color_regen_mode == BALL_REGEN_CLEAR && st->color_regen_active) {
        return;
    }
    freerange_clear_ball_and_start_regen(st);
}

static void freerange_color_set_and_regen(FreedomState *st,
                                        const uint8_t light_rgb[3],
                                        const uint8_t dark_rgb[3]) {
    if (!st || !st->frames_ref || !light_rgb || !dark_rgb) {
        return;
    }

    g_color_light_rgb[0] = light_rgb[0];
    g_color_light_rgb[1] = light_rgb[1];
    g_color_light_rgb[2] = light_rgb[2];
    g_color_dark_rgb[0] = dark_rgb[0];
    g_color_dark_rgb[1] = dark_rgb[1];
    g_color_dark_rgb[2] = dark_rgb[2];

    invalidate_sphere_pixel_cache();
    if (!ensure_sphere_pixel_cache(BALL_W)) {
        return;
    }

    if (!freerange_color_regen_prepare_assets(st, st->frames_ref)) {
        return;
    }
    freerange_color_regen_start(st, st->frames_ref);
}

static GLuint freerange_gl_create_shader(const char *source, GLenum type) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    return shader;
}



static bool freerange_gl_upload_frames(FreedomState *st, const FreedomFrameSet *frames) {
    if (!st || !frames || !frames->frames || frames->frame_count <= 0) {
        return false;
    }

    st->gl_textures = calloc((size_t)frames->frame_count, sizeof(GLuint));
    if (!st->gl_textures) {
        return false;
    }

    st->gl_texture_count = frames->frame_count;
    glGenTextures(frames->frame_count, st->gl_textures);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    uint8_t *blank = NULL;
    if (st->color_regen_active && st->color_regen_mode == BALL_REGEN_FILL &&
        st->regen_units_uploaded == 0) {
        blank = calloc(frames->frame_size, 1);
    }

    for (int i = 0; i < frames->frame_count; i++) {
        const uint8_t *frame = blank ? blank
                                     : frames->frames + (size_t)i * frames->frame_size;
        glBindTexture(GL_TEXTURE_2D, st->gl_textures[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                     frames->frame_w, frames->frame_h,
                     0, GL_RGBA, GL_UNSIGNED_BYTE, frame);
    }
    free(blank);
    return true;
}

static bool freerange_gl_init(FreedomState *st, const FreedomFrameSet *frames) {
    if (!st || !frames) {
        return false;
    }

    GLuint vert = freerange_gl_create_shader(freerange_vert_shader_text, GL_VERTEX_SHADER);
    GLuint frag = freerange_gl_create_shader(freerange_frag_shader_text, GL_FRAGMENT_SHADER);

    st->gl_program = glCreateProgram();
    glAttachShader(st->gl_program, vert);
    glAttachShader(st->gl_program, frag);
    glLinkProgram(st->gl_program);
    glUseProgram(st->gl_program);

    glDeleteShader(vert);
    glDeleteShader(frag);

    st->gl_pos_loc = glGetAttribLocation(st->gl_program, "pos");
    st->gl_uv_loc = glGetAttribLocation(st->gl_program, "uv_in");
    st->gl_tex_loc = glGetUniformLocation(st->gl_program, "tex");
    st->gl_alpha_loc = glGetUniformLocation(st->gl_program, "alpha");
    if (st->gl_pos_loc < 0 || st->gl_uv_loc < 0 || st->gl_tex_loc < 0) {
        return false;
    }

    glUniform1i(st->gl_tex_loc, 0);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    if (!freerange_gl_upload_frames(st, frames)) {
        return false;
    }
    
    {
        uint32_t *bg = ghost_icon_create_bg(false);
        if (bg) {
            glGenTextures(1, &st->gl_ghost_bg_texture);
            glBindTexture(GL_TEXTURE_2D, st->gl_ghost_bg_texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, GHOST_ICON_SIZE, GHOST_ICON_SIZE,
                         0, GL_RGBA, GL_UNSIGNED_BYTE, bg);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            free(bg);
        }
    }

    st->gl_ready = true;
    return true;
}

static void freerange_gl_shutdown(FreedomState *st) {
    if (!st) {
        return;
    }
    if (st->gl_textures && st->gl_texture_count > 0) {
        glDeleteTextures(st->gl_texture_count, st->gl_textures);
        free(st->gl_textures);
        st->gl_textures = NULL;
        st->gl_texture_count = 0;
    }
    if (st->gl_program) {
        glDeleteProgram(st->gl_program);
        st->gl_program = 0;
    }
    if (st->gl_hud_texture) {
        glDeleteTextures(1, &st->gl_hud_texture);
        st->gl_hud_texture = 0;
    }
    if (st->gl_ghost_bg_texture) {
        glDeleteTextures(1, &st->gl_ghost_bg_texture);
        st->gl_ghost_bg_texture = 0;
    }
    free(st->hud_pixels);
    st->hud_pixels = NULL;
    st->hud_pixels_size = 0;
    st->gl_hud_w = 0;
    st->gl_hud_h = 0;
    st->gl_ready = false;
}


typedef struct {
    int x, y, w, h;
} FreerangeRect;

static void freerange_rect_union(FreerangeRect *acc, const FreerangeRect *r) {
    if (!r || r->w <= 0 || r->h <= 0) {
        return;
    }
    if (acc->w <= 0 || acc->h <= 0) {
        *acc = *r;
        return;
    }
    int x0 = acc->x < r->x ? acc->x : r->x;
    int y0 = acc->y < r->y ? acc->y : r->y;
    int x1 = (acc->x + acc->w) > (r->x + r->w) ? (acc->x + acc->w) : (r->x + r->w);
    int y1 = (acc->y + acc->h) > (r->y + r->h) ? (acc->y + acc->h) : (r->y + r->h);
    acc->x = x0;
    acc->y = y0;
    acc->w = x1 - x0;
    acc->h = y1 - y0;
}

static void freerange_rect_clamp(FreerangeRect *r, int width, int height) {
    if (r->x < 0) { r->w += r->x; r->x = 0; }
    if (r->y < 0) { r->h += r->y; r->y = 0; }
    if (r->x + r->w > width)  r->w = width  - r->x;
    if (r->y + r->h > height) r->h = height - r->y;
    if (r->w < 0) r->w = 0;
    if (r->h < 0) r->h = 0;
}

static bool freerange_ball_dest_quad(const FreedomState *st, const FreedomFrameSet *frames,
                                     float extrap_dt,
                                     float *out_x, float *out_y, float *out_w, float *out_h) {
    if (!st || !frames || frames->frame_count <= 0 ||
        st->width <= 0 || st->height <= 0) {
        return false;
    }

    float exscale = extrap_dt * 60.0f * g_speed_multiplier;
    float rx = st->ball_x + (st->ball_vx * (float)st->ball_vx_direction) * exscale;
    float ry = st->ball_y + st->ball_vy * exscale;
    float rx_max = (float)st->width - st->ball_diameter;
    float ry_max = 1.0f - st->ball_diameter_norm;
    if (rx < 0.0f) rx = 0.0f;
    if (rx > rx_max) rx = rx_max;
    if (ry < 0.0f) ry = 0.0f;
    if (ry > ry_max) ry = ry_max;

    float ball_y_pixels = ry * (float)st->height;
    float composite_scale = st->ball_diameter / (float)BALL_W;
    float composite_offset_x = (float)g_ball_texture_offset_x * composite_scale;
    float composite_offset_y = (float)g_ball_texture_offset_y * composite_scale;

    *out_x = rx - composite_offset_x;
    *out_y = ball_y_pixels - composite_offset_y;
    *out_w = (float)frames->frame_w * composite_scale;
    *out_h = (float)frames->frame_h * composite_scale;
    return true;
}

static void freerange_gl_draw_frame(FreedomState *st, const FreedomFrameSet *frames, float extrap_dt) {
    if (!st || !st->gl_ready) {
        return;
    }

    float dest_x, dest_y, composite_w, composite_h;
    if (!freerange_ball_dest_quad(st, frames, extrap_dt,
                                  &dest_x, &dest_y, &composite_w, &composite_h)) {
        return;
    }

    int frame_index = st->phase_i;
    if (frame_index < 0 || frame_index >= frames->frame_count) {
        return;
    }

    float x0 = (dest_x / (float)st->width) * 2.0f - 1.0f;
    float x1 = ((dest_x + composite_w) / (float)st->width) * 2.0f - 1.0f;
    float y0 = 1.0f - (dest_y / (float)st->height) * 2.0f;
    float y1 = 1.0f - ((dest_y + composite_h) / (float)st->height) * 2.0f;

    GLfloat verts[] = {
        x0, y1, 0.0f, 1.0f,
        x1, y1, 1.0f, 1.0f,
        x0, y0, 0.0f, 0.0f,
        x1, y0, 1.0f, 0.0f
    };

    glUseProgram(st->gl_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, st->gl_textures[frame_index]);
    glVertexAttribPointer(st->gl_pos_loc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), verts);
    glVertexAttribPointer(st->gl_uv_loc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), verts + 2);
    glEnableVertexAttribArray(st->gl_pos_loc);
    glEnableVertexAttribArray(st->gl_uv_loc);
    glUniform1f(st->gl_alpha_loc, st->ghost_mode ? 0.4f : 1.0f);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void hud_blend_pixel(uint8_t *dst, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (a == 0) {
        return;
    }

    uint8_t dst_a = dst[3];
    if (dst_a == 0) {
        dst[0] = r;
        dst[1] = g;
        dst[2] = b;
        dst[3] = a;
        return;
    }

    uint16_t inv_a = (uint16_t)(255 - a);
    uint16_t out_a = (uint16_t)a + (uint16_t)(dst_a * inv_a) / 255;
    if (out_a == 0) {
        return;
    }
    dst[0] = (uint8_t)(((uint16_t)r * a + (uint16_t)dst[0] * dst_a * inv_a / 255) / out_a);
    dst[1] = (uint8_t)(((uint16_t)g * a + (uint16_t)dst[1] * dst_a * inv_a / 255) / out_a);
    dst[2] = (uint8_t)(((uint16_t)b * a + (uint16_t)dst[2] * dst_a * inv_a / 255) / out_a);
    dst[3] = (uint8_t)out_a;
}

static void hud_fill_rect(uint8_t *buf, int buf_w, int buf_h,
                          int x, int y, int w, int h,
                          uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (!buf || buf_w <= 0 || buf_h <= 0 || w <= 0 || h <= 0) {
        return;
    }

    int x0 = x;
    int y0 = y;
    int x1 = x + w;
    int y1 = y + h;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > buf_w) x1 = buf_w;
    if (y1 > buf_h) y1 = buf_h;

    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    for (int yy = y0; yy < y1; yy++) {
        uint8_t *row = buf + ((size_t)yy * (size_t)buf_w + (size_t)x0) * 4;
        for (int xx = x0; xx < x1; xx++) {
            hud_blend_pixel(row, r, g, b, a);
            row += 4;
        }
    }
}

static void hud_draw_text(uint8_t *buf, int buf_w, int buf_h,
                          int x, int y, const char *text, int scale,
                          uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (!buf || buf_w <= 0 || buf_h <= 0 || !text || *text == '\0' || scale <= 0) {
        return;
    }

    int cursor_x = x;
    for (const char *p = text; *p != '\0'; ++p) {
        const GlyphDef *glyph = lookup_volume_glyph(*p);
        if (glyph) {
            for (int row = 0; row < GLYPH_ROWS; row++) {
                const char *pattern = glyph->rows[row];
                for (int col = 0; col < GLYPH_COLS; col++) {
                    if (pattern[col] != '.' && pattern[col] != ' ') {
                        int px = cursor_x + col * scale;
                        int py = y + row * scale;
                        hud_fill_rect(buf, buf_w, buf_h, px, py, scale, scale, r, g, b, a);
                    }
                }
            }
        }
        cursor_x += GLYPH_COLS * scale;
        if (p[1] != '\0') {
            cursor_x += scale;
        }
    }
}

static bool freerange_gl_update_hud(FreedomState *st) {
    if (!st || !st->gl_ready || st->width <= 0 || st->height <= 0) {
        return false;
    }

    float hud_alpha = fmaxf(g_volume_hud_alpha, g_speed_hud_alpha);
    if (hud_alpha <= 0.0f) {
        st->gl_hud_w = 0;
        st->gl_hud_h = 0;
        return false;
    }

    float hud_width = fminf(380.0f, st->width * 0.7f);
    float hud_x = (st->width - hud_width) * 0.5f;
    float hud_y = st->height * 0.08f;
    float inner_margin = 10.0f;
    float bar_height = 14.0f;
    float bar_text_gap = 6.0f;
    float text_scale_f = 4.0f;
    float text_height = GLYPH_ROWS * text_scale_f;
    float section_gap = 12.0f;

    float volume_bar_y = hud_y + inner_margin;
    float volume_text_y = volume_bar_y + bar_height + bar_text_gap;
    float speed_bar_y = volume_text_y + text_height + section_gap;
    float speed_text_y = speed_bar_y + bar_height + bar_text_gap;
    float hud_height = (speed_text_y + text_height + inner_margin) - hud_y;

    char volume_label[32];
    if (g_volume_muted) {
        snprintf(volume_label, sizeof(volume_label), "^ MUTED v");
    } else {
        int percent = (int)lroundf((g_volume_hud_value / MASTER_VOLUME_MAX) * 100.0f);
        percent = percent < 0 ? 0 : (percent > 999 ? 999 : percent);
        snprintf(volume_label, sizeof(volume_label), "^ VOL %d%% v", percent);
    }

    char speed_label[48];
    float display_speed = clamp_speed(g_speed_multiplier);
    snprintf(speed_label, sizeof(speed_label), "< SPEED %.1fX>", display_speed);

    float volume_text_width = measure_text(volume_label, text_scale_f);
    float speed_text_width = measure_text(speed_label, text_scale_f);
    float max_text_width = fmaxf(volume_text_width, speed_text_width);
    float bar_width = hud_width - inner_margin * 2.0f;
    bool hud_fits =
        hud_width >= max_text_width + inner_margin * 2.0f &&
        bar_width > 1.0f &&
        hud_x >= 2.0f &&
        hud_x + hud_width <= (float)st->width - 2.0f &&
        hud_y >= 2.0f &&
        hud_y + hud_height <= (float)st->height - 2.0f;
    if (!hud_fits) {
        st->gl_hud_w = 0;
        st->gl_hud_h = 0;
        return false;
    }

    int hud_w = (int)ceilf(hud_width);
    int hud_h = (int)ceilf(hud_height);
    if (hud_w <= 0 || hud_h <= 0) {
        st->gl_hud_w = 0;
        st->gl_hud_h = 0;
        return false;
    }

    size_t needed = (size_t)hud_w * (size_t)hud_h * 4;
    if (needed > st->hud_pixels_size) {
        uint8_t *new_pixels = realloc(st->hud_pixels, needed);
        if (!new_pixels) {
            st->gl_hud_w = 0;
            st->gl_hud_h = 0;
            return false;
        }
        st->hud_pixels = new_pixels;
        st->hud_pixels_size = needed;
    }
    memset(st->hud_pixels, 0, needed);

    int text_scale = (int)lroundf(text_scale_f);
    float volume_ratio = (g_volume_hud_value - MASTER_VOLUME_MIN) /
                         (MASTER_VOLUME_MAX - MASTER_VOLUME_MIN);
    volume_ratio = fmaxf(0.0f, fminf(1.0f, volume_ratio));

    int bar_x = (int)lroundf(inner_margin);
    int bar_y = (int)lroundf(inner_margin);
    int bar_w = (int)lroundf(bar_width);
    int bar_h = (int)lroundf(bar_height);
    int volume_text_x = (int)lroundf((hud_width - volume_text_width) * 0.5f);
    int volume_text_y_int = (int)lroundf(volume_text_y - hud_y);
    int speed_text_x = (int)lroundf((hud_width - speed_text_width) * 0.5f);
    int speed_text_y_int = (int)lroundf(speed_text_y - hud_y);

    uint8_t bg_alpha = (uint8_t)lroundf(hud_alpha * 180.0f);
    hud_fill_rect(st->hud_pixels, hud_w, hud_h, 0, 0, hud_w, hud_h, 0, 0, 0, bg_alpha);

    uint8_t bar_bg_alpha = (uint8_t)lroundf(hud_alpha * 120.0f);
    hud_fill_rect(st->hud_pixels, hud_w, hud_h, bar_x, bar_y, bar_w, bar_h,
                  255, 255, 255, bar_bg_alpha);

    int fill_w = (int)lroundf(bar_w * volume_ratio);
    uint8_t fill_alpha = (uint8_t)lroundf(hud_alpha * 230.0f);
    if (fill_w > 0) {
        if (g_volume_muted) {
            hud_fill_rect(st->hud_pixels, hud_w, hud_h, bar_x, bar_y, fill_w, bar_h,
                          255, 90, 90, fill_alpha);
        } else {
            hud_fill_rect(st->hud_pixels, hud_w, hud_h, bar_x, bar_y, fill_w, bar_h,
                          80, 200, 255, fill_alpha);
        }
    }

    uint8_t text_alpha = (uint8_t)lroundf(hud_alpha * 255.0f);
    if (g_volume_muted) {
        hud_draw_text(st->hud_pixels, hud_w, hud_h, volume_text_x, volume_text_y_int,
                      volume_label, text_scale, 255, 200, 200, text_alpha);
    } else {
        hud_draw_text(st->hud_pixels, hud_w, hud_h, volume_text_x, volume_text_y_int,
                      volume_label, text_scale, 220, 240, 255, text_alpha);
    }

    int speed_bar_y_int = (int)lroundf(speed_bar_y - hud_y);
    hud_fill_rect(st->hud_pixels, hud_w, hud_h, bar_x, speed_bar_y_int, bar_w, bar_h,
                  255, 255, 255, bar_bg_alpha);

    float speed_ratio = (g_speed_multiplier - SPEED_MIN) / (SPEED_MAX - SPEED_MIN);
    speed_ratio = fmaxf(0.0f, fminf(1.0f, speed_ratio));
    int speed_fill_w = (int)lroundf(bar_w * speed_ratio);
    uint8_t speed_fill_alpha = (uint8_t)lroundf(hud_alpha * 230.0f);
    if (speed_fill_w > 0) {
        hud_fill_rect(st->hud_pixels, hud_w, hud_h, bar_x, speed_bar_y_int,
                      speed_fill_w, bar_h, 50, 220, 150, speed_fill_alpha);
    }

    float normal_speed_ratio = (1.0f - SPEED_MIN) / (SPEED_MAX - SPEED_MIN);
    normal_speed_ratio = fmaxf(0.0f, fminf(1.0f, normal_speed_ratio));
    int crossbar_x = bar_x + (int)lroundf(bar_w * normal_speed_ratio);
    uint8_t crossbar_alpha = (uint8_t)lroundf(hud_alpha * 0.9f * 255.0f);
    hud_fill_rect(st->hud_pixels, hud_w, hud_h, crossbar_x, speed_bar_y_int, 2, bar_h,
                  255, 255, 255, crossbar_alpha);

    hud_draw_text(st->hud_pixels, hud_w, hud_h, speed_text_x, speed_text_y_int,
                  speed_label, text_scale, 210, 255, 210, text_alpha);

    if (!st->gl_hud_texture) {
        glGenTextures(1, &st->gl_hud_texture);
    }
    glBindTexture(GL_TEXTURE_2D, st->gl_hud_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    if (st->gl_hud_w != hud_w || st->gl_hud_h != hud_h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, hud_w, hud_h, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, st->hud_pixels);
        st->gl_hud_w = hud_w;
        st->gl_hud_h = hud_h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, hud_w, hud_h, GL_RGBA,
                        GL_UNSIGNED_BYTE, st->hud_pixels);
    }

    st->hud_x = hud_x;
    st->hud_y = hud_y;
    return true;
}

static void freerange_gl_draw_hud(FreedomState *st) {
    if (!st || !st->gl_hud_texture || st->gl_hud_w <= 0 || st->gl_hud_h <= 0) {
        return;
    }

    float x0 = (st->hud_x / (float)st->width) * 2.0f - 1.0f;
    float x1 = ((st->hud_x + st->gl_hud_w) / (float)st->width) * 2.0f - 1.0f;
    float y0 = 1.0f - (st->hud_y / (float)st->height) * 2.0f;
    float y1 = 1.0f - ((st->hud_y + st->gl_hud_h) / (float)st->height) * 2.0f;

    GLfloat verts[] = {
        x0, y1, 0.0f, 1.0f,
        x1, y1, 1.0f, 1.0f,
        x0, y0, 0.0f, 0.0f,
        x1, y0, 1.0f, 0.0f
    };

    glUseProgram(st->gl_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, st->gl_hud_texture);
    glVertexAttribPointer(st->gl_pos_loc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), verts);
    glVertexAttribPointer(st->gl_uv_loc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), verts + 2);
    glEnableVertexAttribArray(st->gl_pos_loc);
    glEnableVertexAttribArray(st->gl_uv_loc);
    glUniform1f(st->gl_alpha_loc, 1.0f);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void freerange_pump_events(struct wl_display *display, int timeout_ms) {
    if (!display) {
        return;
    }

    while (wl_display_prepare_read(display) != 0) {
        wl_display_dispatch_pending(display);
    }

    wl_display_flush(display);

    struct pollfd pfd = {
        .fd = wl_display_get_fd(display),
        .events = POLLIN,
        .revents = 0
    };

    int poll_result = poll(&pfd, 1, timeout_ms);
    if (poll_result <= 0) {
        wl_display_cancel_read(display);
    } else {
        wl_display_read_events(display);
    }

    wl_display_dispatch_pending(display);
}

static void freerange_update_input_region(struct wl_compositor *compositor,
                                        struct wl_surface *surface,
                                        int width, int height,
                                        float ball_x, float ball_y,
                                        float ball_diameter, bool ghost_mode) {
    if (!compositor || !surface || width <= 0 || height <= 0 || ball_diameter <= 0.0f) {
        return;
    }

    struct wl_region *region = wl_compositor_create_region(compositor);
    if (!region) {
        return;
    }

    if (g_menu && ringmenu_is_open(g_menu)) {
        wl_region_add(region, 0, 0, width, height);
    } else if (ghost_mode) {
        wl_region_add(region, width - GHOST_ICON_SIZE - GHOST_ICON_MARGIN, GHOST_ICON_MARGIN, GHOST_ICON_SIZE, GHOST_ICON_SIZE);
    } else {
        float ball_center_x = ball_x + ball_diameter * 0.5f;
        float ball_center_y = (ball_y * (float)height) + ball_diameter * 0.5f;
    float radius = ball_diameter * 0.5f;
    int step = 2;
    int y_start = (int)floorf(ball_center_y - radius);
    int y_end = (int)ceilf(ball_center_y + radius);

    if (y_start < 0) y_start = 0;
    if (y_end > height) y_end = height;

    for (int y = y_start; y < y_end; y += step) {
        float dy = ((float)y + 0.5f) - ball_center_y;
        float inside = radius * radius - dy * dy;
        if (inside <= 0.0f) {
            continue;
        }
        float dx = sqrtf(inside);
        int x = (int)floorf(ball_center_x - dx);
        int w = (int)ceilf(dx * 2.0f);
        int h = step;
        if (x < 0) {
            w += x;
            x = 0;
        }
        if (x + w > width) {
            w = width - x;
        }
        if (w > 0) {
            wl_region_add(region, x, y, w, h);
        }
        }
    }

    wl_surface_set_input_region(surface, region);
    wl_region_destroy(region);
}

static bool freerange_generate_frames(FreedomFrameSet *out_frames, int frame_count, float angle_period) {
    if (!out_frames || frame_count <= 0) {
        return false;
    }

    compute_axis_vectors();

    const int sphere_texture_size = BALL_W;
    const float canonical_total_prop = 1.0f;
    const float canonical_ball_diameter = 124.0f * canonical_total_prop;
    const float canonical_ball_radius = canonical_ball_diameter * 0.5f;
    const float shadow_offset_x_screen = canonical_total_prop * 8.0f * 0.6f;
    const float shadow_offset_y_screen = canonical_total_prop * 8.0f * 1.0f;
    const float shadow_size_screen = canonical_ball_diameter * 1.1f + 20.0f * canonical_total_prop;

    const float scale_to_texture = sphere_texture_size / canonical_ball_diameter;
    float desired_shadow_width_pixels = shadow_size_screen * scale_to_texture;
    int desired_shadow_size = (int)ceilf(desired_shadow_width_pixels);

    if (!ensure_sphere_pixel_cache(sphere_texture_size)) {
        fprintf(stderr, "Failed to build sphere pixel cache\n");
        return false;
    }

    int shadow_w = 0, shadow_h = 0;
    uint8_t *shadow_pixels = generate_shadow_pixels(desired_shadow_size, &shadow_w, &shadow_h);
    if (!shadow_pixels) {
        fprintf(stderr, "Failed to generate shadow texture\n");
        return false;
    }

    const float shadow_x_screen = (canonical_ball_radius + shadow_offset_x_screen) - shadow_size_screen * 0.5f;
    const float shadow_y_screen = (canonical_ball_radius + shadow_offset_y_screen) - shadow_size_screen * 0.5f;
    const float shadow_x_tex = shadow_x_screen * scale_to_texture;
    const float shadow_y_tex = shadow_y_screen * scale_to_texture;

    const float min_x = fminf(0.0f, shadow_x_tex);
    const float min_y = fminf(0.0f, shadow_y_tex);
    const float max_x = fmaxf((float)sphere_texture_size, shadow_x_tex + shadow_w);
    const float max_y = fmaxf((float)sphere_texture_size, shadow_y_tex + shadow_h);

    g_frame_texture_w = (int)ceilf(max_x - min_x);
    g_frame_texture_h = (int)ceilf(max_y - min_y);
    g_ball_texture_offset_x = (int)lroundf(-min_x);
    g_ball_texture_offset_y = (int)lroundf(-min_y);

    int shadow_offset_x = (int)lroundf(shadow_x_tex + g_ball_texture_offset_x);
    int shadow_offset_y = (int)lroundf(shadow_y_tex + g_ball_texture_offset_y);

    size_t frame_size = (size_t)g_frame_texture_w * (size_t)g_frame_texture_h * 4;
    size_t total_frame_bytes = frame_size * (size_t)frame_count;
    uint8_t *frame_buffer = NULL;
    if (posix_memalign((void**)&frame_buffer, 32, total_frame_bytes) != 0) {
        fprintf(stderr, "Failed to allocate frame pixel buffer\n");
        free(shadow_pixels);
        return false;
    }

    BallFrameJobContext job_ctx = {
        .frame_count = frame_count,
        .angle_period = angle_period,
        .sphere_texture_size = sphere_texture_size,
        .shadow_pixels = shadow_pixels,
        .shadow_w = shadow_w,
        .shadow_h = shadow_h,
        .composite_w = g_frame_texture_w,
        .composite_h = g_frame_texture_h,
        .ball_offset_x = g_ball_texture_offset_x,
        .ball_offset_y = g_ball_texture_offset_y,
        .shadow_offset_x = shadow_offset_x,
        .shadow_offset_y = shadow_offset_y,
        .frame_buffer = frame_buffer,
        .frame_size = frame_size
    };

    poingo_atomic_set(&job_ctx.next_index, 0);
    poingo_atomic_set(&job_ctx.failure, 0);
    poingo_atomic_set(&job_ctx.completed, 0);

    int thread_count = poingo_cpu_count();
    if (thread_count < 1) {
        thread_count = 1;
    }
    if (thread_count > frame_count) {
        thread_count = frame_count;
    }

    pthread_t **threads = calloc((size_t)thread_count, sizeof(pthread_t *));
    if (!threads) {
        fprintf(stderr, "Failed to allocate frame threads\n");
        free(frame_buffer);
        free(shadow_pixels);
        return false;
    }

    for (int t = 0; t < thread_count; t++) {
        threads[t] = poingo_thread_create(generate_ball_frame_worker, "ball_frame_worker", &job_ctx);
        if (!threads[t]) {
            poingo_atomic_set(&job_ctx.failure, 1);
            thread_count = t;
            break;
        }
    }

    for (int t = 0; t < thread_count; t++) {
        if (threads[t]) {
            poingo_thread_wait(threads[t], NULL);
        }
    }

    bool failed = poingo_atomic_get(&job_ctx.failure) != 0 ||
                  poingo_atomic_get(&job_ctx.completed) != frame_count;

    free(threads);
    free(shadow_pixels);

    if (failed) {
        fprintf(stderr, "Ball frame generation failed\n");
        free(frame_buffer);
        return false;
    }

    out_frames->frames = frame_buffer;
    out_frames->frame_size = frame_size;
    out_frames->frame_count = frame_count;
    out_frames->frame_w = g_frame_texture_w;
    out_frames->frame_h = g_frame_texture_h;
    return true;
}

static bool freerange_prepare_blank_frames(FreedomFrameSet *out_frames, int frame_count) {
    if (!out_frames || frame_count <= 0) {
        return false;
    }

    compute_axis_vectors();

    const int sphere_texture_size = BALL_W;
    const float canonical_total_prop = 1.0f;
    const float canonical_ball_diameter = 124.0f * canonical_total_prop;
    const float canonical_ball_radius = canonical_ball_diameter * 0.5f;
    const float shadow_offset_x_screen = canonical_total_prop * 8.0f * 0.6f;
    const float shadow_offset_y_screen = canonical_total_prop * 8.0f * 1.0f;
    const float shadow_size_screen = canonical_ball_diameter * 1.1f + 20.0f * canonical_total_prop;

    const float scale_to_texture = sphere_texture_size / canonical_ball_diameter;
    float desired_shadow_width_pixels = shadow_size_screen * scale_to_texture;
    int desired_shadow_size = (int)ceilf(desired_shadow_width_pixels);

    if (!ensure_sphere_pixel_cache(sphere_texture_size)) {
        fprintf(stderr, "Failed to build sphere pixel cache\n");
        return false;
    }

    int shadow_w = 0, shadow_h = 0;
    uint8_t *shadow_pixels = generate_shadow_pixels(desired_shadow_size, &shadow_w, &shadow_h);
    if (!shadow_pixels) {
        fprintf(stderr, "Failed to generate shadow texture\n");
        return false;
    }

    const float shadow_x_screen = (canonical_ball_radius + shadow_offset_x_screen) - shadow_size_screen * 0.5f;
    const float shadow_y_screen = (canonical_ball_radius + shadow_offset_y_screen) - shadow_size_screen * 0.5f;
    const float shadow_x_tex = shadow_x_screen * scale_to_texture;
    const float shadow_y_tex = shadow_y_screen * scale_to_texture;

    const float min_x = fminf(0.0f, shadow_x_tex);
    const float min_y = fminf(0.0f, shadow_y_tex);
    const float max_x = fmaxf((float)sphere_texture_size, shadow_x_tex + shadow_w);
    const float max_y = fmaxf((float)sphere_texture_size, shadow_y_tex + shadow_h);

    g_frame_texture_w = (int)ceilf(max_x - min_x);
    g_frame_texture_h = (int)ceilf(max_y - min_y);
    g_ball_texture_offset_x = (int)lroundf(-min_x);
    g_ball_texture_offset_y = (int)lroundf(-min_y);

    free(shadow_pixels);

    size_t frame_size = (size_t)g_frame_texture_w * (size_t)g_frame_texture_h * 4;
    size_t total_frame_bytes = frame_size * (size_t)frame_count;
    uint8_t *frame_buffer = NULL;
    if (posix_memalign((void**)&frame_buffer, 32, total_frame_bytes) != 0) {
        fprintf(stderr, "Failed to allocate blank frame buffer\n");
        return false;
    }
    memset(frame_buffer, 0, total_frame_bytes);

    out_frames->frames = frame_buffer;
    out_frames->frame_size = frame_size;
    out_frames->frame_count = frame_count;
    out_frames->frame_w = g_frame_texture_w;
    out_frames->frame_h = g_frame_texture_h;
    return true;
}

static void freerange_destroy_frames(FreedomFrameSet *frames) {
    if (!frames) {
        return;
    }
    free(frames->frames);
    frames->frames = NULL;
    frames->frame_size = 0;
    frames->frame_count = 0;
    frames->frame_w = 0;
    frames->frame_h = 0;
}

static void freerange_wm_base_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener freerange_wm_base_listener = {
    .ping = freerange_wm_base_ping,
};

static void freerange_toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                                       int32_t width, int32_t height, struct wl_array *states) {
    (void)toplevel;
    (void)states;
    FreedomState *st = data;
    if (!st) {
        return;
    }
    if (width > 0 && height > 0) {
        st->pending_width = width;
        st->pending_height = height;
    }
}

static void freerange_toplevel_close(void *data, struct xdg_toplevel *toplevel) {
    (void)toplevel;
    FreedomState *st = data;
    if (st) {
        /* Treat compositor/taskbar close exactly like the in-app QUIT action. */
        freerange_request_graceful_shutdown(st);
    }
}

static const struct xdg_toplevel_listener freerange_toplevel_listener = {
    .configure = freerange_toplevel_configure,
    .close = freerange_toplevel_close,
};

static void freerange_xdg_surface_configure(void *data, struct xdg_surface *surface, uint32_t serial) {
    FreedomState *st = data;
    if (!st) {
        return;
    }
    xdg_surface_ack_configure(surface, serial);
    st->configured = true;
    if (st->pending_width > 0 && st->pending_height > 0) {
        st->width = st->pending_width;
        st->height = st->pending_height;
        st->pending_width = 0;
        st->pending_height = 0;
        st->resize_pending = true;
    }
}

static const struct xdg_surface_listener freerange_xdg_surface_listener = {
    .configure = freerange_xdg_surface_configure,
};

static void freerange_output_mode(void *data, struct wl_output *output,
                                uint32_t flags, int width, int height, int refresh) {
    (void)output;
    FreedomState *st = data;
    if (!st) {
        return;
    }
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        st->width = width;
        st->height = height;
        st->refresh_mhz = refresh;
    }
}

static void freerange_output_geometry(void *data, struct wl_output *output,
                                    int32_t x, int32_t y,
                                    int32_t phys_width, int32_t phys_height,
                                    int32_t subpixel,
                                    const char *make, const char *model,
                                    int32_t transform) {
    (void)data;
    (void)output;
    (void)x;
    (void)y;
    (void)phys_width;
    (void)phys_height;
    (void)subpixel;
    (void)make;
    (void)model;
    (void)transform;
}

static void freerange_output_done(void *data, struct wl_output *output) {
    (void)data;
    (void)output;
}

static void freerange_output_scale(void *data, struct wl_output *output, int32_t factor) {
    (void)data;
    (void)output;
    (void)factor;
}

static const struct wl_output_listener freerange_output_listener = {
    .geometry = freerange_output_geometry,
    .mode = freerange_output_mode,
    .done = freerange_output_done,
    .scale = freerange_output_scale,
};

static void freerange_pointer_enter(void *data, struct wl_pointer *pointer,
                                  uint32_t serial, struct wl_surface *surface,
                                  wl_fixed_t surface_x, wl_fixed_t surface_y) {
    (void)surface;
    FreedomState *st = data;
    if (!st) {
        return;
    }
    st->pointer_x = wl_fixed_to_int(surface_x);
    st->pointer_y = wl_fixed_to_int(surface_y);
    ball_cursor_set(pointer, serial);
}

static void freerange_pointer_leave(void *data, struct wl_pointer *pointer,
                                  uint32_t serial, struct wl_surface *surface) {
    (void)data;
    (void)pointer;
    (void)serial;
    (void)surface;
}

static void freerange_pointer_motion(void *data, struct wl_pointer *pointer,
                                   uint32_t time, wl_fixed_t surface_x, wl_fixed_t surface_y) {
    (void)pointer;
    (void)time;
    FreedomState *st = data;
    if (!st) {
        return;
    }
    int x = wl_fixed_to_int(surface_x);
    int y = wl_fixed_to_int(surface_y);
    st->pointer_x = x;
    st->pointer_y = y;

    if (g_menu && ringmenu_is_open(g_menu)) {
        ringmenu_motion(g_menu, x, y);

        int raw = ringmenu_field_slot(g_menu, x, y);
        int slot = -1;
        if (raw >= 0) {
            if (g_picker_locked) {
                slot = g_picker_slot;
            } else {
                if (raw < 2) slot = raw;
                g_picker_locked = true;
            }
        } else {
            g_picker_locked = false;
        }

        if (slot != g_picker_slot) {
            if (g_picker_slot >= 0) {
                poingo_menu_set_drop(g_picker_slot, g_picker_original);
            }
            g_picker_slot = slot;
            if (slot >= 0) {
                const uint8_t *cur = (slot == 0) ? g_color_light_rgb
                                                 : g_color_dark_rgb;
                memcpy(g_picker_original, cur, 3);
                memcpy(g_picker_color, cur, 3);
            }
        }

        if (slot >= 0) {
            uint8_t pr, pg, pb;
            if (ringmenu_field_color(g_menu, slot, x, y, &pr, &pg, &pb)) {
                g_picker_color[0] = pr;
                g_picker_color[1] = pg;
                g_picker_color[2] = pb;
                poingo_menu_set_drop(slot, g_picker_color);
            }
        }
        return;
    }

    uint32_t now = poingo_ticks_ms();
    st->mouse_history[st->mouse_history_index].x = x;
    st->mouse_history[st->mouse_history_index].y = y;
    st->mouse_history[st->mouse_history_index].time = now;
    st->mouse_history_index = (st->mouse_history_index + 1) % FREEDOM_MOUSE_HISTORY_SIZE;

	    if (st->pointer_down && st->ball_grabbed && st->height > 0) {
	        float desired_ball_x = (float)x - (st->grab_u * st->ball_diameter);
	        float desired_ball_y = ((float)y - (st->grab_v * st->ball_diameter)) / (float)st->height;

        float border_inset = 0.0f;
        float border_inset_norm = 0.0f;
        float ball_diameter = st->ball_diameter;
        float ball_diameter_norm = st->ball_diameter_norm;

        st->ball_x = fmaxf(border_inset, fminf(desired_ball_x,
                         (float)st->width - ball_diameter - border_inset));
        st->ball_y = fmaxf(border_inset_norm, fminf(desired_ball_y,
                         1.0f - ball_diameter_norm - border_inset_norm));

        st->slingshot_pull_x = desired_ball_x - st->ball_x;
        st->slingshot_pull_y = desired_ball_y - st->ball_y;
    }
}

static void freerange_pointer_button(void *data, struct wl_pointer *pointer,
                                   uint32_t serial, uint32_t time, uint32_t button,
                                   uint32_t state) {
    (void)pointer;
    (void)serial;
    (void)time;
    FreedomState *st = data;
    if (!st) {
        return;
    }
    if (g_menu && ringmenu_is_open(g_menu)) {
        int rbtn = -1;
        if (button == BTN_LEFT) rbtn = RINGMENU_BTN_LEFT;
        else if (button == BTN_RIGHT) rbtn = RINGMENU_BTN_RIGHT;
        else if (button == BTN_MIDDLE) rbtn = RINGMENU_BTN_MIDDLE;

        if (rbtn >= 0) {
            bool pressed = (state == WL_POINTER_BUTTON_STATE_PRESSED);

            if (!pressed && g_picker_slot >= 0 &&
                (rbtn == RINGMENU_BTN_LEFT || rbtn == RINGMENU_BTN_RIGHT)) {
                int slot = g_picker_slot;
                g_picker_slot = -1;
                g_picker_locked = false;
                ringmenu_button(g_menu, rbtn, pressed); 
                uint8_t light_rgb[3], dark_rgb[3];
                memcpy(light_rgb, g_color_light_rgb, 3);
                memcpy(dark_rgb, g_color_dark_rgb, 3);
                if (slot == 0) memcpy(light_rgb, g_picker_color, 3);
                else memcpy(dark_rgb, g_picker_color, 3);
                freerange_color_set_and_regen(st, light_rgb, dark_rgb);
                poingo_menu_sync_drops();
                return;
            }

            int result = ringmenu_button(g_menu, rbtn, pressed);
            if (result >= 0 && g_picker_slot >= 0) {
                poingo_menu_set_drop(g_picker_slot, g_picker_original);
                g_picker_slot = -1;
                g_picker_locked = false;
            }
            if (result > 0) {
                if (result == POINGO_MENU_NOSTALGIA) {
                    const uint8_t light_rgb[3] = { 255, 255, 255 };
                    const uint8_t dark_rgb[3] = { 255, 0, 0 };
                    set_a_mode_enabled(true);
                    freerange_color_set_and_regen(st, light_rgb, dark_rgb);
                } else if (result == POINGO_MENU_POINGO) {
                    const uint8_t light_rgb[3] = { COLOR_LIGHT_R, COLOR_LIGHT_G, COLOR_LIGHT_B };
                    const uint8_t dark_rgb[3] = { COLOR_DARK_R, COLOR_DARK_G, COLOR_DARK_B };
                    set_a_mode_enabled(false);
                    freerange_color_set_and_regen(st, light_rgb, dark_rgb);
                } else if (result == POINGO_MENU_NEWCOLOR) {
                    freerange_color_randomize_and_regen(st);
                } else if (result == POINGO_MENU_MUTE) {
                    toggle_master_mute();
                } else if (result == POINGO_MENU_GHOST) {
                    st->ghost_mode = true;
                } else if (result == POINGO_MENU_QUIT) {
                    freerange_request_graceful_shutdown(st);
                }
                poingo_menu_sync_drops();
            }
        }
        return;
    }

    if (state == WL_POINTER_BUTTON_STATE_PRESSED && button == BTN_RIGHT) {
        if (g_menu) {
            g_picker_slot = -1;
            g_picker_locked = false;
            poingo_menu_sync_drops();
            ringmenu_set_led(g_menu, POINGO_MENU_MUTE - 1,
                             g_mixer.muted ? RINGMENU_LED_ON : RINGMENU_LED_OFF);
            ringmenu_set_led(g_menu, POINGO_MENU_GHOST - 1,
                             st->ghost_mode ? RINGMENU_LED_ON : RINGMENU_LED_OFF);
            ringmenu_open(g_menu, st->pointer_x, st->pointer_y, st->width, st->height);
        }
        return;
    }

    if (st->ghost_mode) {
        if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
            st->ghost_mode = false;
            g_ghost_mute = false;
        }
        return;
    }

    if (button == BTN_MIDDLE) {
        return;
    }

    if (button != BTN_LEFT) {
        return;
    }

    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        st->pointer_down = true;

	        if (is_mouse_over_ball(st->pointer_x, st->pointer_y,
	                               st->ball_x, st->ball_y,
	                               st->width, st->height)) {
	            st->ball_grabbed = true;
	            interaction_begin_ball_drag(st->pointer_x, st->pointer_y);
	            if (st->ball_diameter > 0.0f) {
	                float ball_y_pixels = st->ball_y * (float)st->height;
	                st->grab_u = ((float)st->pointer_x - st->ball_x) / st->ball_diameter;
	                st->grab_v = ((float)st->pointer_y - ball_y_pixels) / st->ball_diameter;
	                st->grab_u = fmaxf(0.0f, fminf(1.0f, st->grab_u));
	                st->grab_v = fmaxf(0.0f, fminf(1.0f, st->grab_v));
	            } else {
	                st->grab_u = 0.5f;
	                st->grab_v = 0.5f;
	            }
	            st->slingshot_pull_x = 0.0f;
	            st->slingshot_pull_y = 0.0f;
	        }
	    } else if (state == WL_POINTER_BUTTON_STATE_RELEASED) {
        st->pointer_down = false;

        if (st->ball_grabbed) {
            st->ball_grabbed = false;
            interaction_end_ball_drag();

            float slingshot_threshold_x = 20.0f;
            float slingshot_threshold_y = 20.0f / (float)st->height;
            bool has_slingshot_x = fabsf(st->slingshot_pull_x) > slingshot_threshold_x;
            bool has_slingshot_y = fabsf(st->slingshot_pull_y) > slingshot_threshold_y;

            if (has_slingshot_x || has_slingshot_y) {
                float slingshot_scale = 0.3f * 3.0f;
                if (has_slingshot_x) {
                    st->ball_vx = fabsf(st->slingshot_pull_x * slingshot_scale);
                    st->ball_vx_direction = (st->slingshot_pull_x > 0.0f) ? -1 : 1;
                } else {
                    st->ball_vx = 0.0f;
                }
                if (has_slingshot_y) {
                    st->ball_vy = -(st->slingshot_pull_y * slingshot_scale);
                } else {
                    st->ball_vy = 0.0f;
                }

                float max_vx = (float)st->width * 0.4f;
                float max_vy = 0.4f;
                if (st->ball_vx > max_vx) st->ball_vx = max_vx;
                if (fabsf(st->ball_vy) > max_vy) {
                    st->ball_vy = (st->ball_vy > 0.0f) ? max_vy : -max_vy;
                }
            } else {
                uint32_t current_time = poingo_ticks_ms();
                uint32_t flick_time_window = 100;
                int oldest_valid_index = -1;
                uint32_t oldest_time = current_time;
                for (int i = 0; i < FREEDOM_MOUSE_HISTORY_SIZE; i++) {
                    if (st->mouse_history[i].time > 0 &&
                        (current_time - st->mouse_history[i].time) <= flick_time_window &&
                        st->mouse_history[i].time < oldest_time) {
                        oldest_time = st->mouse_history[i].time;
                        oldest_valid_index = i;
                    }
                }
                if (oldest_valid_index >= 0) {
                    float dx = (float)st->pointer_x - (float)st->mouse_history[oldest_valid_index].x;
                    float dy = (float)st->pointer_y - (float)st->mouse_history[oldest_valid_index].y;
                    float dt = (float)(current_time - st->mouse_history[oldest_valid_index].time);
                    if (dt > 0.001f) {
                        float vx_pixels_per_ms = dx / dt;
                        float vy_pixels_per_ms = dy / dt;
                        float total_distance = sqrtf(dx * dx + dy * dy);
                        float speed_pixels_per_ms = total_distance / dt;
                        if (speed_pixels_per_ms > 1.0f) {
                            float ms_to_frame_60fps = 16.67f;
                            float scale_factor_x = 2.0f * ms_to_frame_60fps;
                            float scale_factor_y = 1.0f * ms_to_frame_60fps;

                            st->ball_vx = fabsf(vx_pixels_per_ms * scale_factor_x);
                            st->ball_vx_direction = (vx_pixels_per_ms >= 0.0f) ? 1 : -1;
                            st->ball_vy = (vy_pixels_per_ms * scale_factor_y) / (float)st->height;

                            float max_vx = (float)st->width * 0.4f;
                            float max_vy = 0.4f;
                            if (st->ball_vx > max_vx) st->ball_vx = max_vx;
                            if (fabsf(st->ball_vy) > max_vy) {
                                st->ball_vy = (st->ball_vy > 0.0f) ? max_vy : -max_vy;
                            }
                        } else {
                            st->ball_vx = 0.0f;
                            st->ball_vy = 0.0f;
                        }
                    } else {
                        st->ball_vx = 0.0f;
                        st->ball_vy = 0.0f;
                    }
                } else {
                    st->ball_vx = 0.0f;
                    st->ball_vy = 0.0f;
                }
            }

            st->slingshot_pull_x = 0.0f;
            st->slingshot_pull_y = 0.0f;
        }
    }
}

static void freerange_adjust_ball_scale(FreedomState *st, int direction) {
    if (!st || !st->ball_grabbed || direction == 0) {
        return;
    }

    g_freerange_ball_scale += (float)direction * FREEDOM_BALL_SCALE_STEP;
    g_freerange_ball_scale = clamp_freerange_ball_scale(g_freerange_ball_scale);

    if (st->width > 0 && st->height > 0) {
        float prop_x = (float)st->width / CANVAS_WIDTH;
        float prop_y = (float)st->height / CANVAS_HEIGHT;
        float total_prop = fminf(prop_x, prop_y);
        float ball_diameter = 124.0f * total_prop * g_freerange_ball_scale;
        st->ball_diameter = ball_diameter;
        st->ball_diameter_norm = ball_diameter / (float)st->height;

        float border_inset = 0.0f;
        float border_inset_norm = 0.0f;
        float desired_ball_x = (float)st->pointer_x - (st->grab_u * ball_diameter);
        float desired_ball_y = ((float)st->pointer_y - (st->grab_v * ball_diameter)) / (float)st->height;

        st->ball_x = fmaxf(border_inset, fminf(desired_ball_x,
                         (float)st->width - ball_diameter - border_inset));
        st->ball_y = fmaxf(border_inset_norm, fminf(desired_ball_y,
                         1.0f - st->ball_diameter_norm - border_inset_norm));
        st->slingshot_pull_x = desired_ball_x - st->ball_x;
        st->slingshot_pull_y = desired_ball_y - st->ball_y;

        float rescaled_for_sound = remap_normalized_scale_to_sound(total_prop * g_freerange_ball_scale);
        mark_sounds_dirty(rescaled_for_sound);
    }
}

static void freerange_pointer_axis(void *data, struct wl_pointer *pointer,
                                 uint32_t time, uint32_t axis, wl_fixed_t value) {
    (void)pointer;
    (void)time;
    FreedomState *st = data;
    if (!st || !st->ball_grabbed) {
        return;
    }
    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
        return;
    }
    double delta = wl_fixed_to_double(value);
    if (delta == 0.0) {
        return;
    }
    freerange_adjust_ball_scale(st, delta < 0.0 ? 1 : -1);
	}

static const struct wl_pointer_listener freerange_pointer_listener = {
    .enter = freerange_pointer_enter,
    .leave = freerange_pointer_leave,
    .motion = freerange_pointer_motion,
    .button = freerange_pointer_button,
    .axis = freerange_pointer_axis,
};

static void freerange_keyboard_keymap(void *data, struct wl_keyboard *keyboard,
                                    uint32_t format, int fd, uint32_t size) {
    (void)data;
    (void)keyboard;
    (void)format;
    (void)size;
    if (fd >= 0) {
        close(fd);
    }
}

static void freerange_keyboard_enter(void *data, struct wl_keyboard *keyboard,
                                   uint32_t serial, struct wl_surface *surface, struct wl_array *keys) {
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)surface;
    (void)keys;
}

static void freerange_keyboard_leave(void *data, struct wl_keyboard *keyboard,
                                   uint32_t serial, struct wl_surface *surface) {
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)surface;
}

static void freerange_keyboard_key(void *data, struct wl_keyboard *keyboard,
                                 uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)time;
    FreedomState *st = data;
    if (!st) {
        return;
    }
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        if (key == KEY_Q) {
            freerange_request_graceful_shutdown(st);
        } else if (key == KEY_ESC) {
            freerange_request_graceful_shutdown(st);
        } else if (key == KEY_M) {
            toggle_master_mute();
        } else if (key == KEY_A) {
            const uint8_t light_rgb[3] = { 255, 255, 255 };
            const uint8_t dark_rgb[3] = { 255, 0, 0 };
            set_a_mode_enabled(true);
            freerange_color_set_and_regen(st, light_rgb, dark_rgb);
        } else if (key == KEY_P) {
            const uint8_t light_rgb[3] = { COLOR_LIGHT_R, COLOR_LIGHT_G, COLOR_LIGHT_B };
            const uint8_t dark_rgb[3] = { COLOR_DARK_R, COLOR_DARK_G, COLOR_DARK_B };
            set_a_mode_enabled(false);
            freerange_color_set_and_regen(st, light_rgb, dark_rgb);
        } else if (key == KEY_C) {
            freerange_color_randomize_and_regen(st);
        } else if (key == KEY_SPACE) {
            st->ghost_mode = !st->ghost_mode;
            g_ghost_mute = st->ghost_mode;
        } else if (key == KEY_LEFTBRACE) {
            freerange_adjust_ball_scale(st, -1);
        } else if (key == KEY_RIGHTBRACE) {
            freerange_adjust_ball_scale(st, 1);
        } else if (key == KEY_UP) {
            st->key_vol_up_pressed = true;
        } else if (key == KEY_DOWN) {
            st->key_vol_down_pressed = true;
        } else if (key == KEY_RIGHT) {
            st->key_speed_up_pressed = true;
        } else if (key == KEY_LEFT) {
            st->key_speed_down_pressed = true;
        }
    } else if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
        if (key == KEY_UP) {
            st->key_vol_up_pressed = false;
        } else if (key == KEY_DOWN) {
            st->key_vol_down_pressed = false;
        } else if (key == KEY_RIGHT) {
            st->key_speed_up_pressed = false;
        } else if (key == KEY_LEFT) {
            st->key_speed_down_pressed = false;
        }
    }
}

static void freerange_keyboard_modifiers(void *data, struct wl_keyboard *keyboard,
                                       uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched,
                                       uint32_t mods_locked, uint32_t group) {
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)mods_depressed;
    (void)mods_latched;
    (void)mods_locked;
    (void)group;
}

static void freerange_keyboard_repeat_info(void *data, struct wl_keyboard *keyboard,
                                         int32_t rate, int32_t delay) {
    (void)data;
    (void)keyboard;
    (void)rate;
    (void)delay;
}

static const struct wl_keyboard_listener freerange_keyboard_listener = {
    .keymap = freerange_keyboard_keymap,
    .enter = freerange_keyboard_enter,
    .leave = freerange_keyboard_leave,
    .key = freerange_keyboard_key,
    .modifiers = freerange_keyboard_modifiers,
    .repeat_info = freerange_keyboard_repeat_info,
};

static void freerange_seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities) {
    FreedomState *st = data;
    if (!st) {
        return;
    }
    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !st->pointer) {
        st->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(st->pointer, &freerange_pointer_listener, st);
    }
    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !st->keyboard) {
        st->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(st->keyboard, &freerange_keyboard_listener, st);
    }
    if (!(capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && st->keyboard) {
        wl_keyboard_destroy(st->keyboard);
        st->keyboard = NULL;
    }
}

static void freerange_seat_name(void *data, struct wl_seat *seat, const char *name) {
    (void)data;
    (void)seat;
    (void)name;
}

static const struct wl_seat_listener freerange_seat_listener = {
    .capabilities = freerange_seat_capabilities,
    .name = freerange_seat_name,
};

static void freerange_registry_global(void *data, struct wl_registry *registry,
                                    uint32_t name, const char *interface, uint32_t version) {
    FreedomState *st = data;
    if (!st) {
        return;
    }
    (void)version;

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        st->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        st->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        st->seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
        wl_seat_add_listener(st->seat, &freerange_seat_listener, st);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        st->output = wl_registry_bind(registry, name, &wl_output_interface, 2);
        wl_output_add_listener(st->output, &freerange_output_listener, st);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        st->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(st->wm_base, &freerange_wm_base_listener, st);
    }
}

static void freerange_registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener freerange_registry_listener = {
    .global = freerange_registry_global,
    .global_remove = freerange_registry_global_remove,
};

static void freerange_signal_handler(int signum) {
    (void)signum;
    g_freerange_quit_requested = 1;
}
typedef struct {
    bool     ready;
    uint32_t compositor_ms;   
    uint64_t delivery_ns;     
} FrameCbData;

static void wayland_frame_done(void *data, struct wl_callback *callback, uint32_t time) {
    struct timespec _ts;
    clock_gettime(CLOCK_MONOTONIC, &_ts);
    FrameCbData *fcb = (FrameCbData *)data;
    fcb->delivery_ns  = (uint64_t)_ts.tv_sec * 1000000000ULL + (uint64_t)_ts.tv_nsec;
    fcb->ready        = true;
    fcb->compositor_ms = time;
    wl_callback_destroy(callback);
}

static const struct wl_callback_listener wayland_frame_listener = {
    .done = wayland_frame_done
};


static int run_freerange_wayland(bool start_muted) {
    FreedomState st = {0};
    st.running = true;
    st.make_noise = true;
    st.last_total_prop = -1.0f;
    g_suppress_hud = false;
    g_freerange_ball_scale = clamp_freerange_ball_scale(g_freerange_ball_scale);

#if defined(__unix__)
    g_floor_y_normalized = 1.0f;
    g_target_peak_y = 0.0f;
#endif

#if defined(__unix__)
    g_freerange_quit_requested = 0;
    signal(SIGINT, freerange_signal_handler);
    signal(SIGTERM, freerange_signal_handler);
#endif

    static bool seeded = false;
    if (!seeded) {
        unsigned int seed = (unsigned int)time(NULL) ^ (unsigned int)getpid() ^ (unsigned int)poingo_ticks_ms();
        srandom(seed);
        seeded = true;
    }

    st.display = wl_display_connect(NULL);
    if (!st.display) {
        fprintf(stderr, "Failed to connect to Wayland display\n");
        return 1;
    }

    st.registry = wl_display_get_registry(st.display);
    if (!st.registry) {
        fprintf(stderr, "Failed to get Wayland registry\n");
        wl_display_disconnect(st.display);
        return 1;
    }

    wl_registry_add_listener(st.registry, &freerange_registry_listener, &st);
    wl_display_roundtrip(st.display);
    if (st.output) {
        wl_display_roundtrip(st.display);
    }

    if (!st.compositor || !st.wm_base) {
        fprintf(stderr, "Wayland compositor/wm_base missing\n");
        wl_display_disconnect(st.display);
        return 1;
    }

    if (st.width <= 0 || st.height <= 0) {
        st.width = 1280;
        st.height = 720;
    }

    st.egl_display = eglGetDisplay((EGLNativeDisplayType)st.display);
    if (st.egl_display == EGL_NO_DISPLAY || !eglInitialize(st.egl_display, NULL, NULL)) {
        fprintf(stderr, "Failed to initialize EGL\n");
        wl_display_disconnect(st.display);
        return 1;
    }
    eglBindAPI(EGL_OPENGL_ES_API);

    EGLint egl_attr[] = {
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_NONE
    };
    EGLConfig egl_cfg = NULL;
    EGLint egl_num = 0;
    if (!eglChooseConfig(st.egl_display, egl_attr, &egl_cfg, 1, &egl_num) || egl_num < 1) {
        fprintf(stderr, "Failed to choose EGL config\n");
        eglTerminate(st.egl_display);
        wl_display_disconnect(st.display);
        return 1;
    }

    st.egl_context = eglCreateContext(st.egl_display, egl_cfg, EGL_NO_CONTEXT,
                                      (EGLint[]){EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE});
    if (st.egl_context == EGL_NO_CONTEXT) {
        fprintf(stderr, "Failed to create EGL context\n");
        eglTerminate(st.egl_display);
        wl_display_disconnect(st.display);
        return 1;
    }

    int target_fps = 60;
    if (st.refresh_mhz > 0) {
        int refresh = st.refresh_mhz / 1000;
        if (refresh >= 15 && refresh <= 240) {
            target_fps = refresh;
        }
    }

    float angle_period = (4.0f * PI) / LON_TILES;
    int frame_count = (int)(target_fps * ROT_PERIOD + 0.5f);
    float frames_per_second = (float)target_fps;

    if (!init_audio(start_muted)) {
        eglDestroyContext(st.egl_display, st.egl_context);
        eglTerminate(st.egl_display);
        wl_display_disconnect(st.display);
        return 1;
    }
    probe_cubeb_start();

    FreedomFrameSet frames = {0};
    bool use_blank_frames = freerange_prepare_blank_frames(&frames, frame_count);
    if (use_blank_frames) {
        st.frames_ref = &frames;
        st.color_regen_angle_period = angle_period;
        if (!freerange_color_regen_prepare_assets(&st, &frames) ||
            !freerange_regen_workspace_prepare(&st, &frames)) {
            use_blank_frames = false;
        } else {
            freerange_color_regen_start(&st, &frames);
        }
    }
    if (!use_blank_frames) {
        freerange_color_regen_shutdown(&st);
        freerange_destroy_frames(&frames);
        if (!freerange_generate_frames(&frames, frame_count, angle_period)) {
            shutdown_audio();
            eglDestroyContext(st.egl_display, st.egl_context);
            eglTerminate(st.egl_display);
            wl_display_disconnect(st.display);
            return 1;
        }
        st.frames_ref = &frames;
        st.color_regen_angle_period = angle_period;
        if (!freerange_regen_workspace_prepare(&st, &frames)) {
            fprintf(stderr, "Failed to allocate regeneration workspace\n");
            freerange_destroy_frames(&frames);
            shutdown_audio();
            eglDestroyContext(st.egl_display, st.egl_context);
            eglTerminate(st.egl_display);
            wl_display_disconnect(st.display);
            return 1;
        }
    }

    /* Reserve regeneration assets even when the initial frame set was fully
     * generated. Runtime color/mode changes must never allocate. */
    if (!freerange_color_regen_prepare_assets(&st, &frames)) {
        fprintf(stderr, "Failed to allocate regeneration assets\n");
        freerange_destroy_frames(&frames);
        shutdown_audio();
        eglDestroyContext(st.egl_display, st.egl_context);
        eglTerminate(st.egl_display);
        wl_display_disconnect(st.display);
        return 1;
    }

    st.surface = wl_compositor_create_surface(st.compositor);
    if (!st.surface) {
        fprintf(stderr, "Failed to create Wayland surface\n");
        freerange_color_regen_shutdown(&st);
        freerange_destroy_frames(&frames);
        shutdown_audio();
        eglDestroyContext(st.egl_display, st.egl_context);
        eglTerminate(st.egl_display);
        wl_display_disconnect(st.display);
        return 1;
    }

    wl_surface_set_opaque_region(st.surface, NULL);

    if (!ball_cursor_create(st.shm, st.compositor)) {
        fprintf(stderr, "Poingo: no ball cursor, using the default pointer\n");
    }

    st.xdg_surface = xdg_wm_base_get_xdg_surface(st.wm_base, st.surface);
    if (!st.xdg_surface) {
        fprintf(stderr, "Failed to create xdg_surface\n");
        wl_surface_destroy(st.surface);
        freerange_color_regen_shutdown(&st);
        freerange_destroy_frames(&frames);
        shutdown_audio();
        eglDestroyContext(st.egl_display, st.egl_context);
        eglTerminate(st.egl_display);
        wl_display_disconnect(st.display);
        return 1;
    }

    xdg_surface_add_listener(st.xdg_surface, &freerange_xdg_surface_listener, &st);
    st.xdg_toplevel = xdg_surface_get_toplevel(st.xdg_surface);
    if (!st.xdg_toplevel) {
        fprintf(stderr, "Failed to create xdg_toplevel\n");
        xdg_surface_destroy(st.xdg_surface);
        wl_surface_destroy(st.surface);
        freerange_color_regen_shutdown(&st);
        freerange_destroy_frames(&frames);
        shutdown_audio();
        eglDestroyContext(st.egl_display, st.egl_context);
        eglTerminate(st.egl_display);
        wl_display_disconnect(st.display);
        return 1;
    }
    xdg_toplevel_add_listener(st.xdg_toplevel, &freerange_toplevel_listener, &st);
    xdg_toplevel_set_title(st.xdg_toplevel, "Poingo");
    xdg_toplevel_set_app_id(st.xdg_toplevel, "poingo");
    xdg_toplevel_set_maximized(st.xdg_toplevel);

    st.egl_window = wl_egl_window_create(st.surface, st.width, st.height);
    if (!st.egl_window) {
        fprintf(stderr, "Failed to create EGL window\n");
        xdg_toplevel_destroy(st.xdg_toplevel);
        xdg_surface_destroy(st.xdg_surface);
        wl_surface_destroy(st.surface);
        eglDestroyContext(st.egl_display, st.egl_context);
        eglTerminate(st.egl_display);
        freerange_color_regen_shutdown(&st);
        freerange_destroy_frames(&frames);
        shutdown_audio();
        wl_display_disconnect(st.display);
        return 1;
    }

    st.egl_surface = eglCreateWindowSurface(st.egl_display, egl_cfg,
                                            (EGLNativeWindowType)st.egl_window, NULL);
    if (st.egl_surface == EGL_NO_SURFACE) {
        fprintf(stderr, "Failed to create EGL window surface\n");
        wl_egl_window_destroy(st.egl_window);
        xdg_toplevel_destroy(st.xdg_toplevel);
        xdg_surface_destroy(st.xdg_surface);
        wl_surface_destroy(st.surface);
        eglDestroyContext(st.egl_display, st.egl_context);
        eglTerminate(st.egl_display);
        freerange_color_regen_shutdown(&st);
        freerange_destroy_frames(&frames);
        shutdown_audio();
        wl_display_disconnect(st.display);
        return 1;
    }

    if (!eglMakeCurrent(st.egl_display, st.egl_surface, st.egl_surface, st.egl_context)) {
        fprintf(stderr, "Failed to make EGL context current\n");
        eglDestroySurface(st.egl_display, st.egl_surface);
        wl_egl_window_destroy(st.egl_window);
        xdg_toplevel_destroy(st.xdg_toplevel);
        xdg_surface_destroy(st.xdg_surface);
        wl_surface_destroy(st.surface);
        eglDestroyContext(st.egl_display, st.egl_context);
        eglTerminate(st.egl_display);
        freerange_color_regen_shutdown(&st);
        freerange_destroy_frames(&frames);
        shutdown_audio();
        wl_display_disconnect(st.display);
        return 1;
    }
    eglSwapInterval(st.egl_display, 0);

    uint32_t *light_drop = g_color_drop_light;
    uint32_t *dark_drop = g_color_drop_dark;
    ringmenu_color_drop_into(light_drop, POINGO_MENU_DISC * POINGO_MENU_DISC,
                             g_color_light_rgb[0], g_color_light_rgb[1],
                             g_color_light_rgb[2], 0, POINGO_MENU_ITEMS,
                             POINGO_MENU_DISC);
    ringmenu_color_drop_into(dark_drop, POINGO_MENU_DISC * POINGO_MENU_DISC,
                             g_color_dark_rgb[0], g_color_dark_rgb[1],
                             g_color_dark_rgb[2], 1, POINGO_MENU_ITEMS,
                             POINGO_MENU_DISC);
    RingMenuItem menu_items[POINGO_MENU_ITEMS] = {
        { .image = light_drop, .image_w = POINGO_MENU_DISC, .image_h = POINGO_MENU_DISC },
        { .image = dark_drop, .image_w = POINGO_MENU_DISC, .image_h = POINGO_MENU_DISC },
        { .label = "NOSTALGIA" },
        { .label = "POINGO" },
        { .label = "NEW COLOR" },
        { .label = "MUTE", .led = RINGMENU_LED_OFF },
        { .label = "GHOST", .led = RINGMENU_LED_OFF },
        { .label = "QUIT" },
    };
    if (light_drop && dark_drop) {
        g_menu = ringmenu_create(menu_items, POINGO_MENU_ITEMS);
    }
    if (g_menu) {
        int menu_size = ringmenu_size(g_menu);
        g_menu_scratch_cap = (size_t)menu_size * menu_size;
        g_menu_scratch = malloc(g_menu_scratch_cap * 4);
        if (!g_menu_scratch) {
            fprintf(stderr, "Failed to allocate menu workspace\n");
            ringmenu_destroy(g_menu);
            g_menu = NULL;
        }
    }
    glGenTextures(1, &g_menu_tex);
    glBindTexture(GL_TEXTURE_2D, g_menu_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    PFNEGLSWAPBUFFERSWITHDAMAGEKHRPROC pfn_swap_damage = NULL;
    bool has_buffer_age = false;
    {
        const char *egl_exts = eglQueryString(st.egl_display, EGL_EXTENSIONS);
        if (egl_exts) {
            if (strstr(egl_exts, "EGL_KHR_swap_buffers_with_damage")) {
                pfn_swap_damage = (PFNEGLSWAPBUFFERSWITHDAMAGEKHRPROC)
                    eglGetProcAddress("eglSwapBuffersWithDamageKHR");
            } else if (strstr(egl_exts, "EGL_EXT_swap_buffers_with_damage")) {
                pfn_swap_damage = (PFNEGLSWAPBUFFERSWITHDAMAGEKHRPROC)
                    eglGetProcAddress("eglSwapBuffersWithDamageEXT");
            }
            has_buffer_age = strstr(egl_exts, "EGL_EXT_buffer_age") != NULL;
        }
        if (g_debug_mode) {
            fprintf(stderr, "[egl] swap_with_damage=%s buffer_age=%s\n",
                    pfn_swap_damage ? "yes" : "no",
                    has_buffer_age ? "yes" : "no");
        }
    }

    wl_surface_commit(st.surface);
    while (!st.configured) {
        if (wl_display_dispatch(st.display) < 0) {
            break;
        }
    }

    if (st.resize_pending) {
        wl_egl_window_resize(st.egl_window, st.width, st.height, 0, 0);
        st.resize_pending = false;
    }
    glViewport(0, 0, st.width, st.height);

    if (!freerange_gl_init(&st, &frames)) {
        fprintf(stderr, "Failed to initialize GL resources\n");
        freerange_gl_shutdown(&st);
        eglDestroySurface(st.egl_display, st.egl_surface);
        wl_egl_window_destroy(st.egl_window);
        xdg_toplevel_destroy(st.xdg_toplevel);
        xdg_surface_destroy(st.xdg_surface);
        wl_surface_destroy(st.surface);
        eglDestroyContext(st.egl_display, st.egl_context);
        eglTerminate(st.egl_display);
        freerange_color_regen_shutdown(&st);
        freerange_destroy_frames(&frames);
        shutdown_audio();
        wl_display_disconnect(st.display);
        return 1;
    }

    calculate_equilibrium_state(st.width, st.height,
                                GRAVITY, g_floor_y_normalized,
                                g_target_peak_y,
                                &st.ball_y, &st.ball_vy, &(float){0});
    st.ball_x = 100.0f;
    st.ball_vx = get_natural_vx(st.width);
    st.ball_vx_direction = 1;
    st.shutdown_pending = false;
    st.ball_cleared = false;
    st.shutdown_last_check_ticks = 0;
    st.shutdown_start_ticks = 0;

    {
        float prop_x = st.width / CANVAS_WIDTH;
        float prop_y = st.height / CANVAS_HEIGHT;
        float total_prop = fminf(prop_x, prop_y);
        float scaled_prop = total_prop * g_freerange_ball_scale;
        st.ball_diameter = 124.0f * scaled_prop;
        st.ball_diameter_norm = st.ball_diameter / (float)st.height;
        g_audio_sim_time = 0.0f;
        audio_predict_reset(g_audio_sim_time);
        build_audio_predict_buffer(&g_audio_predict,
                                   g_audio_sim_time,
                                   st.ball_x, st.ball_y,
                                   st.ball_vx, st.ball_vy,
                                   st.ball_vx_direction,
                                   st.width, st.height,
                                   0.0f, 0.0f,
                                   st.ball_diameter, st.ball_diameter_norm,
                                   1.0f / frames_per_second);
    }

    st.performance_frequency = poingo_perf_freq();
    if (st.performance_frequency == 0) {
        st.performance_frequency = 1;
    }
    st.last_counter = poingo_perf_counter();
    st.has_last_counter = true;

    const double TARGET_FRAME_SECONDS = 1.0 / (double)target_fps;
    const double SNAP_THRESHOLD = fmax(0.00075, TARGET_FRAME_SECONDS * 0.08);
    double delta_error_accum = 0.0;

    #define FREERANGE_DAMAGE_HISTORY 8
    FreerangeRect damage_hist[FREERANGE_DAMAGE_HISTORY] = {0};
    int damage_hist_depth = 0;

    float in_reg_x = -1e9f, in_reg_y = -1e9f, in_reg_d = -1e9f;
    int   in_reg_w = 0, in_reg_h = 0;
    bool  menu_open_in_reg = false;
    bool  in_reg_ghost = false;

    FrameCbData fr_cb = { .ready = true, .compositor_ms = 0 };
    uint32_t prev_compositor_ms = 0;
    bool     has_prev_compositor_ms = false;
    int obscured_frames = 0; 

    FILE   *fr_log        = NULL;
    uint64_t  fr_last       = 0;
    double  fr_mean       = 0.0, fr_M2 = 0.0, fr_max = 0.0;
    int     fr_n          = 0;
    uint64_t  fr_report_at  = poingo_perf_counter();

    FILE   *ov_log        = NULL;
    double  ov_mean       = 0.0, ov_M2 = 0.0, ov_max = 0.0;
    int     ov_n          = 0;

    FILE   *gf_log        = NULL;  
    double  gf_mean       = 0.0, gf_M2 = 0.0, gf_max = 0.0;
    int     gf_n          = 0;

    FILE    *fcb_log      = NULL;
    uint64_t fcb_prev_del = 0;                          
    uint64_t pump_enter_ns = 0;                         
    int      fcbi_n       = 0;                          
    double   fcbi_mean    = 0.0, fcbi_M2 = 0.0, fcbi_max = 0.0;
    int      pw_n         = 0;                          
    double   pw_mean      = 0.0, pw_M2  = 0.0, pw_max  = 0.0;
    int      wk_n         = 0;                          
    double   wk_mean      = 0.0, wk_M2  = 0.0, wk_max  = 0.0;
    int      cs_n         = 0;                          
    double   cs_mean      = 0.0, cs_M2  = 0.0;
    uint64_t   fcb_report_at = poingo_perf_counter();

    uint64_t  tick_start         = 0;
    double  prev_non_upload_ms  = 0.0;
    double  regen_ms_per_unit   = 0.0;
    bool    prev_regen_active   = false;
    int     regen_measure_frames = 0;
    bool    regen_rechunked      = false;

    int    drop_red          = 0;
    int    drop_yellow       = 0;
    int    normal_count      = 0;
    uint32_t drop_window_start = poingo_ticks_ms();
    double apb_ms_sum        = 0.0;
    double apb_ms_max        = 0.0;
    int    apb_count         = 0;

    if (g_debug_mode) {
        char fr_path[64];
        snprintf(fr_path, sizeof(fr_path), "/tmp/poingo_frames_%d.csv", (int)getpid());
        fr_log = fopen(fr_path, "w");
        if (fr_log) {
            fprintf(fr_log, "# interval_ms\n");
            fflush(fr_log);
        }
        char ov_path[64];
        snprintf(ov_path, sizeof(ov_path), "/tmp/poingo_overhead_%d.csv", (int)getpid());
        ov_log = fopen(ov_path, "w");
        if (ov_log) {
            fprintf(ov_log, "# overhead_ms\n");
            fflush(ov_log);
        }
        fprintf(stderr, "[debug] frame log:    %s\n", fr_path);
        fprintf(stderr, "[debug] overhead log: %s\n", ov_path);
        char gf_path[64];
        snprintf(gf_path, sizeof(gf_path), "/tmp/poingo_glfinish_%d.csv", (int)getpid());
        gf_log = fopen(gf_path, "w");
        if (gf_log) {
            fprintf(gf_log, "# glfinish_ms\n");
            fflush(gf_log);
        }
        fprintf(stderr, "[debug] glfinish log: %s\n", gf_path);
        char fcb_path[64];
        snprintf(fcb_path, sizeof(fcb_path), "/tmp/poingo_fcb_%d.csv", (int)getpid());
        fcb_log = fopen(fcb_path, "w");
        if (fcb_log) {
            fprintf(fcb_log, "delivery_ns,interval_us,poll_wait_us,wake_us,cstamp_off_us\n");
            fflush(fcb_log);
        }
        fprintf(stderr, "[debug] fcb log:      %s\n", fcb_path);
    }

    while (st.running) {
        bool was_ball_grabbed = st.ball_grabbed;
        bool miss_this_frame  = false;
        {
            struct timespec _pe;
            clock_gettime(CLOCK_MONOTONIC, &_pe);
            pump_enter_ns = (uint64_t)_pe.tv_sec * 1000000000ULL + (uint64_t)_pe.tv_nsec;
        }
        freerange_pump_events(st.display, 20);
        tick_start = poingo_perf_counter();
#if defined(__unix__)
        if (g_freerange_quit_requested) {
            st.running = false;
            break;
        }
#endif

        uint64_t current_counter = poingo_perf_counter();
        double delta_seconds = (double)(current_counter - st.last_counter) /
                               (double)st.performance_frequency;
        st.last_counter = current_counter;

        if (delta_seconds < 0.0) {
            delta_seconds = 0.0;
        } else if (delta_seconds > 0.25) {
            delta_seconds = 0.25;
        }

        double sim_delta = delta_seconds;
        double delta_diff = delta_seconds - TARGET_FRAME_SECONDS;
        if (fabs(delta_diff) < SNAP_THRESHOLD) {
            sim_delta = TARGET_FRAME_SECONDS;
            delta_error_accum += delta_diff;
        } else if (delta_seconds < TARGET_FRAME_SECONDS) {
            sim_delta = delta_seconds;
        } else {
            double clamped = SNAP_THRESHOLD;
            sim_delta = TARGET_FRAME_SECONDS + clamped;
            delta_error_accum += (delta_diff - clamped);
            double accum_cap = TARGET_FRAME_SECONDS * 2.0;
            if (delta_error_accum >  accum_cap) delta_error_accum =  accum_cap;
            if (delta_error_accum < -accum_cap) delta_error_accum = -accum_cap;
        }
        if (delta_error_accum > SNAP_THRESHOLD) {
            sim_delta += SNAP_THRESHOLD;
            delta_error_accum -= SNAP_THRESHOLD;
        } else if (delta_error_accum < -SNAP_THRESHOLD) {
            sim_delta -= SNAP_THRESHOLD;
            delta_error_accum += SNAP_THRESHOLD;
        }
        if (sim_delta < 0.0) sim_delta = 0.0;
        if (sim_delta > 0.25) sim_delta = 0.25;

        if (fr_cb.ready && has_prev_compositor_ms && fr_cb.compositor_ms != 0) {
            uint32_t comp_delta_ms = fr_cb.compositor_ms - prev_compositor_ms; 
            double comp_delta_s    = (double)comp_delta_ms * 0.001;
            if (comp_delta_s >= TARGET_FRAME_SECONDS * 1.5 &&
                comp_delta_s <= TARGET_FRAME_SECONDS * 8.0) {
                miss_this_frame = true;
                if (comp_delta_ms >= 35) drop_red++;
                else drop_yellow++;
                if (g_debug_mode) {
                    bool use_color = isatty(STDERR_FILENO);
                    const char *col = use_color
                        ? (comp_delta_ms >= 35 ? "\033[91m"   
                          : comp_delta_ms < 32 ? "\033[93m"   
                          :                      "")           
                        : "";
                    const char *rst = use_color ? "\033[0m" : "";
                    fprintf(stderr, "%s[miss] T+%.1fs  %u ms%s\n",
                            col, poingo_ticks_ms() / 1000.0, comp_delta_ms, rst);
                }
                if (sim_delta > TARGET_FRAME_SECONDS) {
                    delta_error_accum += (sim_delta - TARGET_FRAME_SECONDS);
                    sim_delta = TARGET_FRAME_SECONDS;
                    double max_accum = TARGET_FRAME_SECONDS * 2.0;
                    if (delta_error_accum > max_accum) delta_error_accum = max_accum;
                }
            }
        }

#if defined(__unix__)
        if (g_freerange_quit_requested) {
            st.running = false;
        }
        if (!st.running) {
            break;
        }

        probe_cubeb_update();
        float frame_elapsed = (float)delta_seconds;
        update_volume_hud(frame_elapsed);
        update_speed_hud(frame_elapsed);
#endif
        int vol_up_count = process_key_repeat(&g_vol_up_key, st.key_vol_up_pressed, frame_elapsed);
        int vol_down_count = process_key_repeat(&g_vol_down_key, st.key_vol_down_pressed, frame_elapsed);
        int speed_up_count = process_key_repeat(&g_speed_up_key, st.key_speed_up_pressed, frame_elapsed);
        int speed_down_count = process_key_repeat(&g_speed_down_key, st.key_speed_down_pressed, frame_elapsed);

        for (int i = 0; i < vol_up_count; i++) {
            adjust_master_volume(MASTER_VOLUME_STEP);
        }
        for (int i = 0; i < vol_down_count; i++) {
            adjust_master_volume(-MASTER_VOLUME_STEP);
        }
        for (int i = 0; i < speed_up_count; i++) {
            adjust_speed(SPEED_STEP);
        }
        for (int i = 0; i < speed_down_count; i++) {
            adjust_speed(-SPEED_STEP);
        }

        bool hud_visible = freerange_gl_update_hud(&st);

        if (was_ball_grabbed && !st.ball_grabbed) {
            audio_predict_reset(g_audio_sim_time);
        }

        if (st.resize_pending) {
            wl_egl_window_resize(st.egl_window, st.width, st.height, 0, 0);
            glViewport(0, 0, st.width, st.height);
            st.resize_pending = false;
            damage_hist_depth = 0;
        }

        float prop_x = st.width / CANVAS_WIDTH;
        float prop_y = st.height / CANVAS_HEIGHT;
        float total_prop = fminf(prop_x, prop_y);
        float scaled_prop = total_prop * g_freerange_ball_scale;
        st.ball_diameter = 124.0f * scaled_prop;
        st.ball_diameter_norm = st.ball_diameter / (float)st.height;

        if (fabsf(scaled_prop - st.last_total_prop) > 0.001f) {
            float rescaled_for_sound = remap_normalized_scale_to_sound(scaled_prop);
            mark_sounds_dirty(rescaled_for_sound);
            st.last_total_prop = scaled_prop;
        }

        float frame_advance = (float)(sim_delta * frames_per_second);
        st.phase_f = fmodf(st.phase_f + frame_advance, (float)frame_count);
        if (st.phase_f < 0.0f) {
            st.phase_f += frame_count;
        }
        st.phase_i = (int)floorf(st.phase_f);
        if (st.phase_i < 0) st.phase_i = 0;
        if (st.phase_i >= frame_count) st.phase_i = frame_count - 1;
        ball_cursor_animate(st.phase_i, frame_count);

        if (!st.ball_cleared) {
            update_ball_physics(&st.ball_x, &st.ball_y,
                                &st.ball_vx, &st.ball_vy,
                                &st.ball_vx_direction,
                                st.width, st.height,
                                0.0f, 0.0f,
                                st.ball_diameter, st.ball_diameter_norm,
                                sim_delta,
                                st.ball_grabbed, st.make_noise && !g_predictive_audio_enabled,
                                NULL, NULL, 0, 0.0f);
        }

        g_audio_sim_time += (float)sim_delta;
        if (g_predictive_audio_enabled && !st.ball_grabbed && !st.ball_cleared) {
            uint64_t apb_t0 = poingo_perf_counter();
            build_audio_predict_buffer(&g_audio_predict,
                                       g_audio_sim_time,
                                       st.ball_x, st.ball_y,
                                       st.ball_vx, st.ball_vy,
                                       st.ball_vx_direction,
                                       st.width, st.height,
                                       0.0f, 0.0f,
                                       st.ball_diameter, st.ball_diameter_norm,
                                       (float)sim_delta);
            double apb_ms = get_perf_seconds(apb_t0, poingo_perf_counter()) * 1000.0;
            apb_ms_sum += apb_ms;
            apb_count++;
            if (apb_ms > apb_ms_max) apb_ms_max = apb_ms;
            trigger_predictive_audio(&g_audio_predict, g_audio_sim_time, st.make_noise);
        }

        if (st.shutdown_pending) {
            uint32_t now_ticks = poingo_ticks_ms();
            if (now_ticks - st.shutdown_start_ticks >= AUDIO_SHUTDOWN_MAX_MS) {
                st.running = false;
            }
            if (now_ticks - st.shutdown_last_check_ticks >= AUDIO_SHUTDOWN_CHECK_MS) {
                st.shutdown_last_check_ticks = now_ticks;
                if (audio_is_perceptually_quiet()) {
                    st.running = false;
                }
            }
        }

        if (fr_cb.ready) {
            bool menu_open_for_input = g_menu && ringmenu_is_open(g_menu);
            bool ghost_changed = (st.ghost_mode != in_reg_ghost);
            if (!st.ball_cleared &&
                (st.ball_x != in_reg_x || st.ball_y != in_reg_y ||
                 st.ball_diameter != in_reg_d ||
                 st.width != in_reg_w || st.height != in_reg_h ||
                 menu_open_for_input != menu_open_in_reg || ghost_changed)) {
                freerange_update_input_region(st.compositor, st.surface,
                                            st.width, st.height,
                                            st.ball_x, st.ball_y, st.ball_diameter, st.ghost_mode);
                in_reg_x = st.ball_x;
                in_reg_y = st.ball_y;
                in_reg_d = st.ball_diameter;
                in_reg_w = st.width;
                in_reg_h = st.height;
                menu_open_in_reg = menu_open_for_input;
                in_reg_ghost = st.ghost_mode;
                if (ghost_changed) {
                    damage_hist_depth = 0;
                }
            }
            obscured_frames = 0;

            uint64_t fr_now = poingo_perf_counter();
            if (g_debug_mode && fr_last != 0) {
                double ms = (double)(fr_now - fr_last) /
                            (double)st.performance_frequency * 1000.0;
                if (fr_log) fprintf(fr_log, "%.3f\n", ms);
                fr_n++;
                double d1 = ms - fr_mean;
                fr_mean += d1 / fr_n;
                fr_M2   += d1 * (ms - fr_mean);
                if (ms > fr_max) fr_max = ms;
                double elapsed = (double)(fr_now - fr_report_at) /
                                 (double)st.performance_frequency;
                if (elapsed >= 2.0 && fr_n >= 2) {
                    double stdev = sqrt(fr_M2 / (fr_n - 1));
                    double ov_stdev = (ov_n >= 2) ? sqrt(ov_M2 / (ov_n - 1)) : 0.0;
                    double gf_stdev = (gf_n >= 2) ? sqrt(gf_M2 / (gf_n - 1)) : 0.0;
                    fprintf(stderr,
                            "[fps] %.1f fps  interval: mean=%.2fms stdev=%.2fms max=%.1fms"
                            "  overhead: mean=%.2fms stdev=%.2fms max=%.1fms"
                            "  glfinish: mean=%.2fms stdev=%.2fms max=%.1fms\n",
                            fr_n / elapsed,
                            fr_mean, stdev, fr_max,
                            ov_mean, ov_stdev, ov_max,
                            gf_mean, gf_stdev, gf_max);
                    fr_n = 0; fr_mean = 0.0; fr_M2 = 0.0; fr_max = 0.0;
                    ov_n = 0; ov_mean = 0.0; ov_M2 = 0.0; ov_max = 0.0;
                    gf_n = 0; gf_mean = 0.0; gf_M2 = 0.0; gf_max = 0.0;
                    fr_report_at = fr_now;
                }
            }
            fr_last = fr_now;

            if (g_debug_mode && fr_cb.delivery_ns != 0) {
                struct timespec _noticed;
                clock_gettime(CLOCK_MONOTONIC, &_noticed);
                uint64_t noticed_ns = (uint64_t)_noticed.tv_sec * 1000000000ULL
                                    + (uint64_t)_noticed.tv_nsec;

                double wake_us = (double)(int64_t)(noticed_ns - fr_cb.delivery_ns) / 1000.0;

                uint64_t del_ms_mod = (fr_cb.delivery_ns / 1000000ULL) & 0xFFFFFFFFULL;
                int32_t  cs_diff_ms = (int32_t)(del_ms_mod - (uint32_t)fr_cb.compositor_ms);
                double   cstamp_off_us = cs_diff_ms * 1000.0;

                double poll_wait_us = 0.0;
                if (pump_enter_ns != 0 && fr_cb.delivery_ns >= pump_enter_ns) {
                    poll_wait_us = (double)(fr_cb.delivery_ns - pump_enter_ns) / 1000.0;
                }

                if (fcb_prev_del != 0 && fr_cb.delivery_ns > fcb_prev_del) {
                    double interval_us = (double)(fr_cb.delivery_ns - fcb_prev_del) / 1000.0;
                    fcbi_n++;
                    double _d = interval_us - fcbi_mean;
                    fcbi_mean += _d / fcbi_n;
                    fcbi_M2   += _d * (interval_us - fcbi_mean);
                    if (interval_us > fcbi_max) fcbi_max = interval_us;
                    if (poll_wait_us >= 0.0 && poll_wait_us < 30000.0) {
                        pw_n++;
                        double _p = poll_wait_us - pw_mean;
                        pw_mean += _p / pw_n;
                        pw_M2   += _p * (poll_wait_us - pw_mean);
                        if (poll_wait_us > pw_max) pw_max = poll_wait_us;
                    }
                    if (wake_us >= 0.0 && wake_us < 30000.0) {
                        wk_n++;
                        double _w = wake_us - wk_mean;
                        wk_mean += _w / wk_n;
                        wk_M2   += _w * (wake_us - wk_mean);
                        if (wake_us > wk_max) wk_max = wake_us;
                    }
                    cs_n++;
                    double _c = cstamp_off_us - cs_mean;
                    cs_mean += _c / cs_n;
                    cs_M2   += _c * (cstamp_off_us - cs_mean);

                    if (fcb_log) {
                        fprintf(fcb_log, "%llu,%.1f,%.1f,%.1f,%.1f\n",
                                (unsigned long long)fr_cb.delivery_ns,
                                interval_us, poll_wait_us, wake_us, cstamp_off_us);
                    }
                }
                fcb_prev_del = fr_cb.delivery_ns;

                double fcb_elapsed = (double)(fr_now - fcb_report_at)
                                   / (double)st.performance_frequency;
                if (fcb_elapsed >= 10.0 && fcbi_n >= 10) {
                    double i_sd  = (fcbi_n > 1) ? sqrt(fcbi_M2 / (fcbi_n - 1)) : 0.0;
                    double pw_sd = (pw_n   > 1) ? sqrt(pw_M2   / (pw_n   - 1)) : 0.0;
                    double w_sd  = (wk_n   > 1) ? sqrt(wk_M2   / (wk_n   - 1)) : 0.0;
                    double c_sd  = (cs_n   > 1) ? sqrt(cs_M2   / (cs_n   - 1)) : 0.0;
                    fprintf(stderr,
                            "[fcb] interval mean=%.3fms \xc2\xb1%.3fms max=%.3fms"
                            "  poll_wait mean=%.3fms \xc2\xb1%.3fms max=%.3fms"
                            "  wake mean=%.0f\xc2\xb5s \xc2\xb1%.0f\xc2\xb5s"
                            "  cstamp off=%.0f\xc2\xb5s \xc2\xb1%.0f\xc2\xb5s\n",
                            fcbi_mean / 1000.0, i_sd / 1000.0, fcbi_max / 1000.0,
                            pw_mean / 1000.0, pw_sd / 1000.0, pw_max / 1000.0,
                            wk_mean, w_sd,
                            cs_mean, c_sd);
                    fcbi_n = 0; fcbi_mean = 0.0; fcbi_M2 = 0.0; fcbi_max = 0.0;
                    pw_n   = 0; pw_mean  = 0.0; pw_M2  = 0.0; pw_max  = 0.0;
                    wk_n   = 0; wk_mean  = 0.0; wk_M2  = 0.0; wk_max  = 0.0;
                    cs_n   = 0; cs_mean  = 0.0; cs_M2  = 0.0;
                    fcb_report_at = fr_now;
                }
            }

            if (has_prev_compositor_ms) {
            } else if (fr_cb.compositor_ms != 0) {
                prev_compositor_ms = fr_cb.compositor_ms;
                has_prev_compositor_ms = true;
            }

            struct wl_callback *callback = wl_surface_frame(st.surface);
            wl_callback_add_listener(callback, &wayland_frame_listener, &fr_cb);
            fr_cb.ready = false;

            float render_extrap_dt = (float)(TARGET_FRAME_SECONDS - sim_delta);
            float extrap_cap = (float)(SNAP_THRESHOLD * 4.0);
            if (render_extrap_dt >  extrap_cap) render_extrap_dt =  extrap_cap;
            if (render_extrap_dt < -extrap_cap) render_extrap_dt = -extrap_cap;

            if (st.color_regen_active && !prev_regen_active) {
                regen_ms_per_unit    = 0.0;
                regen_measure_frames = 0;
                regen_rechunked      = false;
            }

            double upload_budget_ms = TARGET_FRAME_SECONDS * 1000.0
                                      - prev_non_upload_ms - 2.0;
            if (upload_budget_ms > FREERANGE_REGEN_TARGET_MS)
                upload_budget_ms = FREERANGE_REGEN_TARGET_MS;

            int upload_max;
            if (regen_ms_per_unit <= 0.0) {
                upload_max = 1;  
            } else {
                upload_max = (int)(upload_budget_ms / regen_ms_per_unit);
                if (upload_max < 1) upload_max = 1;  
            }

            int units_before = (int)st.regen_units_uploaded;
            uint64_t upload_t0 = poingo_perf_counter();
            freerange_regen_upload_step(&st, &frames, upload_max, upload_budget_ms);
            double upload_actual_ms =
                get_perf_seconds(upload_t0, poingo_perf_counter()) * 1000.0;

            int units_done = (int)st.regen_units_uploaded - units_before;
            if (units_done > 0) {
                double mpu = upload_actual_ms / (double)units_done;
                regen_ms_per_unit = (regen_ms_per_unit <= 0.0)
                    ? mpu
                    : 0.2 * regen_ms_per_unit + 0.8 * mpu;
                st.regen_ms_per_block = (float)regen_ms_per_unit;
                regen_measure_frames++;
            }

            if (st.color_regen_active && !regen_rechunked && regen_measure_frames >= 2
                && st.regen_ms_per_block > 0.0f
                && (int)st.regen_units_uploaded * 4 < (int)st.color_regen_units_total) {
                int cur   = st.color_regen_block_size;
                int ideal = freerange_regen_ideal_block_size(st.regen_ms_per_block,
                                                             frames.frame_count, frames.frame_h);
                if (ideal >= cur * 2 || ideal * 2 <= cur) {
                    regen_rechunked = true;
                    freerange_color_regen_start(&st, &frames);
                }
            }
            prev_regen_active = st.color_regen_active;
            if (st.color_regen_mode == BALL_REGEN_CLEAR && !st.color_regen_active && !st.ball_cleared) {
                st.ball_cleared = true;
                st.shutdown_pending = true;
                st.shutdown_start_ticks = poingo_ticks_ms();
                st.shutdown_last_check_ticks = st.shutdown_start_ticks;
            }

            bool menu_open = g_menu && ringmenu_is_open(g_menu);
            bool menu_active = menu_open || g_menu_was_open;

            FreerangeRect cur_rect = {0, 0, 0, 0};
            if (menu_active) {
                cur_rect = (FreerangeRect){0, 0, st.width, st.height};
            } else {
                float bx, by, bw, bh;
                if (freerange_ball_dest_quad(&st, &frames, render_extrap_dt,
                                             &bx, &by, &bw, &bh)) {
                    FreerangeRect br = { (int)floorf(bx) - 2, (int)floorf(by) - 2,
                                         (int)ceilf(bw) + 4, (int)ceilf(bh) + 4 };
                    freerange_rect_union(&cur_rect, &br);
                }
                if (hud_visible && st.gl_hud_w > 0 && st.gl_hud_h > 0) {
                    FreerangeRect hr = { (int)floorf(st.hud_x) - 2,
                                         (int)floorf(st.hud_y) - 2,
                                         st.gl_hud_w + 4, st.gl_hud_h + 4 };
                    freerange_rect_union(&cur_rect, &hr);
                }
                if (st.ghost_mode && !st.ball_cleared) {
                    FreerangeRect br = { st.width - GHOST_ICON_SIZE - GHOST_ICON_MARGIN, GHOST_ICON_MARGIN, GHOST_ICON_SIZE, GHOST_ICON_SIZE };
                    freerange_rect_union(&cur_rect, &br);
                }
                freerange_rect_clamp(&cur_rect, st.width, st.height);
            }

            bool repaint_full = true;
            FreerangeRect repaint = cur_rect;
            if (has_buffer_age && !menu_active) {
                EGLint age = 0;
                if (eglQuerySurface(st.egl_display, st.egl_surface,
                                    EGL_BUFFER_AGE_EXT, &age) &&
                     age >= 1 && (int)age <= damage_hist_depth) {
                    for (int i = 0; i < (int)age; i++) {
                        freerange_rect_union(&repaint, &damage_hist[i]);
                    }
                    freerange_rect_clamp(&repaint, st.width, st.height);
                    repaint_full = false;
                }
            }
            if (!repaint_full) {
                glEnable(GL_SCISSOR_TEST);
                glScissor(repaint.x, st.height - (repaint.y + repaint.h),
                          repaint.w, repaint.h);
            }

            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            freerange_gl_draw_frame(&st, &frames, render_extrap_dt);
            if (hud_visible) {
                freerange_gl_draw_hud(&st);
            }

            if (st.ghost_mode && !st.ball_cleared) {
                float bg_size = (float)GHOST_ICON_SIZE;
                float bx = (float)st.width - bg_size - (float)GHOST_ICON_MARGIN;
                float by = (float)GHOST_ICON_MARGIN;
                
                glUseProgram(st.gl_program);
                glActiveTexture(GL_TEXTURE0);
                glEnableVertexAttribArray(st.gl_pos_loc);
                glEnableVertexAttribArray(st.gl_uv_loc);
                glUniform1f(st.gl_alpha_loc, 1.0f);
                
                if (st.gl_ghost_bg_texture) {
                    float bg_x0 = (bx / (float)st.width) * 2.0f - 1.0f;
                    float bg_x1 = ((bx + bg_size) / (float)st.width) * 2.0f - 1.0f;
                    float bg_y0 = 1.0f - (by / (float)st.height) * 2.0f;
                    float bg_y1 = 1.0f - ((by + bg_size) / (float)st.height) * 2.0f;
                    GLfloat bg_verts[] = {
                        bg_x0, bg_y1, 0.0f, 1.0f,
                        bg_x1, bg_y1, 1.0f, 1.0f,
                        bg_x0, bg_y0, 0.0f, 0.0f,
                        bg_x1, bg_y0, 1.0f, 0.0f
                    };
                    glBindTexture(GL_TEXTURE_2D, st.gl_ghost_bg_texture);
                    glVertexAttribPointer(st.gl_pos_loc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), bg_verts);
                    glVertexAttribPointer(st.gl_uv_loc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), bg_verts + 2);
                    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                }
                
                /* The frame contains the ball plus its complete soft
                 * shadow.  The previous 580px UV crop centered the ball,
                 * but cut off the lower edge of the shadow.  Draw the whole
                 * frame instead, positioning it from the ball's true center
                 * so the ball remains centered in the badge.  A 128px ball
                 * leaves the same comfortable visual weight as the other
                 * Ghost-mode tools. */
                const float ball_diameter = 128.0f;
                const float frame_scale = ball_diameter / (float)BALL_W;
                const float icon_w = frames.frame_w * frame_scale;
                const float icon_h = frames.frame_h * frame_scale;
                const float ball_center_x = g_ball_texture_offset_x + BALL_W * 0.5f;
                const float ball_center_y = g_ball_texture_offset_y + BALL_H * 0.5f;
                const float ix = bx + bg_size * 0.5f - ball_center_x * frame_scale;
                const float iy = by + bg_size * 0.5f - ball_center_y * frame_scale;
                float px0 = (ix / (float)st.width) * 2.0f - 1.0f;
                float px1 = ((ix + icon_w) / (float)st.width) * 2.0f - 1.0f;
                float py0 = 1.0f - (iy / (float)st.height) * 2.0f;
                float py1 = 1.0f - ((iy + icon_h) / (float)st.height) * 2.0f;
                GLfloat bverts[] = {
                    px0, py1, 0.0f, 1.0f,
                    px1, py1, 1.0f, 1.0f,
                    px0, py0, 0.0f, 0.0f,
                    px1, py0, 1.0f, 0.0f
                };
                glBindTexture(GL_TEXTURE_2D, st.gl_textures[0]);
                glVertexAttribPointer(st.gl_pos_loc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), bverts);
                glVertexAttribPointer(st.gl_uv_loc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), bverts + 2);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            }

            g_menu_was_open = menu_open;

            if (menu_open) {
                int mx, my, mw, mh;
                ringmenu_rect(g_menu, &mx, &my, &mw, &mh);
                if (g_picker_slot >= 0) {
                    int mcx, mcy;
                    ringmenu_geometry(g_menu, &mcx, &mcy, NULL, NULL);
                    float fr = ringmenu_field_radius(g_menu);
                    int x1 = mx + mw, y1 = my + mh;
                    int fx0 = (int)floorf((float)mcx - fr);
                    int fy0 = (int)floorf((float)mcy - fr);
                    int fx1 = (int)ceilf((float)mcx + fr);
                    int fy1 = (int)ceilf((float)mcy + fr);
                    if (fx0 < mx) mx = fx0;
                    if (fy0 < my) my = fy0;
                    if (fx1 > x1) x1 = fx1;
                    if (fy1 > y1) y1 = fy1;
                    mw = x1 - mx;
                    mh = y1 - my;
                }
                bool menu_redraw = ringmenu_take_dirty(g_menu);
                if (mx != g_menu_rect[0] || my != g_menu_rect[1] ||
                    mw != g_menu_rect[2] || mh != g_menu_rect[3]) {
                    menu_redraw = true;
                    g_menu_rect[0] = mx;
                    g_menu_rect[1] = my;
                    g_menu_rect[2] = mw;
                    g_menu_rect[3] = mh;
                }
                if (menu_redraw) {
                    size_t need = (size_t)mw * mh;
                    if (g_menu_scratch && need <= g_menu_scratch_cap) {
                        memset(g_menu_scratch, 0, (size_t)mw * mh * 4);
                        if (g_picker_slot >= 0) {
                            ringmenu_field_draw(g_menu, g_picker_slot,
                                                g_menu_scratch, mw, mh, mx, my);
                        }
                        ringmenu_draw(g_menu, g_menu_scratch, mw, mh, mx, my);
                        for (size_t i = 0; i < (size_t)mw * mh; i++) {
                            uint32_t c = g_menu_scratch[i];
                            uint32_t a = c >> 24;
                            if (a == 0) { g_menu_scratch[i] = 0; continue; }
                            if (a == 255) continue;
                            uint32_t r = ((c & 0xFF) * a + 127) / 255;
                            uint32_t g_val = (((c >> 8) & 0xFF) * a + 127) / 255;
                            uint32_t b = (((c >> 16) & 0xFF) * a + 127) / 255;
                            g_menu_scratch[i] = r | (g_val << 8) | (b << 16) | (a << 24);
                        }
                        glBindTexture(GL_TEXTURE_2D, g_menu_tex);
                        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, mw, mh, 0, GL_RGBA, GL_UNSIGNED_BYTE, g_menu_scratch);
                    }
                }
                float x0 = 2.f * mx / st.width - 1.f;
                float x1 = 2.f * (mx + mw) / st.width - 1.f;
                float y0 = 1.f - 2.f * my / st.height;
                float y1 = 1.f - 2.f * (my + mh) / st.height;
                GLfloat mverts[16] = {
                    x0, y0, 0.f, 0.f,
                    x1, y0, 1.f, 0.f,
                    x0, y1, 0.f, 1.f,
                    x1, y1, 1.f, 1.f,
                };
                glUseProgram(st.gl_program);
                glBindTexture(GL_TEXTURE_2D, g_menu_tex);
                glVertexAttribPointer(st.gl_pos_loc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), mverts);
                glVertexAttribPointer(st.gl_uv_loc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), mverts + 2);
                glEnableVertexAttribArray(st.gl_pos_loc);
                glEnableVertexAttribArray(st.gl_uv_loc);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            }

            if (!repaint_full) {
                glDisable(GL_SCISSOR_TEST);
            }
            if (g_debug_mode) {
                uint64_t gf_t0 = poingo_perf_counter();
                glFinish();
                uint64_t gf_t1 = poingo_perf_counter();
                double gf_ms = (double)(gf_t1 - gf_t0) /
                               (double)st.performance_frequency * 1000.0;
                if (gf_log) fprintf(gf_log, "%.3f\n", gf_ms);
                gf_n++;
                double gd1 = gf_ms - gf_mean;
                gf_mean += gd1 / gf_n;
                gf_M2   += gd1 * (gf_ms - gf_mean);
                if (gf_ms > gf_max) gf_max = gf_ms;
            }
            if (pfn_swap_damage) {
                FreerangeRect swap_dmg = cur_rect;
                if (damage_hist_depth >= 1) {
                    freerange_rect_union(&swap_dmg, &damage_hist[0]);
                }
                freerange_rect_clamp(&swap_dmg, st.width, st.height);
                EGLint dmg[4];
                EGLint n_dmg = 0;
                if (damage_hist_depth >= 1 && swap_dmg.w > 0 && swap_dmg.h > 0) {
                    dmg[0] = swap_dmg.x;
                    dmg[1] = st.height - (swap_dmg.y + swap_dmg.h);
                    dmg[2] = swap_dmg.w;
                    dmg[3] = swap_dmg.h;
                    n_dmg = 1;
                }
                pfn_swap_damage(st.egl_display, st.egl_surface,
                                n_dmg ? dmg : NULL, n_dmg);
            } else {
                eglSwapBuffers(st.egl_display, st.egl_surface);
            }
            wl_display_flush(st.display);

            for (int i = FREERANGE_DAMAGE_HISTORY - 1; i > 0; i--) {
                damage_hist[i] = damage_hist[i - 1];
            }
            damage_hist[0] = cur_rect;
            if (damage_hist_depth < FREERANGE_DAMAGE_HISTORY) {
                damage_hist_depth++;
            }

            if (fr_cb.compositor_ms != 0) {
                prev_compositor_ms = fr_cb.compositor_ms;
                has_prev_compositor_ms = true;
            }

            if (tick_start != 0) {
                double ov_ms = get_perf_seconds(tick_start, poingo_perf_counter()) * 1000.0;
                double non_upload = ov_ms - upload_actual_ms;
                prev_non_upload_ms = non_upload > 0.0 ? non_upload : 0.0;
                if (g_debug_mode) {
                    if (ov_log) fprintf(ov_log, "%.3f\n", ov_ms);
                    ov_n++;
                    double od1 = ov_ms - ov_mean;
                    ov_mean += od1 / ov_n;
                    ov_M2   += od1 * (ov_ms - ov_mean);
                    if (ov_ms > ov_max) ov_max = ov_ms;
                }
            }

            if (g_debug_mode && has_prev_compositor_ms) {
                if (!miss_this_frame) normal_count++;
                uint32_t now_ms = poingo_ticks_ms();
                if (now_ms - drop_window_start >= 10000) {
                    bool use_color = isatty(STDERR_FILENO);
                    double apb_mean = apb_count > 0 ? apb_ms_sum / apb_count : 0.0;
                    fprintf(stderr, "[drops] %s%dr%s  %s%dy%s  %dok  |  apb mean=%.3fms max=%.3fms\n",
                            (use_color && drop_red)    ? "\033[91m" : "", drop_red,
                            (use_color && drop_red)    ? "\033[0m"  : "",
                            (use_color && drop_yellow) ? "\033[93m" : "", drop_yellow,
                            (use_color && drop_yellow) ? "\033[0m"  : "",
                            normal_count, apb_mean, apb_ms_max);
                    drop_red = drop_yellow = normal_count = 0;
                    apb_ms_sum = apb_ms_max = 0.0; apb_count = 0;
                    drop_window_start = now_ms;
                }
            }

        } else {
            if (++obscured_frames % 300 == 0) {
                struct wl_callback *callback = wl_surface_frame(st.surface);
                wl_callback_add_listener(callback, &wayland_frame_listener, &fr_cb);
                wl_surface_commit(st.surface);
                wl_display_flush(st.display);
            }
        }
    } 

    if (fr_log) {
        fflush(fr_log);
        fclose(fr_log);
    }
    if (ov_log) {
        fflush(ov_log);
        fclose(ov_log);
    }
    if (gf_log) {
        fflush(gf_log);
        fclose(gf_log);
    }
    if (fcb_log) {
        fflush(fcb_log);
        fclose(fcb_log);
    }

    freerange_gl_shutdown(&st);
    if (st.egl_surface != EGL_NO_SURFACE) {
        eglDestroySurface(st.egl_display, st.egl_surface);
    }
    if (st.egl_window) {
        wl_egl_window_destroy(st.egl_window);
    }
    if (st.egl_context != EGL_NO_CONTEXT) {
        eglDestroyContext(st.egl_display, st.egl_context);
    }
    if (st.egl_display != EGL_NO_DISPLAY) {
        eglTerminate(st.egl_display);
    }

    if (st.pointer) {
        wl_pointer_destroy(st.pointer);
    }
    if (st.seat) {
        wl_seat_destroy(st.seat);
    }
    if (st.output) {
        wl_output_destroy(st.output);
    }
    if (st.xdg_toplevel) {
        xdg_toplevel_destroy(st.xdg_toplevel);
    }
    if (st.xdg_surface) {
        xdg_surface_destroy(st.xdg_surface);
    }
    if (st.surface) {
        wl_surface_destroy(st.surface);
    }
    if (st.wm_base) {
        xdg_wm_base_destroy(st.wm_base);
    }
    if (st.shm) {
        wl_shm_destroy(st.shm);
    }
    if (st.compositor) {
        wl_compositor_destroy(st.compositor);
    }
    if (st.registry) {
        wl_registry_destroy(st.registry);
    }
    if (st.display) {
        wl_display_disconnect(st.display);
    }

    freerange_color_regen_shutdown(&st);
    ball_cursor_destroy();
    freerange_destroy_frames(&frames);
    shutdown_audio();
    if (g_menu) { ringmenu_destroy(g_menu); g_menu = NULL; }
    if (g_menu_scratch) { free(g_menu_scratch); g_menu_scratch = NULL; }
    free(st.regen_unit_done_storage);
    free(st.regen_order_storage);
    free(st.regen_thread_storage);
    release_sphere_pixel_cache();
    return 0;
}



int main(int argc, char *argv[]) {
    bool start_muted = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [options]\n", argv[0]);
            printf("Options:\n");
            printf("  --mute                Start with audio muted\n");
            printf("  --light-color <color> Set the light ball color (format: R,G,B or #RRGGBB)\n");
            printf("  --dark-color <color>  Set the dark ball color (format: R,G,B or #RRGGBB)\n");
            printf("  --start-size <scale>  Set initial ball size (0.25 to 2.0)\n");
            printf("  --debug               Print FPS to stderr and show FPS HUD\n");
            printf("  --help, -h            Show this help message\n");
            return 0;
        } else if (strcmp(argv[i], "--mute") == 0 || strcmp(argv[i], "-mute") == 0 || strcmp(argv[i], "mute") == 0) {
            start_muted = true;
        } else if (strcmp(argv[i], "--light-color") == 0 && i + 1 < argc) {
            if (!parse_color_arg(argv[i + 1], g_color_light_rgb)) {
                fprintf(stderr, "Invalid --light-color (use #RRGGBB or R,G,B)\n");
                return 1;
            }
            i++;
        } else if (strcmp(argv[i], "--dark-color") == 0 && i + 1 < argc) {
            if (!parse_color_arg(argv[i + 1], g_color_dark_rgb)) {
                fprintf(stderr, "Invalid --dark-color (use #RRGGBB or R,G,B)\n");
                return 1;
            }
            i++;
        } else if (strcmp(argv[i], "--start-size") == 0 && i + 1 < argc) {
            char *end = NULL;
            double value = strtod(argv[i + 1], &end);
            if (!end || *end != '\0' ||
                value < FREEDOM_BALL_SCALE_MIN ||
                value > FREEDOM_BALL_SCALE_MAX) {
                fprintf(stderr, "Invalid --start-size (use %.2f to %.2f)\n",
                        (double)FREEDOM_BALL_SCALE_MIN,
                        (double)FREEDOM_BALL_SCALE_MAX);
                return 1;
            }
            g_freerange_ball_scale = (float)value;
            i++;
        } else if (strcmp(argv[i], "--debug") == 0) {
            g_debug_mode = true;
        }
    }

    if (getenv("POINGO_FREEDOM_TRACE") != NULL) {
        g_freerange_trace = true;
    }

    const char *wayland_display_env = getenv("WAYLAND_DISPLAY");
    if (!wayland_display_env || wayland_display_env[0] == '\0') {
        fprintf(stderr, "poingo requires a Wayland compositor (WAYLAND_DISPLAY is not set).\n");
        return 1;
    }

    return run_freerange_wayland(start_muted);
}
