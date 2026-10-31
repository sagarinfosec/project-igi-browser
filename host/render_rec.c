/* Render recorder: the game-thread half of the renderer (see render_cmd.h).
 *
 * Implements the Direct3D backend interface by doing all CPU work here - expanding primitives to
 * lists, running vertex shaders, converting changed textures to RGBA, building uniforms and pipeline
 * keys - and emitting self-contained commands for the executor (render_wgpu.c). Natively every
 * command runs immediately; in the browser commands are queued per frame for the page's thread. */
#include "render.h"
#include "render_cmd.h"
#include "win32.h"   /* g_watch */
#include "vshader.h"

#include <math.h>
#include <sys/stat.h>
#include <unistd.h>

int texture_to_rgba8(uint32_t fmt, const uint8_t *src, uint32_t pitch, uint32_t w, uint32_t h, uint8_t *dst,
                     const uint8_t *palette);
extern uint8_t g_palettes[][1024];

/* WGPUPrimitiveTopology values (kept numeric so this file doesn't need webgpu.h) */
enum { TOPO_POINTS = 1, TOPO_LINES = 2, TOPO_TRIANGLES = 4 };

/* RC_INPUT "N:sig" (input.c): log a signature of every draw of frame N, to diff the native and browser
   recorders. Structure is hashed exactly; floats are quantized first so last-bit differences don't count. */
uint32_t g_cmd_sig_frame = 0xFFFFFFFFu;
static uint32_t fnv_qa(uint32_t h, const float *f, size_t n, float scale) {
    for (size_t i = 0; i < n; i++) {
        double v = (double)f[i] * scale;
        int32_t q = v != v ? 0x7FFFFFFF : v > 2e9 ? 2000000000 : v < -2e9 ? -2000000000 : (int32_t)lrint(v);
        h = (h ^ (uint32_t)q) * 16777619u;
    }
    return h;
}
static uint32_t sig_pos(const CVert *v, uint32_t n) { uint32_t h = 2166136261u; for (uint32_t i = 0; i < n; i++) h = fnv_qa(h, &v[i].x, 4, 64.f); return h; }
static uint32_t sig_tc(const CVert *v, uint32_t n) { uint32_t h = 2166136261u; for (uint32_t i = 0; i < n; i++) h = fnv_qa(h, &v[i].tc[0][0], 16, 4096.f); return h; }
static uint32_t sig_col(const CVert *v, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) { h = (h ^ v[i].diffuse) * 16777619u; h = (h ^ v[i].specular) * 16777619u; }
    return h;
}

/* ------------------------------------------------------------------ command buffer
 * One recorder for the whole process: Direct3D calls come from the game thread, but objects can be
 * released from others, so every entry point holds g_rec_lock (recursive). */

#include <pthread.h>
static pthread_mutex_t g_rec_lock;
static pthread_once_t g_rec_once = PTHREAD_ONCE_INIT;
static void rec_lock_init(void) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_rec_lock, &a);
}
static void rec_enter(void) { pthread_once(&g_rec_once, rec_lock_init); pthread_mutex_lock(&g_rec_lock); }
static void rec_leave(void) { pthread_mutex_unlock(&g_rec_lock); }

static uint8_t *g_cb;
static size_t g_cb_len, g_cb_cap;

static void *cmd_alloc(uint32_t type, size_t size) {
    size = (size + 7) & ~(size_t)7;
    if (g_cb_len + size > g_cb_cap) {
        g_cb_cap = (g_cb_len + size) * 2 + (1u << 20);
        g_cb = realloc(g_cb, g_cb_cap);
    }
    CmdHdr *h = (CmdHdr *)(g_cb + g_cb_len);
    memset(h, 0, size);
    h->type = type;
    h->size = (uint32_t)size;
    g_cb_len += size;
    return h;
}

#ifdef __EMSCRIPTEN__
/* browser: completed command chunks go to the page's thread (web_gpu.c) */
void web_queue_chunk(uint8_t *buf, size_t len, int ends_frame);
int web_queued_frames(void);
int web_gpu_ready(void);

static void cmd_end(void) {}

static void cmd_flush(int ends_frame) {
    if (!g_cb_len && !ends_frame) return;
    web_queue_chunk(g_cb, g_cb_len, ends_frame);   /* takes ownership */
    g_cb = NULL; g_cb_len = g_cb_cap = 0;
}
#else
/* native: run each command as soon as it is recorded */
static void cmd_end(void) {
    size_t off = 0;
    while (off < g_cb_len) {
        const CmdHdr *h = (const CmdHdr *)(g_cb + off);
        gpu_exec(h);
        off += h->size;
    }
    g_cb_len = 0;
}
static void cmd_flush(int ends_frame) { (void)ends_frame; }
#endif

/* ------------------------------------------------------------------ debugging aids
 * RC_SCREENSHOTS="120,600" saves /tmp/rc_frame_N.png; `touch /tmp/rc_snap` or RC_CAPTURE=N logs
 * every draw of a frame and saves the back buffer after each one (native only). */

static int g_snap_detail;
static uint32_t g_f_drawn;

/* In-mission flag: set while the 3D world is being drawn (any non-pre-transformed geometry), latched per
   frame at present. dinput.c reads it to invert mouse look only in a mission, not in the 2D menus (where the
   same relative mouse-Y drives the cursor the other way). */
int g_in_mission;
static int g_world_this_frame;
/* Paused: the game's clock (PROFILE_TICK) has not moved for a few frames while a world is being drawn: the
   Esc menu is up over the frozen world, and the mouse drives its cursor, not the view. */
int g_paused;
int g_map;   /* the map computer is open over a mission: the mouse drives its cursor, not the view (PROFILE_MAP_OPEN) */
static uint32_t g_tick_last; static int g_tick_still;
/* The zoom: the projection the world is drawn with. m11 (y scale = 1 / tan(fov_y / 2)) grows as the sniper
   scope or the binoculars zoom in. Latched per frame like g_in_mission; the page's scope toggle reads it
   (input.c input_zoom) and holds the game's Zoom keys until the view actually reaches the zoom it wants. */
float g_zoom;                       /* last frame's projection m11, 0 when no world was drawn */
static float g_zoom_this_frame;
static uint32_t g_proj_log_frame;   /* RC_PROJ_LOG=frame: log every distinct world projection of that frame */
static uint32_t g_trace_vs;

