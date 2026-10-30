/* Subtitles read back from the screen. The game draws speech ("ANYA: ...") in a tiny bitmap font, unreadable on
 * a phone. Each glyph is a pre-transformed quad whose texture rectangle identifies the character (one font atlas
 * per font, glyphs packed); the profile's table (PROFILE_GLYPHS, learned from glyph logs and the game's own text
 * files) maps rectangles to characters. Per frame the recorder hands every small 2D quad here; at Present the
 * glyphs are grouped into lines, spaces inferred from gaps, and the lines that start with a speaker ("NAME: ")
 * plus the lines that continue them become the current subtitle, which the page reads (rc_subtitle) and shows in
 * a readable caption. */
#define RC_WANT_GLYPHS   /* the profile includes its glyph table here only */
#include "win32.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define SUB_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define SUB_EXPORT
#endif

#ifdef PROFILE_GLYPHS
#define NGLYPH_TAB (sizeof k_glyphs / sizeof k_glyphs[0])
#endif

typedef struct { float x0, x1, yc, h; char ch; uint32_t col; } Gl;   /* positions in units of the game screen's height */
static float g_aspect = 4.f / 3.f;   /* the game screen's width / height */
static Gl g_gl[1024];
static int g_ngl;
static char g_sub[2][512];
static volatile int g_sub_idx, g_sub_seq;
/* where the game drew it (fractions of its screen: left, top, right, bottom, the line pitch) and its colour
   (ARGB), for the page to draw its larger copy in the same place and style */
static float g_box[2][5];
static uint32_t g_col[2];

void subtitle_glyph(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t col, float aspect) {
    if (aspect > 0.5f) g_aspect = aspect;
#ifdef PROFILE_GLYPHS
    if (g_ngl >= (int)(sizeof g_gl / sizeof g_gl[0])) return;
    int a = (int)lroundf(u0 * 1024.f), b = (int)lroundf(v0 * 1024.f), c = (int)lroundf(u1 * 1024.f), d = (int)lroundf(v1 * 1024.f);
    for (size_t i = 0; i < NGLYPH_TAB; i++)
        if (k_glyphs[i].u0 == a && k_glyphs[i].v0 == b && k_glyphs[i].u1 == c && k_glyphs[i].v1 == d) {
            g_gl[g_ngl++] = (Gl){ x0, x1, (y0 + y1) / 2, y1 - y0, k_glyphs[i].ch, col };
            return;
        }
#else
    (void)x0; (void)y0; (void)x1; (void)y1; (void)u0; (void)v0; (void)u1; (void)v1; (void)col; (void)aspect;
#endif
}

static int by_line(const void *pa, const void *pb) {
    const Gl *a = pa, *b = pb;
    if (fabsf(a->yc - b->yc) > 0.6f * fmaxf(a->h, b->h)) return a->yc < b->yc ? -1 : 1;
    return a->x0 < b->x0 ? -1 : a->x0 > b->x0;
}
static int speaker_line(const char *s) {   /* "NAME: " or "NAME NAME: " at the start */
    int i = 0;
    while (s[i] && (isupper((unsigned char)s[i]) || s[i] == ' ' || s[i] == '.') && i < 24) i++;
    return i >= 2 && s[i] == ':' && isupper((unsigned char)s[0]);
}

void subtitle_present(void) {
    char out[512]; int on = 0;
    float sbx0 = 0, sby0 = 0, sbx1 = 0, sby1 = 0, spitch = 0; uint32_t scol = 0;
    if (g_ngl >= 4) {
        qsort(g_gl, (size_t)g_ngl, sizeof g_gl[0], by_line);
        char line[256]; int ln = 0, in_sub = 0;
        float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f, pitch = 0, lastyc = -1; uint32_t col = 0;
        float lx0 = 1e9f, lx1 = -1e9f; uint32_t lcol = 0;   /* this line's extent and colour */
        float lyc = g_gl[0].yc, lh = g_gl[0].h, last_x1 = -1e9f, sub_yc = -1e9f;
        for (int i = 0; i <= g_ngl; i++) {
            int newline = i == g_ngl || fabsf(g_gl[i].yc - lyc) > 0.6f * fmaxf(lh, g_gl[i].h);
            if (newline) {
                line[ln] = 0;
                if (ln >= 3) {
                    if (speaker_line(line)) { in_sub = 1; sub_yc = lyc; if (on && on < (int)sizeof out - 2) out[on++] = '\n'; }
                    else if (in_sub && lyc - sub_yc > 3.5f * lh) in_sub = 0;   /* too far below: something else */
                    if (in_sub) {
                        if (lx0 < bx0) bx0 = lx0; if (lx1 > bx1) bx1 = lx1;
                        if (lyc - lh / 2 < by0) by0 = lyc - lh / 2; if (lyc + lh / 2 > by1) by1 = lyc + lh / 2;
                        if (lastyc >= 0 && lyc - lastyc > pitch) pitch = lyc - lastyc;
                        lastyc = lyc;
                        if (!col) col = lcol;
                        if (on && out[on - 1] != '\n' && on < (int)sizeof out - 2) out[on++] = ' ';
                        for (int k = 0; k < ln && on < (int)sizeof out - 1; k++) out[on++] = line[k];
                        sub_yc = lyc;
                    }
                }
                if (i == g_ngl) { sbx0 = bx0; sby0 = by0; sbx1 = bx1; sby1 = by1; spitch = pitch; scol = col; break; }
                ln = 0; lyc = g_gl[i].yc; lh = g_gl[i].h; last_x1 = -1e9f; lx0 = 1e9f; lx1 = -1e9f; lcol = 0;
            }
            const Gl *g = &g_gl[i];
            if (g->x0 < lx0) lx0 = g->x0; if (g->x1 > lx1) lx1 = g->x1; if (g->h > lh) lh = g->h;
            if (!lcol) lcol = g->col;
            /* a gap wider than a third of the font's capital height is a space (heights vary per glyph: use the line's) */
            float cap = fmaxf(lh, g->h);
            if (last_x1 > -1e8f && g->x0 - last_x1 > 0.28f * cap && ln < (int)sizeof line - 2) line[ln++] = ' ';
            if (ln < (int)sizeof line - 1) line[ln++] = g->ch;
            last_x1 = g->x1;
        }
    }
    out[on] = 0;
    int next = !g_sub_idx;
    memcpy(g_sub[next], out, (size_t)on + 1);
    if (on) { g_box[next][0] = sbx0 / g_aspect; g_box[next][1] = sby0; g_box[next][2] = sbx1 / g_aspect; g_box[next][3] = sby1; g_box[next][4] = spitch; g_col[next] = scol; }
    else memcpy(g_box[next], g_box[g_sub_idx], sizeof g_box[0]);
    if (strcmp(g_sub[next], g_sub[g_sub_idx])) { g_sub_idx = next; g_sub_seq++; }
    if (getenv("RC_SUB_LOG") && on && g_sub_seq) { static int last; if (last != g_sub_seq) { last = g_sub_seq; fprintf(stderr, "[sub] %s\n", out); } }
    g_ngl = 0;
}

/* the page: the current subtitle (empty when none) */
SUB_EXPORT const char *rc_subtitle(void) { return g_sub[g_sub_idx]; }
/* and where: left, top, right, bottom, line pitch (fractions of the game's screen), then the colour (ARGB) */
SUB_EXPORT const float *rc_subtitle_box(void) { return g_box[g_sub_idx]; }
SUB_EXPORT uint32_t rc_subtitle_color(void) { return g_col[g_sub_idx]; }
