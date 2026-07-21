
#define main paint_main
#include "paint.c"
#undef main

#include "lodepng.h"

#define TW 860
#define TH 520

static void write_canvas_png(const PaintState *st, const char *name) {
    unsigned char *img = malloc((size_t)st->width * st->height * 4);
    if (!img) exit(1);
    for (int i = 0; i < st->width * st->height; i++) {
        uint32_t px = st->canvas[i];
        double a = (px >> 24) / 255.0;
        double r = (px & 0xFF) * a + 235.0 * (1.0 - a);
        double g = ((px >> 8) & 0xFF) * a + 235.0 * (1.0 - a);
        double b = ((px >> 16) & 0xFF) * a + 238.0 * (1.0 - a);
        img[i * 4 + 0] = (unsigned char)r;
        img[i * 4 + 1] = (unsigned char)g;
        img[i * 4 + 2] = (unsigned char)b;
        img[i * 4 + 3] = 255;
    }
    lodepng_encode32_file(name, img, st->width, st->height);
    free(img);
    printf("wrote %s\n", name);
}

static void write_cursor_png(PaintState *st, const char *name) {
    uint32_t *buf = malloc((size_t)CURSOR_BUF * CURSOR_BUF * 4);
    unsigned char *img = malloc((size_t)CURSOR_BUF * CURSOR_BUF * 4);
    if (!buf || !img) exit(1);
    render_cursor(st, buf);
    for (int i = 0; i < CURSOR_BUF * CURSOR_BUF; i++) {
        uint32_t px = buf[i];
        double a = (px >> 24) / 255.0;
        double r = ((px >> 16) & 0xFF) + 235.0 * (1.0 - a);
        double g = ((px >> 8) & 0xFF) + 235.0 * (1.0 - a);
        double b = (px & 0xFF) + 238.0 * (1.0 - a);
        img[i * 4 + 0] = (unsigned char)(r > 255 ? 255 : r);
        img[i * 4 + 1] = (unsigned char)(g > 255 ? 255 : g);
        img[i * 4 + 2] = (unsigned char)(b > 255 ? 255 : b);
        img[i * 4 + 3] = 255;
    }
    lodepng_encode32_file(name, img, CURSOR_BUF, CURSOR_BUF);
    free(buf);
    free(img);
    printf("wrote %s\n", name);
}

static float density_at(const PaintState *st, int x, int y) {
    return st->paint[((size_t)y * st->width + x) * 4];
}

static float field_at(const PaintState *st, int x, int y) {
    return st->field[(size_t)y * st->width + x];
}

static uint32_t canvas_at(const PaintState *st, int x, int y) {
    return st->canvas[(size_t)y * st->width + x];
}

