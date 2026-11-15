/* Space Destroyer 3D - Nintendo DS backend.
 * Top screen (main engine): the game as textured quads on the 3D hardware, one per draw list item, painter's order.
 * Bottom screen (sub engine): 8 bpp bitmap with HUD + touch menu, redrawn only where something changed.
 * World rendering is integer only: every vertex coordinate comes from fixed point maths, so there is no NaN path like
 * the one that hung the 3DS GPU. */
#include <nds.h>
#include <fat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "game.h"
#include "audio.h"
#include "textures_bin.h"
#include "tex_palettes_bin.h"
typedef struct { uint8_t h; const uint8_t *px; const uint16_t (*tab)[2]; } UiFont;
typedef struct { uint16_t w, h; const uint8_t *px; } UiImage;
#include "ds_tex_gen.h"
#include "ui_gen.h"

#define FRAME_HZ 59.8261
#define STAGE_TO_TOUCH 1.875f     /* 480/256: stage units per touch screen pixel */

/* ------------------------------------------------------------------ top screen */
#define POLY_1DOT (1 << 13)
static void video_init(void) {
    videoSetMode(MODE_0_3D);
    videoSetModeSub(MODE_5_2D);
    vramSetBankA(VRAM_A_LCD); vramSetBankB(VRAM_B_LCD); vramSetBankC(VRAM_C_LCD); vramSetBankD(VRAM_D_LCD);
    vramSetBankE(VRAM_E_LCD);
    /* A-D are contiguous both as LCD memory and as texture slots 0-3 */
    DC_FlushRange(textures_bin, textures_bin_size);
    dmaCopyWords(3, textures_bin, VRAM_A, textures_bin_size);
    DC_FlushRange(tex_palettes_bin, tex_palettes_bin_size);
    dmaCopyHalfWords(3, tex_palettes_bin, VRAM_E, tex_palettes_bin_size);
    vramSetBankA(VRAM_A_TEXTURE_SLOT0); vramSetBankB(VRAM_B_TEXTURE_SLOT1);
    vramSetBankC(VRAM_C_TEXTURE_SLOT2); vramSetBankD(VRAM_D_TEXTURE_SLOT3);
    vramSetBankE(VRAM_E_TEX_PALETTE);
    /* H + I = exactly the 48 KB of a 256x192 8 bpp bitmap */
    vramSetBankH(VRAM_H_SUB_BG); vramSetBankI(VRAM_I_SUB_BG_0x06208000);
    bgInitSub(3, BgType_Bmp8, BgSize_B8_256x256, 0, 0);
    dmaCopyHalfWords(3, ui_palette, BG_PALETTE_SUB, sizeof ui_palette);
    lcdMainOnTop();

    glInit();
    glEnable(GL_TEXTURE_2D | GL_BLEND | GL_ALPHA_TEST);
    glAlphaFunc(0);
    glClearColor(0, 0, 0, 31);
    glClearPolyID(63);
    glClearDepth(GL_MAX_DEPTH);
    glViewport(0, 0, 255, 191);
    glMatrixMode(GL_TEXTURE); glLoadIdentity();
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    /* raw vertex values are pixels; later items get a larger z and win the depth test */
    glOrthof32(0, 256, 192, 0, -4096, 4096);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
}

static DrawItem items[MAX_DRAW];
static int nitems, npolys;

/* The hardware renders translucent polygons (all A3I5 textures count) in submission order thanks to manual sort.
   Each quad gets its own z, and polygon IDs cycle because a translucent pixel is not drawn over a translucent
   pixel that has the same ID. */