static int want_screenshot(uint32_t frame) {
    static uint32_t last_check;
    static int cap_frame = -1;
    if (cap_frame < 0) {
        cap_frame = getenv("RC_CAPTURE") ? atoi(getenv("RC_CAPTURE")) : 0;
        g_trace_vs = getenv("RC_TRACE_VS") ? (uint32_t)strtoul(getenv("RC_TRACE_VS"), NULL, 16) : 0;
    }
    if (cap_frame && frame + 1 == (uint32_t)cap_frame) { g_snap_detail = 1; return 0; }
    if (frame - last_check >= 15) {
        last_check = frame;
        if (!access("/tmp/rc_snap", F_OK)) {
            unlink("/tmp/rc_snap");
            g_snap_detail = 1;
            return 0;
        }
    }
    if (g_snap_detail) {
        g_snap_detail = 0;
        fprintf(stderr, "[snap] frame %u: %u draws\n", frame, g_f_drawn);
        return 1;
    }
    static const char *list = (const char *)1;
    if (list == (const char *)1) list = getenv("RC_SCREENSHOTS");
    if (!list) return 0;
    char tmp[64];
    snprintf(tmp, sizeof tmp, "%u", frame);
    for (const char *p = list; *p;) {
        size_t n = strcspn(p, ",");
        if (n == strlen(tmp) && !strncmp(p, tmp, n)) return 1;
        p += n + (p[n] == ',');
    }
    return 0;
}

/* ------------------------------------------------------------------ render target tracking */

static CmdTarget g_last_target;   /* zero: back buffer, not yet sent */
static int g_target_sent;
static uint32_t g_tgt_lw, g_tgt_lh;
static int g_rec_bb;        /* the current target is the back buffer */
/* Widescreen (render_wgpu.c g_wide, ddraw7.c: the game is told a display as wide as the window). A frame is
   drawn wide when the player wants it, the previous frame drew a 3D world and its 2D quads didn't cover the
   screen (the sniper scope and binocular masks do: those frames keep the 4:3 picture, squeezed back from the
   wide display, so the mask stays round). On a wide frame the game's field of view is widened by rc_aspect_x
   (runtime.h, read by the patched game code: ports/project-igi/patch.sh) and the executor draws the world over
   the whole window, with the game's 2D (laid out for the wide display) squeezed back into a 4:3 box. */
int g_wide;                 /* the player wants widescreen (render_wgpu.c rc_set_wide / RC_WIDE) */
int g_frame_wide;
static float g_rec_cover;   /* share of the game's screen covered by its 2D quads this frame */

static void select_target(Device *d) {
    const DevState *st = &d->st;
    Surface *rt = st->render_target && st->render_target != d->backbuffer ? com_data(st->render_target) : NULL;
    Surface *ds = st->depth_stencil && st->depth_stencil != d->depth ? com_data(st->depth_stencil) : NULL;
    CmdTarget t;
    memset(&t, 0, sizeof t);
    if (rt) { t.color_id = rt->gpu_id; t.color_w = rt->width; t.color_h = rt->height; t.lw = rt->width; t.lh = rt->height; }
    else { t.lw = d->width; t.lh = d->height; }
    if (ds) { t.depth_id = ds->gpu_id; t.depth_w = ds->width; t.depth_h = ds->height; }
    g_tgt_lw = t.lw; g_tgt_lh = t.lh;
    g_rec_bb = !rt;
    if (g_target_sent && !memcmp(&t.color_id, &g_last_target.color_id, sizeof t - sizeof t.h)) return;
    if (g_snap_detail)
        fprintf(stderr, "[target] color %u (%ux%u, logical %ux%u) depth %u\n", t.color_id, t.color_w, t.color_h, t.lw, t.lh,
                t.depth_id);
    CmdTarget *c = cmd_alloc(CMD_TARGET, sizeof *c);
    memcpy(&c->color_id, &t.color_id, sizeof t - sizeof t.h);
    cmd_end();
    g_last_target = t;
    g_target_sent = 1;
}

/* ------------------------------------------------------------------ vertices */

typedef struct { uint32_t stride, pos_type, has_normal, has_psize, has_diffuse, has_specular, ntex,
                 off_normal, off_diffuse, off_specular, off_tex[8]; } FvfLayout;

static void fvf_layout(uint32_t fvf, FvfLayout *L) {
    memset(L, 0, sizeof *L);
    uint32_t off = 0;
    L->pos_type = fvf & 0xE;
    switch (L->pos_type) {
    case 0x2: off = 12; break;              /* XYZ */
    case 0x4: off = 16; break;              /* XYZRHW */
    case 0x6: off = 16; break; case 0x8: off = 20; break; case 0xA: off = 24; break;   /* XYZB1-3 */
    case 0xC: off = 28; break; case 0xE: off = 32; break;
    }
    if (fvf & 0x10) { L->has_normal = 1; L->off_normal = off; off += 12; }
    if (fvf & 0x20) { L->has_psize = 1; off += 4; }
    if (fvf & 0x40) { L->has_diffuse = 1; L->off_diffuse = off; off += 4; }
    if (fvf & 0x80) { L->has_specular = 1; L->off_specular = off; off += 4; }
    L->ntex = (fvf >> 8) & 0xF;
    for (uint32_t i = 0; i < L->ntex && i < 8; i++) {
        L->off_tex[i] = off;
        uint32_t f = (fvf >> (16 + 2 * i)) & 3;
        off += f == 0 ? 8 : f == 1 ? 12 : f == 2 ? 16 : 4;
    }
    L->stride = off;
}

static void convert_vertex(const uint8_t *v, const FvfLayout *L, CVert *o) {
    float f[4] = { 0, 0, 0, 1 };
    memcpy(f, v, L->pos_type == 0x4 ? 16 : 12);
    o->x = f[0]; o->y = f[1]; o->z = f[2]; o->w = L->pos_type == 0x4 ? f[3] : 1.f;
    o->diffuse = 0xFFFFFFFFu; o->specular = 0;
    if (L->has_diffuse) memcpy(&o->diffuse, v + L->off_diffuse, 4);
    if (L->has_specular) memcpy(&o->specular, v + L->off_specular, 4);
    memset(o->tc, 0, sizeof o->tc);
    for (uint32_t t = 0; t < 4; t++) {
        o->tc[t][3] = 1.f;
        if (t < L->ntex) memcpy(o->tc[t], v + L->off_tex[t], 8);
    }
}

static void mat_mul(const float *a, const float *b, float *r) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a[i * 4 + k] * b[k * 4 + j];
            r[i * 4 + j] = s;
        }
}

static uint32_t pack_color(const float *f) {   /* RGBA floats -> D3DCOLOR (ARGB) */
    uint32_t b[4];
    for (int i = 0; i < 4; i++) { float v = f[i] < 0 ? 0 : f[i] > 1 ? 1 : f[i]; b[i] = (uint32_t)(v * 255.f + 0.5f); }
    return b[2] | b[1] << 8 | b[0] << 16 | b[3] << 24;
}

static void unpack_color(uint32_t c, float *o) {
    o[0] = ((c >> 16) & 255) / 255.f; o[1] = ((c >> 8) & 255) / 255.f; o[2] = (c & 255) / 255.f; o[3] = (c >> 24) / 255.f;
}

