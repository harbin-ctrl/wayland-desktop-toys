
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <malloc.h>
#include <sys/mman.h>

#include <wayland-client.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-client-protocol.h"
#include "lodepng.h"
#include "balloon_gen.h"
#include "ghost_icon.h"
#include "audio.h"
#include "ringmenu.h"

#define BALLOON_SOUND_SCALE 0.45f
#define POP_SOUND_VOLUME 32   /* full whack (0-64 scale, typical 8-32) */
#define BOP_SOUND_VOLUME 14   /* gentle knuckle tap */


typedef struct {
    int min_x, min_y;
    int max_x, max_y;
} AlphaBounds;

static AlphaBounds g_ghost_balloon_bounds;

static bool find_alpha_bounds(const Anim *animation, AlphaBounds *bounds) {
    if (!animation || !animation->frames || !animation->frames[0].rgba || !bounds) {
        return false;
    }

    int min_x = animation->w;
    int min_y = animation->h;
    int max_x = -1;
    int max_y = -1;
    const uint8_t *pixels = animation->frames[0].rgba;
    for (int y = 0; y < animation->h; y++) {
        for (int x = 0; x < animation->w; x++) {
            if (pixels[((size_t)y * animation->w + x) * 4 + 3] == 0) continue;
            if (x < min_x) min_x = x;
            if (y < min_y) min_y = y;
            if (x > max_x) max_x = x;
            if (y > max_y) max_y = y;
        }
    }
    if (max_x < min_x || max_y < min_y) return false;
    *bounds = (AlphaBounds){ min_x, min_y, max_x, max_y };
    return true;
}

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint16_t be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} Buf;

static void buf_append(Buf *b, const void *src, size_t n) {
    if (b->len + n > b->cap) {
        b->cap = (b->cap ? b->cap * 2 : 4096);
        while (b->cap < b->len + n) b->cap *= 2;
        b->data = realloc(b->data, b->cap);
        if (!b->data) { fprintf(stderr, "out of memory\n"); exit(1); }
    }
    memcpy(b->data + b->len, src, n);
    b->len += n;
}

static void buf_append_chunk(Buf *b, const char type[4], const uint8_t *data, uint32_t len) {
    uint8_t hdr[8];
    put_be32(hdr, len);
    memcpy(hdr + 4, type, 4);
    buf_append(b, hdr, 8);
    if (len) buf_append(b, data, len);
    uint8_t *crcbuf = malloc(4 + len);
    if (!crcbuf) { fprintf(stderr, "out of memory\n"); exit(1); }
    memcpy(crcbuf, type, 4);
    if (len) memcpy(crcbuf + 4, data, len);
    uint8_t crc[4];
    put_be32(crc, lodepng_crc32(crcbuf, 4 + len));
    free(crcbuf);
    buf_append(b, crc, 4);
}

typedef struct {
    uint32_t w, h, x, y;
    int delay_ms;
    uint8_t dispose, blend;
    Buf idat;          
    bool has_data;
} PendingFrame;

static uint8_t *decode_frame_png(const uint8_t *ihdr_data, 
                                 const Buf *shared_chunks, 
                                 const PendingFrame *pf) {
    unsigned out_w, out_h;
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    Buf png = {0};
    buf_append(&png, sig, 8);
    uint8_t ihdr[13];
    memcpy(ihdr, ihdr_data, 13);
    put_be32(ihdr, pf->w);
    put_be32(ihdr + 4, pf->h);
    buf_append_chunk(&png, "IHDR", ihdr, 13);
    if (shared_chunks->len) buf_append(&png, shared_chunks->data, shared_chunks->len);
    buf_append_chunk(&png, "IDAT", pf->idat.data, (uint32_t)pf->idat.len);
    buf_append_chunk(&png, "IEND", NULL, 0);

    uint8_t *rgba = NULL;
    unsigned err = lodepng_decode32(&rgba, &out_w, &out_h, png.data, png.len);
    free(png.data);
    if (err) {
        fprintf(stderr, "apngo: frame decode failed: %s\n", lodepng_error_text(err));
        return NULL;
    }
    if (out_w != pf->w || out_h != pf->h) {
        fprintf(stderr, "apngo: frame size mismatch (%ux%u vs %ux%u)\n",
                out_w, out_h, pf->w, pf->h);
        free(rgba);
        return NULL;
    }
    return rgba;
}

static void blend_region(uint8_t *canvas, int cw, int ch,
                         const uint8_t *src, const PendingFrame *pf) {
    for (uint32_t row = 0; row < pf->h; row++) {
        uint32_t cy = pf->y + row;
        if ((int)cy >= ch) break;
        for (uint32_t col = 0; col < pf->w; col++) {
            uint32_t cx = pf->x + col;
            if ((int)cx >= cw) break;
            const uint8_t *s = src + (row * pf->w + col) * 4;
            uint8_t *d = canvas + (cy * (uint32_t)cw + cx) * 4;
            if (pf->blend == 0 ) {
                memcpy(d, s, 4);
            } else { 
                uint32_t sa = s[3];
                if (sa == 255) {
                    memcpy(d, s, 4);
                } else if (sa > 0) {
                    uint32_t da = d[3];
                    uint32_t oa = sa + da * (255 - sa) / 255;
                    if (oa > 0) {
                        for (int c = 0; c < 3; c++) {
                            uint32_t sc = s[c], dc = d[c];
                            d[c] = (uint8_t)((sc * sa + dc * da * (255 - sa) / 255) / oa);
                        }
                    }
                    d[3] = (uint8_t)oa;
                }
            }
        }
    }
}

static void clear_region(uint8_t *canvas, int cw, int ch, const PendingFrame *pf) {
    for (uint32_t row = 0; row < pf->h; row++) {
        uint32_t cy = pf->y + row;
        if ((int)cy >= ch) break;
        uint32_t cx = pf->x;
        uint32_t wpix = pf->w;
        if ((int)(cx + wpix) > cw) wpix = (uint32_t)cw - cx;
        memset(canvas + (cy * (uint32_t)cw + cx) * 4, 0, (size_t)wpix * 4);
    }
}

static void emit_frame(Anim *anim, int *cap, const uint8_t *canvas, int delay_ms) {
    if (anim->nframes == *cap) {
        *cap = *cap ? *cap * 2 : 8;
        anim->frames = realloc(anim->frames, (size_t)*cap * sizeof(AnimFrame));
        if (!anim->frames) { fprintf(stderr, "out of memory\n"); exit(1); }
    }
    size_t npix = (size_t)anim->w * anim->h;
    uint8_t *pm = malloc(npix * 4);
    if (!pm) { fprintf(stderr, "out of memory\n"); exit(1); }
    for (size_t i = 0; i < npix; i++) {
        const uint8_t *s = canvas + i * 4;
        uint8_t *d = pm + i * 4;
        uint32_t a = s[3];
        d[0] = (uint8_t)(s[0] * a / 255);
        d[1] = (uint8_t)(s[1] * a / 255);
        d[2] = (uint8_t)(s[2] * a / 255);
        d[3] = (uint8_t)a;
    }
    anim->frames[anim->nframes].rgba = pm;
    anim->frames[anim->nframes].delay_ms = delay_ms;
    anim->nframes++;
}

static __attribute__((unused)) bool anim_load_mem(const char *path, const uint8_t *file, size_t flen, Anim *anim) {
    memset(anim, 0, sizeof(*anim));
    if (flen < 8 + 25) { fprintf(stderr, "balloons: %s: too small\n", path); return false; }

    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (memcmp(file, sig, 8) != 0) {
        fprintf(stderr, "balloons: %s: not a PNG\n", path);
        return false;
    }

    uint8_t ihdr_data[13] = {0};
    bool have_ihdr = false;
    bool have_actl = false;
    bool seen_idat = false;
    Buf shared = {0};              
    PendingFrame cur = {0};        
    bool cur_active = false;
    bool default_is_frame = false; 

    uint8_t *canvas = NULL, *prev_save = NULL;
    int cap = 0;
    bool ok = true;

    size_t pos = 8;
    while (pos + 12 <= (size_t)flen) {
        uint32_t len = be32(file + pos);
        const uint8_t *type = file + pos + 4;
        const uint8_t *data = file + pos + 8;
        if (pos + 12 + len > (size_t)flen) break;

        if (!memcmp(type, "IHDR", 4) && len == 13) {
            memcpy(ihdr_data, data, 13);
            anim->w = (int)be32(data);
            anim->h = (int)be32(data + 4);
            have_ihdr = true;
            canvas = calloc((size_t)anim->w * anim->h, 4);
            prev_save = malloc((size_t)anim->w * anim->h * 4);
            if (!canvas || !prev_save) { fprintf(stderr, "out of memory\n"); exit(1); }
        } else if (!memcmp(type, "acTL", 4)) {
            have_actl = true;
        } else if (!memcmp(type, "fcTL", 4) && len == 26) {
            if (cur_active && cur.has_data) {
                uint8_t *rgba = decode_frame_png(ihdr_data, &shared, &cur);
                if (!rgba) { ok = false; break; }
                if (cur.dispose == 2) memcpy(prev_save, canvas, (size_t)anim->w * anim->h * 4);
                blend_region(canvas, anim->w, anim->h, rgba, &cur);
                free(rgba);
                emit_frame(anim, &cap, canvas, cur.delay_ms);
                if (cur.dispose == 1) clear_region(canvas, anim->w, anim->h, &cur);
                else if (cur.dispose == 2) memcpy(canvas, prev_save, (size_t)anim->w * anim->h * 4);
                free(cur.idat.data);
                memset(&cur, 0, sizeof(cur));
            }
            cur_active = true;
            cur.w = be32(data + 4);
            cur.h = be32(data + 8);
            cur.x = be32(data + 12);
            cur.y = be32(data + 16);
            uint16_t dnum = be16(data + 20), dden = be16(data + 22);
            if (dden == 0) dden = 100;
            cur.delay_ms = (int)((uint32_t)dnum * 1000 / dden);
            if (cur.delay_ms < 10) cur.delay_ms = 10;
            cur.dispose = data[24];
            cur.blend = data[25];
            if (anim->nframes == 0) {
                if (cur.dispose == 2) cur.dispose = 1; 
                if (!seen_idat) default_is_frame = true;
            }
        } else if (!memcmp(type, "IDAT", 4)) {
            seen_idat = true;
            if (cur_active && default_is_frame) {
                buf_append(&cur.idat, data, len);
                cur.has_data = true;
            }
        } else if (!memcmp(type, "fdAT", 4) && len >= 4) {
            if (cur_active) {
                buf_append(&cur.idat, data + 4, len - 4);
                cur.has_data = true;
            }
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        } else if (!seen_idat && have_ihdr) {
            buf_append(&shared, file + pos, 12 + len);
        }
        pos += 12 + len;
    }

    if (ok && cur_active && cur.has_data) {
        uint8_t *rgba = decode_frame_png(ihdr_data, &shared, &cur);
        if (rgba) {
            blend_region(canvas, anim->w, anim->h, rgba, &cur);
            free(rgba);
            emit_frame(anim, &cap, canvas, cur.delay_ms);
        } else {
            ok = false;
        }
    }
    free(cur.idat.data);
    free(shared.data);
    free(canvas);
    free(prev_save);

    if (ok && anim->nframes == 0) {
        unsigned w, h;
        uint8_t *rgba = NULL;
        if (lodepng_decode32(&rgba, &w, &h, file, (size_t)flen) == 0) {
            anim->w = (int)w;
            anim->h = (int)h;
            emit_frame(anim, &cap, rgba, 1000);
            free(rgba);
        } else {
            ok = false;
        }
    }
    if (!ok || anim->nframes == 0 || !have_ihdr) {
        fprintf(stderr, "balloons: %s: failed to load\n", path);
        return false;
    }
    if (!have_actl) {
    }
    return true;
}