ITCM_CODE static void draw_top(void) {
    const GameView *v = game_view();
    GFX_CLEAR_COLOR = RGB15((int)(v->bg_r * 31.0f + 0.5f), (int)(v->bg_g * 31.0f + 0.5f), (int)(v->bg_b * 31.0f + 0.5f)) | (31 << 16) | (63 << 24);
    uint32_t cur_fmt = 0xffffffffu, cur_pal = 0xffffffffu;
    int z = -2040, id = 0, polys = 0;
    for (int i = 0; i < nitems; i++) {
        const DrawItem *d = &items[i];
        int a5 = (d->alpha * 31 + 128) >> 8;
        z++;
        if (a5 <= 0) continue;     /* polygon alpha 0 means wireframe on this hardware */
        id = id >= 62 ? 0 : id + 1;
        int x0, y0, x1, y1, u = 0, tv = 0;
        uint32_t fmt = 0, pal = 0;
        uint32_t attr = POLY_ALPHA(a5) | POLY_CULL_NONE | POLY_ID(id) | POLY_MODULATION;
        uint16_t col = RGB15(31, 31, 31);
        if (d->kind == DI_FILL) {
            x0 = 0; y0 = 0; x1 = 256; y1 = 192; col = 0;
        } else {
            const DsCostume *c = &ds_costumes[d->costume];
            /* stage q12 -> screen q4: x * 8/15 (34953/65536) / 256 */
            int sx = 128 * 16 + (int)(((int64_t)d->x * 34953) >> 24);
            int sy = 96 * 16 - (int)(((int64_t)d->y * 34953) >> 24);
            int ox = (c->off_x * d->size) >> 12, oy = (c->off_y * d->size) >> 12;
            int w = (c->wq4 * d->size) >> 12, h = (c->hq4 * d->size) >> 12;
            x0 = (sx + ox + 8) >> 4; y0 = (sy + oy + 8) >> 4;
            int pw = (w + 8) >> 4, ph = (h + 8) >> 4;
            if (pw < 1) pw = 1;
            if (ph < 1) ph = 1;
            x1 = x0 + pw; y1 = y0 + ph;
            if (x1 <= 0 || x0 >= 256 || y1 <= 0 || y0 >= 192) continue;
            if (c->solid && d->size < 6144) {
                col = c->solid;
                attr |= POLY_1DOT;
            } else {
                int m = c->first + c->nmips - 1;       /* smallest mip that is big enough for this size */
                while (m > c->first && ds_mips[m].uw * 16 < w) m--;
                const DsMip *t = &ds_mips[m];
                fmt = t->fmt;
                pal = c->pal;
                if (c->nhues == 2) pal += (d->hue >= 25) * c->hue_step;
                else if (c->nhues > 2) pal += ((d->hue * c->nhues) / 200) * c->hue_step;
                u = t->uw << 4; tv = t->uh << 4;
            }
        }
        GFX_POLY_FORMAT = attr;
        if (fmt != cur_fmt) { GFX_TEX_FORMAT = fmt; cur_fmt = fmt; }
        if (fmt && pal != cur_pal) { GFX_PAL_FORMAT = pal; cur_pal = pal; }
        GFX_COLOR = col;
        GFX_BEGIN = GL_QUAD;
        const uint32_t zw = (uint32_t)z & 0xffff, top = (uint32_t)y0 << 16, bot = (uint32_t)y1 << 16;
        const uint32_t xl = (uint32_t)x0 & 0xffff, xr = (uint32_t)x1 & 0xffff;
        GFX_TEX_COORD = 0;                       GFX_VERTEX16 = top | xl; GFX_VERTEX16 = zw;
        GFX_TEX_COORD = (uint32_t)tv << 16;      GFX_VERTEX16 = bot | xl; GFX_VERTEX16 = zw;
        GFX_TEX_COORD = ((uint32_t)tv << 16) | u; GFX_VERTEX16 = bot | xr; GFX_VERTEX16 = zw;
        GFX_TEX_COORD = u;                       GFX_VERTEX16 = top | xr; GFX_VERTEX16 = zw;
        GFX_END = 0;
        polys++;
    }
    npolys = polys;
    glFlush(GL_TRANS_MANUALSORT);
}