/* ------------------------------------------------------------------ fixed-function lighting and vertex fog
 * Direct3D's per-vertex lighting (LIGHTING with normals: material, ambient and up to 8 lights) and vertex fog
 * (the fog factor, from the eye distance, goes in the specular alpha), done here on the CPU for untransformed
 * vertices. Direct3D 7 games rely on both. */

static float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void xform_point(const float *m, const float *p, float *o) {
    for (int j = 0; j < 3; j++) o[j] = p[0] * m[j] + p[1] * m[4 + j] + p[2] * m[8 + j] + m[12 + j];
}
static void xform_dir(const float *m, const float *p, float *o) {
    for (int j = 0; j < 3; j++) o[j] = p[0] * m[j] + p[1] * m[4 + j] + p[2] * m[8 + j];
}
static void normalize3(float *v) {
    float l = sqrtf(dot3(v, v));
    if (l > 0) { v[0] /= l; v[1] /= l; v[2] /= l; }
}

static void light_and_fog(const DevState *st, const FvfLayout *L, const uint8_t *vb, uint32_t stride, const uint32_t *idx,
                          uint32_t n, CVert *cv) {
    int lighting = st->rs[137] && L->has_normal;
    int fog = st->rs[28] && (st->rs[35] || st->rs[140]);   /* table or vertex fog: computed from the eye distance */
    if (!lighting && !fog) return;
    const float *world = st->transform[256], *view = st->transform[2];
    float amb[4];
    unpack_color(st->rs[139], amb);
    const float *mat = st->material;   /* diffuse 0-3, ambient 4-7, specular 8-11, emissive 12-15 */
    int lights[8], nl = 0;
    for (int k = 0; k < 8; k++) if (st->light_enabled[k]) lights[nl++] = k;
    float fstart, fend, fdens;
    memcpy(&fstart, &st->rs[36], 4); memcpy(&fend, &st->rs[37], 4); memcpy(&fdens, &st->rs[38], 4);
    uint32_t fmode = st->rs[35] ? st->rs[35] : st->rs[140];
    int range = st->rs[48] != 0 && !st->rs[35];
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *v = vb + (size_t)idx[i] * stride;
        float p[3], pw[3];
        memcpy(p, v, 12);
        xform_point(world, p, pw);
        if (lighting) {
            float nrm[3], nw[3];
            memcpy(nrm, v + L->off_normal, 12);
            xform_dir(world, nrm, nw);
            normalize3(nw);
            /* material sources: COLORVERTEX takes diffuse/ambient/emissive from the vertex color when asked */
            float vd[4] = { 1, 1, 1, 1 }, vs[4] = { 0, 0, 0, 0 };
            if (L->has_diffuse) unpack_color(cv[i].diffuse, vd);
            if (L->has_specular) unpack_color(cv[i].specular, vs);
            int cvx = st->rs[141] != 0;
            const float *md = cvx && L->has_diffuse && st->rs[145] == 1 ? vd : cvx && L->has_specular && st->rs[145] == 2 ? vs : mat;
            const float *ma = cvx && L->has_diffuse && st->rs[147] == 1 ? vd : cvx && L->has_specular && st->rs[147] == 2 ? vs : mat + 4;
            const float *me = cvx && L->has_diffuse && st->rs[148] == 1 ? vd : cvx && L->has_specular && st->rs[148] == 2 ? vs : mat + 12;
            float col[3];
            for (int c = 0; c < 3; c++) col[c] = me[c] + amb[c] * ma[c];
            for (int k = 0; k < nl; k++) {
                const uint8_t *lt = st->light[lights[k]];
                uint32_t type;
                float ld[4], la[4], pos[3], dir[3], rng, falloff, a0, a1, a2, theta, phi;
                memcpy(&type, lt, 4); memcpy(ld, lt + 4, 16); memcpy(la, lt + 36, 16);
                memcpy(pos, lt + 52, 12); memcpy(dir, lt + 64, 12);
                memcpy(&rng, lt + 76, 4); memcpy(&falloff, lt + 80, 4); memcpy(&a0, lt + 84, 4); memcpy(&a1, lt + 88, 4);
                memcpy(&a2, lt + 92, 4); memcpy(&theta, lt + 96, 4); memcpy(&phi, lt + 100, 4);
                float tl[3], att = 1.f;
                if (type == 3) {   /* directional */
                    tl[0] = -dir[0]; tl[1] = -dir[1]; tl[2] = -dir[2];
                    normalize3(tl);
                } else {           /* point / spot */
                    tl[0] = pos[0] - pw[0]; tl[1] = pos[1] - pw[1]; tl[2] = pos[2] - pw[2];
                    float d = sqrtf(dot3(tl, tl));
                    if (rng > 0 && d > rng) continue;
                    if (d > 0) { tl[0] /= d; tl[1] /= d; tl[2] /= d; }
                    float den = a0 + a1 * d + a2 * d * d;
                    att = den > 0 ? 1.f / den : 1.f;
                    if (type == 2) {
                        float sd[3] = { dir[0], dir[1], dir[2] };
                        normalize3(sd);
                        float rho = -dot3(tl, sd), ct = cosf(theta * 0.5f), cp = cosf(phi * 0.5f);
                        if (rho <= cp) att = 0;
                        else if (rho < ct && ct > cp) att *= powf((rho - cp) / (ct - cp), falloff > 0 ? falloff : 1.f);
                    }
                }
                float ndl = dot3(nw, tl);
                if (ndl < 0) ndl = 0;
                for (int c = 0; c < 3; c++) col[c] += att * (la[c] * ma[c] + ndl * ld[c] * md[c]);
            }
            float out[4] = { col[0], col[1], col[2], md[3] };
            uint32_t a = cv[i].specular & 0xFF000000u;
            cv[i].diffuse = pack_color(out);
            cv[i].specular = a;   /* no specular highlights; keep the alpha (fog) */
        }
        if (fog) {
            float pv[3];
            xform_point(view, pw, pv);
            float d = range ? sqrtf(dot3(pv, pv)) : fabsf(pv[2]), f;
            if (fmode == 3) f = fend != fstart ? (fend - d) / (fend - fstart) : 1.f;   /* linear */
            else if (fmode == 1) f = expf(-fdens * d);
            else f = expf(-(fdens * d) * (fdens * d));
            f = f < 0 ? 0 : f > 1 ? 1 : f;
            cv[i].specular = (cv[i].specular & 0x00FFFFFFu) | (uint32_t)(f * 255.f + 0.5f) << 24;
        }
    }
}

/* ------------------------------------------------------------------ textures */