typedef enum { MODE_BOUNCE, MODE_FLOAT, MODE_SCURRY, MODE_DRIFT } Mode;

typedef struct {
    const Anim *anim;
    GLuint *textures;      
    float x, y;            
    float vx, vy;
    float base_x;          
    float phase;
    int facing;            
    int frame;
    float frame_time;      
    float action_timer;    
    int action;            
    float dash;            
    bool grabbed;          
    float grab_dx, grab_dy;
    float fall_v;          
    int pr_x, pr_y, pr_w, pr_h; 
    bool pr_valid;
    bool dead;
    bool popped;
    float pop_timer;
    float scheduled_pop_time;
    float scale;           
    bool raspberry;        
} Sprite;

static float frandf(void) { return (float)rand() / (float)RAND_MAX; }
static float frandf_sq(void) { float r = frandf(); return r * r; }

static bool g_trace;

#ifndef EGL_BUFFER_AGE_EXT
#define EGL_BUFFER_AGE_EXT 0x313D
#endif

static PFNEGLSWAPBUFFERSWITHDAMAGEEXTPROC g_swap_damage;
static EGLint *g_damage_rects;
static bool g_full_damage = true;
static bool g_has_buffer_age = false;

typedef struct { int x, y, w, h; } Rect;

static void rect_union(Rect *acc, const Rect *r) {
    if (!r || r->w <= 0 || r->h <= 0) return;
    if (acc->w <= 0 || acc->h <= 0) { *acc = *r; return; }
    int x0 = acc->x < r->x ? acc->x : r->x;
    int y0 = acc->y < r->y ? acc->y : r->y;
    int x1 = (acc->x + acc->w) > (r->x + r->w) ? (acc->x + acc->w) : (r->x + r->w);
    int y1 = (acc->y + acc->h) > (r->y + r->h) ? (acc->y + acc->h) : (r->y + r->h);
    acc->x = x0; acc->y = y0; acc->w = x1 - x0; acc->h = y1 - y0;
}

static void rect_clamp(Rect *r, int max_w, int max_h) {
    if (r->x < 0) { r->w += r->x; r->x = 0; }
    if (r->y < 0) { r->h += r->y; r->y = 0; }
    if (r->w < 0) r->w = 0;
    if (r->h < 0) r->h = 0;
    if (r->x + r->w > max_w) r->w = max_w - r->x;
    if (r->y + r->h > max_h) r->h = max_h - r->y;
}

static bool rect_intersects(const Rect *a, const Rect *b) {
    return a->x < b->x + b->w && b->x < a->x + a->w &&
           a->y < b->y + b->h && b->y < a->y + a->h;
}

#define DAMAGE_HISTORY 4
static Rect *g_damage_hist[DAMAGE_HISTORY]; 
static int g_damage_hist_n[DAMAGE_HISTORY];
static int g_damage_hist_depth = 0;
static Rect *g_cur_damage;                  
static int g_ncur_damage;
static Rect *g_sprite_rects;                

static void damage_push(Rect r, int max_w, int max_h) {
    rect_clamp(&r, max_w, max_h);
    if (r.w <= 0 || r.h <= 0) return;
    g_cur_damage[g_ncur_damage++] = r;
}

static Anim *g_anims;
static GLuint **g_anim_tex;
static int g_nanims;

static Anim *g_pop_anims;
static GLuint **g_pop_tex;
static int g_npop_anims;

static Anim g_str_anim;
static GLuint *g_str_tex;

static bool g_raspberry;      
static Anim g_rasp_anim;
static GLuint *g_rasp_tex;
static GLuint g_ghost_bg_tex;

static bool load_raspberry_from_pi(void) {
    static const char *paths[] = {
        "/usr/share/piwiz/raspberry-pi-logo.png",
        "/usr/share/raspberrypi-artwork/raspberry-pi-logo-small.png",
        "/usr/share/raspberrypi-artwork/raspberry-pi-logo.png",
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        unsigned w = 0, h = 0;
        uint8_t *rgba = NULL;
        if (lodepng_decode32_file(&rgba, &w, &h, paths[i]) == 0 && rgba) {
            size_t npix = (size_t)w * h;
            for (size_t p = 0; p < npix; p++) {
                uint32_t a = rgba[p * 4 + 3];
                rgba[p * 4 + 0] = (uint8_t)(rgba[p * 4 + 0] * a / 255);
                rgba[p * 4 + 1] = (uint8_t)(rgba[p * 4 + 1] * a / 255);
                rgba[p * 4 + 2] = (uint8_t)(rgba[p * 4 + 2] * a / 255);
            }
            g_rasp_anim.w = (int)w;
            g_rasp_anim.h = (int)h;
            g_rasp_anim.nframes = 1;
            g_rasp_anim.frames = malloc(sizeof(AnimFrame));
            if (!g_rasp_anim.frames) { free(rgba); return false; }
            g_rasp_anim.frames[0].rgba = rgba;
            g_rasp_anim.frames[0].delay_ms = 1000;
            return true;
        }
    }
    return false;
}

static float g_wind;           
static float g_wind_target;
static float g_breeze_timer = 8.f;  
static bool g_breezing;

static bool g_gust_synthesizing; 
static float g_gust_delay;     
static float g_gust_target;    
static float g_gust_duration;

static void start_gust(void) {
    g_breezing = true;
    g_wind_target = g_gust_target;
    g_breeze_timer = g_gust_duration;
}

static bool g_storm_active;
static float g_storm_timer;      
static float g_storm_leg_timer;  
static float g_storm_pop_timer;  
static float g_storm_duration;   
static float g_storm_alpha;      
static int g_over_w8;            
static int g_over_a8;            
static float g_storm_countdown;  

#ifndef STORM_PERIOD_MIN_S
#define STORM_PERIOD_MIN_S 420.f
#endif
#ifndef STORM_PERIOD_MAX_S
#define STORM_PERIOD_MAX_S 660.f
#endif
static float roll_storm_countdown(void) {
    return STORM_PERIOD_MIN_S +
           frandf() * (STORM_PERIOD_MAX_S - STORM_PERIOD_MIN_S);
}

#define LIGHTNING_MAX_STROKES 3
static float g_lightning_t = -1.f;   
static int   g_lightning_nstrokes;
static float g_lightning_start[LIGHTNING_MAX_STROKES];
static float g_lightning_hold[LIGHTNING_MAX_STROKES];
static float g_lightning_peak[LIGHTNING_MAX_STROKES];
static float g_lightning_total;
static float g_flash01;              
static float g_dry_flash_delay;      

#define THUNDER_MIN_DELAY_S 0.85f
#define THUNDER_MAX_DELAY_S 3.60f
static float g_thunder_delay = -1.f; 
static float g_thunder_dist;         

static void lightning_strike(void) {
    float dist01 = frandf();          
    g_lightning_nstrokes = 1 + rand() % LIGHTNING_MAX_STROKES;
    float t = 0.f;
    for (int k = 0; k < g_lightning_nstrokes; k++) {
        g_lightning_start[k] = t;
        g_lightning_hold[k] = 0.03f + frandf() * 0.05f;
        g_lightning_peak[k] = 0.55f + 0.45f * frandf();
        t += g_lightning_hold[k] + 0.04f + frandf() * 0.09f;
    }
    g_lightning_peak[0] = fmaxf(g_lightning_peak[0], 0.85f);
    for (int k = 0; k < g_lightning_nstrokes; k++) {
        g_lightning_peak[k] *= 1.f - 0.35f * dist01;
    }
    g_lightning_total = t + 0.45f;   
    g_lightning_t = 0.f;
    g_thunder_dist = dist01;
    g_thunder_delay = THUNDER_MIN_DELAY_S +
                      dist01 * (THUNDER_MAX_DELAY_S - THUNDER_MIN_DELAY_S);
    if (g_trace) fprintf(stderr,
                         "[trace] lightning: %d strokes over %.0f ms, dist %.2f, "
                         "thunder in %.0f ms\n",
                         g_lightning_nstrokes, (double)g_lightning_total * 1e3,
                         (double)dist01, (double)g_thunder_delay * 1e3);
}

static void thunder_update(float dt) {
    if (g_thunder_delay < 0.f) return;
    g_thunder_delay -= dt;
    if (g_thunder_delay <= 0.f) {
        g_thunder_delay = -1.f;
        play_thunder_sound(g_thunder_dist);
    }
}

static void lightning_update(float dt) {
    g_flash01 = 0.f;
    if (g_lightning_t < 0.f) return;
    g_lightning_t += dt;
    if (g_lightning_t >= g_lightning_total) {
        g_lightning_t = -1.f;
        return;
    }
    for (int k = 0; k < g_lightning_nstrokes; k++) {
        float ts = g_lightning_t - g_lightning_start[k];
        if (ts < 0.f) break;
        float v;
        if (ts < g_lightning_hold[k]) {
            v = g_lightning_peak[k];
        } else {
            float tau = (k == g_lightning_nstrokes - 1) ? 0.11f : 0.045f;
            v = g_lightning_peak[k] * expf(-(ts - g_lightning_hold[k]) / tau);
        }
        if (v > g_flash01) g_flash01 = v;
    }
}

static void pop_random_sprite(void);   

static void storm_new_leg(bool first) {
    float strength = 170.f + frandf() * 110.f;
    float dir;
    if (first) {
        dir = (frandf() < 0.5f) ? -1.f : 1.f;
    } else {
        dir = (g_wind_target > 0.f) ? -1.f : 1.f;
        if (frandf() < 0.25f) dir = -dir;
    }
    g_wind_target = dir * strength;
    g_storm_leg_timer = 4.5f + frandf() * 4.5f;
    if (g_storm_leg_timer > g_storm_timer) {
        g_storm_leg_timer = g_storm_timer;
    }
    /* Storm wind is the continuous bed; keep enough headroom for lightning's
     * thunder to move decisively into the foreground. */
    play_whoosh_async(1.0f, g_storm_leg_timer, 1.5f, false);
}