/* ------------------------------------------------------------------ bottom screen primitives */
static uint8_t fb[192][256] ALIGN(4);
static int dirty_x0 = 256, dirty_y0 = 192, dirty_x1 = 0, dirty_y1 = 0;
typedef struct { int x, y, w, h; } Rect;
static void mark(int x, int y, int w, int h) {
    if (x < dirty_x0) dirty_x0 = x;
    if (y < dirty_y0) dirty_y0 = y;
    if (x + w > dirty_x1) dirty_x1 = x + w;
    if (y + h > dirty_y1) dirty_y1 = y + h;
}
static void ui_flush(void) {
    if (dirty_x1 <= dirty_x0 || dirty_y1 <= dirty_y0) return;
    int x0 = dirty_x0 < 0 ? 0 : dirty_x0 & ~1, x1 = dirty_x1 > 256 ? 256 : (dirty_x1 + 1) & ~1;
    int y0 = dirty_y0 < 0 ? 0 : dirty_y0, y1 = dirty_y1 > 192 ? 192 : dirty_y1;
    u16 *vram = (u16 *)BG_BMP_RAM_SUB(0);
    for (int y = y0; y < y1; y++) {
        const u16 *src = (const u16 *)&fb[y][x0];
        u16 *dst = vram + y * 128 + x0 / 2;
        for (int n = (x1 - x0) / 2; n > 0; n--) *dst++ = *src++;
    }
    dirty_x0 = 256; dirty_y0 = 192; dirty_x1 = 0; dirty_y1 = 0;
}
static void ui_fill(int x, int y, int w, int h, int c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > 256) w = 256 - x;
    if (y + h > 192) h = 192 - y;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) memset(&fb[y + j][x], c, w);
    mark(x, y, w, h);
}
static void ui_rrect(int x, int y, int w, int h, int c) {     /* radius 2 */
    ui_fill(x + 2, y, w - 4, 1, c); ui_fill(x + 1, y + 1, w - 2, 1, c);
    ui_fill(x, y + 2, w, h - 4, c);
    ui_fill(x + 1, y + h - 2, w - 2, 1, c); ui_fill(x + 2, y + h - 1, w - 4, 1, c);
}
static int glyph_index(unsigned char ch) { return ch == 1 ? 0 : (ch < 32 || ch > 126 ? 1 : ch - 31); }
static int ui_text_w(const UiFont *f, const char *s) {
    int w = 0;
    for (; *s; s++) w += f->tab[glyph_index((unsigned char)*s)][1];
    return w;
}
static void ui_text(const UiFont *f, const char *s, int x, int y, int pair) {
    const int fg = ui_pairs[pair][0], r1 = UI_RAMP_BASE + pair * 2, r2 = r1 + 1;
    const int x_start = x;
    for (; *s; s++) {
        const uint16_t *t = f->tab[glyph_index((unsigned char)*s)];
        const uint8_t *p = f->px + t[0];
        int w = t[1];
        for (int j = 0; j < f->h; j++) for (int i = 0; i < w; i++) {
            int v = *p++, px = x + i, py = y + j;
            if (v && px >= 0 && px < 256 && py >= 0 && py < 192) fb[py][px] = (uint8_t)(v == 3 ? fg : (v == 2 ? r2 : r1));
        }
        x += w;
    }
    mark(x_start, y, x - x_start, f->h);
}
static void ui_text_c(const UiFont *f, const char *s, int cx, int y, int pair) { ui_text(f, s, cx - ui_text_w(f, s) / 2, y, pair); }
static void ui_image(const UiImage *im, int x, int y) {
    for (int j = 0; j < im->h; j++) memcpy(&fb[y + j][x], im->px + j * im->w, im->w);
    mark(x, y, im->w, im->h);
}
static int in_rect(Rect r, int tx, int ty) { return tx >= r.x && tx < r.x + r.w && ty >= r.y && ty < r.y + r.h; }

/* ------------------------------------------------------------------ bottom screen widgets */
/* the 3DS layout x 0.8 (same shape: 320x240 -> 256x192) */
static const Rect r_btn[BTN_COUNT] = { { 5, 74, 78, 40 }, { 89, 69, 78, 50 }, { 173, 74, 78, 40 }, { 173, 141, 78, 45 } };
static const Rect r_slider = { 6, 149, 157, 32 }, r_slider_p = { 50, 149, 157, 32 };
static const Rect r_pause = { 6, 152, 77, 34 }, r_resume = { 24, 96, 96, 35 }, r_quit = { 136, 96, 96, 35 };

/* widget caches: a widget only redraws when what it shows changed */
typedef struct { char label[28], value[16]; int x, y, w; } MonCache;
enum { MC_SCORE, MC_AMMO, MC_TIMER, MC_SPEED, MC_WRS, MC_WRT, MC_FPS, MC_COUNT };
static MonCache mc[MC_COUNT];
static int ui_mode = -1, ui_btn_co[BTN_COUNT], ui_btn_hov[BTN_COUNT], ui_music = -1, ui_slider = -1, ui_hard = -1, ui_rec = -1;
static int music_available, records_status;   /* shown on the title screen; records_status: 1 = saved to card */

