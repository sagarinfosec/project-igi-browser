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

typedef struct { float x0, x1, yc, h; char ch; } Gl;
static Gl g_gl[1024];
static int g_ngl;
static char g_sub[2][512];
static volatile int g_sub_idx, g_sub_seq;

void subtitle_glyph(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1) {
#ifdef PROFILE_GLYPHS
    if (g_ngl >= (int)(sizeof g_gl / sizeof g_gl[0])) return;
    int a = (int)lroundf(u0 * 1024.f), b = (int)lroundf(v0 * 1024.f), c = (int)lroundf(u1 * 1024.f), d = (int)lroundf(v1 * 1024.f);
    for (size_t i = 0; i < NGLYPH_TAB; i++)
        if (k_glyphs[i].u0 == a && k_glyphs[i].v0 == b && k_glyphs[i].u1 == c && k_glyphs[i].v1 == d) {
            g_gl[g_ngl++] = (Gl){ x0, x1, (y0 + y1) / 2, y1 - y0, k_glyphs[i].ch };
            return;
        }
#else
    (void)x0; (void)y0; (void)x1; (void)y1; (void)u0; (void)v0; (void)u1; (void)v1;
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
    if (g_ngl >= 4) {
        qsort(g_gl, (size_t)g_ngl, sizeof g_gl[0], by_line);
        char line[256]; int ln = 0, in_sub = 0;
        float lyc = g_gl[0].yc, lh = g_gl[0].h, last_x1 = -1e9f, sub_yc = -1e9f;
        for (int i = 0; i <= g_ngl; i++) {
            int newline = i == g_ngl || fabsf(g_gl[i].yc - lyc) > 0.6f * fmaxf(lh, g_gl[i].h);
            if (newline) {
                line[ln] = 0;
                if (ln >= 3) {
                    if (speaker_line(line)) { in_sub = 1; sub_yc = lyc; if (on && on < (int)sizeof out - 2) out[on++] = '\n'; }
                    else if (in_sub && lyc - sub_yc > 3.5f * lh) in_sub = 0;   /* too far below: something else */
                    if (in_sub) {
                        if (on && out[on - 1] != '\n' && on < (int)sizeof out - 2) out[on++] = ' ';
                        for (int k = 0; k < ln && on < (int)sizeof out - 1; k++) out[on++] = line[k];
                        sub_yc = lyc;
                    }
                }
                if (i == g_ngl) break;
                ln = 0; lyc = g_gl[i].yc; lh = g_gl[i].h; last_x1 = -1e9f;
            }
            const Gl *g = &g_gl[i];
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
    if (strcmp(g_sub[next], g_sub[g_sub_idx])) { g_sub_idx = next; g_sub_seq++; }
    if (getenv("RC_SUB_LOG") && on && g_sub_seq) { static int last; if (last != g_sub_seq) { last = g_sub_seq; fprintf(stderr, "[sub] %s\n", out); } }
    g_ngl = 0;
}

/* the page: the current subtitle (empty when none) */
SUB_EXPORT const char *rc_subtitle(void) { return g_sub[g_sub_idx]; }