static void start_storm(void) {
    g_storm_timer = 37.5f + frandf() * 15.f;
    g_storm_duration = g_storm_timer;
    g_storm_pop_timer = 4.6f + frandf() * 7.7f;
    if (!g_storm_active) {
        g_storm_active = true;
        g_breezing = false;
        g_gust_delay = 0.f;
        g_gust_synthesizing = false;
        whoosh_gust_cancel();
        storm_new_leg(true);
    }
}

static void stop_storm(void) {
    if (g_storm_active) {
        g_storm_active = false;
        g_wind_target = 0.f;
        g_breeze_timer = 20.f + frandf_sq() * 20.f;
        g_storm_alpha = 0.0f;
        g_dry_flash_delay = 0.f;
        g_storm_countdown = roll_storm_countdown();
        whoosh_gust_cancel();
        whoosh_stop_playing();
    }
}

static void breeze_update(float dt) {
    lightning_update(dt);   
    thunder_update(dt);     
    if (g_storm_active) {
        g_storm_timer -= dt;
        g_storm_leg_timer -= dt;
        g_storm_pop_timer -= dt;
        if (g_storm_pop_timer <= 0.f) {
            g_storm_pop_timer = 4.6f + frandf() * 7.7f;
            if (frandf() < 0.55f) lightning_strike();
            else if (frandf() < 0.20f)
                g_dry_flash_delay = 0.4f + frandf() * 1.2f;
            float r = frandf();
            int casualties = (r < 0.5f) ? 1 : (r < 0.75f) ? 2 : 3;
            for (int k = 0; k < casualties; k++)
                pop_random_sprite();
        }
        if (g_dry_flash_delay > 0.f) {
            g_dry_flash_delay -= dt;
            if (g_dry_flash_delay <= 0.f) lightning_strike();
        }
        if (g_storm_timer <= 0.f) {
            g_storm_active = false;
            g_wind_target = 0.f;
            g_breeze_timer = 20.f + frandf_sq() * 20.f;
            g_storm_alpha = 0.0f;
            g_dry_flash_delay = 0.f;
            g_storm_countdown = roll_storm_countdown();
        } else if (g_storm_leg_timer <= 0.f) {
            storm_new_leg(false);
        }
        g_wind += (g_wind_target - g_wind) * fminf(1.f, dt * 1.6f);

        if (g_storm_active) {
            float elapsed = g_storm_duration - g_storm_timer;
            float remaining = g_storm_timer;
            float fade_in_time = 4.0f;
            float fade_out_time = 4.0f;
            float max_alpha = 0.6f;

            if (elapsed < fade_in_time) {
                g_storm_alpha = (elapsed / fade_in_time) * max_alpha;
            } else if (remaining < fade_out_time) {
                g_storm_alpha = (remaining / fade_out_time) * max_alpha;
            } else {
                g_storm_alpha = max_alpha;
            }
        }
        return;
    }
    g_storm_alpha = 0.0f;
    g_storm_countdown -= dt;
    if (g_storm_countdown <= 0.f) {
        start_storm();   
        return;
    }
    if (g_gust_synthesizing) {
        float latency = 0.f;
        if (whoosh_gust_poll(&latency)) {
            g_gust_synthesizing = false;
            g_gust_delay = latency;
            if (g_gust_delay <= 0.f) start_gust();
        }
    } else if (g_gust_delay > 0.f) {
        g_gust_delay -= dt;
        if (g_gust_delay <= 0.f) start_gust();
    } else {
        g_breeze_timer -= dt;
        if (g_breeze_timer <= 0.f) {
            if (g_breezing) {
                g_breezing = false;
                g_wind_target = 0.f;
                g_breeze_timer = 20.f + frandf_sq() * 20.f;
            } else {
                float strength = 70.f + frandf() * 80.f;
                g_gust_target = (frandf() < 0.5f) ? -strength : strength;
                g_gust_duration = 4.0f + frandf_sq() * 5.0f;
                if (play_whoosh_async((strength - 90.f) / 110.f,
                                      g_gust_duration, 1.0f, true))
                    g_gust_synthesizing = true;
                else
                    start_gust();  
            }
        }
    }
    g_wind += (g_wind_target - g_wind) * fminf(1.f, dt * 1.6f);
}

#define BALLOON_SCALE_MIN 0.769f
#define BALLOON_SCALE_MAX 1.0f

#define BALLOON_TIE_X (36.0f / 72.0f)
#define BALLOON_TIE_Y (71.5f / 128.0f)

#define BALLOON_BODY_CX (36.0f / 72.0f)
#define BALLOON_BODY_CY (34.0f / 128.0f)
#define BALLOON_BOB_FRAC (3.0f / 128.0f)
#define RASPBERRY_FRAC 0.26f           /* logo width as a fraction of sprite width */
#define RASPBERRY_CHANCE (1.0f / 300.0f)
#define RASPBERRY_ALPHA 0.6f           /* ink opacity: lets the balloon show through */

static float sprite_w(const Sprite *s, float scale) { return s->anim->w * scale * s->scale; }
static float sprite_h(const Sprite *s, float scale) { return s->anim->h * scale * s->scale; }

static void sprite_roll_anim(Sprite *s) {
    int i = rand() % g_nanims;
    s->anim = &g_anims[i];
    s->textures = g_anim_tex[i];
    s->frame = rand() % s->anim->nframes;
    s->frame_time = frandf() * s->anim->frames[s->frame].delay_ms;
    s->scale = BALLOON_SCALE_MIN + frandf() * (BALLOON_SCALE_MAX - BALLOON_SCALE_MIN);
    s->raspberry = g_raspberry && (frandf() < RASPBERRY_CHANCE);
}

static void sprite_init(Sprite *s, Mode mode, float scale, float speed, int sw, int sh) {
    float w = sprite_w(s, scale), h = sprite_h(s, scale);
    s->frame = rand() % s->anim->nframes;
    s->frame_time = frandf() * s->anim->frames[s->frame].delay_ms;
    s->facing = 1;
    s->phase = frandf() * 6.2831853f;
    switch (mode) {
    case MODE_BOUNCE:
        s->x = frandf() * (sw - w);
        s->y = frandf() * (sh - h);
        s->vx = (frandf() < 0.5f ? -1.f : 1.f) * (60.f + frandf() * 60.f) * speed;
        s->vy = (frandf() < 0.5f ? -1.f : 1.f) * (60.f + frandf() * 60.f) * speed;
        break;
    case MODE_FLOAT:
        s->base_x = frandf() * (sw - w);
        s->x = s->base_x;
        s->y = sh - frandf() * (sh + h);
        s->vy = -(20.f + frandf() * 25.f) * speed;
        break;
    case MODE_SCURRY:
        s->x = frandf() * (sw - w);
        s->y = sh - h;
        s->vx = (100.f + frandf() * 80.f) * speed;
        if (frandf() < 0.5f) s->vx = -s->vx;
        s->action = 0;
        s->action_timer = 1.0f + frandf_sq() * 1.5f;
        break;
    case MODE_DRIFT:
        s->x = frandf() * (sw - w);
        s->y = frandf() * (sh - h);
        break;
    }
    if (s->vx < 0) s->facing = -1;
}

static void sprite_update(Sprite *s, Mode mode, float dt, float t,
                          float scale, float speed, int sw, int sh) {
    if (s->dead) return;
    if (s->popped) {
        s->pop_timer -= dt;
        if (s->pop_timer <= 0.f) {
            s->dead = true;
        }
    }
    float w = sprite_w(s, scale), h = sprite_h(s, scale);

    float anim_rate = 1.f + (mode == MODE_SCURRY ? s->dash / 300.f : 0.f);
    s->frame_time += dt * 1000.f * anim_rate;
    while (s->frame_time >= s->anim->frames[s->frame].delay_ms) {
        s->frame_time -= s->anim->frames[s->frame].delay_ms;
        s->frame++;
        if (s->frame >= s->anim->nframes) {
            s->frame = 0;
        }
    }

    if (s->grabbed) return; 

    switch (mode) {
    case MODE_BOUNCE:
        s->x += s->vx * dt;
        s->y += s->vy * dt;
        if (s->x < 0)       { s->x = 0;       s->vx = fabsf(s->vx); }
        if (s->x + w > sw)  { s->x = sw - w;  s->vx = -fabsf(s->vx); }
        if (s->y < 0)       { s->y = 0;       s->vy = fabsf(s->vy); }
        if (s->y + h > sh)  { s->y = sh - h;  s->vy = -fabsf(s->vy); }
        s->facing = (s->vx < 0) ? -1 : 1;
        break;
    case MODE_FLOAT:
        s->y += s->vy * dt;
        s->base_x += g_wind * (0.75f + 0.5f * fabsf(sinf(s->phase))) * dt;
        if (s->base_x > sw + 40.f)     s->base_x = -w - 40.f;
        if (s->base_x < -w - 40.f)     s->base_x = sw + 40.f;
        s->x = s->base_x + sinf(t * 0.9f + s->phase) * 24.f;
        if (s->y + h < 0) { 
            sprite_roll_anim(s); 
            s->y = sh;
            s->base_x = frandf() * (sw - sprite_w(s, scale));
            s->vy = -(20.f + frandf() * 25.f) * speed;
        }
        break;
    case MODE_SCURRY:
        s->action_timer -= dt;
        if (s->action_timer <= 0.f) {
            if (s->action == 0 && frandf() < 0.4f) {
                s->action = 1; 
                s->action_timer = 0.3f + frandf_sq() * 1.7f;
            } else {
                s->action = 0;
                s->action_timer = 0.6f + frandf_sq() * 2.4f;
                float v = (100.f + frandf() * 120.f) * speed;
                s->vx = (frandf() < 0.5f ? -v : v);
            }
        }
        s->dash *= exp2f(-2.0f * dt); 
        if (s->action == 0 || s->dash > 1.f) {
            float dir = (s->vx < 0) ? -1.f : 1.f;
            s->x += (s->vx + dir * s->dash) * dt;
        }
        if (s->x < 0)      { s->x = 0;      s->vx = fabsf(s->vx); }
        if (s->x + w > sw) { s->x = sw - w; s->vx = -fabsf(s->vx); }
        if (s->y < sh - h - 0.5f) { 
            s->fall_v += 1400.f * dt;
            s->y += s->fall_v * dt;
        }
        if (s->y >= sh - h) { s->y = sh - h; s->fall_v = 0.f; }
        s->facing = (s->vx < 0) ? -1 : 1;
        break;
    case MODE_DRIFT: {
        float ang = sinf(t * 0.31f + s->phase) * 2.2f + sinf(t * 0.13f + s->phase * 2.f) * 1.7f;
        float v = (30.f + 20.f * sinf(t * 0.21f + s->phase)) * speed;
        s->x += cosf(ang) * v * dt;
        s->y += sinf(ang) * v * dt * 0.6f;
        if (s->x < 0)      s->x = 0;
        if (s->x + w > sw) s->x = sw - w;
        if (s->y < 0)      s->y = 0;
        if (s->y + h > sh) s->y = sh - h;
        s->facing = (cosf(ang) < 0) ? -1 : 1;
        break;
    }
    }
}