static void monitor(MonCache *c, int x, int y, const char *label, const char *value, int minw) {
    if (!strcmp(c->value, value)) return;
    int lw = ui_text_w(&font_label, label), vw = ui_text_w(&font_label, value);
    int valw = vw + 6 < 29 ? 29 : vw + 6;
    int w = 3 + 3 + lw + 5 + valw + 3; if (w < minw) w = minw;
    if (c->w > w) ui_fill(x, y, c->w, 18, UI_BG);   /* value got narrower: clean up */
    c->w = w; strncpy(c->value, value, sizeof c->value - 1);
    const int h = 18;
    ui_rrect(x, y, w, h, UI_MONBRD);
    ui_rrect(x + 1, y + 1, w - 2, h - 2, UI_MONBG);
    ui_text(&font_label, label, x + 5, y + 3, PAIR_LABEL_MONBG);
    int vx = x + w - 3 - valw;
    ui_rrect(vx, y + 3, valw, h - 6, UI_ORANGE);
    ui_text(&font_label, value, vx + (valw - vw) / 2, y + 3, PAIR_WHITE_ORANGE);
}

static void button_box(Rect r, const char *label) {
    ui_rrect(r.x - 1, r.y - 1, r.w + 2, r.h + 2, UI_BLACK);
    ui_fill(r.x + 1, r.y + 1, r.w - 2, r.h - 2, UI_WHITE);
    ui_text_c(&font_pixel, label, r.x + r.w / 2, r.y + r.h / 2 - font_pixel.h / 2, PAIR_BLACK_WHITE);
}

static void draw_slider(Rect r, int value) {
    if (value == ui_slider) return;
    ui_slider = value;
    char v[16]; siprintf(v, "%d", value);
    ui_rrect(r.x, r.y, r.w, r.h, UI_MONBRD);
    ui_rrect(r.x + 1, r.y + 1, r.w - 2, r.h - 2, UI_MONBG);
    ui_text(&font_label, "Number of stars", r.x + 5, r.y + 2, PAIR_LABEL_MONBG);
    int vw = ui_text_w(&font_label, v), valw = vw + 6 < 24 ? 24 : vw + 6;
    ui_rrect(r.x + r.w - 4 - valw, r.y + 2, valw, 13, UI_ORANGE);
    ui_text(&font_label, v, r.x + r.w - 4 - valw + (valw - vw) / 2, r.y + 2, PAIR_WHITE_ORANGE);
    int sx0 = r.x + 8, sx1 = r.x + r.w - 8, sy = r.y + 24, kx = sx0 + (sx1 - sx0) * value / 300;
    ui_rrect(sx0, sy - 2, sx1 - sx0, 4, UI_TRACK);
    if (kx > sx0) ui_rrect(sx0, sy - 2, kx - sx0 + 2, 4, UI_BLUE);
    ui_rrect(kx - 5, sy - 5, 10, 10, UI_WHITE);
    ui_rrect(kx - 4, sy - 4, 8, 8, UI_BLUE);
}

static const UiImage *btn_image(int co, int music) {
    switch (co) {
    case CO_PLAY: return &img_play; case CO_BACK: return &img_back; case CO_SELECTMODE: return &img_selectmode;
    case CO_HARDMODE: return &img_hardmode; case CO_WHATSNEW: return &img_whatsnew; case CO_NORMALMODE: return &img_normalmode;
    default: return music ? &img_togglemusic_h50 : &img_togglemusic;
    }
}

enum { UM_TITLE, UM_MODESEL, UM_CHANGELOG, UM_EASTER, UM_PAUSED, UM_PLAYING, UM_GAMEOVER };

static void fmt_ms(char *out, int ms) {           /* "12.34", like the Scratch monitors */
    if (ms % 1000 == 0) { siprintf(out, "%d", ms / 1000); return; }
    siprintf(out, "%d.%03d", ms / 1000, ms % 1000);
    char *p = out + strlen(out) - 1;
    while (*p == '0') *p-- = 0;
}