static uint32_t sampler_key(const uint32_t *tss) {
    uint32_t mag = tss[16] >= 2, min = tss[17] >= 2, mip = tss[18];
    uint32_t au = tss[13] & 7, av = tss[14] & 7;
    return mag | min << 1 | (mip & 3) << 2 | au << 4 | av << 7;   /* < 1024 */
}

/* send a texture's contents if they changed since the last upload */
/* DXT textures go to the GPU as BC blocks when it can sample them: 4-8x less GPU memory than RGBA8 (a match
   needs hundreds of MB uncompressed, more than some GPUs/browsers give a page). WebGPU wants whole blocks,
   so the base level must be a multiple of 4; other textures are converted. */
static uint32_t bc_format(const Texture *t) {
    if (!g_gpu_bc || !format_is_dxt(t->format) || t->lw[0] % 4 || t->lh[0] % 4) return TEXFMT_RGBA8;
    return t->format == FMT_DXT1 ? TEXFMT_BC1 : t->format <= FMT_DXT3 ? TEXFMT_BC2 : TEXFMT_BC3;
}

/* DirectDraw source color key: texels whose raw value lies in [ck_lo, ck_hi] become transparent */
static void apply_color_key(const Texture *t, uint32_t l, uint8_t *rgba) {
    uint32_t bpp = format_bpp(t->format);
    if (format_is_dxt(t->format) || bpp < 8) return;
    for (uint32_t y = 0; y < t->lh[l]; y++) {
        const uint8_t *row = HP(t->mem[0][l] + y * t->pitch[l]);
        for (uint32_t x = 0; x < t->lw[l]; x++) {
            uint32_t v = 0;
            memcpy(&v, row + x * (bpp / 8), bpp / 8);
            if (bpp == 32) v &= 0x00FFFFFFu;
            if (v >= (t->ck_lo & (bpp == 32 ? 0xFFFFFFu : 0xFFFFFFFFu)) && v <= (t->ck_hi & (bpp == 32 ? 0xFFFFFFu : 0xFFFFFFFFu)))
                rgba[((size_t)y * t->lw[l] + x) * 4 + 3] = 0;
        }
    }
}

/* Bumped by the renderer after it replaced a lost GPU device (render_wgpu.c): every texture is sent again on its
   next use, from the copy the game keeps in its own memory. */
volatile uint32_t g_gpu_gen;
static void sync_texture(Device *d, Texture *t) {
    uint32_t gen = __atomic_load_n(&g_gpu_gen, __ATOMIC_ACQUIRE);
    if (t->sent_version == t->version && t->sent_gen == gen) return;
    t->sent_gen = gen;
    uint32_t fmt = bc_format(t), bs = fmt == TEXFMT_BC1 ? 8 : 16;
    size_t total = 0;
    for (uint32_t l = 0; l < t->levels && l < 14; l++)
        total += fmt ? (size_t)((t->lw[l] + 3) / 4) * ((t->lh[l] + 3) / 4) * bs : (size_t)t->lw[l] * t->lh[l] * 4;
    CmdUploadTex *c = cmd_alloc(CMD_UPLOAD_TEX, sizeof *c + total);
    c->id = t->gpu_id;
    c->levels = t->levels < 14 ? t->levels : 14;
    c->fmt = fmt;
    if (fmt) {
        uint8_t *out = (uint8_t *)(c + 1);
        for (uint32_t l = 0; l < c->levels; l++) {
            uint32_t bw = (t->lw[l] + 3) / 4, bh = (t->lh[l] + 3) / 4;
            c->w[l] = t->lw[l]; c->h_[l] = t->lh[l];
            for (uint32_t by = 0; by < bh; by++) memcpy(out + (size_t)by * bw * bs, HP(t->mem[0][l] + by * t->pitch[l]), (size_t)bw * bs);
            out += (size_t)bw * bh * bs;
        }
        cmd_end();
        t->sent_version = t->version;
        return;
    }
    uint8_t *out = (uint8_t *)(c + 1);
    const uint8_t *pal = d->st.palette_current < 1024 ? g_palettes[d->st.palette_current] : NULL;
    for (uint32_t l = 0; l < c->levels; l++) {
        uint32_t w = t->lw[l], h = t->lh[l];
        c->w[l] = w; c->h_[l] = h;
        if (!texture_to_rgba8(t->format, HP(t->mem[0][l]), t->pitch[l], w, h, out, pal)) {
            static int warned;
            if (!warned++) fprintf(stderr, "[render] unsupported texture format %x\n", t->format);
            memset(out, 255, (size_t)w * h * 4);
        }
        if (t->has_ck) apply_color_key(t, l, out);
        out += (size_t)w * h * 4;
    }
    cmd_end();
    t->sent_version = t->version;
}

/* ------------------------------------------------------------------ clears */

static void clear_quad(Device *d, float x1, float y1, float x2, float y2, uint32_t flags, uint32_t color, float z) {
    if (x2 <= x1 || y2 <= y1 || !(flags & 3)) return;
    CmdDraw *c = cmd_alloc(CMD_DRAW, sizeof *c + 6 * sizeof(CVert));
    c->key = (PKey){ 0, 2, 1, 1, 1, (flags & 2) ? 1u : 0u, 8, 1, (flags & 1) ? 0xFu : 0u, TOPO_TRIANGLES, 0 };
    c->full_viewport = 1;
    c->wide = (uint32_t)g_frame_wide;
    c->minz = 0.f; c->maxz = 1.f;
    c->capture_index = -1;
    for (int i = 0; i < 4; i++) c->samp_key[i] = sampler_key(d->st.tss[i]);
    c->u.vp[2] = (float)g_tgt_lw; c->u.vp[3] = (float)g_tgt_lh;
    c->u.flags[0] = 1;   /* pre-transformed */
    c->u.st0[0] = 1;     /* stage disabled: output = diffuse */
    c->nverts = 6;
    CVert *v = (CVert *)(c + 1);
    const float px[6] = { x1, x2, x1, x1, x2, x2 }, py[6] = { y1, y1, y2, y2, y1, y2 };
    for (int i = 0; i < 6; i++) { v[i].x = px[i] - 0.5f; v[i].y = py[i] - 0.5f; v[i].z = z; v[i].w = 1.f; v[i].diffuse = color; }
    cmd_end();
}