typedef struct {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct xdg_wm_base *wm_base;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;
    struct zxdg_decoration_manager_v1 *deco_mgr;
    struct zxdg_toplevel_decoration_v1 *deco;

    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;

    struct wl_egl_window *egl_window;
    EGLDisplay egl_display;
    EGLContext egl_context;
    EGLSurface egl_surface;

    int width, height;
    bool configured;
    bool running;
    bool need_redraw;

    double ptr_x, ptr_y;

    struct wl_shm *shm;
    struct wl_surface *cursor_surface;
    struct wl_buffer *cursor_buffer;
    void *cursor_map;
    size_t cursor_map_size;
} Ctx;

static volatile sig_atomic_t g_signal_quit = 0;
static double g_quit_fade = 0.0;
static bool g_ghost = false;
static double g_startup_fade = 0.0;
static GLint g_fade_loc = -1;
static GLint g_color_loc = -1;
static void on_signal(int sig) { (void)sig; g_signal_quit = 1; }

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                            const char *iface, uint32_t version) {
    Ctx *ctx = data;
    if (!strcmp(iface, wl_compositor_interface.name)) {
        ctx->compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    } else if (!strcmp(iface, xdg_wm_base_interface.name)) {
        ctx->wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
    } else if (!strcmp(iface, wl_seat_interface.name)) {
        ctx->seat = wl_registry_bind(reg, name, &wl_seat_interface, version < 5 ? version : 5);
    } else if (!strcmp(iface, zxdg_decoration_manager_v1_interface.name)) {
        ctx->deco_mgr = wl_registry_bind(reg, name, &zxdg_decoration_manager_v1_interface, 1);
    } else if (!strcmp(iface, wl_shm_interface.name)) {
        ctx->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    }
}
static void registry_global_remove(void *data, struct wl_registry *reg, uint32_t name) {
    (void)data; (void)reg; (void)name;
}
static const struct wl_registry_listener registry_listener = {
    registry_global, registry_global_remove
};

static void wm_base_ping(void *data, struct xdg_wm_base *wm, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(wm, serial);
}
static const struct xdg_wm_base_listener wm_base_listener = { wm_base_ping };

static void xdg_surface_configure(void *data, struct xdg_surface *xs, uint32_t serial) {
    Ctx *ctx = data;
    xdg_surface_ack_configure(xs, serial);
    ctx->configured = true;
    g_full_damage = true;
    if (ctx->egl_window && ctx->width > 0 && ctx->height > 0) {
        wl_egl_window_resize(ctx->egl_window, ctx->width, ctx->height, 0, 0);
    }
    ctx->need_redraw = true;
}
static const struct xdg_surface_listener xdg_surface_listener = { xdg_surface_configure };

static void toplevel_configure(void *data, struct xdg_toplevel *tl,
                               int32_t w, int32_t h, struct wl_array *states) {
    (void)tl; (void)states;
    Ctx *ctx = data;
    if (w > 0 && h > 0) {
        ctx->width = w;
        ctx->height = h;
    }
}
static Ctx *g_ctx;
static Sprite *g_sprites;
static int g_nsprites;
static Mode g_mode = MODE_BOUNCE;
static float g_scale = 1.0f;
static float g_speed = 1.0f;
static float g_time = 0.0f;
static int g_grab_index = -1;
static double g_press_x, g_press_y;
static struct timespec g_press_ts;

static void pop_sprite(Sprite *s, float pop_x, float pop_y) {
    int pop_idx = rand() % g_npop_anims;
    s->popped = true;
    s->pop_timer = 0.15f;
    s->anim = &g_pop_anims[pop_idx];
    s->textures = g_pop_tex[pop_idx];
    s->frame = 0;
    s->frame_time = 0;
    float pw = sprite_w(s, g_scale);
    float ph = sprite_h(s, g_scale);
    s->x = pop_x - pw * 0.5f;
    s->y = pop_y - ph * 0.5f;
    play_pop_sound(POP_SOUND_VOLUME);
}

static void pop_random_sprite(void) {
    int pick = -1, seen = 0;
    for (int i = 0; i < g_nsprites; i++) {
        if (g_sprites[i].dead || g_sprites[i].popped) continue;
        seen++;
        if (rand() % seen == 0) pick = i;
    }
    if (pick >= 0) {
        Sprite *s = &g_sprites[pick];
        float cx = s->x + sprite_w(s, g_scale) * 0.5f;
        float cy = s->y + sprite_h(s, g_scale) * (32.f / 128.f); 
        pop_sprite(s, cx, cy);
    }
}

static bool g_mass_pop_active = false;
static bool g_mass_pop_quit = false;   
static float g_mass_pop_timer = 0.f;

static void start_mass_pop(bool then_quit) {
    if (g_mass_pop_active) {
        g_mass_pop_quit = g_mass_pop_quit || then_quit;
        return;
    }
    g_mass_pop_active = true;
    g_mass_pop_quit = then_quit;
    g_mass_pop_timer = 0.f;
    for (int i = 0; i < g_nsprites; i++) {
        if (!g_sprites[i].dead && !g_sprites[i].popped) {
            float u1 = frandf();
            if (u1 < 0.0001f) u1 = 0.0001f;
            float u2 = frandf();
            float z0 = sqrtf(-2.0f * logf(u1)) * cosf(2.0f * (float)M_PI * u2);
            float val = fabsf(z0) * 1.5f;
            if (val > 5.0f) val = 5.0f;
            g_sprites[i].scheduled_pop_time = val;
        }
    }
}

static void trigger_quit(void) {
    g_signal_quit = 1;
}

static RingMenu *g_menu;
static GLuint g_menu_tex;
static uint32_t *g_menu_scratch;
static bool g_menu_was_open;   

enum { MENU_STORM = 1, MENU_POP_ALL, MENU_GHOST, MENU_QUIT };

static void menu_result(int r) {
    if (r == MENU_STORM) {
        if (g_storm_active) {
            stop_storm();
        } else {
            start_storm();
        }
    }
    else if (r == MENU_POP_ALL) start_mass_pop(false);
    else if (r == MENU_GHOST) g_ghost = true;
    else if (r == MENU_QUIT) trigger_quit();
}

static void toplevel_close(void *data, struct xdg_toplevel *tl) {
    (void)tl; (void)data;
    trigger_quit();
}
static const struct xdg_toplevel_listener toplevel_listener = {
    toplevel_configure, toplevel_close
};

static int sprite_hit(double px, double py) {
    for (int i = g_nsprites - 1; i >= 0; i--) {
        Sprite *s = &g_sprites[i];
        if (s->dead || s->popped) continue;
        float w = sprite_w(s, g_scale), h = sprite_h(s, g_scale);
        
        float bx = s->x + w * (12.f / 72.f);
        float by = s->y + h * (4.f / 128.f);
        float bw = w * (48.f / 72.f);
        float bh = h * (60.f / 128.f);
        
        if (px >= bx && px < bx + bw && py >= by && py < by + bh)
            return i;
    }
    return -1;
}

#define NEEDLE_SIZE 64
#define NEEDLE_TIP 4.5      /* tip position (x == y) inside the buffer */
#define NEEDLE_LEN 38.0     /* tip to shaft end, along the diagonal */
#define NEEDLE_W 2.6        /* shaft half-width once fully tapered */
#define NEEDLE_HEAD_R 7.0
#define NAXIS 0.70710678

static double clampd01(double v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

static void render_needle_cursor(uint32_t *px, int size) {
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            double rx = (x + 0.5) - NEEDLE_TIP;
            double ry = (y + 0.5) - NEEDLE_TIP;
            double t = (rx + ry) * NAXIS;   
            double s = (ry - rx) * NAXIS;   

            double half = NEEDLE_W * (t < 12.0 ? clampd01(t / 12.0) : 1.0);
            double d_sh = fabs(s) - half;
            double dcap = t - (NEEDLE_LEN + 2.0);
            if (dcap > d_sh) d_sh = dcap;
            if (t < 0.0) d_sh = sqrt(t * t + s * s);

            double ht = t - (NEEDLE_LEN + 3.0);
            double d_hd = sqrt(ht * ht + s * s) - NEEDLE_HEAD_R;

            double d = d_sh < d_hd ? d_sh : d_hd;
            double cov = clampd01(0.5 - d);
            if (cov <= 0.0) {
                px[(size_t)y * size + x] = 0;
                continue;
            }

            double fr, fg, fb;
            if (d_hd < d_sh) {
                fr = 210.0; fg = 45.0; fb = 62.0;
                double glint = clampd01((2.4 - sqrt((ht + 2.4) * (ht + 2.4) +
                                                    (s + 2.4) * (s + 2.4))) / 1.4);
                fr += (255.0 - fr) * glint * 0.9;
                fg += (255.0 - fg) * glint * 0.9;
                fb += (255.0 - fb) * glint * 0.9;
            } else {
                fr = 205.0; fg = 210.0; fb = 222.0;
                double line = clampd01(1.0 - fabs(s + half * 0.4) / 0.9);
                fr += (255.0 - fr) * line * 0.7;
                fg += (255.0 - fg) * line * 0.7;
                fb += (255.0 - fb) * line * 0.7;
            }

            double ot = clampd01((d + 1.1) / 0.8);
            fr = fr * (1.0 - ot) + 40.0 * ot;
            fg = fg * (1.0 - ot) + 40.0 * ot;
            fb = fb * (1.0 - ot) + 46.0 * ot;

            uint32_t a8 = (uint32_t)(cov * 255.0 + 0.5);
            uint32_t r8 = (uint32_t)(fr * cov + 0.5);
            uint32_t g8 = (uint32_t)(fg * cov + 0.5);
            uint32_t b8 = (uint32_t)(fb * cov + 0.5);
            px[(size_t)y * size + x] = (a8 << 24) | (r8 << 16) | (g8 << 8) | b8;
        }
    }
}