static void draw_bottom(void) {
    const GameView *v = game_view();
    int mode;
    if (v->state == GS_TITLE || v->state == GS_STARTING) mode = v->changelog_open ? UM_CHANGELOG : (v->mode_selection ? UM_MODESEL : UM_TITLE);
    else if (v->state == GS_EASTER) mode = UM_EASTER;
    else if (v->paused) mode = UM_PAUSED;
    else mode = v->state == GS_PLAYING ? UM_PLAYING : UM_GAMEOVER;
    if (mode != ui_mode) {
        ui_mode = mode;
        ui_fill(0, 0, 256, 192, UI_BG);
        memset(mc, 0, sizeof mc);
        for (int b = 0; b < BTN_COUNT; b++) { ui_btn_co[b] = -1; ui_btn_hov[b] = -1; }
        ui_music = -1; ui_slider = -1; ui_hard = -1; ui_rec = -1;
        switch (mode) {
        case UM_TITLE: case UM_MODESEL:
            ui_text_c(&font_big, "Space Destroyer 3D", 128, 5, PAIR_YELLOW_BG);
            ui_text_c(&font_pixel, mode == UM_MODESEL ? "Choose a mode, then Back" : "D-Pad + A or touch.  START: play", 128, 33, PAIR_DIM_BG);
            if (!music_available) ui_text_c(&font_pixel, "Music data damaged by the loader: no music", 128, 47, PAIR_YELLOW_BG);
            break;
        case UM_CHANGELOG:
            ui_text_c(&font_big, "Space Destroyer 3D", 128, 5, PAIR_YELLOW_BG);
            ui_text_c(&font_pixel, "What's new is on the top screen.", 128, 70, PAIR_YELLOW_BG);
            ui_text_c(&font_pixel, "Tap or press any button to close", 128, 90, PAIR_YELLOW_BG);
            break;
        case UM_EASTER:
            ui_text_c(&font_big, "???", 128, 70, PAIR_YELLOW_BG);
            ui_text_c(&font_pixel, "SELECT: back to title", 128, 160, PAIR_YELLOW_BG);
            break;
        case UM_PAUSED:
            ui_text_c(&font_big, "Game Paused", 128, 24, PAIR_YELLOW_BG);
            ui_text_c(&font_pixel, "START to resume, SELECT to quit", 128, 54, PAIR_YELLOW_BG);
            button_box(r_resume, "Resume");
            button_box(r_quit, "Quit to title");
            break;
        case UM_PLAYING:
            ui_text_c(&font_pixel, "Drag to steer, tap to shoot", 168, 120, PAIR_DIM_BG);
            button_box(r_pause, "Pause");
            break;
        case UM_GAMEOVER:
            ui_text_c(&font_big, "Game over", 128, 120, PAIR_YELLOW_BG);
            break;
        }
    }
    if (mode == UM_TITLE || mode == UM_MODESEL) {
        ButtonView bv[BTN_COUNT]; game_buttons(bv);
        for (int b = 0; b < BTN_COUNT; b++) {
            int co = bv[b].costume, hov = bv[b].hovered;
            int music_var = b == BTN_TOGGLEMUSIC ? v->player_music : 0;
            if (co != ui_btn_co[b] || hov != ui_btn_hov[b] || (b == BTN_TOGGLEMUSIC && music_var != ui_music)) {
                ui_btn_co[b] = co; ui_btn_hov[b] = hov;
                if (b == BTN_TOGGLEMUSIC) ui_music = music_var;
                Rect r = r_btn[b];
                ui_fill(r.x - 4, r.y - 4, r.w + 8, r.h + 8, UI_BG);
                const UiImage *im = btn_image(co, music_var);
                int x0 = r.x + r.w / 2 - im->w / 2, y0 = r.y + r.h / 2 - im->h / 2;
                if (hov) ui_rrect(x0 - 2, y0 - 2, im->w + 4, im->h + 4, UI_YELLOW);
                ui_image(im, x0, y0);
            }
        }
        if (mode == UM_TITLE) {
            draw_slider(r_slider, v->stars_slider);
            if (records_status != ui_rec) {
                ui_rec = records_status;
                ui_fill(0, 182, 170, 10, UI_BG);
                ui_text(&font_pixel, records_status ? "Records save to the card" : "No card access: records won't save", 6, 182, PAIR_DIM_BG);
            }
        }
        else if (v->hard_mode != ui_hard) {
            ui_hard = v->hard_mode;
            ui_fill(0, 118, 168, 74, UI_BG);
            ui_fill(0, 118, 256, 20, UI_BG);
            /* [SELECTED] under the active mode's button */
            Rect r = r_btn[v->hard_mode ? BTN_SELECTMODE : BTN_WHATSNEW];
            ui_image(&img_selected, r.x + r.w / 2 - img_selected.w / 2, r.y + r.h + 3);
            ui_text(&font_pixel, v->hard_mode ? "Hard: planets every frame," : "Normal: the original mode,", 6, 142, PAIR_YELLOW_BG);
            ui_text(&font_pixel, v->hard_mode ? "full speed, no stars." : "best to get started.", 6, 156, PAIR_YELLOW_BG);
        }
    } else if (mode == UM_PAUSED) {
        draw_slider(r_slider_p, v->stars_slider);
    } else if (mode == UM_PLAYING || mode == UM_GAMEOVER) {
        char b[16];
        siprintf(b, "%d", (int)v->score); monitor(&mc[MC_SCORE], 5, 6, "Score:", b, 0);
        siprintf(b, "%d", (int)v->ammo); monitor(&mc[MC_AMMO], 136, 6, "Ammo:", b, 115);
        fmt_ms(b, (int)(v->timer * 1000.0)); monitor(&mc[MC_TIMER], 5, 29, "Timer:", b, 0);
        fmt_ms(b, (int)(v->speed * 1000.0f + 0.5f)); monitor(&mc[MC_SPEED], 136, 29, "Ship speed:", b, 115);
        siprintf(b, "%d", (int)v->wr_score); monitor(&mc[MC_WRS], 5, 56, "\x01 World record score:", b, 0);
        fmt_ms(b, (int)(v->wr_timer * 1000.0)); monitor(&mc[MC_WRT], 5, 78, "\x01 World record timer:", b, 0);
    }
    if (mode == UM_PLAYING || mode == UM_GAMEOVER) {
#ifndef STRESS
        char f[20]; int t = (int)(v->fps * 10.0f + 0.5f);
        if (t % 10) siprintf(f, "FPS: %d.%d", t / 10, t % 10); else siprintf(f, "FPS: %d", t / 10);
        if (strcmp(mc[MC_FPS].value, f)) {
            strcpy(mc[MC_FPS].value, f);
            ui_fill(180, 178, 76, 12, UI_BG);
            ui_text(&font_label, f, 250 - ui_text_w(&font_label, f), 178, PAIR_GREY_BG);
        }
#endif
    }
}