static void rec_clear_impl(Device *d, uint32_t count, uint32_t rects, uint32_t flags, uint32_t color, float z,
                      uint32_t stencil) {
    select_target(d);
    const DevState *st = &d->st;
    float vx1 = (float)st->viewport.x, vy1 = (float)st->viewport.y;
    float vx2 = vx1 + (float)(st->viewport.w ? st->viewport.w : g_tgt_lw);
    float vy2 = vy1 + (float)(st->viewport.h ? st->viewport.h : g_tgt_lh);
    int full = !count && vx1 <= 0 && vy1 <= 0 && vx2 >= (float)g_tgt_lw && vy2 >= (float)g_tgt_lh;
    /* a pass clear covers the whole window; on a 4:3 frame the back buffer's picture is a box in the middle
       (render_wgpu.c exec_draw), so clear it with a quad, which the executor keeps inside the box */
    if (full && !(g_rec_bb && !g_frame_wide)) {
        CmdClear *c = cmd_alloc(CMD_CLEAR, sizeof *c);
        c->flags = flags & 7; c->color = color; c->z = z; c->stencil = stencil;
        cmd_end();
        return;
    }
    /* Direct3D clears only the viewport (or the given rects): draw quads instead of a pass clear */
    for (uint32_t i = 0; i < (count ? count : 1); i++) {
        float x1 = vx1, y1 = vy1, x2 = vx2, y2 = vy2;
        if (count && rects) {
            int32_t r[4];
            memcpy(r, HP(rects + 16 * i), 16);
            if (r[0] > x1) x1 = (float)r[0];
            if (r[1] > y1) y1 = (float)r[1];
            if (r[2] < x2) x2 = (float)r[2];
            if (r[3] < y2) y2 = (float)r[3];
        }
        clear_quad(d, x1, y1, x2, y2, flags, color, z);
    }
}

/* ------------------------------------------------------------------ draw */