static bool create_needle_cursor(Ctx *ctx) {
    if (!ctx->shm || !ctx->compositor) return false;

    int stride = NEEDLE_SIZE * 4;
    size_t bytes = (size_t)stride * NEEDLE_SIZE;
    int fd = memfd_create("balloons-cursor", MFD_CLOEXEC);
    if (fd < 0) return false;
    if (ftruncate(fd, (off_t)bytes) < 0) { close(fd); return false; }
    void *data = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) { close(fd); return false; }

    struct wl_shm_pool *pool = wl_shm_create_pool(ctx->shm, fd, (int32_t)bytes);
    ctx->cursor_buffer = wl_shm_pool_create_buffer(pool, 0, NEEDLE_SIZE, NEEDLE_SIZE,
                                                   stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    render_needle_cursor(data, NEEDLE_SIZE);
    ctx->cursor_map = data;
    ctx->cursor_map_size = bytes;

    ctx->cursor_surface = wl_compositor_create_surface(ctx->compositor);
    if (!ctx->cursor_surface) return false;
    wl_surface_attach(ctx->cursor_surface, ctx->cursor_buffer, 0, 0);
    wl_surface_damage(ctx->cursor_surface, 0, 0, NEEDLE_SIZE, NEEDLE_SIZE);
    wl_surface_commit(ctx->cursor_surface);
    return true;
}

static void pointer_enter(void *d, struct wl_pointer *p, uint32_t serial,
                          struct wl_surface *sf, wl_fixed_t x, wl_fixed_t y) {
    (void)d; (void)sf;
    g_ctx->ptr_x = wl_fixed_to_double(x);
    g_ctx->ptr_y = wl_fixed_to_double(y);
    if (g_ctx->cursor_surface) {
        wl_pointer_set_cursor(p, serial, g_ctx->cursor_surface,
                              (int)NEEDLE_TIP, (int)NEEDLE_TIP);
    }
    if (g_trace) fprintf(stderr, "[trace] enter %.0f,%.0f\n", g_ctx->ptr_x, g_ctx->ptr_y);
}
static void pointer_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *sf) {
    (void)d; (void)p; (void)serial; (void)sf;
}
static void grabbed_follow_pointer(void) {
    Sprite *s = &g_sprites[g_grab_index];
    float w = sprite_w(s, g_scale), h = sprite_h(s, g_scale);
    s->x = (float)g_ctx->ptr_x - s->grab_dx;
    s->y = (float)g_ctx->ptr_y - s->grab_dy;
    if (s->x < 0) s->x = 0;
    if (s->x + w > g_ctx->width) s->x = g_ctx->width - w;
    if (s->y < 0) s->y = 0;
    if (s->y + h > g_ctx->height) s->y = g_ctx->height - h;
}
static void pointer_motion(void *d, struct wl_pointer *p, uint32_t time,
                           wl_fixed_t x, wl_fixed_t y) {
    (void)d; (void)p; (void)time;
    g_ctx->ptr_x = wl_fixed_to_double(x);
    g_ctx->ptr_y = wl_fixed_to_double(y);
    if (g_grab_index >= 0) grabbed_follow_pointer();
    if (ringmenu_is_open(g_menu))
        ringmenu_motion(g_menu, (int)g_ctx->ptr_x, (int)g_ctx->ptr_y);
}
static void pointer_button(void *d, struct wl_pointer *p, uint32_t serial,
                           uint32_t time, uint32_t button, uint32_t state) {
    (void)d; (void)p; (void)serial; (void)time;
    bool pressed = state == WL_POINTER_BUTTON_STATE_PRESSED;

    if (g_ghost) {
        if (pressed) {
            g_ghost = false;
        }
        return;
    }

    if (g_menu && ringmenu_is_open(g_menu)) {
        int btn = -1;
        if (button == 0x110) btn = RINGMENU_BTN_LEFT;        
        else if (button == 0x111) btn = RINGMENU_BTN_RIGHT;  
        else if (button == 0x112) btn = RINGMENU_BTN_MIDDLE; 
        if (btn >= 0) {
            int r = ringmenu_button(g_menu, btn, pressed);
            if (r >= 0) menu_result(r);
        }
        return;
    }

    if (button == 0x112) { 
        if (pressed) {
            int hit = sprite_hit(g_ctx->ptr_x, g_ctx->ptr_y);
            if (hit >= 0 && !g_sprites[hit].popped) {
                Sprite *s = &g_sprites[hit];
                g_grab_index = hit;
                s->grabbed = true;
                s->grab_dx = (float)g_ctx->ptr_x - s->x;
                s->grab_dy = (float)g_ctx->ptr_y - s->y;
                g_press_x = g_ctx->ptr_x;
                g_press_y = g_ctx->ptr_y;
                clock_gettime(CLOCK_MONOTONIC, &g_press_ts);
            }
        } else if (g_grab_index >= 0) { 
            Sprite *s = &g_sprites[g_grab_index];
            s->grabbed = false;
            g_grab_index = -1;

            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double held = (now.tv_sec - g_press_ts.tv_sec) +
                          (now.tv_nsec - g_press_ts.tv_nsec) * 1e-9;
            double moved = fabs(g_ctx->ptr_x - g_press_x) + fabs(g_ctx->ptr_y - g_press_y);
            if (held >= 0.3 || moved >= 6.0) {
                if (g_mode == MODE_FLOAT) {
                    s->base_x = s->x;
                    s->phase = -0.9f * g_time;
                } else if (g_mode == MODE_SCURRY) {
                    s->fall_v = 0.f; 
                }
            }
        }
        return;
    }
    if (button == 0x111 && pressed) { 
        if (g_menu) {
            ringmenu_set_led(g_menu, MENU_STORM - 1,
                             g_storm_active ? RINGMENU_LED_ON : RINGMENU_LED_OFF);
            ringmenu_set_led(g_menu, MENU_GHOST - 1,
                             g_ghost ? RINGMENU_LED_ON : RINGMENU_LED_OFF);
            ringmenu_open(g_menu, (int)g_ctx->ptr_x, (int)g_ctx->ptr_y,
                          g_ctx->width, g_ctx->height);
        }
        return;
    }
    if (button != 0x110 || !pressed) return; 

    int hit = sprite_hit(g_ctx->ptr_x, g_ctx->ptr_y);
    if (g_trace) fprintf(stderr, "[trace] press at %.0f,%.0f hit=%d\n",
                         g_ctx->ptr_x, g_ctx->ptr_y, hit);
    if (hit >= 0 && !g_sprites[hit].popped)
        pop_sprite(&g_sprites[hit], (float)g_ctx->ptr_x, (float)g_ctx->ptr_y);
}
static void pointer_axis(void *d, struct wl_pointer *p, uint32_t time,
                         uint32_t axis, wl_fixed_t value) {
    (void)d; (void)p; (void)time; (void)axis; (void)value;
}
static void pointer_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void pointer_axis_source(void *d, struct wl_pointer *p, uint32_t s) { (void)d; (void)p; (void)s; }
static void pointer_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) {
    (void)d; (void)p; (void)t; (void)a;
}
static void pointer_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v) {
    (void)d; (void)p; (void)a; (void)v;
}
static const struct wl_pointer_listener pointer_listener = {
    pointer_enter, pointer_leave, pointer_motion, pointer_button, pointer_axis,
    pointer_frame, pointer_axis_source, pointer_axis_stop, pointer_axis_discrete
};

static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t fmt, int32_t fd, uint32_t size) {
    (void)d; (void)k; (void)fmt; (void)size;
    close(fd);
}
static void kb_enter(void *d, struct wl_keyboard *k, uint32_t serial,
                     struct wl_surface *sf, struct wl_array *keys) {
    (void)d; (void)k; (void)serial; (void)sf; (void)keys;
}
static void kb_leave(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *sf) {
    (void)d; (void)k; (void)serial; (void)sf;
}
static void kb_key(void *d, struct wl_keyboard *k, uint32_t serial,
                   uint32_t time, uint32_t key, uint32_t state) {
    (void)d; (void)k; (void)serial; (void)time;
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED) return;
    if (key == 1  || key == 16 )
        trigger_quit();
    else if (key == 57 )
        g_ghost = !g_ghost;
    else if (key == 50 )
        toggle_master_mute();
    else if (key == 103 )
        adjust_master_volume(0.05f);
    else if (key == 108 )
        adjust_master_volume(-0.05f);
}
static void kb_modifiers(void *d, struct wl_keyboard *k, uint32_t serial,
                         uint32_t dep, uint32_t lat, uint32_t lock, uint32_t grp) {
    (void)d; (void)k; (void)serial; (void)dep; (void)lat; (void)lock; (void)grp;
}
static void kb_repeat_info(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay) {
    (void)d; (void)k; (void)rate; (void)delay;
}
static const struct wl_keyboard_listener keyboard_listener = {
    kb_keymap, kb_enter, kb_leave, kb_key, kb_modifiers, kb_repeat_info
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
    Ctx *ctx = data;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !ctx->pointer) {
        ctx->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(ctx->pointer, &pointer_listener, ctx);
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !ctx->keyboard) {
        ctx->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(ctx->keyboard, &keyboard_listener, ctx);
    }
}
static void seat_name(void *data, struct wl_seat *seat, const char *name) {
    (void)data; (void)seat; (void)name;
}
static const struct wl_seat_listener seat_listener = { seat_capabilities, seat_name };

static void frame_done(void *data, struct wl_callback *cb, uint32_t time);
static const struct wl_callback_listener frame_listener = { frame_done };
static void frame_done(void *data, struct wl_callback *cb, uint32_t time) {
    (void)time;
    Ctx *ctx = data;
    wl_callback_destroy(cb);
    ctx->need_redraw = true;
}


static const char *vert_src =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "varying vec2 v_uv;\n"
    "void main() { v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";

static const char *frag_src =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "uniform float u_fade;\n"
    "uniform vec4 u_color_mult;\n"
    "void main() { gl_FragColor = texture2D(u_tex, v_uv) * u_color_mult * u_fade; }\n";

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(sh, sizeof(log), NULL, log);
        fprintf(stderr, "apngo: shader error: %s\n", log);
        exit(1);
    }
    return sh;
}