/* ------------------------------------------------------------------ records (flashcart / SD card, if there is one) */
/* A bootleg loader may not patch DLDI, or patch it badly. So every write is read back, and the title screen tells the
   player whether records really reach the card. */
#define SAVE_DIR "/data/space-destroyer"
#define SAVE_FILE SAVE_DIR "/records.txt"
static int have_fat, records_ok;
static char saved_line[48];
static void records_line(char *b, double t, double s, int pm) { snprintf(b, 48, "%.3f %.0f %d\n", t, s, pm); }
/* writes the line and reads it back: 1 if the card really holds it now */
static int records_write(const char *line) {
    mkdir("/data", 0777); mkdir(SAVE_DIR, 0777);
    FILE *f = fopen(SAVE_FILE, "w");
    if (!f) return 0;
    int ok = fputs(line, f) >= 0;
    if (fclose(f)) ok = 0;
    char back[48] = "";
    f = fopen(SAVE_FILE, "r");
    if (!f) return 0;
    if (!fgets(back, sizeof back, f)) ok = 0;
    fclose(f);
    return ok && !strcmp(back, line);
}
static void load_records(double *t, double *s, int *pm) {
    *t = 687.19; *s = 1348; *pm = 1;
    if (!have_fat) return;
    FILE *f = fopen(SAVE_FILE, "r");
    double a, b; int c;
    if (f && fscanf(f, "%lf %lf %d", &a, &b, &c) == 3) {
        if (a > *t) *t = a;
        if (b > *s) *s = b;
        *pm = c;
        records_ok = 1;
    }
    if (f) fclose(f);
    records_line(saved_line, *t, *s, *pm);
    if (!records_ok) records_ok = records_write(saved_line);   /* first run: find out whether saving works at all */
}
static void save_records(void) {
    if (!have_fat) return;
    const GameView *v = game_view();
    char line[48];
    records_line(line, v->wr_timer, v->wr_score, v->player_music);
    if (!strcmp(line, saved_line)) return;   /* nothing new: spare the card */
    records_ok = records_write(line);
    if (records_ok) strcpy(saved_line, line);
}

/* ------------------------------------------------------------------ sleep mode */
/* Runs inside pmMainLoop() on the main thread, so never in the middle of a records write or a frame. The hardware
   switches off the screens and sound while the lid is closed. A running game comes back paused, like on the 3DS. */
static volatile int woke_up;
static void on_power_event(void *user, PmEvent event) {
    (void)user;
    if (event == PmEvent_OnSleep) {
        const GameView *v = game_view();
        if (v->state == GS_PLAYING && !v->paused) game_toggle_pause();
        audio_suspend();
    } else if (event == PmEvent_OnWakeup) {
        woke_up = 1;          /* audio restarts from the main loop, not from inside pmEnterSleep */
    }
}