int main(void) {
    PaintState st = {0};
    st.width = TW;
    st.height = TH;
    st.canvas = calloc((size_t)TW * TH, 4);
    st.paint = calloc((size_t)TW * TH * 4, sizeof(float));
    st.field = calloc((size_t)TW * TH, sizeof(float));
    st.color_map = malloc((size_t)TW * TH);
    st.spray_radius = SPRAY_R_DEFAULT;
    st.eraser_radius = ERASER_R_DEFAULT;
    st.splat_size = SPLAT_S_DEFAULT;
    for (size_t i = 0; i < PALETTE_SIZE; i++) st.palette[i] = DEFAULT_PALETTE[i];
    st.color_picker_slot = -1;
    if (!st.canvas || !st.paint || !st.field || !st.color_map) return 1;
    memset(st.color_map, COLOR_NONE, (size_t)TW * TH);
    alpha_lut_init();
    srand(42);

    const double dt = 1.0 / 120.0;
    int fails = 0;

    st.palette_index = 0;
    for (int i = 0; i < 18; i++) spray_stamp(&st, 130, 140, dt);
    float d_quick = density_at(&st, 130, 140);

    for (int i = 0; i < 240; i++) spray_stamp(&st, 130, 360, dt);
    float d_soak = density_at(&st, 130, 360);

    st.palette_index = 0;
    for (int i = 0; i < 100; i++) spray_stamp(&st, 380, 250, dt);
    st.palette_index = 2;
    for (int i = 0; i < 100; i++) spray_stamp(&st, 470, 250, dt);

    st.palette_index = 4;
    st.spray_radius = 55;
    for (int i = 0; i < 400; i++) {
        spray_stamp(&st, 690, 130, dt);
        drips_step(&st, dt);
    }
    int drips_seen = st.num_drips;
    for (int i = 0; i < 4000 && st.num_drips > 0; i++) drips_step(&st, dt);

    shade_region(&st, 0, 0, st.width - 1, st.height - 1);
    uint32_t quick_px = canvas_at(&st, 130, 140);
    uint32_t soak_px = canvas_at(&st, 130, 360);
    unsigned quick_a = quick_px >> 24;
    unsigned soak_a = soak_px >> 24;

    for (int i = 0; i <= 90; i++) {
        erase_stamp(&st, 60.0 + i, 360, 2.0 * dt);
    }
    float d_erased = density_at(&st, 130, 360);

    shade_region(&st, 0, 0, st.width - 1, st.height - 1);
    write_canvas_png(&st, "test_canvas.png");

    printf("quick puff:  density %.2f alpha %u\n", (double)d_quick, quick_a);
    printf("long soak:   density %.2f alpha %u\n", (double)d_soak, soak_a);
    printf("after erase: density %.4f\n", (double)d_erased);
    printf("drips while spraying: %d\n", drips_seen);
    if (!(quick_a > 20 && quick_a < 200)) { puts("FAIL: quick puff not translucent"); fails++; }
    if (!(soak_a > 230)) { puts("FAIL: soak not near-opaque"); fails++; }
    if (!(d_erased < 0.05f)) { puts("FAIL: eraser left paint behind"); fails++; }
    if (!(drips_seen > 0)) { puts("FAIL: heavy paint never dripped"); fails++; }

    uint32_t mix_px = canvas_at(&st, 425, 250);
    unsigned mr = mix_px & 0xFF, mg = (mix_px >> 8) & 0xFF;
    printf("mix zone: r %u g %u\n", mr, mg);
    if (!(mg > 60 && mr > 150)) { puts("FAIL: colors did not mix"); fails++; }

    clear_canvas(&st);
    st.dirty = false;

    st.palette_index = 2;   
    for (int i = 0; i < 60; i++) spray_stamp(&st, 430, 260, dt);
    float mist_before = density_at(&st, 430, 260);
    draw_splat(&st, 430, 260, 150, 5);   
    float mist_after = density_at(&st, 430, 260);
    flush_pending_shade(&st);
    uint32_t blob_px = canvas_at(&st, 430, 260);
    const PaintColor purple = DEFAULT_PALETTE[5];
    printf("blob center: %08x (flat should be a=ff b/g/r %02x%02x%02x)\n",
           blob_px, purple.b, purple.g, purple.r);
    printf("mist under blob: before %.2f after %.2f\n",
           (double)mist_before, (double)mist_after);
    if (!(mist_before > 0.1f)) { puts("FAIL: no mist laid before the blob"); fails++; }
    if (!(mist_after == 0.0f)) { puts("FAIL: blob did not bury the mist"); fails++; }
    if (blob_px != (purple.r | ((uint32_t)purple.g << 8) |
                    ((uint32_t)purple.b << 16) | 0xFF000000u)) {
        puts("FAIL: blob interior is not the flat palette color");
        fails++;
    }

    bool gloss_seen = false;
    for (int y = 260; y < 260 + 120 && !gloss_seen; y++) {
        for (int x = 430; x < 430 + 120; x++) {
            uint32_t px = canvas_at(&st, x, y);
            if ((px >> 24) == 255 &&
                (px & 0xFF) > (uint32_t)purple.r + 30 &&
                ((px >> 8) & 0xFF) > (uint32_t)purple.g + 30) {
                gloss_seen = true;
                break;
            }
        }
    }
    if (!gloss_seen) { puts("FAIL: no gloss line on the lit edge"); fails++; }

    st.palette_index = 2;
    for (int i = 0; i < 50; i++) spray_stamp(&st, 430, 260, dt);
    flush_pending_shade(&st);
    uint32_t dusted_px = canvas_at(&st, 430, 260);
    printf("dusted blob: %08x (mist density %.2f)\n",
           dusted_px, (double)density_at(&st, 430, 260));
    if (!((dusted_px >> 24) == 255)) { puts("FAIL: dusting made the blob translucent"); fails++; }
    if (!(((dusted_px >> 8) & 0xFF) > (uint32_t)purple.g + 15)) {
        puts("FAIL: mist over the blob left no tint"); fails++;
    }
    if (!(density_at(&st, 430, 260) > 0.1f)) {
        puts("FAIL: mist did not survive on top of the blob"); fails++;
    }

    draw_splat(&st, 520, 260, 130, 0);   
    flush_pending_shade(&st);
    if (!(density_at(&st, 520, 260) == 0.0f)) {
        puts("FAIL: second blob did not bury the mist it landed on"); fails++;
    }
    uint32_t second_px = canvas_at(&st, 520, 260);
    const PaintColor red = DEFAULT_PALETTE[0];
    if ((second_px & 0xFF) != red.r || ((second_px >> 24) != 255)) {
        puts("FAIL: second blob did not claim its footprint"); fails++;
    }

    write_canvas_png(&st, "test_splat.png");

    for (int i = 0; i < 600 && (field_at(&st, 430, 260) > (float)FIELD_ISO ||
                                density_at(&st, 430, 260) > 0.004f); i++) {
        erase_stamp(&st, 430, 260, 2.0 * dt);
    }
    flush_pending_shade(&st);
    printf("after scrubbing: field %.3f canvas %08x\n",
           (double)field_at(&st, 430, 260), canvas_at(&st, 430, 260));
    if (!(field_at(&st, 430, 260) <= (float)FIELD_ISO)) {
        puts("FAIL: eraser could not dissolve the enamel"); fails++;
    }
    if ((canvas_at(&st, 430, 260) >> 24) > 8) {
        puts("FAIL: dissolved enamel still visible"); fails++;
    }

    st.palette_index = 3;
    st.tool = TOOL_SPRAY;
    st.spray_radius = SPRAY_R_DEFAULT;
    write_cursor_png(&st, "test_cursor_default.png");
    st.spray_radius = SPRAY_R_MAX;
    write_cursor_png(&st, "test_cursor_big.png");
    st.spray_radius = SPRAY_R_MIN;
    write_cursor_png(&st, "test_cursor_small.png");
    st.tool = TOOL_SPLAT;
    st.splat_size = SPLAT_S_DEFAULT;
    write_cursor_png(&st, "test_cursor_brush.png");
    st.eraser_mode = true;
    st.eraser_radius = 90;
    write_cursor_png(&st, "test_cursor_eraser.png");

    printf(fails ? "%d FAILURES\n" : "all checks passed\n", fails);
    free(st.canvas);
    free(st.paint);
    free(st.field);
    free(st.color_map);
    return fails ? 1 : 0;
}