static void draw_tex_quad(GLuint tex, float x, float y, float w, float h,
                          int sw, int sh, int facing) {
    float x0 = 2.f * x / sw - 1.f;
    float x1 = 2.f * (x + w) / sw - 1.f;
    float y0 = 1.f - 2.f * y / sh;
    float y1 = 1.f - 2.f * (y + h) / sh;
    float u0 = facing < 0 ? 1.f : 0.f;
    float u1 = facing < 0 ? 0.f : 1.f;
    GLfloat verts[16] = {
        x0, y0, u0, 0.f,
        x1, y0, u1, 0.f,
        x0, y1, u0, 1.f,
        x1, y1, u1, 1.f,
    };
    glBindTexture(GL_TEXTURE_2D, tex);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), verts);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), verts + 2);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void draw_ghost_shadow(GLuint texture, float x, float y, float w, float h,
                              int screen_width, int screen_height) {
    enum { radius = 3 };
    const float sigma = 1.65f;
    const float two_sigma_squared = 2.0f * sigma * sigma;
    float weight_sum = 0.0f;

    for (int dy = -radius; dy <= radius; dy++)
        for (int dx = -radius; dx <= radius; dx++)
            weight_sum += expf(-(float)(dx * dx + dy * dy) / two_sigma_squared);

    /* Source-over blending makes several samples less opaque than their
     * arithmetic sum. Convert each normalized weight so a solid silhouette
     * still reaches Poingo's 40% black core. */
    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            float weight = expf(-(float)(dx * dx + dy * dy) / two_sigma_squared) / weight_sum;
            float alpha = 1.0f - powf(1.0f - GHOST_ICON_SHADOW_OPACITY, weight);
            if (g_color_loc >= 0) glUniform4f(g_color_loc, 0.0f, 0.0f, 0.0f, alpha);
            draw_tex_quad(texture, x + GHOST_ICON_SHADOW_OFFSET_X + dx,
                          y + GHOST_ICON_SHADOW_OFFSET_Y + dy,
                          w, h, screen_width, screen_height, 1);
        }
    }
    if (g_color_loc >= 0) glUniform4f(g_color_loc, 1.0f, 1.0f, 1.0f, 1.0f);
}

static void draw_sprite(const Sprite *s, float scale, int sw, int sh) {
    draw_tex_quad(s->textures[s->frame], s->x, s->y,
                  sprite_w(s, scale), sprite_h(s, scale), sw, sh, s->facing);
}

static void string_quad(const Sprite *s, float *x, float *y, float *w, float *h) {
    float bw = sprite_w(s, g_scale), bh = sprite_h(s, g_scale);
    *w = (float)g_str_anim.w;
    *h = (float)g_str_anim.h;
    *x = s->x + bw * BALLOON_TIE_X - *w * BALLOON_TIE_X;
    *y = s->y + bh * BALLOON_TIE_Y - *h * BALLOON_TIE_Y;
}

static void draw_string(const Sprite *s, int sw, int sh) {
    if (!g_str_tex || g_str_anim.nframes <= 0) return;
    float x, y, w, h;
    string_quad(s, &x, &y, &w, &h);
    int f = s->frame % g_str_anim.nframes;   
    draw_tex_quad(g_str_tex[f], x, y, w, h, sw, sh, 1);
}

static void draw_raspberry(const Sprite *s, int sw, int sh, float fade) {
    if (!g_rasp_tex || g_rasp_anim.w <= 0) return;
    float bw = sprite_w(s, g_scale), bh = sprite_h(s, g_scale);
    int nf = s->anim->nframes > 0 ? s->anim->nframes : 1;
    float bob = BALLOON_BOB_FRAC * sinf(6.2831853f * (float)s->frame / (float)nf);
    float cx = s->x + bw * BALLOON_BODY_CX;
    float cy = s->y + bh * (BALLOON_BODY_CY + bob);
    float lw = bw * RASPBERRY_FRAC;
    float lh = lw * (float)g_rasp_anim.h / (float)g_rasp_anim.w;
    if (g_fade_loc >= 0) glUniform1f(g_fade_loc, fade * RASPBERRY_ALPHA);
    draw_tex_quad(g_rasp_tex[0], cx - lw * 0.5f, cy - lh * 0.5f, lw, lh, sw, sh, 1);
    if (g_fade_loc >= 0) glUniform1f(g_fade_loc, fade);   
}