/* ------------------------------------------------------------------ main */
static volatile uint32_t vblanks;
static void on_vblank(void) { vblanks++; }

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    irqSet(IRQ_VBLANK, on_vblank);
    irqEnable(IRQ_VBLANK);
    video_init();
    have_fat = fatInitDefault();
    music_available = audio_init();

    double wr_t, wr_s; int pm;
    load_records(&wr_t, &wr_s, &pm);
    records_status = records_ok;
    static PmEventCookie pm_cookie;
    pmAddEventHandler(&pm_cookie, on_power_event, NULL);
    /* boot time scrambled with the clock and the first vblanks count */
    game_init((uint32_t)time(NULL) * 2654435761u + vblanks + 1, wr_t, wr_s, pm);
    keysSetRepeat(0, 0);

    uint32_t last = vblanks;
    int nav_boot = 1;
    int touch_was = 0, touch_btn = -1, touch_on_ui = 0, slider_drag = 0;
    int prev_tx = 0, prev_ty = 0, have_prev = 0, last_tx = 0, last_ty = 0;
#ifdef STRESS
    int stress_peak = 0, stress_cnt = 0, stress_shown = 0, stress_late = 0, stress_frames = 0;
    uint32_t stress_t0 = vblanks;
#if STRESS < 3
    game_set_stars_slider(300);
#endif
#if STRESS == 4
    pmSetSleepAllowed(false);          /* see below */