static void rec_draw_impl(Device *d, uint32_t prim, uint32_t prim_count, uint32_t start_vertex, uint32_t num_vertices,
                     const uint8_t *vertices_up, uint32_t up_stride, const void *indices, int index32,
                     uint32_t start_index) {
    DevState *st = &d->st;
    (void)num_vertices;
    if (!prim_count) return;
    Shader *sh = (st->vs & 1) ? shader_get(st->vs) : NULL;   /* vertex shader handle (FVFs have bit 0 clear) */
    if ((st->vs & 1) && !sh) return;
    g_f_drawn++;
    select_target(d);
    if (g_snap_detail) {
        Texture *t0 = com_data(st->texture[0]), *t1 = com_data(st->texture[1]);
        fprintf(stderr, "[draw] #%04u vs %x ps %x prim %u n %u | tex0 %s fmt %x tex1 %s fmt %x | tss0 op %u a %u,%u aop %u | "
                "tss1 op %u | blend %u %u/%u atest %u\n", g_f_drawn - 1, st->vs, st->ps, prim, prim_count,
                t0 ? (t0->type == RT_TEXTURE ? "2d" : "cube") : "-", t0 ? t0->format : 0,
                t1 ? (t1->type == RT_TEXTURE ? "2d" : "cube") : "-", t1 ? t1->format : 0,
                st->tss[0][1], st->tss[0][2], st->tss[0][3], st->tss[0][4], st->tss[1][1], st->rs[27], st->rs[19],
                st->rs[20], st->rs[15]);
    }

    FvfLayout L;
    fvf_layout(sh ? 0x2 /* XYZ: transformed by the shader */ : st->vs, &L);
    if (L.pos_type != 0x4) {   /* non-RHW = 3D world: we're in a mission, not a menu */
        g_world_this_frame = 1;
        g_rec_cover = 0;   /* only 2D drawn after the last world draw can be a mask over it (the sky's cloud strips come before) */
    }
    if (!sh && L.pos_type != 0x4) {
        float m11 = fabsf(st->transform[3][5]);   /* the game may bake a y flip in: the magnitude is the zoom */
        if (m11 > g_zoom_this_frame) g_zoom_this_frame = m11;   /* the most zoomed projection of the frame */
        if (d->frame == g_proj_log_frame) {
            static float seen[32]; static int nseen;
            int dup = 0;
            for (int i = 0; i < nseen; i++) if (seen[i] == m11) dup = 1;
            if (!dup && nseen < 32) { seen[nseen++] = m11; fprintf(stderr, "[proj] frame %u: m00 %.4f m11 %.4f (draw #%u)\n", d->frame, st->transform[3][0], m11, g_f_drawn - 1); }
        }
    }
    const uint8_t *vb = NULL;
    uint32_t stride;
    if (vertices_up) { vb = vertices_up; stride = up_stride; }
    else {
        Buffer *b = com_data(st->stream_vb[0]);
        if (!b && !sh) return;
        if (b) vb = HP(b->mem);
        stride = st->stream_stride[0];
    }
    if (!stride && !sh) stride = L.stride;
    uint32_t base = indices && !vertices_up ? st->base_vertex : 0;

    /* expand to a list topology */
    uint32_t topo, n = 0;
    uint32_t cap = prim_count * 3 + 4;
    uint32_t *idx = malloc(4 * cap);
#define IDX(i) (indices ? (index32 ? ((const uint32_t *)indices)[start_index + (i)] : ((const uint16_t *)indices)[start_index + (i)]) + base : start_vertex + (i))
    switch (prim) {
    case 1: topo = TOPO_POINTS; for (uint32_t i = 0; i < prim_count; i++) idx[n++] = IDX(i); break;
    case 2: topo = TOPO_LINES; for (uint32_t i = 0; i < 2 * prim_count; i++) idx[n++] = IDX(i); break;
    case 3: topo = TOPO_LINES;
        for (uint32_t i = 0; i < prim_count; i++) { idx[n++] = IDX(i); idx[n++] = IDX(i + 1); } break;
    case 4: topo = TOPO_TRIANGLES; for (uint32_t i = 0; i < 3 * prim_count; i++) idx[n++] = IDX(i); break;
    case 5: topo = TOPO_TRIANGLES;
        for (uint32_t i = 0; i < prim_count; i++) {
            if (i & 1) { idx[n++] = IDX(i + 1); idx[n++] = IDX(i); idx[n++] = IDX(i + 2); }
            else { idx[n++] = IDX(i); idx[n++] = IDX(i + 1); idx[n++] = IDX(i + 2); }
        }
        break;
    case 6: topo = TOPO_TRIANGLES;
        for (uint32_t i = 0; i < prim_count; i++) { idx[n++] = IDX(0); idx[n++] = IDX(i + 1); idx[n++] = IDX(i + 2); }
        break;
    default: free(idx); return;
    }
#undef IDX

    /* textures first: an upload is its own command and must precede the draw */
    for (int s = 0; s < 4; s++) {
        Texture *t = com_data(st->texture[s]);
        if (t && t->type == RT_TEXTURE && !(t->usage & 1)) sync_texture(d, t);
    }

    CmdDraw *c = cmd_alloc(CMD_DRAW, sizeof *c + (size_t)n * sizeof(CVert));
    CVert *cv = (CVert *)(c + 1);
    c->nverts = n;
    c->wide = (uint32_t)g_frame_wide;
    c->capture_index = g_snap_detail ? (int32_t)(g_f_drawn - 1) : -1;
    if (sh) {
        VsStreams vss;
        memset(&vss, 0, sizeof vss);
        if (vertices_up) { vss.data[0] = vertices_up; vss.stride[0] = up_stride; }
        else
            for (int s = 0; s < 16; s++) {
                Buffer *sb = com_data(st->stream_vb[s]);
                if (sb) { vss.data[s] = HP(sb->mem); vss.stride[s] = st->stream_stride[s]; }
            }
        float wv[16], ff_mvp[16];
        mat_mul(st->transform[256], st->transform[2], wv);
        mat_mul(wv, st->transform[3], ff_mvp);
        vss.ff_mvp = ff_mvp;
        /* run the shader once per distinct vertex: over the referenced range when it is compact */
        uint32_t lo = 0xFFFFFFFFu, hi = 0;
        for (uint32_t i = 0; i < n; i++) { if (idx[i] < lo) lo = idx[i]; if (idx[i] > hi) hi = idx[i]; }
        uint32_t range = n ? hi - lo + 1 : 0;
        int use_range = range <= 2 * n + 64;
        CVert *tmp = use_range ? malloc(sizeof(CVert) * (range ? range : 1)) : NULL;
        for (uint32_t i = 0; i < (use_range ? range : n); i++) {
            VsOut o;
            extern int vs_trace;
            vs_trace = g_snap_detail && i == 0 && st->vs == g_trace_vs;
            vs_run(sh, (const float (*)[4])st->vs_const, &vss, use_range ? lo + i : idx[i], &o);
            vs_trace = 0;
            CVert *v = use_range ? &tmp[i] : &cv[i];
            v->x = o.pos[0]; v->y = o.pos[1]; v->z = o.pos[2]; v->w = o.pos[3];
            v->diffuse = pack_color(o.d0); v->specular = pack_color(o.d1);
            memcpy(v->tc, o.tex, sizeof v->tc);
        }
        if (use_range) { for (uint32_t i = 0; i < n; i++) cv[i] = tmp[idx[i] - lo]; free(tmp); }
    } else {
        for (uint32_t i = 0; i < n; i++) convert_vertex(vb + (size_t)idx[i] * stride, &L, &cv[i]);
        if (L.pos_type != 0x4) light_and_fog(st, &L, vb, stride, idx, n, cv);
        else if (st->rs[28] && !L.has_specular)
            for (uint32_t i = 0; i < n; i++) cv[i].specular |= 0xFF000000u;   /* no fog factor: no fog */
    }
    free(idx);

    for (int s = 0; s < 4; s++) {
        Texture *t = com_data(st->texture[s]);
        c->samp_key[s] = sampler_key(st->tss[s]);
        if (t && t->type == RT_TEXTURE && (t->usage & 1)) {
            Surface *s0 = t->surfaces[0][0] ? com_data(t->surfaces[0][0]) : NULL;
            if (s0) { c->slot_kind[s] = TS_RTSURF; c->slot_id[s] = s0->gpu_id; }
        } else if (t && t->type == RT_TEXTURE) {
            c->slot_kind[s] = TS_TEX; c->slot_id[s] = t->gpu_id;
        }
    }
    c->pixel_shader = st->ps;

    Uniforms *u = &c->u;
    if (!sh && L.pos_type != 0x4) {
        float wv[16];
        mat_mul(st->transform[256], st->transform[2], wv);    /* WORLD * VIEW */
        mat_mul(wv, st->transform[3], u->mvp);                /* * PROJECTION */
    }
    u->vp[0] = (float)st->viewport.x; u->vp[1] = (float)st->viewport.y;
    u->vp[2] = (float)(st->viewport.w ? st->viewport.w : g_tgt_lw);
    u->vp[3] = (float)(st->viewport.h ? st->viewport.h : g_tgt_lh);
    u->flags[0] = sh ? 2 : L.pos_type == 0x4;
    u->flags[1] = st->rs[15] ? st->rs[25] : 0;                 /* ALPHATESTENABLE ? ALPHAFUNC */
    u->flags[2] = st->rs[29] != 0;                             /* SPECULARENABLE */
    u->flags[3] = !sh && L.pos_type != 0x4 && st->rs[137] && !L.has_normal;  /* LIGHTING without normals: ambient */
    for (int s = 0; s < 2; s++) {
        const uint32_t *t = st->tss[s];
        uint32_t *a = s ? u->st1 : u->st0, *b = s ? u->st1b : u->st0b;
        a[0] = t[1]; a[1] = t[2]; a[2] = t[3]; a[3] = t[4];
        b[0] = t[5]; b[1] = t[6]; b[2] = sh ? (uint32_t)s : t[11] & 7; b[3] = (t[26] & 255) | (t[27] & 255) << 8;
    }
    unpack_color(st->rs[60], u->tfactor);
    u->misc[0] = (st->rs[24] & 255) / 255.f;
    if (st->rs[41] && !u->flags[1]) {   /* Direct3D 7 COLORKEYENABLE: keyed texels (alpha 0) are not drawn */
        Texture *t0 = com_data(st->texture[0]);
        if (t0 && t0->has_ck) { u->flags[1] = 5; u->misc[0] = 0.f; }
    }
    for (int i = 0; i < 4; i++) u->mat_diffuse[i] = 1.f;
    if (u->flags[3]) {
        /* no normals: lit color is emissive + ambient * material ambient (diffuse lighting needs a normal) */
        float amb[4];
        unpack_color(st->rs[139], amb);
        for (int i = 0; i < 3; i++) { u->mat_diffuse[i] = 0.f; u->ambient[i] = amb[i] * st->material[8 + i] + st->material[12 + i]; }
        u->mat_diffuse[3] = 1.f;
        if (st->rs[141] && L.has_diffuse && st->rs[147] == 1) {  /* COLORVERTEX + AMBIENTMATERIALSOURCE=COLOR1 */
            for (int i = 0; i < 3; i++) { u->mat_diffuse[i] = amb[i]; u->ambient[i] = st->material[12 + i]; }
        }
    }
    memcpy(u->psc, st->ps_const, sizeof u->psc);
    if (st->rs[28] && !sh) { unpack_color(st->rs[34], u->fog); u->fog[3] = 1.f; }
    for (int s = 0; s < 4; s++) {
        memcpy(u->bem[s], &st->tss[s][7], 16);        /* BUMPENVMAT00, 01, 10, 11 (float bits) */
        memcpy(&u->bel[s][0], &st->tss[s][22], 4);    /* BUMPENVLSCALE */
        memcpy(&u->bel[s][1], &st->tss[s][23], 4);    /* BUMPENVLOFFSET */
    }

    PKey k = { st->rs[27] != 0, st->rs[19], st->rs[20], st->rs[171] ? st->rs[171] : 1, st->rs[7] != 0 && st->depth_stencil,
               st->rs[14], st->rs[23], st->rs[22], st->rs[168] & 0xF, topo, st->ps };
    if (L.pos_type == 0x4) k.cull = 1;  /* keep 2D simple: no culling for pre-transformed vertices */
    /* otherwise k.cull keeps D3D's value (1 none, 2 cull clockwise, 3 cull counter-clockwise): the executor
       maps it per WebGPU implementation (pipeline_for) */
    c->key = k;
    c->vp[0] = (float)st->viewport.x; c->vp[1] = (float)st->viewport.y;
    c->vp[2] = (float)(st->viewport.w ? st->viewport.w : g_tgt_lw);
    c->vp[3] = (float)(st->viewport.h ? st->viewport.h : g_tgt_lh);
    c->minz = st->viewport.minz;
    c->maxz = st->viewport.maxz > st->viewport.minz ? st->viewport.maxz : 1.f;
    if (d->frame == g_cmd_sig_frame)
        fprintf(stderr, "[sig] f%u d%u n=%u key=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%x fl=%u,%u,%u,%u st=%u/%u,%u/%u "
                "tex=%u:%u,%u:%u,%u:%u,%u:%u ps=%x vp=%g,%g,%g,%g z=%g,%g mvp=%08x pos=%08x col=%08x tc=%08x\n",
                d->frame, g_f_drawn - 1, n, k.blend, k.src, k.dst, k.op, k.zen, k.zwrite, k.zfunc, k.cull, k.cmask, k.topo, k.ps,
                u->flags[0], u->flags[1], u->flags[2], u->flags[3], u->st0[0], u->st0[3], u->st1[0], u->st1[3],
                c->slot_kind[0], c->slot_id[0], c->slot_kind[1], c->slot_id[1], c->slot_kind[2], c->slot_id[2],
                c->slot_kind[3], c->slot_id[3], c->pixel_shader, c->vp[0], c->vp[1], c->vp[2], c->vp[3], c->minz, c->maxz,
                fnv_qa(2166136261u, u->mvp, 16, 1024.f), sig_pos(cv, n), sig_col(cv, n), sig_tc(cv, n));
    if (L.pos_type == 0x4 && !k.zen && n <= 8 && g_rec_bb && g_tgt_lw && g_tgt_lh) {   /* a 2D quad (no depth test: not a near polygon the game pre-transformed): how much of the screen it covers */
        float lw = (float)g_tgt_lw, lh = (float)g_tgt_lh, x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (uint32_t i = 0; i < n; i++) { x0 = fminf(x0, cv[i].x); y0 = fminf(y0, cv[i].y); x1 = fmaxf(x1, cv[i].x); y1 = fmaxf(y1, cv[i].y); }
        float frac = fminf(1.f, fmaxf(0.f, fminf(x1, lw) - fmaxf(x0, 0.f)) * fmaxf(0.f, fminf(y1, lh) - fmaxf(y0, 0.f)) / (lw * lh));
        if (n == 6 && y1 - y0 < 24.f) {   /* small quads may be subtitle glyphs (host/subtitle.c) */
            extern void subtitle_glyph(float, float, float, float, float, float, float, float, uint32_t, float);
            float gu0 = 1e9f, gv0 = 1e9f, gu1 = -1e9f, gv1 = -1e9f;
            for (uint32_t i = 0; i < n; i++) { gu0 = fminf(gu0, cv[i].tc[0][0]); gv0 = fminf(gv0, cv[i].tc[0][1]); gu1 = fmaxf(gu1, cv[i].tc[0][0]); gv1 = fmaxf(gv1, cv[i].tc[0][1]); }
            subtitle_glyph(x0 / lh, y0 / lh, x1 / lh, y1 / lh, gu0, gv0, gu1, gv1, cv[0].diffuse, lw / lh);
        }
        if (frac < 0.9f) g_rec_cover += frac;   /* one quad over the whole screen is a fade, drawn over the wide picture as it is */
        if (d->frame == g_cmd_sig_frame) fprintf(stderr, "[cover] d%u n=%u box %g,%g-%g,%g frac %.3f total %.3f\n", g_f_drawn - 1, n, x0, y0, x1, y1, frac, g_rec_cover);
        static int gevery = -1; if (gevery < 0) { const char *e = getenv("RC_GLYPH_EVERY"); gevery = e ? atoi(e) : 0; }
        if ((d->frame == g_cmd_sig_frame || (gevery && d->frame % gevery == 0)) && n == 6) {   /* glyph quads: texture, position, texture coordinates */
            float u0 = 1e9f, v0 = 1e9f, u1 = -1e9f, v1 = -1e9f;
            for (uint32_t i = 0; i < n; i++) { u0 = fminf(u0, cv[i].tc[0][0]); v0 = fminf(v0, cv[i].tc[0][1]); u1 = fmaxf(u1, cv[i].tc[0][0]); v1 = fmaxf(v1, cv[i].tc[0][1]); }
            fprintf(stderr, "[glyph] f%u d%u tex %u:%u xy %g,%g-%g,%g uv %.5f,%.5f-%.5f,%.5f col %08x\n", d->frame, g_f_drawn - 1, c->slot_kind[0], c->slot_id[0], x0, y0, x1, y1, u0, v0, u1, v1, cv[0].diffuse);
        }
    }
    cmd_end();
}