int main(int argc, char **argv) {
    (void)argc; (void)argv;                   
    const Mode mode = MODE_FLOAT;
    const int count = 30;
    const float scale = 1.0f, speed = 1.0f;  
    const bool pixel = false;
    int nfiles = 0;

    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    g_trace = getenv("APNGO_TRACE") != NULL;
    g_storm_countdown = roll_storm_countdown();

    audio_init();
    mark_sounds_dirty(BALLOON_SOUND_SCALE); 
    audio_pregen_async();                   

    Anim *anims = NULL;
    if (!balloon_generate_assets(&anims, &nfiles, &g_str_anim,
                                 &g_pop_anims, &g_npop_anims)) {
        fprintf(stderr, "balloons: failed to generate runtime assets\n");
        return 1;
    }
    if (!find_alpha_bounds(&anims[0], &g_ghost_balloon_bounds)) {
        fprintf(stderr, "balloons: unable to measure ghost icon source\n");
        return 1;
    }

    g_raspberry = load_raspberry_from_pi();

    Ctx ctx = {0};
    g_ctx = &ctx;
    ctx.running = true;
    ctx.width = 1280;
    ctx.height = 720;
    g_mode = mode;
    g_scale = scale;
    g_speed = speed;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    ctx.display = wl_display_connect(NULL);
    if (!ctx.display) { fprintf(stderr, "apngo: no Wayland display\n"); return 1; }
    ctx.registry = wl_display_get_registry(ctx.display);
    wl_registry_add_listener(ctx.registry, &registry_listener, &ctx);
    wl_display_roundtrip(ctx.display);
    if (!ctx.compositor || !ctx.wm_base) {
        fprintf(stderr, "apngo: compositor lacks wl_compositor/xdg_wm_base\n");
        return 1;
    }
    xdg_wm_base_add_listener(ctx.wm_base, &wm_base_listener, &ctx);
    if (ctx.seat) wl_seat_add_listener(ctx.seat, &seat_listener, &ctx);

    ctx.surface = wl_compositor_create_surface(ctx.compositor);
    wl_surface_set_opaque_region(ctx.surface, NULL);

    ctx.xdg_surface = xdg_wm_base_get_xdg_surface(ctx.wm_base, ctx.surface);
    xdg_surface_add_listener(ctx.xdg_surface, &xdg_surface_listener, &ctx);
    ctx.toplevel = xdg_surface_get_toplevel(ctx.xdg_surface);
    xdg_toplevel_add_listener(ctx.toplevel, &toplevel_listener, &ctx);
    xdg_toplevel_set_title(ctx.toplevel, "balloons");
    xdg_toplevel_set_app_id(ctx.toplevel, "balloons");
    xdg_toplevel_set_maximized(ctx.toplevel);

    if (ctx.deco_mgr) {
        ctx.deco =
            zxdg_decoration_manager_v1_get_toplevel_decoration(ctx.deco_mgr, ctx.toplevel);
        zxdg_toplevel_decoration_v1_set_mode(ctx.deco, ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
    }

    wl_surface_commit(ctx.surface);
    while (!ctx.configured && wl_display_dispatch(ctx.display) != -1) {}

    ctx.egl_display = eglGetDisplay((EGLNativeDisplayType)ctx.display);
    if (ctx.egl_display == EGL_NO_DISPLAY || !eglInitialize(ctx.egl_display, NULL, NULL)) {
        fprintf(stderr, "apngo: EGL init failed\n");
        return 1;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint cfg_attr[] = {
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_NONE
    };
    EGLConfig cfg;
    EGLint ncfg = 0;
    if (!eglChooseConfig(ctx.egl_display, cfg_attr, &cfg, 1, &ncfg) || ncfg < 1) {
        fprintf(stderr, "apngo: no ARGB EGL config\n");
        return 1;
    }
    EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    ctx.egl_context = eglCreateContext(ctx.egl_display, cfg, EGL_NO_CONTEXT, ctx_attr);
    ctx.egl_window = wl_egl_window_create(ctx.surface, ctx.width, ctx.height);
    ctx.egl_surface = eglCreateWindowSurface(ctx.egl_display, cfg,
                                             (EGLNativeWindowType)ctx.egl_window, NULL);
    if (ctx.egl_context == EGL_NO_CONTEXT || ctx.egl_surface == EGL_NO_SURFACE ||
        !eglMakeCurrent(ctx.egl_display, ctx.egl_surface, ctx.egl_surface, ctx.egl_context)) {
        fprintf(stderr, "apngo: EGL surface/context failed\n");
        return 1;
    }
    eglSwapInterval(ctx.egl_display, 0); 

    if (!getenv("APNGO_NO_DAMAGE")) {
        const char *eglexts = eglQueryString(ctx.egl_display, EGL_EXTENSIONS);
        if (eglexts) {
            if (strstr(eglexts, "EGL_EXT_swap_buffers_with_damage"))
                g_swap_damage = (PFNEGLSWAPBUFFERSWITHDAMAGEEXTPROC)
                    eglGetProcAddress("eglSwapBuffersWithDamageEXT");
            else if (strstr(eglexts, "EGL_KHR_swap_buffers_with_damage"))
                g_swap_damage = (PFNEGLSWAPBUFFERSWITHDAMAGEEXTPROC)
                    eglGetProcAddress("eglSwapBuffersWithDamageKHR");
            
            g_has_buffer_age = strstr(eglexts, "EGL_EXT_buffer_age") != NULL;
        }
    }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, compile_shader(GL_VERTEX_SHADER, vert_src));
    glAttachShader(prog, compile_shader(GL_FRAGMENT_SHADER, frag_src));
    glBindAttribLocation(prog, 0, "a_pos");
    glBindAttribLocation(prog, 1, "a_uv");
    glLinkProgram(prog);
    glUseProgram(prog);
    g_fade_loc = glGetUniformLocation(prog, "u_fade");
    g_color_loc = glGetUniformLocation(prog, "u_color_mult");
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "u_tex"), 0);
    if (g_fade_loc >= 0) glUniform1f(g_fade_loc, 1.0f);
    if (g_color_loc >= 0) glUniform4f(g_color_loc, 1.0f, 1.0f, 1.0f, 1.0f);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); 
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);

    {
        RingMenuItem items[4] = {
            { .label = "STORM", .led = RINGMENU_LED_OFF },
            { .label = "POP ALL" },
            { .label = "GHOST", .led = RINGMENU_LED_OFF },
            { .label = "QUIT" },
        };
        g_menu = ringmenu_create(items, 4);
        if (!g_menu) fprintf(stderr, "balloons: no ring menu\n");
    }
    glGenTextures(1, &g_menu_tex);
    glBindTexture(GL_TEXTURE_2D, g_menu_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    if (!create_needle_cursor(&ctx)) {
        fprintf(stderr, "balloons: no needle cursor, using the default pointer\n");
    }

    GLuint **anim_tex = calloc((size_t)nfiles, sizeof(GLuint *));
    if (!anim_tex) { fprintf(stderr, "out of memory\n"); return 1; }
    for (int i = 0; i < nfiles; i++) {
        anim_tex[i] = malloc((size_t)anims[i].nframes * sizeof(GLuint));
        glGenTextures(anims[i].nframes, anim_tex[i]);
        for (int f = 0; f < anims[i].nframes; f++) {
            glBindTexture(GL_TEXTURE_2D, anim_tex[i][f]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, anims[i].w, anims[i].h, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, anims[i].frames[f].rgba);
            GLint filt = pixel ? GL_NEAREST : GL_LINEAR;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filt);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            free(anims[i].frames[f].rgba);   
            anims[i].frames[f].rgba = NULL;
        }
    }

    g_str_tex = malloc((size_t)g_str_anim.nframes * sizeof(GLuint));
    if (!g_str_tex) { fprintf(stderr, "out of memory\n"); return 1; }
    glGenTextures(g_str_anim.nframes, g_str_tex);
    for (int f = 0; f < g_str_anim.nframes; f++) {
        glBindTexture(GL_TEXTURE_2D, g_str_tex[f]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_str_anim.w, g_str_anim.h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, g_str_anim.frames[f].rgba);
        GLint filt = pixel ? GL_NEAREST : GL_LINEAR;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filt);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        free(g_str_anim.frames[f].rgba);
        g_str_anim.frames[f].rgba = NULL;
    }

    if (g_raspberry && g_rasp_anim.nframes > 0) {
        g_rasp_tex = malloc(sizeof(GLuint));
        if (!g_rasp_tex) { fprintf(stderr, "out of memory\n"); return 1; }
        glGenTextures(1, g_rasp_tex);
        glBindTexture(GL_TEXTURE_2D, g_rasp_tex[0]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_rasp_anim.w, g_rasp_anim.h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, g_rasp_anim.frames[0].rgba);
        GLint filt = pixel ? GL_NEAREST : GL_LINEAR;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filt);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        free(g_rasp_anim.frames[0].rgba);
        g_rasp_anim.frames[0].rgba = NULL;
    }

    g_pop_tex = calloc((size_t)g_npop_anims, sizeof(GLuint *));
    for (int i = 0; i < g_npop_anims; i++) {
        g_pop_tex[i] = malloc((size_t)g_pop_anims[i].nframes * sizeof(GLuint));
        glGenTextures(g_pop_anims[i].nframes, g_pop_tex[i]);
        for (int f = 0; f < g_pop_anims[i].nframes; f++) {
            glBindTexture(GL_TEXTURE_2D, g_pop_tex[i][f]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_pop_anims[i].w, g_pop_anims[i].h, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, g_pop_anims[i].frames[f].rgba);
            GLint filt = pixel ? GL_NEAREST : GL_LINEAR;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filt);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            free(g_pop_anims[i].frames[f].rgba);
        }
    }
    
    {
        uint32_t *bg = ghost_icon_create_bg(true);
        if (bg) {
            glGenTextures(1, &g_ghost_bg_tex);
            glBindTexture(GL_TEXTURE_2D, g_ghost_bg_tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, GHOST_ICON_SIZE, GHOST_ICON_SIZE,
                         0, GL_RGBA, GL_UNSIGNED_BYTE, bg);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            free(bg);
        }
    }
    
    malloc_trim(0);   


    g_anims = anims;
    g_anim_tex = anim_tex;
    g_nanims = nfiles;
    g_nsprites = count;
    g_sprites = calloc((size_t)g_nsprites, sizeof(Sprite));
    int max_damage = 2 * g_nsprites;   
    g_damage_rects = calloc((size_t)max_damage * 4, sizeof(EGLint));
    g_cur_damage = calloc((size_t)max_damage, sizeof(Rect));
    g_sprite_rects = calloc((size_t)g_nsprites, sizeof(Rect));
    bool dmg_ok = g_damage_rects && g_cur_damage && g_sprite_rects;
    for (int i = 0; i < DAMAGE_HISTORY; i++) {
        g_damage_hist[i] = calloc((size_t)max_damage, sizeof(Rect));
        if (!g_damage_hist[i]) dmg_ok = false;
    }
    if (!g_sprites || !dmg_ok) { fprintf(stderr, "out of memory\n"); return 1; }
    for (int i = 0; i < g_nsprites; i++) {
        Sprite *s = &g_sprites[i];
        sprite_roll_anim(s);
        sprite_init(s, mode, scale, speed, ctx.width, ctx.height);
    }

    if (g_ghost) {
        struct wl_region *empty = wl_compositor_create_region(ctx.compositor);
        wl_surface_set_input_region(ctx.surface, empty);
        wl_region_destroy(empty);
    }

    if (getenv("BALLOONS_TEST_STORM")) start_storm();
    if (getenv("BALLOONS_TEST_THUNDER")) {
        start_storm();
        lightning_strike();
    }

    struct timespec ts_prev;
    clock_gettime(CLOCK_MONOTONIC, &ts_prev);
    float t = 0.f;
    float respawn_timer = 10.0f + frandf() * 5.0f;

    ctx.need_redraw = true;
    while (ctx.running) {
        if (g_signal_quit == 1 || (!ctx.running && g_quit_fade == 0.0)) {
            g_signal_quit = 2;
            ctx.running = true;
            g_quit_fade = 1.0;
        }
        
        if (g_quit_fade > 0.0 || g_startup_fade < 0.5) {
            ctx.need_redraw = true;
        }

        if (wl_display_dispatch_pending(ctx.display) == -1) break;
        if (!ctx.need_redraw) {
            if (wl_display_dispatch(ctx.display) == -1) break;
            continue;
        }
        ctx.need_redraw = false;

        struct timespec ts_now;
        clock_gettime(CLOCK_MONOTONIC, &ts_now);
        float dt = (float)(ts_now.tv_sec - ts_prev.tv_sec) +
                   (float)(ts_now.tv_nsec - ts_prev.tv_nsec) * 1e-9f;
        ts_prev = ts_now;
        if (dt > 0.1f) dt = 0.1f;
        t += dt;
        g_time = t;

        if (g_quit_fade > 0.0) {
            g_quit_fade -= dt;
            if (g_quit_fade <= 0.0) break;
        }

        if (g_startup_fade < 0.5) {
            g_startup_fade += dt;
            if (g_startup_fade > 0.5) g_startup_fade = 0.5;
        }

        if (g_mass_pop_active) {
            g_mass_pop_timer += dt;
            bool any_alive = false;
            for (int i = 0; i < g_nsprites; i++) {
                if (!g_sprites[i].dead && !g_sprites[i].popped) {
                    if (g_mass_pop_timer >= g_sprites[i].scheduled_pop_time) {
                        Sprite *s = &g_sprites[i];
                        float cx = s->x + sprite_w(s, scale) * 0.5f;
                        float cy = s->y + sprite_h(s, scale) * (32.f / 128.f);
                        pop_sprite(s, cx, cy);
                    }
                    any_alive = true; 
                } else if (!g_sprites[i].dead) {
                    any_alive = true;
                }
            }
            if (!any_alive) {
                if (g_mass_pop_quit) {
                    ctx.running = false;
                } else {
                    g_mass_pop_active = false;
                }
            }
        } else {
            respawn_timer -= dt;
            if (respawn_timer <= 0.f) {
                respawn_timer = 10.0f + frandf() * 5.0f;
                int live_count = 0;
                for (int i = 0; i < g_nsprites; i++) {
                    if (!g_sprites[i].dead && !g_sprites[i].popped) {
                        live_count++;
                    }
                }
                if (live_count < count) {
                    for (int i = 0; i < g_nsprites; i++) {
                        if (g_sprites[i].dead) {
                            Sprite *s = &g_sprites[i];
                            sprite_roll_anim(s);
                            sprite_init(s, mode, scale, speed, ctx.width, ctx.height);
                            s->y = (float)ctx.height;
                            s->dead = false;
                            s->popped = false;
                            s->grabbed = false;
                            break;
                        }
                    }
                }
            }
        }

        breeze_update(dt);
        for (int i = 0; i < g_nsprites; i++)
            sprite_update(&g_sprites[i], mode, dt, t, scale, speed, ctx.width, ctx.height);

        bool menu_open = ringmenu_is_open(g_menu);
        struct wl_region *region = wl_compositor_create_region(ctx.compositor);
        if (menu_open) {
            wl_region_add(region, 0, 0, ctx.width, ctx.height);
        } else if (g_ghost) {
            wl_region_add(region, ctx.width - GHOST_ICON_SIZE - GHOST_ICON_MARGIN, GHOST_ICON_MARGIN, GHOST_ICON_SIZE, GHOST_ICON_SIZE);
        } else {
            for (int i = 0; i < g_nsprites; i++) {
                Sprite *s = &g_sprites[i];
                if (s->dead || s->popped) continue;
                float w = sprite_w(s, scale), h = sprite_h(s, scale);
                float bx = s->x + w * (12.f / 72.f);
                float by = s->y + h * (4.f / 128.f);
                float bw = w * (48.f / 72.f);
                float bh = h * (60.f / 128.f);
                wl_region_add(region, (int)floorf(bx), (int)floorf(by),
                              (int)ceilf(bw), (int)ceilf(bh));
            }
        }
        wl_surface_set_input_region(ctx.surface, region);
        wl_region_destroy(region);

        if (menu_open || g_menu_was_open) g_full_damage = true;
        g_menu_was_open = menu_open;

        float veil = g_storm_alpha * (1.f - g_flash01);
        float white = 0.22f * g_flash01;
        float over_a = white + veil - white * veil;
        int over_w8 = (int)(white * 255.f + 0.5f);
        int over_a8 = (int)(over_a * 255.f + 0.5f);
        if (over_w8 != g_over_w8 || over_a8 != g_over_a8) {
            g_over_w8 = over_w8;
            g_over_a8 = over_a8;
            g_full_damage = true;
        }

        bool frame_was_full_damage = g_full_damage;

        g_ncur_damage = 0;
        for (int i = 0; i < g_nsprites; i++) {
            Sprite *s = &g_sprites[i];
            Rect nr = {0, 0, 0, 0};
            if (!s->dead) {
                float x0 = s->x, y0 = s->y;
                float x1 = s->x + sprite_w(s, scale), y1 = s->y + sprite_h(s, scale);
                if (!s->popped) {
                    float sx, sy, sw2, sh2;
                    string_quad(s, &sx, &sy, &sw2, &sh2);
                    if (sx < x0) x0 = sx;
                    if (sy < y0) y0 = sy;
                    if (sx + sw2 > x1) x1 = sx + sw2;
                    if (sy + sh2 > y1) y1 = sy + sh2;
                }
                nr.x = (int)floorf(x0) - 2;
                nr.y = (int)floorf(y0) - 2;
                nr.w = (int)ceilf(x1 - x0) + 4;
                nr.h = (int)ceilf(y1 - y0) + 4;
            }
            g_sprite_rects[i] = nr;
            Rect pr = {0, 0, 0, 0};
            if (s->pr_valid) pr = (Rect){ s->pr_x, s->pr_y, s->pr_w, s->pr_h };
            if (nr.w > 0 && pr.w > 0 && rect_intersects(&nr, &pr)) {
                rect_union(&pr, &nr);
                damage_push(pr, ctx.width, ctx.height);
            } else {
                if (nr.w > 0) damage_push(nr, ctx.width, ctx.height);
                if (pr.w > 0) damage_push(pr, ctx.width, ctx.height);
            }
            if (s->dead) {
                s->pr_valid = false;  
            } else {
                s->pr_x = nr.x; s->pr_y = nr.y;
                s->pr_w = nr.w; s->pr_h = nr.h;
                s->pr_valid = true;
            }
        }

        bool repaint_full = true;
        Rect repaint = {0, 0, 0, 0};
        if (g_has_buffer_age && !frame_was_full_damage) {
            EGLint age = 0;
            if (eglQuerySurface(ctx.egl_display, ctx.egl_surface, EGL_BUFFER_AGE_EXT, &age) &&
                age >= 1 && (int)age <= g_damage_hist_depth) {
                for (int d = 0; d < g_ncur_damage; d++)
                    rect_union(&repaint, &g_cur_damage[d]);
                for (int k = 0; k < (int)age; k++)
                    for (int d = 0; d < g_damage_hist_n[k]; d++)
                        rect_union(&repaint, &g_damage_hist[k][d]);
                if ((long long)repaint.w * repaint.h <=
                    (long long)ctx.width * ctx.height / 2)
                    repaint_full = false;
            }
        }

        glViewport(0, 0, ctx.width, ctx.height);
        static bool g_ghost_was = false;
        if (g_ghost != g_ghost_was) {
            g_full_damage = true;
            g_ghost_was = g_ghost;
        }

        float fade_val = 1.0f;
        if (g_startup_fade < 0.5) {
            fade_val *= (float)(g_startup_fade / 0.5);
            repaint_full = true;
        }
        if (g_quit_fade > 0.0) {
            fade_val *= (float)(g_quit_fade / 1.0);
            repaint_full = true; 
        }
        float actual_fade = fade_val * (g_ghost ? 0.4f : 1.0f);
        if (g_fade_loc >= 0) glUniform1f(g_fade_loc, actual_fade);
        glClearColor((float)g_over_w8 / 255.f * fade_val, (float)g_over_w8 / 255.f * fade_val,
                     (float)g_over_w8 / 255.f * fade_val, (float)g_over_a8 / 255.f * fade_val);
        if (repaint_full) {
            glClear(GL_COLOR_BUFFER_BIT);
            for (int i = 0; i < g_nsprites; i++) {
                Sprite *s = &g_sprites[i];
                if (s->dead) continue;
                draw_sprite(s, scale, ctx.width, ctx.height);
                if (!s->popped) {
                    if (s->raspberry) draw_raspberry(s, ctx.width, ctx.height, actual_fade);
                    draw_string(s, ctx.width, ctx.height);
                }
            }
        } else if (repaint.w > 0 && repaint.h > 0) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(repaint.x, ctx.height - (repaint.y + repaint.h), repaint.w, repaint.h);
            glClear(GL_COLOR_BUFFER_BIT);
            for (int i = 0; i < g_nsprites; i++) {
                Sprite *s = &g_sprites[i];
                if (s->dead || g_sprite_rects[i].w <= 0) continue;
                if (!rect_intersects(&repaint, &g_sprite_rects[i])) continue;
                draw_sprite(s, scale, ctx.width, ctx.height);
                if (!s->popped) {
                    if (s->raspberry) draw_raspberry(s, ctx.width, ctx.height, actual_fade);
                    draw_string(s, ctx.width, ctx.height);
                }
            }
            glDisable(GL_SCISSOR_TEST);
        }

        if (g_ghost) {
            if (g_fade_loc >= 0) glUniform1f(g_fade_loc, 1.0f);
            
            float bg_size = (float)GHOST_ICON_SIZE;
            float bg_x = (float)ctx.width - bg_size - (float)GHOST_ICON_MARGIN;
            float bg_y = (float)GHOST_ICON_MARGIN;
            if (g_ghost_bg_tex) {
                draw_tex_quad(g_ghost_bg_tex, bg_x, bg_y, bg_size, bg_size, ctx.width, ctx.height, 1);
            }
            
            /* Center and size the visible balloon body, not its 186x329
             * texture rectangle. The texture deliberately has transparent
             * space below the knot where the separately-rendered string
             * would attach; that space must not make the badge look high or
             * undersized. */
            const Anim *ghost_anim = &g_anims[0];
            const float source_body_height =
                (float)(g_ghost_balloon_bounds.max_y - g_ghost_balloon_bounds.min_y + 1);
            const float body_scale = (GHOST_ICON_SIZE * 0.78f) / source_body_height;
            const float bw = ghost_anim->w * body_scale;
            const float bh = ghost_anim->h * body_scale;
            const float source_center_x =
                (g_ghost_balloon_bounds.min_x + g_ghost_balloon_bounds.max_x + 1) * 0.5f;
            const float source_center_y =
                (g_ghost_balloon_bounds.min_y + g_ghost_balloon_bounds.max_y + 1) * 0.5f;
            const float bx = bg_x + bg_size * 0.5f - source_center_x * body_scale;
            const float by = bg_y + bg_size * 0.5f - source_center_y * body_scale;
            
            draw_ghost_shadow(anim_tex[0][0], bx, by, bw, bh, ctx.width, ctx.height);
            
            // Draw icon
            draw_tex_quad(anim_tex[0][0], bx, by, bw, bh, ctx.width, ctx.height, 1);
        }

        if (menu_open) {
            if (g_fade_loc >= 0) glUniform1f(g_fade_loc, 1.0f);
            int mx, my, mw, mh;
            ringmenu_rect(g_menu, &mx, &my, &mw, &mh);
            if (ringmenu_take_dirty(g_menu)) {
                if (!g_menu_scratch)
                    g_menu_scratch = malloc((size_t)mw * mh * 4);
                if (g_menu_scratch) {
                    memset(g_menu_scratch, 0, (size_t)mw * mh * 4);
                    ringmenu_draw(g_menu, g_menu_scratch, mw, mh, mx, my);
                    for (size_t i = 0; i < (size_t)mw * mh; i++) {
                        uint32_t c = g_menu_scratch[i];
                        uint32_t a = c >> 24;
                        if (a == 0) { g_menu_scratch[i] = 0; continue; }
                        if (a == 255) continue;
                        uint32_t r = ((c & 0xFF) * a + 127) / 255;
                        uint32_t g = (((c >> 8) & 0xFF) * a + 127) / 255;
                        uint32_t b = (((c >> 16) & 0xFF) * a + 127) / 255;
                        g_menu_scratch[i] = r | (g << 8) | (b << 16) | (a << 24);
                    }
                    glBindTexture(GL_TEXTURE_2D, g_menu_tex);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, mw, mh, 0,
                                 GL_RGBA, GL_UNSIGNED_BYTE, g_menu_scratch);
                }
            }
            float x0 = 2.f * mx / ctx.width - 1.f;
            float x1 = 2.f * (mx + mw) / ctx.width - 1.f;
            float y0 = 1.f - 2.f * my / ctx.height;
            float y1 = 1.f - 2.f * (my + mh) / ctx.height;
            GLfloat mverts[16] = {
                x0, y0, 0.f, 0.f,
                x1, y0, 1.f, 0.f,
                x0, y1, 0.f, 1.f,
                x1, y1, 1.f, 1.f,
            };
            glBindTexture(GL_TEXTURE_2D, g_menu_tex);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), mverts);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), mverts + 2);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }

        struct wl_callback *cb = wl_surface_frame(ctx.surface);
        wl_callback_add_listener(cb, &frame_listener, &ctx);
        if (g_swap_damage && !frame_was_full_damage) {
            EGLint n = 0;
            for (int d = 0; d < g_ncur_damage; d++) {
                const Rect *r = &g_cur_damage[d];
                g_damage_rects[n * 4 + 0] = r->x;
                g_damage_rects[n * 4 + 1] = ctx.height - r->y - r->h; 
                g_damage_rects[n * 4 + 2] = r->w;
                g_damage_rects[n * 4 + 3] = r->h;
                n++;
            }
            if (n == 0) {
                g_damage_rects[0] = 0; g_damage_rects[1] = 0;
                g_damage_rects[2] = 1; g_damage_rects[3] = 1;
                n = 1;
            }
            g_swap_damage(ctx.egl_display, ctx.egl_surface, g_damage_rects, n);
        } else {
            g_full_damage = false;
            eglSwapBuffers(ctx.egl_display, ctx.egl_surface);
        }

        if (g_damage_hist_depth < DAMAGE_HISTORY) g_damage_hist_depth++;
        Rect *oldest = g_damage_hist[DAMAGE_HISTORY - 1];
        for (int i = DAMAGE_HISTORY - 1; i > 0; i--) {
            g_damage_hist[i] = g_damage_hist[i - 1];
            g_damage_hist_n[i] = g_damage_hist_n[i - 1];
        }
        g_damage_hist[0] = oldest;
        if (frame_was_full_damage) {
            g_damage_hist[0][0] = (Rect){0, 0, ctx.width, ctx.height};
            g_damage_hist_n[0] = 1;
        } else {
            memcpy(g_damage_hist[0], g_cur_damage,
                   (size_t)g_ncur_damage * sizeof(Rect));
            g_damage_hist_n[0] = g_ncur_damage;
        }

        wl_display_flush(ctx.display);
    }

    ringmenu_destroy(g_menu);
    free(g_menu_scratch);
    if (ctx.cursor_surface) wl_surface_destroy(ctx.cursor_surface);
    if (ctx.cursor_buffer) wl_buffer_destroy(ctx.cursor_buffer);
    if (ctx.cursor_map) munmap(ctx.cursor_map, ctx.cursor_map_size);
    eglMakeCurrent(ctx.egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(ctx.egl_display, ctx.egl_surface);
    wl_egl_window_destroy(ctx.egl_window);
    eglDestroyContext(ctx.egl_display, ctx.egl_context);
    eglTerminate(ctx.egl_display);
    if (ctx.deco) {
        zxdg_toplevel_decoration_v1_destroy(ctx.deco);
    }
    if (ctx.deco_mgr) {
        zxdg_decoration_manager_v1_destroy(ctx.deco_mgr);
    }
    xdg_toplevel_destroy(ctx.toplevel);
    xdg_surface_destroy(ctx.xdg_surface);
    wl_surface_destroy(ctx.surface);
    wl_display_roundtrip(ctx.display);

    audio_shutdown_graceful(); 

    wl_display_disconnect(ctx.display);
    return 0;
}