#endif
#endif
    while (pmMainLoop()) {             /* also does the lid: without this the DS never sleeps */
#if defined(STRESS) && STRESS == 4
        /* test build: the real sleep handlers without the BIOS sleep itself, because melonDS 1.1 wakes from that unreliably
           (a stock calico ROM with none of this code hangs there too). Lets the resume path be checked deterministically. */
        if (keysCurrent() & KEY_LID) {
            on_power_event(NULL, PmEvent_OnSleep);
            u32 pc = REG_POWCNT; REG_POWCNT = pc & ~POWCNT_LCD;
            while (keysCurrent() & KEY_LID) swiWaitForVBlank();
            REG_POWCNT = pc;
            on_power_event(NULL, PmEvent_OnWakeup);
        }
#endif
        swiWaitForVBlank();
        ui_flush();                    /* still inside vblank: no tearing on the bottom screen */
        uint32_t now = vblanks;
        if (woke_up) { woke_up = 0; last = now; audio_resume(); }
        scanKeys();
        u32 kd = keysDown(), kh = keysHeld();
        touchPosition tp; touchRead(&tp);
        int touching = (kh & KEY_TOUCH) != 0;
        const GameView *v = game_view();
        GameInput in; memset(&in, 0, sizeof in);
        int on_title = v->state == GS_TITLE || v->state == GS_STARTING;

        in.left = !!(kh & KEY_LEFT); in.right = !!(kh & KEY_RIGHT);
        in.up = !!(kh & KEY_UP); in.down = !!(kh & KEY_DOWN);
        in.speed_up = !!(kh & KEY_R); in.speed_down = !!(kh & KEY_L);
        in.any_key = (kh & ~KEY_TOUCH & (KEY_A | KEY_B | KEY_X | KEY_Y | KEY_L | KEY_R | KEY_START | KEY_SELECT | KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)) != 0 || touching;
        /* the easter egg only listens on the menu: with A and B both shooting, ordinary play would enter it by accident */
        if (on_title) {
            in.egg_up = in.up; in.egg_down = in.down; in.egg_left = in.left; in.egg_right = in.right;
            in.egg_b = !!(kh & KEY_B); in.egg_a = !!(kh & KEY_A);
            in.egg_up_pressed = !!(kd & KEY_UP);
        }
        in.pause_pressed = !!(kd & KEY_START) && !on_title;
        in.quit_pressed = !!(kd & KEY_SELECT);
        in.back_pressed = !!(kd & KEY_B);
        in.music_key_pressed = !!(kd & KEY_X);
        in.nav_left = !!(kd & KEY_LEFT); in.nav_right = !!(kd & KEY_RIGHT);
        in.nav_up = !!(kd & KEY_UP); in.nav_down = !!(kd & KEY_DOWN);
        if (nav_boot && on_title) { in.nav_up = 1; nav_boot = 0; }    /* highlight Play from the start */
        if (on_title) {
            in.confirm_pressed = !!(kd & KEY_A);
            in.space = !!(kh & KEY_START);
        } else in.space = !!(kh & (KEY_A | KEY_B));

        /* touch: the resistive panel's last sample before the pen lifts is often off, so use the sample before it */
        if (touching) {
            if (have_prev) { last_tx = prev_tx; last_ty = prev_ty; }
            else { last_tx = tp.px; last_ty = tp.py; }
            prev_tx = tp.px; prev_ty = tp.py; have_prev = 1;
        } else have_prev = 0;
        int tx = last_tx, ty = last_ty;
        if (touching && !touch_was) {
            touch_on_ui = 0; touch_btn = -1;
            if (v->state == GS_TITLE && !v->changelog_open) {
                for (int b = 0; b < BTN_COUNT; b++) if (in_rect(r_btn[b], tx, ty)) { touch_btn = b; touch_on_ui = 1; }
                if (!v->mode_selection && in_rect(r_slider, tx, ty)) { slider_drag = 1; touch_on_ui = 1; }
            } else if (v->state == GS_PLAYING && !v->paused && in_rect(r_pause, tx, ty)) { game_toggle_pause(); touch_on_ui = 1; }
            else if (v->state == GS_PLAYING && v->paused) {
                touch_on_ui = 1;
                if (in_rect(r_resume, tx, ty)) game_toggle_pause();
                else if (in_rect(r_quit, tx, ty)) game_quit_to_title();
                else if (in_rect(r_slider_p, tx, ty)) slider_drag = 2;
            }
        }
        if (touching && slider_drag) {
            Rect r = slider_drag == 1 ? r_slider : r_slider_p;
            game_set_stars_slider((tx - (r.x + 8)) * 300 / (r.w - 16));
        }
        if (!touching && touch_was) {
            if (touch_btn >= 0 && in_rect(r_btn[touch_btn], tx, ty)) game_press_button(touch_btn);
            touch_btn = -1; slider_drag = 0;
        }
        if (!touching) touch_on_ui = 0;
        touch_was = touching;
        /* the last touched point keeps being reported after release (tap test, 3DS bug fix) */
        in.mouse_down = touching && !touch_on_ui;
        in.mouse_x = (tx - 128) * STAGE_TO_TOUCH;
        in.mouse_y = (96 - ty) * STAGE_TO_TOUCH;

#ifdef STRESS
        /* perf test: hold L so stars live longest (or hard mode), the game starts by itself, no game over */
#if STRESS < 3
        if ((int)(vblanks - stress_t0) > 120 && on_title) in.space = 1;
        in.speed_down = 1;
#endif
#endif
        double dt = (now - last) / FRAME_HZ;
        last = now;
        game_update(dt, &in);
        GameView *vw = (GameView *)game_view();
        /* music on/off is a saved setting too; never start a write once the lid is closing */
        static int last_pm = -1;
        if (last_pm >= 0 && last_pm != vw->player_music) vw->records_dirty = 1;
        last_pm = vw->player_music;
        if (vw->records_dirty && vw->state != GS_PLAYING && vw->state != GS_STARTING && !(keysCurrent() & KEY_LID)) {
            save_records();
            records_status = records_ok;
            vw->records_dirty = 0;
        }

        nitems = game_draw_list(items, MAX_DRAW);
        draw_top();
        draw_bottom();
        audio_update();
#ifdef STRESS
        /* lines of the 263-line frame used since vblank started at line 192 */
        int used = (REG_VCOUNT + 263 - 192) % 263;
        if (used > stress_peak) stress_peak = used;
        stress_frames++;
        if (vblanks - now > 0) stress_late++;
        if (++stress_cnt >= (STRESS >= 3 ? 6 : 120) && vw->state == GS_PLAYING) {
            extern int game_nobj(void);
            extern void game_ship(int *, int *);
            char b[64]; int sx, sy;
            game_ship(&sx, &sy);
            ui_fill(0, 96, 256, 44, UI_BG);
            extern char *fake_heap_end; extern void *sbrk(ptrdiff_t);
            siprintf(b, "ship %d %d heap free %dK", sx, sy, (int)(fake_heap_end - (char *)sbrk(0)) / 1024);
            ui_text(&font_pixel, b, 4, 96, PAIR_YELLOW_BG);
            siprintf(b, "peak %d%% obj %d polys %d", stress_peak * 100 / 263, game_nobj(), npolys);
            ui_text(&font_pixel, b, 4, 106, PAIR_YELLOW_BG);
            siprintf(b, "late %d/%d shown %d rd %d", stress_late, stress_frames, ++stress_shown, (int)GFX_RDLINES_COUNT);
            ui_text(&font_pixel, b, 4, 120, PAIR_YELLOW_BG);
            stress_peak = 0; stress_cnt = 0; stress_late = 0; stress_frames = 0;
        }
#endif
    }
    /* L+R+Start+Select or DSi power button: back to the loader */
    save_records();
    return 0;
}