/* ------------------------------------------------------------------ frames, readback, resources */

static int rec_init(Device *d) {
    (void)d;
#ifdef __EMSCRIPTEN__
    g_watch.game_phase = 3;
    while (!web_gpu_ready()) usleep(1000);   /* the page creates the WebGPU device before starting the game */
    g_watch.game_phase = 0;
    return 1;
#else
    return gpu_init_native();
#endif
}

static void rec_present_impl(Device *d) {
#ifdef __EMSCRIPTEN__
    { extern int g_log; extern void rc_mem_report(const char *, uint32_t);
      if (g_log && d->frame % 300 == 0) rc_mem_report("game", d->frame); }
#endif
    CmdPresent *c = cmd_alloc(CMD_PRESENT, sizeof *c);
    c->frame = d->frame;
    c->screenshot = (uint32_t)want_screenshot(d->frame);
    cmd_end();
    cmd_flush(1);
    g_target_sent = 0;   /* the executor ends every frame on the back buffer */
    if (d->frame == g_cmd_sig_frame + 1) fprintf(stderr, "[sig] f%u end draws=%u\n", g_cmd_sig_frame, g_f_drawn);   /* the present of that frame's draws */
    g_f_drawn = 0;
    { extern void subtitle_present(void); subtitle_present(); }
    {   /* the next frame's widescreen mode (see g_frame_wide): decided here so the game's field of view and the
           executor's mapping agree for the whole frame */
        extern void dd_display_size(int *, int *);
        int dw, dh; dd_display_size(&dw, &dh);
        float factor = dw > 0 && dh > 0 ? ((float)dw / (float)dh) / (4.f / 3.f) : 1.f;
        int overlay = g_rec_cover > 0.5f;
        g_rec_cover = 0;
        g_frame_wide = g_wide && g_world_this_frame && !overlay && factor > 1.01f;
        rc_aspect_x = g_frame_wide ? factor : 1.f;
    }
    { static unsigned nw; if (!g_world_this_frame) nw++; if (getenv("RC_TICK_LOG") && d->frame % 60 == 0) { fprintf(stderr, "[noworld] frame %u: %u of 60\n", d->frame, nw); } if (d->frame % 60 == 0) nw = 0; }
    g_in_mission = g_world_this_frame;   /* latch for this frame's input; reset for the next */
    g_world_this_frame = 0;
#ifdef PROFILE_TICK
    { uint32_t t = R32(PROFILE_TICK);
      g_tick_still = (t == g_tick_last) ? g_tick_still + 1 : 0;
      g_tick_last = t;
#ifdef PROFILE_MAP_OPEN
      g_map = g_in_mission && R32(PROFILE_MAP_OPEN) == 2;
#endif
#ifdef PROFILE_PAUSE_FLAG
      /* the game's running state (profile.h): 0 in the Esc menu and cutscenes, never in play */
      g_paused = g_in_mission && R32(PROFILE_PAUSE_FLAG) == 0;
#else
      g_paused = g_in_mission && g_tick_still >= 3;
#endif
      { static int logit = -1; if (logit < 0) logit = getenv("RC_TICK_LOG") != NULL;   /* RC_TICK_LOG=1: the pause detector's inputs */
        if (logit && d->frame % 60 == 0) fprintf(stderr, "[tick] frame %u tick %u still %d world %d paused %d cand %x %x %x\n", d->frame, t, g_tick_still, g_in_mission, g_paused,
                                                     R32(0xa44390u), R32(0xa4438cu), g_map); } }
#endif
    if (getenv("RC_ZOOM_LOG") && g_zoom_this_frame != g_zoom) fprintf(stderr, "[zoom] frame %u: m11 %.5f\n", d->frame, g_zoom_this_frame);
    g_zoom = g_zoom_this_frame;
    g_zoom_this_frame = 0;
    if (g_proj_log_frame == 0 && getenv("RC_PROJ_LOG")) g_proj_log_frame = (uint32_t)atoi(getenv("RC_PROJ_LOG"));
#ifdef __EMSCRIPTEN__
    /* Don't run ahead of the display: two frames queued (a slow frame doesn't skip a vsync). Online every
       queued frame is a frame of input latency on top of the lockstep delay, so RC_QUEUE=1 (?queue=1 on
       the page) allows one there, for a machine that simulates a frame well within 16 ms. */
    static int depth = -1;
    if (depth < 0) {
        const char *q = getenv("RC_QUEUE");
        depth = q ? atoi(q) : 2;
        if (depth < 1) depth = 1;
    }
    g_watch.game_frame = (int)d->frame;
    g_watch.game_phase = 1;
    while (web_queued_frames() >= depth) usleep(1000);
    g_watch.game_phase = 0;
#endif
}

static void rec_readback_impl(Device *d, Surface *s) {
    int is_bb = com_data(d->backbuffer) == s;
    if (!is_bb && !s->is_rt) return;
    if (!s->mem) s->mem = heap_alloc(s->pitch * s->height + 16, 1);
    volatile int done = 0;
    CmdReadback *c = cmd_alloc(CMD_READBACK, sizeof *c);
    c->id = is_bb ? 0 : s->gpu_id;
    c->w = s->width; c->h_ = s->height; c->fmt = s->format; c->pitch = s->pitch; c->guest_mem = s->mem;
    c->done_ptr = (uint64_t)(uintptr_t)&done;
    cmd_end();
    cmd_flush(0);
    g_watch.game_phase = 2;
    g_watch.readbacks++;
    while (!__atomic_load_n(&done, __ATOMIC_ACQUIRE)) usleep(200);   /* natively it already ran */
    g_watch.game_phase = 0;
}

static void rec_upload_surface_impl(Device *d, Surface *s) {
    if (!s->mem || s->is_depth || com_data(d->backbuffer) == s) return;
    CmdUploadSurf *c = cmd_alloc(CMD_UPLOAD_SURF, sizeof *c + (size_t)s->width * s->height * 4);
    c->id = s->gpu_id; c->w = s->width; c->h_ = s->height;
    if (!texture_to_rgba8(s->format, HP(s->mem), s->pitch, s->width, s->height, (uint8_t *)(c + 1), NULL))
        memset(c + 1, 0, (size_t)s->width * s->height * 4);
    cmd_end();
}

static void rec_destroy_texture_impl(Texture *t) {
    if (t->sent_version == 0xFFFFFFFFu) return;   /* never reached the GPU */
    CmdDestroy *c = cmd_alloc(CMD_DESTROY_TEX, sizeof *c);
    c->id = t->gpu_id;
    cmd_end();
}

static void rec_destroy_surface_impl(Surface *s) {
    CmdDestroy *c = cmd_alloc(CMD_DESTROY_SURF, sizeof *c);
    c->id = s->gpu_id;
    cmd_end();
}

/* ------------------------------------------------------------------ locked entry points */

static void rec_clear(Device *d, uint32_t count, uint32_t rects, uint32_t flags, uint32_t color, float z, uint32_t stencil) {
    rec_enter(); rec_clear_impl(d, count, rects, flags, color, z, stencil); rec_leave();
}
static void rec_draw(Device *d, uint32_t prim, uint32_t prim_count, uint32_t start_vertex, uint32_t num_vertices,
                     const uint8_t *vertices_up, uint32_t up_stride, const void *indices, int index32, uint32_t start_index) {
    rec_enter();
    rec_draw_impl(d, prim, prim_count, start_vertex, num_vertices, vertices_up, up_stride, indices, index32, start_index);
    rec_leave();
}
static void rec_present(Device *d) { rec_enter(); rec_present_impl(d); rec_leave(); }
static void rec_readback(Device *d, Surface *s) { rec_enter(); rec_readback_impl(d, s); rec_leave(); }
static void rec_upload_surface(Device *d, Surface *s) { rec_enter(); rec_upload_surface_impl(d, s); rec_leave(); }
static void rec_destroy_texture(Texture *t) { rec_enter(); rec_destroy_texture_impl(t); rec_leave(); }
static void rec_destroy_surface(Surface *s) { rec_enter(); rec_destroy_surface_impl(s); rec_leave(); }

RenderBackend g_render_gpu = { "webgpu", rec_init, rec_clear, rec_draw, rec_present, rec_destroy_texture,
                               rec_readback, rec_upload_surface, rec_destroy_surface };
