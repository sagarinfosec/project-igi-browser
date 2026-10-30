/* DirectDraw 7 and Direct3D 7 on top of the Direct3D 8 device model (d3d8.c) and its renderer.
 *
 * A Direct3D 7 device is a d3d8 Device: render and texture-stage states use the same numbering, so
 * they land in its DevState and every draw goes through g_render like a Direct3D 8 draw. DirectDraw
 * surfaces keep their pixels in guest memory; a texture surface (and its mip chain) is presented to
 * the renderer as a d3d8 Texture whose levels point at that memory. The surface Direct3D renders to
 * is the device's back buffer; 2D work on it (Blt, BltFast, Lock) is done as textured quads, colour
 * fills as clears, and reads as GPU readbacks. Flip, or a Blt to the primary surface, presents. */
#include "d3d8.h"
#include "render.h"

#include <math.h>

#define W(name) static void name(CPU *c)

#define DD_OK 0u
#define DDERR_GENERIC 0x80004005u
#define DDERR_INVALIDPARAMS 0x80070057u
#define DDERR_UNSUPPORTED 0x80004001u
#define DDERR_NOTFOUND 0x887600FFu
#define DDERR_SURFACELOST 0x887601C2u
#define DDERR_NOTLOCKED 0x887602D8u
#define DDERR_NOPALETTEATTACHED 0x88760230u
#define DDERR_NOCLIPPERATTACHED 0x88760241u
#define DDERR_NOCOLORKEY 0x887600D7u
#define D3DERR_NOTAVAILABLE7 0x8876086Au

/* DDSCAPS */
enum { CAPS_BACKBUFFER = 4, CAPS_COMPLEX = 8, CAPS_FLIP = 0x10, CAPS_FRONTBUFFER = 0x20, CAPS_OFFSCREENPLAIN = 0x40,
       CAPS_PRIMARY = 0x200, CAPS_SYSTEMMEMORY = 0x800, CAPS_TEXTURE = 0x1000, CAPS_3DDEVICE = 0x2000,
       CAPS_VIDEOMEMORY = 0x4000, CAPS_VISIBLE = 0x8000, CAPS_ZBUFFER = 0x20000, CAPS_MIPMAP = 0x400000,
       CAPS_LOCALVIDMEM = 0x10000000 };
/* DDSURFACEDESC2 flags */
enum { DDSD_CAPS = 1, DDSD_HEIGHT = 2, DDSD_WIDTH = 4, DDSD_PITCH = 8, DDSD_BACKBUFFERCOUNT = 0x20,
       DDSD_ZBUFFERBITDEPTH = 0x40, DDSD_LPSURFACE = 0x800, DDSD_PIXELFORMAT = 0x1000, DDSD_CKSRCBLT = 0x10000,
       DDSD_MIPMAPCOUNT = 0x20000, DDSD_REFRESHRATE = 0x40000 };
/* DDPIXELFORMAT flags */
enum { PF_ALPHAPIXELS = 1, PF_ALPHA = 2, PF_FOURCC = 4, PF_PAL8 = 0x20, PF_RGB = 0x40, PF_ZBUFFER = 0x400,
       PF_STENCIL = 0x4000, PF_LUMINANCE = 0x20000 };

typedef struct DDSurf {
    uint32_t self;                  /* guest object */
    uint32_t caps, caps2;
    uint32_t w, h, pitch, bpp, fmt; /* fmt: D3DFORMAT numbering (d3d8.h) */
    uint32_t mem;                   /* pixels in guest memory */
    uint32_t back;                  /* primary: the attached back buffer */
    uint32_t zbuf;                  /* attached depth buffer */
    uint32_t mip_next;              /* next smaller mip level */
    struct DDSurf *mip_top;         /* top level (itself for the top) */
    int level, levels;
    int has_ck;                     /* source color key (DDCKEY_SRCBLT) */
    uint32_t ck_lo, ck_hi;
    uint32_t s8;                    /* render-capable surfaces: the d3d8 Surface whose memory this is */
    Texture *tex;                   /* texture view of the surface (all mip levels), created on demand */
    uint32_t tex_obj;               /* ...and its COM object, what DevState.texture holds */
    uint32_t lock_flags;
    int locked;
    uint32_t palette, clipper;
} DDSurf;

static ComInterface IDirectDraw7_i, IDirectDrawSurface7_i, IDirect3D7_i, IDirect3DDevice7_i,
    IDirect3DVertexBuffer7_i, IDirectDrawClipper_i, IDirectDrawPalette_i, TexView_i;

static uint32_t g_dd_obj, g_d3d7_obj;
static uint32_t g_disp_w = 1024, g_disp_h = 768, g_disp_bpp = 32;   /* desktop until SetDisplayMode */
static uint32_t g_coop_hwnd, g_fullscreen;
static uint32_t g_primary;            /* the primary surface object */
static Device *g_dev;                 /* the Direct3D 7 device (one at a time) */
static uint32_t g_dev_obj;
static DDSurf *g_dev_rt;              /* the surface it renders to */
static int g_render_ready;

static DDSurf *surf(uint32_t g) {
    ComObj *o = com_get(g);
    return o && o->iface == &IDirectDrawSurface7_i ? o->data : NULL;
}

static int guid_is(uint32_t p, const char *s) {   /* s: "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" */
    if (!p) return 0;
    unsigned a, b, cc, d[8];
    sscanf(s, "%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x", &a, &b, &cc, &d[0], &d[1], &d[2], &d[3], &d[4], &d[5], &d[6], &d[7]);
    if (R32(p) != a || R16(p + 4) != b || R16(p + 6) != cc) return 0;
    for (int i = 0; i < 8; i++) if (R8(p + 8 + i) != d[i]) return 0;
    return 1;
}
#define IID_IDirect3D7      "f5049e77-4861-11d2-a407-00a0c90629a8"
#define IID_HALDevice       "84e63de0-46aa-11cf-816f-0000c020156e"
#define IID_TnLHalDevice    "f5049e78-4861-11d2-a407-00a0c90629a8"
#define IID_RGBDevice       "a4665c60-2673-11cf-a31a-00aa00b93356"
static void write_guid(uint32_t p, const char *s) {
    unsigned a, b, cc, d[8];
    sscanf(s, "%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x", &a, &b, &cc, &d[0], &d[1], &d[2], &d[3], &d[4], &d[5], &d[6], &d[7]);
    W32(p, a); W16(p + 4, (uint16_t)b); W16(p + 6, (uint16_t)cc);
    for (int i = 0; i < 8; i++) W8(p + 8 + i, (uint8_t)d[i]);
}

/* ------------------------------------------------------------------ pixel formats */

static uint32_t fmt_from_pf(uint32_t pf, uint32_t *bpp) {
    uint32_t fl = R32(pf + 4), bits = R32(pf + 12), rm = R32(pf + 16), am = R32(pf + 28);
    *bpp = bits;
    if (fl & PF_FOURCC) { *bpp = R32(pf + 8) == FMT_DXT1 ? 4 : 8; return R32(pf + 8); }
    if (fl & PF_ZBUFFER) return bits == 16 ? FMT_D16 : bits == 32 ? ((fl & PF_STENCIL) ? FMT_D24S8 : FMT_D32) : FMT_D24X8;
    if (fl & PF_PAL8) { *bpp = 8; return FMT_P8; }
    if ((fl & PF_ALPHA) && !(fl & PF_RGB)) { *bpp = 8; return FMT_A8; }
    if (fl & PF_LUMINANCE) return bits == 16 ? FMT_A8L8 : FMT_L8;
    if (bits == 32) return (fl & PF_ALPHAPIXELS) && am ? FMT_A8R8G8B8 : FMT_X8R8G8B8;
    if (bits == 24) return FMT_R8G8B8;
    if (bits == 16) {
        if (rm == 0xF800) return FMT_R5G6B5;
        if (rm == 0x0F00) return FMT_A4R4G4B4;
        return (fl & PF_ALPHAPIXELS) && am ? FMT_A1R5G5B5 : FMT_X1R5G5B5;
    }
    if (bits == 8) return FMT_P8;
    return FMT_X8R8G8B8;
}

static void write_pf(uint32_t p, uint32_t fmt) {
    memset(HP(p), 0, 32);
    W32(p, 32);
    uint32_t fl = PF_RGB, bits = 0, r = 0, g = 0, b = 0, a = 0;
    switch (fmt) {
    case FMT_A8R8G8B8: bits = 32; r = 0xFF0000; g = 0xFF00; b = 0xFF; a = 0xFF000000u; fl |= PF_ALPHAPIXELS; break;
    case FMT_X8R8G8B8: bits = 32; r = 0xFF0000; g = 0xFF00; b = 0xFF; break;
    case FMT_R8G8B8: bits = 24; r = 0xFF0000; g = 0xFF00; b = 0xFF; break;
    case FMT_R5G6B5: bits = 16; r = 0xF800; g = 0x07E0; b = 0x1F; break;
    case FMT_X1R5G5B5: bits = 16; r = 0x7C00; g = 0x03E0; b = 0x1F; break;
    case FMT_A1R5G5B5: bits = 16; r = 0x7C00; g = 0x03E0; b = 0x1F; a = 0x8000; fl |= PF_ALPHAPIXELS; break;
    case FMT_A4R4G4B4: bits = 16; r = 0x0F00; g = 0x00F0; b = 0x0F; a = 0xF000; fl |= PF_ALPHAPIXELS; break;
    case FMT_P8: fl = PF_RGB | PF_PAL8; bits = 8; break;
    case FMT_A8: fl = PF_ALPHA; bits = 8; break;
    case FMT_L8: fl = PF_LUMINANCE; bits = 8; r = 0xFF; break;
    case FMT_A8L8: fl = PF_LUMINANCE | PF_ALPHAPIXELS; bits = 16; r = 0xFF; a = 0xFF00; break;
    case FMT_D16: fl = PF_ZBUFFER; bits = 16; g = 0xFFFF; break;
    case FMT_D24X8: fl = PF_ZBUFFER; bits = 32; g = 0xFFFFFF00u; break;
    case FMT_D24S8: fl = PF_ZBUFFER | PF_STENCIL; bits = 32; r = 8; g = 0xFFFFFF00u; b = 0xFF; break;
    case FMT_D32: fl = PF_ZBUFFER; bits = 32; g = 0xFFFFFFFFu; break;
    default:
        if (format_is_dxt(fmt)) { fl = PF_FOURCC; W32(p + 8, fmt); }
        break;
    }
    W32(p + 4, fl); W32(p + 12, bits); W32(p + 16, r); W32(p + 20, g); W32(p + 24, b); W32(p + 28, a);
}

static uint32_t display_fmt(void) { return g_disp_bpp == 16 ? FMT_R5G6B5 : FMT_X8R8G8B8; }

static uint32_t surf_pitch(uint32_t fmt, uint32_t bpp, uint32_t w) {
    if (format_is_dxt(fmt)) return ((w + 3) / 4) * (fmt == FMT_DXT1 ? 8 : 16);
    return (w * bpp / 8 + 3) & ~3u;
}
static uint32_t surf_rows(uint32_t fmt, uint32_t h) { return format_is_dxt(fmt) ? (h + 3) / 4 : h; }

/* pixel value in a surface's format <-> D3DCOLOR (ARGB) */
static uint32_t pixel_to_argb(uint32_t fmt, uint32_t v) {
    switch (fmt) {
    case FMT_R5G6B5: return 0xFF000000u | ((v >> 11 & 31) * 255 / 31) << 16 | ((v >> 5 & 63) * 255 / 63) << 8 | (v & 31) * 255 / 31;
    case FMT_X1R5G5B5: case FMT_A1R5G5B5:
        return 0xFF000000u | ((v >> 10 & 31) * 255 / 31) << 16 | ((v >> 5 & 31) * 255 / 31) << 8 | (v & 31) * 255 / 31;
    case FMT_A4R4G4B4: return ((v >> 12 & 15) * 17) << 24 | ((v >> 8 & 15) * 17) << 16 | ((v >> 4 & 15) * 17) << 8 | (v & 15) * 17;
    default: return v | (fmt == FMT_A8R8G8B8 ? 0 : 0xFF000000u);
    }
}

/* ------------------------------------------------------------------ surfaces */

static void destroy_surf(void *p);

static DDSurf *surf_new(uint32_t caps, uint32_t caps2, uint32_t w, uint32_t h, uint32_t fmt, uint32_t bpp) {
    DDSurf *s = calloc(1, sizeof *s);
    s->caps = caps; s->caps2 = caps2;
    s->w = w ? w : 1; s->h = h ? h : 1; s->fmt = fmt; s->bpp = bpp;
    s->mip_top = s;
    s->levels = 1;
    int render = (caps & (CAPS_3DDEVICE | CAPS_BACKBUFFER)) && !(caps & CAPS_ZBUFFER) && !(caps & CAPS_TEXTURE);
    if (render) {   /* a surface Direct3D can render to: its memory is a d3d8 render-target surface's */
        s->s8 = d3d8_new_surface(fmt, s->w, s->h, 1, 0);
        Surface *x = com_data(s->s8);
        s->mem = x->mem; s->pitch = x->pitch;
    } else {
        s->pitch = surf_pitch(fmt, bpp, s->w);
        s->mem = heap_alloc(s->pitch * surf_rows(fmt, s->h) + 16, 1);
    }
    s->self = com_new(&IDirectDrawSurface7_i, 0, s);
    if (g_log_d3d)
        fprintf(stderr, "[ddraw] surface %08x: %ux%u fmt %x caps %x/%x%s\n", s->self, s->w, s->h, fmt, caps, caps2,
                render ? " (render target)" : "");
    return s;
}

static void addref(uint32_t g) { ComObj *o = com_get(g); if (o) o->refs++; }
static void release(uint32_t g) {
    ComObj *o = com_get(g);
    if (!o) return;
    /* run com_Release through a scratch guest frame: [esp] return address, [esp+4] this */
    CPU tmp;
    memset(&tmp, 0, sizeof tmp);
    static __thread uint32_t scratch;
    if (!scratch) scratch = heap_alloc(64, 1);
    tmp.esp = scratch + 16;
    W32(tmp.esp + 4, g);
    com_Release(&tmp);
}

/* The texture view: a d3d8 Texture over the surface's mip chain (levels share the surfaces' memory). */
static Texture *surf_tex(DDSurf *s) {
    DDSurf *top = s->mip_top;
    if (top->tex) return top->tex;
    Texture *t = calloc(1, sizeof *t);
    t->type = RT_TEXTURE;
    t->gpu_id = gpu_new_id();
    t->sent_version = 0xFFFFFFFFu;
    t->format = top->fmt; t->width = top->w; t->height = top->h; t->faces = 1;
    int n = 0;
    for (DDSurf *l = top; l && n < 14; l = l->mip_next ? surf(l->mip_next) : NULL, n++) {
        t->lw[n] = l->w; t->lh[n] = l->h; t->pitch[n] = l->pitch;
        t->size[n] = l->pitch * surf_rows(l->fmt, l->h);
        t->mem[0][n] = l->mem;
    }
    t->levels = (uint32_t)n;
    t->has_ck = top->has_ck; t->ck_lo = top->ck_lo; t->ck_hi = top->ck_hi;
    top->tex = t;
    top->tex_obj = com_new(&TexView_i, RT_TEXTURE, t);
    return t;
}
static void touch(DDSurf *s) {
    DDSurf *top = s->mip_top;
    if (top->tex) {
        top->tex->version++;
        top->tex->has_ck = top->has_ck; top->tex->ck_lo = top->ck_lo; top->tex->ck_hi = top->ck_hi;
    }
}

static void destroy_surf(void *p) {
    DDSurf *s = p;
    if (s->tex) {
        if (g_render->destroy_texture) g_render->destroy_texture(s->tex);
        com_free(s->tex_obj);   /* the view's slot comes back (it used to stay used for good: ~3 leaked a second) */
        s->tex_obj = 0;
        free(s->tex);
    }
    if (s->mip_next) release(s->mip_next);
    if (s->back) release(s->back);
    if (s->zbuf) release(s->zbuf);
    if (s->self == g_primary) g_primary = 0;
    if (s == g_dev_rt) g_dev_rt = NULL;   /* the device keeps its d3d8 back buffer */
    if (!s->s8) heap_free(s->mem);
    free(s);
}

/* ------------------------------------------------------------------ the device's render target */

static uint32_t new_depth(DDSurf *rt, DDSurf *z) {
    uint32_t f = z && z->fmt ? z->fmt : FMT_D24X8;
    return d3d8_new_surface(f, rt->w, rt->h, 0, 1);
}

/* Creates (or reuses) the Device rendering into `rt`. 2D operations on the back buffer need it too, so
   it can exist before the game creates its Direct3D device. */
static Device *device_for(DDSurf *rt) {
    if (g_dev && g_dev_rt == rt) return g_dev;
    Device *d = calloc(1, sizeof *d);
    d->width = rt->w; d->height = rt->h;
    d->backbuffer_format = rt->fmt;
    d->windowed = !g_fullscreen;
    d->hwnd = g_coop_hwnd ? g_coop_hwnd : MAIN_HWND;
    d->backbuffer = rt->s8;
    DDSurf *z = rt->zbuf ? surf(rt->zbuf) : NULL;
    if (z) { d->depth = new_depth(rt, z); d->depth_format = z->fmt; }
    d->st.render_target = d->backbuffer;
    d->st.depth_stencil = d->depth;
    d3d8_default_state(d);
    if (!g_render_ready && g_render->init) g_render->init(d);
    g_render_ready = 1;
    fprintf(stderr, "[d3d7] device: %ux%u fmt %x%s\n", d->width, d->height, d->backbuffer_format,
            d->depth ? ", depth buffer" : "");
    g_dev = d;
    g_dev_rt = rt;
    g_device = d;   /* d3d8.c reads back render targets through it */
    return d;
}

static int is_target(DDSurf *s) { return s && s->s8 && g_dev && g_dev_rt == s; }

static void present(void) {
    if (!g_dev) return;
    d3d8_present(g_dev);
}

/* GPU contents of the back buffer -> its memory */
static void readback(DDSurf *s) {
    if (!is_target(s) || !g_render->readback) return;
    g_render->readback(g_dev, com_data(s->s8));
}

/* Draws src (rect sx..) stretched over the target's dst rect, as a pre-transformed textured quad. */
static void gpu_blit(DDSurf *src, int32_t sx1, int32_t sy1, int32_t sx2, int32_t sy2, int32_t dx1, int32_t dy1,
                     int32_t dx2, int32_t dy2, int keyed) {
    Device *d = g_dev;
    if (!d || !g_render->draw || dx2 <= dx1 || dy2 <= dy1) return;
    Texture *t = surf_tex(src);
    if (src->mip_top != src) return;   /* blits from a mip sub-level: not needed so far */
    DevState *st = &d->st;
    uint32_t rs[256], tss[2][32], tex[2] = { st->texture[0], st->texture[1] }, vs = st->vs, ps = st->ps;
    uint32_t rt = st->render_target, ds = st->depth_stencil;
    memcpy(rs, st->rs, sizeof rs);
    memcpy(tss, st->tss, sizeof tss);
    typeof(st->viewport) vp = st->viewport;

    st->rs[7] = 0; st->rs[14] = 0; st->rs[27] = 0; st->rs[28] = 0; st->rs[41] = 0; st->rs[168] = 0xF;
    st->rs[15] = keyed ? 1 : 0; st->rs[25] = 5; st->rs[24] = 0; st->rs[22] = 1; st->rs[29] = 0; st->rs[137] = 0;
    uint32_t *t0 = st->tss[0];
    t0[1] = 2; t0[2] = 2; t0[4] = 2; t0[5] = 2; t0[11] = 0; t0[13] = t0[14] = 3;
    int stretch = (sx2 - sx1) != (dx2 - dx1) || (sy2 - sy1) != (dy2 - dy1);
    t0[16] = t0[17] = stretch ? 2 : 1; t0[18] = 0;
    st->tss[1][1] = 1; st->tss[1][4] = 1;
    st->texture[0] = src->mip_top->tex_obj; st->texture[1] = 0;
    st->vs = 0x104;   /* XYZRHW | TEX1 */
    st->ps = 0;
    st->render_target = d->backbuffer; st->depth_stencil = d->depth;
    st->viewport.x = st->viewport.y = 0; st->viewport.w = d->width; st->viewport.h = d->height;
    st->viewport.minz = 0; st->viewport.maxz = 1;
    float u1 = (float)sx1 / t->width, v1 = (float)sy1 / t->height, u2 = (float)sx2 / t->width, v2 = (float)sy2 / t->height;
    float x1 = dx1 - 0.5f, y1 = dy1 - 0.5f, x2 = dx2 - 0.5f, y2 = dy2 - 0.5f;
    float v[4][6] = { { x1, y1, 0, 1, u1, v1 }, { x2, y1, 0, 1, u2, v1 }, { x1, y2, 0, 1, u1, v2 }, { x2, y2, 0, 1, u2, v2 } };
    g_render->draw(d, 5, 2, 0, 4, (const uint8_t *)v, 24, NULL, 0, 0);

    memcpy(st->rs, rs, sizeof rs);
    memcpy(st->tss, tss, sizeof tss);
    st->texture[0] = tex[0]; st->texture[1] = tex[1]; st->vs = vs; st->ps = ps;
    st->render_target = rt; st->depth_stencil = ds;
    st->viewport = vp;
}

/* Clears a rect of the back buffer (color and/or depth) */
static void gpu_fill(uint32_t flags, int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint32_t argb, float z) {
    Device *d = g_dev;
    if (!d || !g_render->clear) return;
    DevState *st = &d->st;
    typeof(st->viewport) vp = st->viewport;
    uint32_t rt = st->render_target, ds = st->depth_stencil;
    st->render_target = d->backbuffer; st->depth_stencil = d->depth;
    st->viewport.x = st->viewport.y = 0; st->viewport.w = d->width; st->viewport.h = d->height;
    uint32_t r = heap_alloc(16, 0);
    W32(r, (uint32_t)x1); W32(r + 4, (uint32_t)y1); W32(r + 8, (uint32_t)x2); W32(r + 12, (uint32_t)y2);
    int whole = x1 <= 0 && y1 <= 0 && x2 >= (int32_t)d->width && y2 >= (int32_t)d->height;
    g_render->clear(d, whole ? 0 : 1, whole ? 0 : r, flags, argb, z, 0);
    heap_free(r);
    st->viewport = vp;
    st->render_target = rt; st->depth_stencil = ds;
}

/* ------------------------------------------------------------------ IDirectDraw7 */

W(dd_QueryInterface) {
    uint32_t riid = ARG(1), out = ARG(2);
    if (guid_is(riid, IID_IDirect3D7)) {
        if (!g_d3d7_obj) g_d3d7_obj = com_new(&IDirect3D7_i, RT_D3D, NULL);
        addref(g_d3d7_obj);
        W32(out, g_d3d7_obj);
        RET(3, S_OK);
    }
    addref(THIS);
    W32(out, THIS);
    RET(3, S_OK);
}

static void fill_ddcaps(uint32_t p) {
    if (!p) return;
    uint32_t sz = R32(p);
    if (sz < 8 || sz > 4096) sz = 380;
    memset(HP(p + 4), 0, sz - 4);
    W32(p + 4, 0x00000001u | 0x00000040u | 0x00000200u | 0x00400000u | 0x04000000u | 0x00002000u);  /* 3D BLT BLTSTRETCH COLORKEY BLTCOLORFILL BLTDEPTHFILL? */
    W32(p + 8, 0x00080000u | 0x00020000u);   /* CAPS2: CANRENDERWINDOWED | WIDESURFACES */
    W32(p + 12, 0x00000200u);                /* CKEYCAPS: SRCBLT */
    W32(p + 56, 0x400 | 0x200 | 0x100);      /* z buffer depths: 16, 24, 32 */
    W32(p + 60, 256u << 20); W32(p + 64, 200u << 20);
    if (sz >= 132) W32(p + 132, CAPS_3DDEVICE | CAPS_TEXTURE | CAPS_ZBUFFER | CAPS_MIPMAP | CAPS_FLIP | CAPS_PRIMARY |
                                 CAPS_BACKBUFFER | CAPS_OFFSCREENPLAIN | CAPS_VIDEOMEMORY);
    if (sz >= 380) W32(p + 364, R32(p + 132));
}
W(dd_GetCaps) { fill_ddcaps(ARG(1)); fill_ddcaps(ARG(2)); RET(3, DD_OK); }

W(dd_SetCooperativeLevel) {
    g_coop_hwnd = ARG(1);
    g_fullscreen = (ARG(2) & 1 /* DDSCL_FULLSCREEN */) != 0;
    LOG("[ddraw] SetCooperativeLevel(hwnd %x, flags %x)\n", ARG(1), ARG(2));
    RET(3, DD_OK);
}
W(dd_SetDisplayMode) {
    fprintf(stderr, "[ddraw] SetDisplayMode(%ux%u, %u bpp)\n", ARG(1), ARG(2), ARG(3));
    g_disp_w = ARG(1); g_disp_h = ARG(2); g_disp_bpp = ARG(3) ? ARG(3) : 32;
    /* testing: RC_FORCE_MODE=WxH makes the display mode this size whatever the game asked for */
    if (getenv("RC_FORCE_MODE")) { unsigned fw, fh; if (sscanf(getenv("RC_FORCE_MODE"), "%ux%u", &fw, &fh) == 2) { g_disp_w = fw; g_disp_h = fh; } }
    g_screen_w = (int)g_disp_w; g_screen_h = (int)g_disp_h;   /* what GetSystemMetrics reports from now on */
    RET(6, DD_OK);
}
W(dd_RestoreDisplayMode) { RET(1, DD_OK); }
/* the display mode the game set (input.c: the Esc menu's cursor is in these pixels) */
void dd_display_size(int *w, int *h) { *w = (int)g_disp_w; *h = (int)g_disp_h; }

static void write_mode_desc(uint32_t p, uint32_t w, uint32_t h, uint32_t bpp) {
    memset(HP(p), 0, 124);
    W32(p, 124);
    W32(p + 4, DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE);
    W32(p + 8, h); W32(p + 12, w); W32(p + 16, w * bpp / 8); W32(p + 24, 60);
    write_pf(p + 72, bpp == 16 ? FMT_R5G6B5 : FMT_X8R8G8B8);
}
W(dd_EnumDisplayModes) {
    uint32_t flags = ARG(1), filter = ARG(2), ctx = ARG(3), cb = ARG(4);
    (void)flags;
    uint32_t modes[][2] = { { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1152, 864 }, { 1280, 960 },
                            { 1280, 1024 }, { 1600, 1200 } };
    /* Widescreen (render_rec.c g_frame_wide): the modes offered are as wide as the window (16:9 at least, 2.4:1
       at most) at the usual heights, and the game picks and sets one by itself, so its viewport, its own transform
       and clipping of the polygons nearest the camera (it submits those pre-transformed, clipped to its screen)
       and its HUD layout all follow; its field of view is widened to match through rc_aspect_x
       (ports/project-igi/patch.sh) on frames drawn wide, and on the others the executor squeezes its 4:3 picture
       back from the wide screen. RC_WIDE_DISPLAY=0 offers the 4:3 modes (testing). */
    { const char *e = getenv("RC_WIDE_DISPLAY");   /* on only when asked (the page asks with keyboard and mouse) */
      if (e && atoi(e)) {
          extern int g_window_w, g_window_h;
          double a = g_window_h > 0 ? (double)g_window_w / (double)g_window_h : 16.0 / 9;
          if (a < 16.0 / 9) a = 16.0 / 9;
          if (a > 2.4) a = 2.4;
          /* The game takes the offered mode nearest in pixel count to the one it wants (seen: 800x600 wanted,
             1068x600 taken over 640x480, but 854x480 taken when both were offered; 900x480 wanted, 640x480 taken
             over 1286x600), so each mode keeps its pixel count and only changes shape: its choice maps one to
             one. The menus' 640x480 becomes 800x450 and is drawn back into a 4:3 box like any 2D screen. */
          /* RC_UI_SCALE (the page's "text size" setting, larger on phones): the modes offered have this many
             times fewer pixels per side. The game draws its HUD text and subtitles in fixed display pixels, so a
             smaller display makes them proportionally larger on screen; the 3D world is drawn at the window's full
             resolution either way (the executor scales the viewport), so it loses nothing. */
          double ui = 1.0; { const char *u = getenv("RC_UI_SCALE"); if (u) ui = atof(u); if (ui < 1.0) ui = 1.0; if (ui > 2.5) ui = 2.5; }
          for (size_t i = 0; i < sizeof modes / sizeof modes[0]; i++) {
              double px = (double)modes[i][0] * modes[i][1] / (ui * ui);
              uint32_t h = ((uint32_t)(sqrt(px / a) + 0.5) + 1) & ~1u;
              /* the game takes a mode at least 800 wide for a mission: a scaled mode never goes below that */
              if (ui > 1.0 && h * a < 800) h = ((uint32_t)(800 / a + 0.5) + 1) & ~1u;
              modes[i][1] = h;
              modes[i][0] = ((uint32_t)(h * a + 0.5) + 1) & ~1u;
          }
      } }
    static const uint32_t bpps[] = { 16, 32 };
    uint32_t p = heap_alloc(124, 1);
    for (int b = 0; b < 2; b++)
        for (size_t i = 0; i < sizeof modes / sizeof modes[0]; i++) {
            if (filter && (R32(filter + 4) & DDSD_WIDTH) && R32(filter + 12) != modes[i][0]) continue;
            if (filter && (R32(filter + 4) & DDSD_HEIGHT) && R32(filter + 8) != modes[i][1]) continue;
            if (filter && (R32(filter + 4) & DDSD_PIXELFORMAT) && R32(filter + 84) != bpps[b]) continue;
            write_mode_desc(p, modes[i][0], modes[i][1], bpps[b]);
            if (getenv("RC_MODE_LOG")) fprintf(stderr, "[ddraw] offer %ux%u %u bpp\n", modes[i][0], modes[i][1], bpps[b]);
            uint32_t a[2] = { p, ctx };
            if (!guest_stdcall(c, cb, 2, a)) { heap_free(p); RET(5, DD_OK); }
        }
    heap_free(p);
    RET(5, DD_OK);
}
W(dd_GetDisplayMode) { write_mode_desc(ARG(1), g_disp_w, g_disp_h, g_disp_bpp); RET(2, DD_OK); }
W(dd_GetMonitorFrequency) { W32(ARG(1), 60); RET(2, DD_OK); }
W(dd_GetScanLine) { W32(ARG(1), 0); RET(2, DD_OK); }
W(dd_GetVerticalBlankStatus) { W32(ARG(1), 1); RET(2, DD_OK); }
W(dd_WaitForVerticalBlank) { RET(3, DD_OK); }
W(dd_GetFourCCCodes) {
    static const uint32_t codes[] = { FMT_DXT1, FMT_DXT2, FMT_DXT3, FMT_DXT4, FMT_DXT5 };
    uint32_t n = 5;
    if (ARG(2)) for (uint32_t i = 0; i < n && i < R32(ARG(1)); i++) W32(ARG(2) + 4 * i, codes[i]);
    W32(ARG(1), n);
    RET(3, DD_OK);
}
W(dd_GetAvailableVidMem) {
    if (ARG(2)) W32(ARG(2), 256u << 20);
    if (ARG(3)) W32(ARG(3), 200u << 20);
    RET(4, DD_OK);
}
W(dd_TestCooperativeLevel) { RET(1, DD_OK); }
W(dd_RestoreAllSurfaces) { RET(1, DD_OK); }
W(dd_GetDeviceIdentifier) {
    uint32_t p = ARG(1);
    memset(HP(p), 0, 1068);
    strcpy((char *)HP(p), "nv4_disp.dll");
    strcpy((char *)HP(p + 512), "NVIDIA GeForce2 GTS");
    W64(p + 1024, 0x0006000E000A1700ull);
    W32(p + 1032, 0x10DE); W32(p + 1036, 0x0150);
    W32(p + 1064, 1);
    RET(3, DD_OK);
}
W(dd_FlipToGDISurface) { RET(1, DD_OK); }
W(dd_Compact) { RET(1, DD_OK); }
W(dd_Initialize) { RET(2, DD_OK); }

W(dd_CreateSurface) {
    uint32_t sd = ARG(1), out = ARG(2);
    uint32_t fl = R32(sd + 4), caps = (fl & DDSD_CAPS) ? R32(sd + 104) : 0, caps2 = (fl & DDSD_CAPS) ? R32(sd + 108) : 0;
    uint32_t w = (fl & DDSD_WIDTH) ? R32(sd + 12) : 0, h = (fl & DDSD_HEIGHT) ? R32(sd + 8) : 0;
    uint32_t fmt, bpp;
    if (fl & DDSD_PIXELFORMAT) fmt = fmt_from_pf(sd + 72, &bpp);
    else if (caps & CAPS_ZBUFFER) {
        uint32_t zb = (fl & DDSD_ZBUFFERBITDEPTH) ? R32(sd + 20) : 16;
        fmt = zb == 16 ? FMT_D16 : FMT_D24X8; bpp = zb == 16 ? 16 : 32;
    } else { fmt = display_fmt(); bpp = g_disp_bpp == 16 ? 16 : 32; }
    if (caps & CAPS_PRIMARY) {
        w = g_fullscreen ? g_disp_w : (uint32_t)g_screen_w;
        h = g_fullscreen ? g_disp_h : (uint32_t)g_screen_h;
        if (g_fullscreen) { w = g_disp_w; h = g_disp_h; }
        fmt = display_fmt(); bpp = g_disp_bpp == 16 ? 16 : 32;
    }
    if (!w || !h) { w = g_disp_w; h = g_disp_h; }
    LOG("[ddraw] CreateSurface %ux%u fmt %x caps %x/%x flags %x\n", w, h, fmt, caps, caps2, fl);

    if (caps & CAPS_PRIMARY) {
        uint32_t nback = (fl & DDSD_BACKBUFFERCOUNT) ? R32(sd + 20) : 0;
        DDSurf *p = surf_new(caps | CAPS_FRONTBUFFER | CAPS_VISIBLE | CAPS_VIDEOMEMORY, caps2, w, h, fmt, bpp);
        if (nback) {
            DDSurf *b = surf_new(CAPS_BACKBUFFER | CAPS_FLIP | CAPS_COMPLEX | CAPS_VIDEOMEMORY | (caps & CAPS_3DDEVICE) | CAPS_3DDEVICE,
                                 0, w, h, fmt, bpp);
            p->back = b->self;
        }
        g_primary = p->self;
        W32(out, p->self);
        RET(4, DD_OK);
    }
    if (caps & CAPS_TEXTURE) {
        uint32_t levels = 1;
        if ((caps & CAPS_MIPMAP) && (caps & CAPS_COMPLEX)) {
            uint32_t maxl = 1;
            for (uint32_t x = w > h ? w : h; x > 1; x >>= 1) maxl++;
            levels = (fl & DDSD_MIPMAPCOUNT) && R32(sd + 24) ? R32(sd + 24) : maxl;
            if (levels > maxl) levels = maxl;
        }
        DDSurf *top = surf_new(caps, caps2, w, h, fmt, bpp), *prev = top;
        top->levels = (int)levels;
        for (uint32_t l = 1; l < levels; l++) {
            uint32_t lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1;
            DDSurf *m = surf_new(caps, caps2 | 0x10000 /* MIPMAPSUBLEVEL */, lw, lh, fmt, bpp);
            m->mip_top = top; m->level = (int)l;
            prev->mip_next = m->self;
            prev = m;
        }
        if (fl & DDSD_CKSRCBLT) { top->has_ck = 1; top->ck_lo = R32(sd + 64); top->ck_hi = R32(sd + 68); }
        W32(out, top->self);
        RET(4, DD_OK);
    }
    DDSurf *s = surf_new(caps, caps2, w, h, fmt, bpp);
    if (fl & DDSD_CKSRCBLT) { s->has_ck = 1; s->ck_lo = R32(sd + 64); s->ck_hi = R32(sd + 68); }
    W32(out, s->self);
    RET(4, DD_OK);
}

W(dd_CreateClipper) { W32(ARG(2), com_new(&IDirectDrawClipper_i, 0, NULL)); RET(4, DD_OK); }
W(dd_CreatePalette) {
    uint32_t *pal = calloc(256, 4);
    if (ARG(2)) memcpy(pal, HP(ARG(2)), 1024);
    W32(ARG(3), com_new(&IDirectDrawPalette_i, 0, pal));
    RET(5, DD_OK);
}
W(dd_GetGDISurface) { if (g_primary) { addref(g_primary); W32(ARG(1), g_primary); RET(2, DD_OK); } RET(2, DDERR_NOTFOUND); }

static const ComMethod dd_methods[] = {
    { "QueryInterface", 2, dd_QueryInterface }, { "AddRef", 0, com_AddRef }, { "Release", 0, com_Release },
    { "Compact", 0, dd_Compact }, { "CreateClipper", 3, dd_CreateClipper }, { "CreatePalette", 4, dd_CreatePalette },
    { "CreateSurface", 3, dd_CreateSurface }, { "DuplicateSurface", 2, NULL }, { "EnumDisplayModes", 4, dd_EnumDisplayModes },
    { "EnumSurfaces", 4, NULL }, { "FlipToGDISurface", 0, dd_FlipToGDISurface }, { "GetCaps", 2, dd_GetCaps },
    { "GetDisplayMode", 1, dd_GetDisplayMode }, { "GetFourCCCodes", 2, dd_GetFourCCCodes },
    { "GetGDISurface", 1, dd_GetGDISurface }, { "GetMonitorFrequency", 1, dd_GetMonitorFrequency },
    { "GetScanLine", 1, dd_GetScanLine }, { "GetVerticalBlankStatus", 1, dd_GetVerticalBlankStatus },
    { "Initialize", 1, dd_Initialize }, { "RestoreDisplayMode", 0, dd_RestoreDisplayMode },
    { "SetCooperativeLevel", 2, dd_SetCooperativeLevel }, { "SetDisplayMode", 5, dd_SetDisplayMode },
    { "WaitForVerticalBlank", 2, dd_WaitForVerticalBlank }, { "GetAvailableVidMem", 3, dd_GetAvailableVidMem },
    { "GetSurfaceFromDC", 2, NULL }, { "RestoreAllSurfaces", 0, dd_RestoreAllSurfaces },
    { "TestCooperativeLevel", 0, dd_TestCooperativeLevel }, { "GetDeviceIdentifier", 2, dd_GetDeviceIdentifier },
    { "StartModeTest", 3, NULL }, { "EvaluateMode", 2, NULL },
};
static ComInterface IDirectDraw7_i = { "IDirectDraw7", dd_methods, 30, 0 };

/* ------------------------------------------------------------------ IDirectDrawSurface7 */

#define SURF DDSurf *s = surf(THIS)

W(s_QueryInterface) {
    /* IDirectDrawSurface4/7 share the vtable; texture interfaces resolve to the surface too */
    addref(THIS);
    W32(ARG(2), THIS);
    RET(3, S_OK);
}

static void write_desc(DDSurf *s, uint32_t p) {
    memset(HP(p + 4), 0, 120);
    W32(p, 124);
    uint32_t fl = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT;
    W32(p + 8, s->h); W32(p + 12, s->w); W32(p + 16, s->pitch);
    if (s->back) { fl |= DDSD_BACKBUFFERCOUNT; W32(p + 20, 1); }
    if (s->mip_top->levels > 1 || s->mip_top != s) { fl |= DDSD_MIPMAPCOUNT; W32(p + 24, (uint32_t)(s->mip_top->levels - s->level)); }
    if (s->has_ck) { fl |= DDSD_CKSRCBLT; W32(p + 64, s->ck_lo); W32(p + 68, s->ck_hi); }
    write_pf(p + 72, s->fmt);
    W32(p + 104, s->caps); W32(p + 108, s->caps2);
    W32(p + 4, fl);
}
W(s_GetSurfaceDesc) { SURF; if (!s) RET(2, DDERR_INVALIDPARAMS); write_desc(s, ARG(1)); RET(2, DD_OK); }
W(s_GetPixelFormat) { SURF; if (!s) RET(2, DDERR_INVALIDPARAMS); write_pf(ARG(1), s->fmt); RET(2, DD_OK); }
W(s_GetCaps) {
    SURF;
    if (!s) RET(2, DDERR_INVALIDPARAMS);
    W32(ARG(1), s->caps); W32(ARG(1) + 4, s->caps2); W32(ARG(1) + 8, 0); W32(ARG(1) + 12, 0);
    RET(2, DD_OK);
}

W(s_Lock) {
    SURF;
    uint32_t rect = ARG(1), sd = ARG(2), flags = ARG(3);
    if (!s) RET(5, DDERR_INVALIDPARAMS);
    if (s->self == g_primary && g_dev_rt) {
        /* the front buffer: what was last presented is what the back buffer held */
    }
    if (is_target(s)) readback(s);
    uint32_t off = 0;
    if (rect) {
        int32_t x = (int32_t)R32(rect), y = (int32_t)R32(rect + 4);
        off = format_is_dxt(s->fmt) ? (uint32_t)(y / 4) * s->pitch + (uint32_t)(x / 4) * (s->fmt == FMT_DXT1 ? 8 : 16)
                                    : (uint32_t)y * s->pitch + (uint32_t)x * s->bpp / 8;
    }
    write_desc(s, sd);
    W32(sd + 4, R32(sd + 4) | DDSD_LPSURFACE);
    W32(sd + 36, s->mem + off);
    s->locked = 1;
    s->lock_flags = flags;
    RET(5, DD_OK);
}
W(s_Unlock) {
    SURF;
    if (!s) RET(2, DDERR_INVALIDPARAMS);
    s->locked = 0;
    if (s->lock_flags & 0x10 /* DDLOCK_READONLY */) RET(2, DD_OK);
    touch(s);
    if (is_target(s)) gpu_blit(s, 0, 0, (int32_t)s->w, (int32_t)s->h, 0, 0, (int32_t)s->w, (int32_t)s->h, 0);
    RET(2, DD_OK);
}

static void rect_of(uint32_t r, DDSurf *s, int32_t *x1, int32_t *y1, int32_t *x2, int32_t *y2) {
    if (r) { *x1 = (int32_t)R32(r); *y1 = (int32_t)R32(r + 4); *x2 = (int32_t)R32(r + 8); *y2 = (int32_t)R32(r + 12); }
    else { *x1 = 0; *y1 = 0; *x2 = (int32_t)s->w; *y2 = (int32_t)s->h; }
}

/* CPU blit between memory surfaces of the same depth: nearest-neighbour stretch, optional source key */
static void cpu_blit(DDSurf *d, int32_t dx1, int32_t dy1, int32_t dx2, int32_t dy2, DDSurf *s, int32_t sx1, int32_t sy1,
                     int32_t sx2, int32_t sy2, int keyed, uint32_t klo, uint32_t khi) {
    if (format_is_dxt(s->fmt) || format_is_dxt(d->fmt)) {
        if (s->fmt == d->fmt && s->w == d->w && s->h == d->h) memcpy(HP(d->mem), HP(s->mem), s->pitch * surf_rows(s->fmt, s->h));
        return;
    }
    uint32_t bs = s->bpp / 8, bd = d->bpp / 8;
    int32_t dw = dx2 - dx1, dh = dy2 - dy1, sw = sx2 - sx1, sh = sy2 - sy1;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    for (int32_t y = 0; y < dh; y++) {
        int32_t ty = dy1 + y, fy = sy1 + (int32_t)((int64_t)y * sh / dh);
        if (ty < 0 || ty >= (int32_t)d->h || fy < 0 || fy >= (int32_t)s->h) continue;
        uint8_t *drow = HP(d->mem + (uint32_t)ty * d->pitch);
        const uint8_t *srow = HP(s->mem + (uint32_t)fy * s->pitch);
        if (!keyed && sw == dw && bs == bd && dx1 >= 0 && dx2 <= (int32_t)d->w && sx1 >= 0 && sx2 <= (int32_t)s->w) {
            memmove(drow + dx1 * bd, srow + sx1 * bs, (size_t)dw * bd);
            continue;
        }
        for (int32_t x = 0; x < dw; x++) {
            int32_t tx = dx1 + x, fx = sx1 + (int32_t)((int64_t)x * sw / dw);
            if (tx < 0 || tx >= (int32_t)d->w || fx < 0 || fx >= (int32_t)s->w) continue;
            uint32_t v = 0;
            memcpy(&v, srow + fx * bs, bs);
            if (keyed && v >= klo && v <= khi) continue;
            if (bs == bd) memcpy(drow + tx * bd, &v, bd);
            else {
                uint32_t argb = pixel_to_argb(s->fmt, v), o;
                if (bd == 2) o = d->fmt == FMT_R5G6B5 ? ((argb >> 8) & 0xF800) | ((argb >> 5) & 0x7E0) | ((argb >> 3) & 0x1F)
                                                      : 0x8000 | ((argb >> 9) & 0x7C00) | ((argb >> 6) & 0x3E0) | ((argb >> 3) & 0x1F);
                else o = argb;
                memcpy(drow + tx * bd, &o, bd);
            }
        }
    }
}

static uint32_t blt_common(CPU *c, DDSurf *d, uint32_t drect, DDSurf *s, uint32_t srect, uint32_t flags, uint32_t fx) {
    int32_t dx1, dy1, dx2, dy2;
    rect_of(drect, d, &dx1, &dy1, &dx2, &dy2);
    (void)c;
    if (flags & 0x400 /* DDBLT_COLORFILL */) {
        uint32_t col = fx ? R32(fx + 80) : 0;
        if (is_target(d)) gpu_fill(1, dx1, dy1, dx2, dy2, pixel_to_argb(d->fmt, col), 1.f);
        else {
            uint32_t b = d->bpp / 8;
            for (int32_t y = dy1 < 0 ? 0 : dy1; y < dy2 && y < (int32_t)d->h; y++)
                for (int32_t x = dx1 < 0 ? 0 : dx1; x < dx2 && x < (int32_t)d->w; x++)
                    memcpy(HP(d->mem + (uint32_t)y * d->pitch + (uint32_t)x * b), &col, b);
            touch(d);
        }
        return DD_OK;
    }
    if (flags & 0x2000000 /* DDBLT_DEPTHFILL */) {
        uint32_t depth = fx ? R32(fx + 80) : 0xFFFF;
        float z = (float)depth / (d->bpp == 16 ? 65535.f : 4294967295.f);
        if (g_dev && g_dev_rt && g_dev_rt->zbuf == d->self) gpu_fill(2, dx1, dy1, dx2, dy2, 0, z);
        return DD_OK;
    }
    if (!s) return DD_OK;
    int32_t sx1, sy1, sx2, sy2;
    rect_of(srect, s, &sx1, &sy1, &sx2, &sy2);
    int keyed = 0;
    uint32_t klo = 0, khi = 0;
    if ((flags & 0x8000 /* DDBLT_KEYSRC */) && s->has_ck) { keyed = 1; klo = s->ck_lo; khi = s->ck_hi; }
    if ((flags & 0x10000 /* DDBLT_KEYSRCOVERRIDE */) && fx) { keyed = 1; klo = R32(fx + 92); khi = R32(fx + 96); }

    if (d->self == g_primary) {   /* windowed present: the back buffer copied to the screen */
        if (is_target(s)) present();
        return DD_OK;
    }
    if (is_target(d)) {
        if (is_target(s)) {   /* back buffer to itself: through memory */
            readback(s);
            touch(s);
        }
        if (keyed && (!s->has_ck || s->ck_lo != klo || s->ck_hi != khi)) {
            s->has_ck = 1; s->ck_lo = klo; s->ck_hi = khi;
            touch(s);
        }
        gpu_blit(s, sx1, sy1, sx2, sy2, dx1, dy1, dx2, dy2, keyed);
        return DD_OK;
    }
    if (is_target(s)) readback(s);
    cpu_blit(d, dx1, dy1, dx2, dy2, s, sx1, sy1, sx2, sy2, keyed, klo, khi);
    touch(d);
    return DD_OK;
}

W(s_Blt) {
    SURF;
    if (!s) RET(6, DDERR_INVALIDPARAMS);
    RET(6, blt_common(c, s, ARG(1), surf(ARG(2)), ARG(3), ARG(4), ARG(5)));
}
W(s_BltFast) {
    SURF;
    DDSurf *src = surf(ARG(3));
    if (!s || !src) RET(6, DDERR_INVALIDPARAMS);
    int32_t sx1, sy1, sx2, sy2;
    rect_of(ARG(4), src, &sx1, &sy1, &sx2, &sy2);
    uint32_t r = heap_alloc(16, 0);
    W32(r, ARG(1)); W32(r + 4, ARG(2)); W32(r + 8, ARG(1) + (uint32_t)(sx2 - sx1)); W32(r + 12, ARG(2) + (uint32_t)(sy2 - sy1));
    uint32_t flags = (ARG(5) & 1) ? 0x8000u : 0;   /* DDBLTFAST_SRCCOLORKEY -> DDBLT_KEYSRC */
    uint32_t hr = blt_common(c, s, r, src, ARG(4), flags, 0);
    heap_free(r);
    RET(6, hr);
}
W(s_Flip) {
    SURF;
    if (s && s->back && g_dev && g_dev_rt == surf(s->back)) present();
    else if (s && s->back) {   /* nothing rendered through Direct3D yet: show what 2D drew */
        DDSurf *b = surf(s->back);
        if (b) { device_for(b); present(); }
    }
    RET(3, DD_OK);
}
W(s_GetAttachedSurface) {
    SURF;
    uint32_t caps = R32(ARG(1)), out = ARG(2);
    if (!s) RET(3, DDERR_INVALIDPARAMS);
    uint32_t r = 0;
    if ((caps & CAPS_BACKBUFFER) && s->back) r = s->back;
    else if ((caps & CAPS_ZBUFFER) && s->zbuf) r = s->zbuf;
    else if ((caps & (CAPS_MIPMAP | CAPS_TEXTURE)) && s->mip_next) r = s->mip_next;
    else if (s->back && (caps & (CAPS_FLIP | CAPS_3DDEVICE))) r = s->back;
    if (!r) { W32(out, 0); RET(3, DDERR_NOTFOUND); }
    addref(r);
    W32(out, r);
    RET(3, DD_OK);
}
W(s_AddAttachedSurface) {
    SURF;
    DDSurf *a = surf(ARG(1));
    if (!s || !a) RET(2, DDERR_INVALIDPARAMS);
    if (a->caps & CAPS_ZBUFFER) {
        if (s->zbuf) release(s->zbuf);
        s->zbuf = a->self;
        addref(a->self);
        if (g_dev && g_dev_rt == s && !g_dev->depth) {
            g_dev->depth = new_depth(s, a);
            g_dev->depth_format = a->fmt;
            g_dev->st.depth_stencil = g_dev->depth;
        }
    }
    RET(2, DD_OK);
}
W(s_DeleteAttachedSurface) {
    SURF;
    if (s && s->zbuf && (!ARG(2) || ARG(2) == s->zbuf)) { release(s->zbuf); s->zbuf = 0; }
    RET(3, DD_OK);
}
W(s_SetColorKey) {
    SURF;
    if (!s) RET(3, DDERR_INVALIDPARAMS);
    uint32_t flags = ARG(1), ck = ARG(2);
    if (flags & 8 /* DDCKEY_SRCBLT */) {
        if (ck) { s->has_ck = 1; s->ck_lo = R32(ck); s->ck_hi = R32(ck + 4); }
        else s->has_ck = 0;
        touch(s);
    }
    RET(3, DD_OK);
}
W(s_GetColorKey) {
    SURF;
    if (!s || !(ARG(1) & 8) || !s->has_ck) RET(3, DDERR_NOCOLORKEY);
    W32(ARG(2), s->ck_lo); W32(ARG(2) + 4, s->ck_hi);
    RET(3, DD_OK);
}
W(s_GetDC) { W32(ARG(1), 0x00050020u); RET(2, DD_OK); }
W(s_ReleaseDC) { RET(2, DD_OK); }
W(s_IsLost) { RET(1, DD_OK); }
W(s_Restore) { RET(1, DD_OK); }
W(s_GetBltStatus) { RET(2, DD_OK); }
W(s_GetFlipStatus) { RET(2, DD_OK); }
W(s_SetClipper) { SURF; if (s) s->clipper = ARG(1); RET(2, DD_OK); }
W(s_GetClipper) { SURF; if (!s || !s->clipper) RET(2, DDERR_NOCLIPPERATTACHED); addref(s->clipper); W32(ARG(1), s->clipper); RET(2, DD_OK); }
W(s_SetPalette) { SURF; if (s) s->palette = ARG(1); RET(2, DD_OK); }
W(s_GetPalette) { SURF; if (!s || !s->palette) RET(2, DDERR_NOPALETTEATTACHED); addref(s->palette); W32(ARG(1), s->palette); RET(2, DD_OK); }
W(s_GetDDInterface) { if (g_dd_obj) addref(g_dd_obj); W32(ARG(1), g_dd_obj); RET(2, DD_OK); }
W(s_PageLock) { RET(2, DD_OK); }
W(s_PageUnlock) { RET(2, DD_OK); }
W(s_SetPrivateData) { RET(5, DD_OK); }
W(s_GetPrivateData) { RET(4, DDERR_NOTFOUND); }
W(s_FreePrivateData) { RET(2, DD_OK); }
W(s_GetUniquenessValue) { W32(ARG(1), 1); RET(2, DD_OK); }
W(s_ChangeUniquenessValue) { RET(1, DD_OK); }
W(s_SetPriority) { RET(2, DD_OK); }
W(s_GetPriority) { W32(ARG(1), 0); RET(2, DD_OK); }
W(s_SetLOD) { RET(2, DD_OK); }
W(s_GetLOD) { W32(ARG(1), 0); RET(2, DD_OK); }
W(s_EnumAttachedSurfaces) {
    SURF;
    uint32_t ctx = ARG(1), cb = ARG(2);
    if (!s) RET(3, DDERR_INVALIDPARAMS);
    uint32_t list[3] = { s->back, s->zbuf, s->mip_next };
    uint32_t sd = heap_alloc(124, 1);
    for (int i = 0; i < 3; i++) {
        DDSurf *a = surf(list[i]);
        if (!a) continue;
        addref(a->self);
        write_desc(a, sd);
        uint32_t args[3] = { a->self, sd, ctx };
        if (!guest_stdcall(c, cb, 3, args)) break;
    }
    heap_free(sd);
    RET(3, DD_OK);
}

static const ComMethod surf_methods[] = {
    { "QueryInterface", 2, s_QueryInterface }, { "AddRef", 0, com_AddRef }, { "Release", 0, com_Release },
    { "AddAttachedSurface", 1, s_AddAttachedSurface }, { "AddOverlayDirtyRect", 1, NULL }, { "Blt", 5, s_Blt },
    { "BltBatch", 3, NULL }, { "BltFast", 5, s_BltFast }, { "DeleteAttachedSurface", 2, s_DeleteAttachedSurface },
    { "EnumAttachedSurfaces", 2, s_EnumAttachedSurfaces }, { "EnumOverlayZOrders", 3, NULL }, { "Flip", 2, s_Flip },
    { "GetAttachedSurface", 2, s_GetAttachedSurface }, { "GetBltStatus", 1, s_GetBltStatus }, { "GetCaps", 1, s_GetCaps },
    { "GetClipper", 1, s_GetClipper }, { "GetColorKey", 2, s_GetColorKey }, { "GetDC", 1, s_GetDC },
    { "GetFlipStatus", 1, s_GetFlipStatus }, { "GetOverlayPosition", 2, NULL }, { "GetPalette", 1, s_GetPalette },
    { "GetPixelFormat", 1, s_GetPixelFormat }, { "GetSurfaceDesc", 1, s_GetSurfaceDesc }, { "Initialize", 2, NULL },
    { "IsLost", 0, s_IsLost }, { "Lock", 4, s_Lock }, { "ReleaseDC", 1, s_ReleaseDC }, { "Restore", 0, s_Restore },
    { "SetClipper", 1, s_SetClipper }, { "SetColorKey", 2, s_SetColorKey }, { "SetOverlayPosition", 2, NULL },
    { "SetPalette", 1, s_SetPalette }, { "Unlock", 1, s_Unlock }, { "UpdateOverlay", 5, NULL },
    { "UpdateOverlayDisplay", 1, NULL }, { "UpdateOverlayZOrder", 2, NULL }, { "GetDDInterface", 1, s_GetDDInterface },
    { "PageLock", 1, s_PageLock }, { "PageUnlock", 1, s_PageUnlock }, { "SetSurfaceDesc", 2, NULL },
    { "SetPrivateData", 4, s_SetPrivateData }, { "GetPrivateData", 3, s_GetPrivateData },
    { "FreePrivateData", 1, s_FreePrivateData }, { "GetUniquenessValue", 1, s_GetUniquenessValue },
    { "ChangeUniquenessValue", 0, s_ChangeUniquenessValue }, { "SetPriority", 1, s_SetPriority },
    { "GetPriority", 1, s_GetPriority }, { "SetLOD", 1, s_SetLOD }, { "GetLOD", 1, s_GetLOD },
};
static ComInterface IDirectDrawSurface7_i = { "IDirectDrawSurface7", surf_methods, 49, 0, destroy_surf };

/* texture views: only reachable through DevState.texture, never handed to the game */
static const ComMethod texview_methods[] = {
    { "QueryInterface", 2, com_QueryInterface }, { "AddRef", 0, com_AddRef }, { "Release", 0, com_Release },
};
static ComInterface TexView_i = { "D3D7TextureView", texview_methods, 3, 0 };

/* ------------------------------------------------------------------ clipper, palette */

W(clip_GetHWnd) { W32(ARG(1), MAIN_HWND); RET(2, DD_OK); }
W(clip_IsClipListChanged) { W32(ARG(1), 0); RET(2, DD_OK); }
static const ComMethod clip_methods[] = {
    { "QueryInterface", 2, com_QueryInterface }, { "AddRef", 0, com_AddRef }, { "Release", 0, com_Release },
    { "GetClipList", 3, NULL }, { "GetHWnd", 1, clip_GetHWnd }, { "Initialize", 2, NULL },
    { "IsClipListChanged", 1, clip_IsClipListChanged }, { "SetClipList", 2, NULL }, { "SetHWnd", 2, NULL },
};
static ComInterface IDirectDrawClipper_i = { "IDirectDrawClipper", clip_methods, 9, 0 };

W(pal_GetCaps) { W32(ARG(1), 4 /* DDPCAPS_8BIT */); RET(2, DD_OK); }
W(pal_GetEntries) {
    uint32_t *pal = com_data(THIS);
    uint32_t base = ARG(2), n = ARG(3);
    if (pal && base < 256) memcpy(HP(ARG(4)), pal + base, 4 * (base + n > 256 ? 256 - base : n));
    RET(5, DD_OK);
}
W(pal_SetEntries) {
    uint32_t *pal = com_data(THIS);
    uint32_t base = ARG(2), n = ARG(3);
    if (pal && base < 256) memcpy(pal + base, HP(ARG(4)), 4 * (base + n > 256 ? 256 - base : n));
    RET(5, DD_OK);
}
static void free_data(void *p) { free(p); }
static const ComMethod pal_methods[] = {
    { "QueryInterface", 2, com_QueryInterface }, { "AddRef", 0, com_AddRef }, { "Release", 0, com_Release },
    { "GetCaps", 1, pal_GetCaps }, { "GetEntries", 4, pal_GetEntries }, { "Initialize", 3, NULL },
    { "SetEntries", 4, pal_SetEntries },
};
static ComInterface IDirectDrawPalette_i = { "IDirectDrawPalette", pal_methods, 7, 0, free_data };

/* ------------------------------------------------------------------ IDirect3D7 */

static void fill_primcaps(uint32_t p) {
    W32(p, 56);
    W32(p + 4, 0x2 | 0x10 | 0x20 | 0x40);         /* MiscCaps: MASKZ, CULLNONE/CW/CCW */
    W32(p + 8, 0x1 | 0x10 | 0x80 | 0x100 | 0x2000 | 0x4000 | 0x10000 | 0x20000 | 0x100000 | 0x200000);  /* RasterCaps */
    W32(p + 12, 0xFF);                             /* ZCmpCaps */
    W32(p + 16, 0x1FFF); W32(p + 20, 0x1FFF);      /* Src/DestBlendCaps */
    W32(p + 24, 0xFF);                             /* AlphaCmpCaps */
    W32(p + 28, 0x00084208 | 0x4000 | 0x80000);    /* ShadeCaps */
    W32(p + 32, 0x1 | 0x4 | 0x8 | 0x80 | 0x400);   /* TextureCaps: PERSPECTIVE ALPHA TRANSPARENCY ALPHAPALETTE PROJECTED */
    W32(p + 36, 0x0303033F);                       /* TextureFilterCaps */
    W32(p + 40, 0xFF);                             /* TextureBlendCaps */
    W32(p + 44, 0x1F);                             /* TextureAddressCaps */
}
static void fill_devdesc7(uint32_t p, int tnl, const char *guid) {
    memset(HP(p), 0, 236);
    W32(p, 0x0009AFF1u | (tnl ? 0x10000u : 0));   /* DevCaps (HW raster, DrawPrimitives2Ex, ...) */
    fill_primcaps(p + 4);
    fill_primcaps(p + 60);
    W32(p + 116, 0x400 | 0x100);                  /* render depths 16, 32 */
    W32(p + 120, 0x400 | 0x200 | 0x100);          /* z depths 16, 24, 32 */
    W32(p + 124, 1); W32(p + 128, 1); W32(p + 132, 2048); W32(p + 136, 2048);
    W32(p + 140, 8192); W32(p + 144, 2048); W32(p + 148, 16);
    WF32(p + 152, -8192.f); WF32(p + 156, -8192.f); WF32(p + 160, 8192.f); WF32(p + 164, 8192.f);
    WF32(p + 168, 0.f);
    W32(p + 172, 0xFF);                            /* StencilCaps */
    W32(p + 176, 8);                               /* FVFCaps: 8 texture coordinate sets */
    W32(p + 180, 0x03FFFFFF);                      /* TextureOpCaps */
    W16(p + 184, 8); W16(p + 186, 2);              /* blend stages, simultaneous textures */
    W32(p + 188, 8);                               /* active lights */
    WF32(p + 192, 1e10f);
    write_guid(p + 196, guid);
    W16(p + 212, 6); W16(p + 214, 4);
    W32(p + 216, tnl ? 0x3Bu : 0u);                /* VertexProcessingCaps */
}

W(d3d_EnumDevices) {
    uint32_t cb = ARG(1), ctx = ARG(2);
    static const struct { const char *desc, *name, *guid; int tnl; } devs[] = {
        { "Microsoft Direct3D RGB Software Emulation", "RGB Emulation", IID_RGBDevice, 0 },
        { "Microsoft Direct3D Hardware acceleration through Direct3D HAL", "Direct3D HAL", IID_HALDevice, 0 },
        { "Microsoft Direct3D Hardware Transform and Lighting acceleration capable device", "Direct3D T&L HAL", IID_TnLHalDevice, 1 },
    };
    uint32_t mem = heap_alloc(1024, 1);
    for (int i = 0; i < 3; i++) {
        strcpy((char *)HP(mem), devs[i].desc);
        strcpy((char *)HP(mem + 128), devs[i].name);
        fill_devdesc7(mem + 256, devs[i].tnl, devs[i].guid);
        uint32_t a[4] = { mem, mem + 128, mem + 256, ctx };
        if (!guest_stdcall(c, cb, 4, a)) break;
    }
    heap_free(mem);
    RET(3, S_OK);
}
W(d3d_EnumZBufferFormats) {
    uint32_t cb = ARG(2), ctx = ARG(3);
    static const uint32_t fmts[] = { FMT_D16, FMT_D24X8, FMT_D24S8, FMT_D32 };
    uint32_t p = heap_alloc(32, 1);
    for (int i = 0; i < 4; i++) {
        write_pf(p, fmts[i]);
        uint32_t a[2] = { p, ctx };
        if (!guest_stdcall(c, cb, 2, a)) break;
    }
    heap_free(p);
    RET(4, S_OK);
}
W(d3d_EvictManagedTextures) { RET(1, S_OK); }

static void d3d7_default_states(Device *d);
W(d3d_CreateDevice) {
    DDSurf *rt = surf(ARG(2));
    if (!rt) RET(4, DDERR_INVALIDPARAMS);
    if (!rt->s8) {   /* not created as a 3D surface: give it render-target memory now */
        rt->s8 = d3d8_new_surface(rt->fmt, rt->w, rt->h, 1, 0);
        Surface *x = com_data(rt->s8);
        memcpy(HP(x->mem), HP(rt->mem), (size_t)rt->pitch * rt->h < (size_t)x->pitch * x->height ? (size_t)rt->pitch * rt->h : (size_t)x->pitch * x->height);
        heap_free(rt->mem);
        rt->mem = x->mem; rt->pitch = x->pitch;
    }
    Device *d = device_for(rt);
    d3d7_default_states(d);
    g_dev_obj = com_new(&IDirect3DDevice7_i, RT_DEVICE, d);
    W32(ARG(3), g_dev_obj);
    RET(4, S_OK);
}

typedef struct { uint32_t fvf, n, stride, mem, caps; } VB7;
static uint32_t fvf_stride(uint32_t fvf);
W(d3d_CreateVertexBuffer) {
    uint32_t desc = ARG(1);
    VB7 *b = calloc(1, sizeof *b);
    b->caps = R32(desc + 4); b->fvf = R32(desc + 8); b->n = R32(desc + 12);
    b->stride = fvf_stride(b->fvf);
    b->mem = heap_alloc(b->stride * b->n + 16, 1);
    W32(ARG(2), com_new(&IDirect3DVertexBuffer7_i, RT_VERTEXBUFFER, b));
    RET(4, S_OK);
}

static const ComMethod d3d_methods[] = {
    { "QueryInterface", 2, com_QueryInterface }, { "AddRef", 0, com_AddRef }, { "Release", 0, com_Release },
    { "EnumDevices", 2, d3d_EnumDevices }, { "CreateDevice", 3, d3d_CreateDevice },
    { "CreateVertexBuffer", 3, d3d_CreateVertexBuffer }, { "EnumZBufferFormats", 3, d3d_EnumZBufferFormats },
    { "EvictManagedTextures", 0, d3d_EvictManagedTextures },
};
static ComInterface IDirect3D7_i = { "IDirect3D7", d3d_methods, 8, 0 };

/* ------------------------------------------------------------------ IDirect3DDevice7 */

static Device *dev7(CPU *c) { return com_data(THIS); }

/* Direct3D 7 values for states whose encoding differs from Direct3D 8's, as the game set them */
static uint32_t g_tss7[8][32];

static void d3d7_default_states(Device *d) {
    for (int s = 0; s < 8; s++) {
        memcpy(g_tss7[s], d->st.tss[s], sizeof g_tss7[s]);
        g_tss7[s][18] = 1;                 /* MIPFILTER: D3DTFP_NONE */
        g_tss7[s][12] = 1;                 /* ADDRESS: wrap */
    }
    d->st.rs[4] = 1;                       /* TEXTUREPERSPECTIVE */
    d->st.rs[26] = 0;                      /* DITHERENABLE */
}

W(dv_GetCaps) { fill_devdesc7(ARG(1), 1, IID_TnLHalDevice); RET(2, S_OK); }
W(dv_EnumTextureFormats) {
    uint32_t cb = ARG(1), ctx = ARG(2);
    static const uint32_t fmts[] = { FMT_R5G6B5, FMT_X1R5G5B5, FMT_A1R5G5B5, FMT_A4R4G4B4, FMT_X8R8G8B8, FMT_A8R8G8B8,
                                     FMT_DXT1, FMT_DXT3, FMT_DXT5 };
    uint32_t p = heap_alloc(32, 1);
    for (size_t i = 0; i < sizeof fmts / sizeof fmts[0]; i++) {
        write_pf(p, fmts[i]);
        uint32_t a[2] = { p, ctx };
        if (!guest_stdcall(c, cb, 2, a)) break;
    }
    heap_free(p);
    RET(3, S_OK);
}
W(dv_BeginScene) { RET(1, S_OK); }
W(dv_EndScene) { RET(1, S_OK); }
W(dv_GetDirect3D) { if (g_d3d7_obj) addref(g_d3d7_obj); W32(ARG(1), g_d3d7_obj); RET(2, S_OK); }
W(dv_SetRenderTarget) {
    DDSurf *s = surf(ARG(1));
    if (s && s != g_dev_rt) fprintf(stderr, "[d3d7] SetRenderTarget to another surface is not supported\n");
    RET(3, S_OK);
}
W(dv_GetRenderTarget) {
    uint32_t r = g_dev_rt ? g_dev_rt->self : 0;
    if (r) addref(r);
    W32(ARG(1), r);
    RET(2, r ? S_OK : DDERR_NOTFOUND);
}
W(dv_Clear) {
    Device *d = dev7(c);
    if (g_render->clear) {
        float z;
        uint32_t zb = ARG(5);
        memcpy(&z, &zb, 4);
        g_render->clear(d, ARG(1), ARG(2), ARG(3), ARG(4), z, ARG(6));
    }
    RET(7, S_OK);
}
static uint32_t ts7(uint32_t t) { return t == 1 ? 256 : t < MAX_TS ? t : MAX_TS - 1; }   /* WORLD is 1 in Direct3D 7 */
W(dv_SetTransform) { memcpy(dev7(c)->st.transform[ts7(ARG(1))], HP(ARG(2)), 64); RET(3, S_OK); }
W(dv_GetTransform) { memcpy(HP(ARG(2)), dev7(c)->st.transform[ts7(ARG(1))], 64); RET(3, S_OK); }
W(dv_MultiplyTransform) {
    float *m = dev7(c)->st.transform[ts7(ARG(1))], r[16], b[16];
    memcpy(b, HP(ARG(2)), 64);
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) {
        float s = 0; for (int k = 0; k < 4; k++) s += m[i * 4 + k] * b[k * 4 + j]; r[i * 4 + j] = s;
    }
    memcpy(m, r, 64);
    RET(3, S_OK);
}
W(dv_SetViewport) { memcpy(&dev7(c)->st.viewport, HP(ARG(1)), 24); RET(2, S_OK); }
W(dv_GetViewport) { memcpy(HP(ARG(1)), &dev7(c)->st.viewport, 24); RET(2, S_OK); }
W(dv_SetMaterial) { memcpy(dev7(c)->st.material, HP(ARG(1)), 68); RET(2, S_OK); }
W(dv_GetMaterial) { memcpy(HP(ARG(1)), dev7(c)->st.material, 68); RET(2, S_OK); }
W(dv_SetLight) { if (ARG(1) < 8) memcpy(dev7(c)->st.light[ARG(1)], HP(ARG(2)), 104); RET(3, S_OK); }
W(dv_GetLight) { if (ARG(1) < 8) memcpy(HP(ARG(2)), dev7(c)->st.light[ARG(1)], 104); RET(3, S_OK); }
W(dv_LightEnable) { if (ARG(1) < 8) dev7(c)->st.light_enabled[ARG(1)] = ARG(2); RET(3, S_OK); }
W(dv_GetLightEnable) { W32(ARG(2), ARG(1) < 8 ? dev7(c)->st.light_enabled[ARG(1)] : 0); RET(3, S_OK); }
W(dv_SetClipPlane) { if (ARG(1) < 6) memcpy(dev7(c)->st.clip_plane[ARG(1)], HP(ARG(2)), 16); RET(3, S_OK); }
W(dv_GetClipPlane) { if (ARG(1) < 6) memcpy(HP(ARG(2)), dev7(c)->st.clip_plane[ARG(1)], 16); RET(3, S_OK); }

static void set_tss(Device *d, uint32_t s, uint32_t t, uint32_t v);
W(dv_SetRenderState) {
    Device *d = dev7(c);
    uint32_t st = ARG(1), v = ARG(2);
    if (st < 256) d->st.rs[st] = v;
    switch (st) {   /* legacy states that alias texture stage 0 */
    case 3: set_tss(d, 0, 12, v); break;          /* TEXTUREADDRESS */
    case 44: set_tss(d, 0, 13, v); break;         /* TEXTUREADDRESSU */
    case 45: set_tss(d, 0, 14, v); break;         /* TEXTUREADDRESSV */
    case 17: set_tss(d, 0, 16, v == 2 || v == 4 || v == 6 ? 2 : 1); break;   /* TEXTUREMAG */
    case 18: set_tss(d, 0, 17, v == 2 || v == 4 || v == 6 ? 2 : 1); break;   /* TEXTUREMIN */
    }
    RET(3, S_OK);
}
W(dv_GetRenderState) { W32(ARG(2), ARG(1) < 256 ? dev7(c)->st.rs[ARG(1)] : 0); RET(3, S_OK); }

static void set_tss(Device *d, uint32_t s, uint32_t t, uint32_t v) {
    if (s >= 8 || t >= 32) return;
    g_tss7[s][t] = v;
    uint32_t *x = d->st.tss[s];
    switch (t) {
    case 12: x[13] = x[14] = v; g_tss7[s][13] = g_tss7[s][14] = v; break;   /* ADDRESS: both */
    case 18: x[18] = v ? v - 1 : 0; break;        /* MIPFILTER: D3DTFP_NONE/POINT/LINEAR = 1/2/3 */
    case 16: x[16] = v >= 2 ? 2 : 1; break;       /* MAGFILTER: anything better than point samples linearly */
    case 17: x[17] = v >= 2 ? 2 : 1; break;
    default: x[t] = v; break;
    }
}
W(dv_SetTextureStageState) { set_tss(dev7(c), ARG(1), ARG(2), ARG(3)); RET(4, S_OK); }
W(dv_GetTextureStageState) { W32(ARG(3), ARG(1) < 8 && ARG(2) < 32 ? g_tss7[ARG(1)][ARG(2)] : 0); RET(4, S_OK); }

static uint32_t g_tex7[8];   /* the surfaces set as textures (DevState holds their texture views) */
W(dv_SetTexture) {
    Device *d = dev7(c);
    uint32_t stage = ARG(1);
    if (stage >= 8) RET(3, S_OK);
    DDSurf *s = surf(ARG(2));
    g_tex7[stage] = s ? s->self : 0;
    if (s) { surf_tex(s); d->st.texture[stage] = s->mip_top->tex_obj; }
    else d->st.texture[stage] = 0;
    RET(3, S_OK);
}
W(dv_GetTexture) {
    uint32_t stage = ARG(1), t = stage < 8 ? g_tex7[stage] : 0;
    if (t && surf(t)) addref(t); else t = 0;
    W32(ARG(2), t);
    RET(3, S_OK);
}
W(dv_ValidateDevice) { if (ARG(1)) W32(ARG(1), 1); RET(2, S_OK); }
W(dv_PreLoad) { RET(2, S_OK); }
W(dv_SetClipStatus) { RET(2, S_OK); }
W(dv_GetClipStatus) { memset(HP(ARG(1)), 0, 24); RET(2, S_OK); }
W(dv_GetInfo) { RET(4, 1); }

/* state blocks: full snapshots */
#define MAX_BLOCKS7 1024
static DevState *g_blocks7[MAX_BLOCKS7];
static uint32_t new_block7(Device *d) {
    for (uint32_t i = 1; i < MAX_BLOCKS7; i++) if (!g_blocks7[i]) {
        g_blocks7[i] = malloc(sizeof(DevState));
        *g_blocks7[i] = d->st;
        return i;
    }
    return 0;
}
W(dv_BeginStateBlock) { RET(1, S_OK); }
W(dv_EndStateBlock) { W32(ARG(1), new_block7(dev7(c))); RET(2, S_OK); }
W(dv_ApplyStateBlock) {
    Device *d = dev7(c);
    uint32_t i = ARG(1);
    if (i < MAX_BLOCKS7 && g_blocks7[i]) {
        uint32_t rt = d->st.render_target, ds = d->st.depth_stencil;
        d->st = *g_blocks7[i];
        d->st.render_target = rt; d->st.depth_stencil = ds;
    }
    RET(2, S_OK);
}
W(dv_CaptureStateBlock) { uint32_t i = ARG(1); if (i < MAX_BLOCKS7 && g_blocks7[i]) *g_blocks7[i] = dev7(c)->st; RET(2, S_OK); }
W(dv_DeleteStateBlock) { uint32_t i = ARG(1); if (i < MAX_BLOCKS7) { free(g_blocks7[i]); g_blocks7[i] = NULL; } RET(2, S_OK); }
W(dv_CreateStateBlock) { W32(ARG(2), new_block7(dev7(c))); RET(3, S_OK); }

/* ---- drawing */

static uint32_t fvf_stride(uint32_t fvf) {
    uint32_t off = 0;
    switch (fvf & 0xE) {
    case 0x2: off = 12; break; case 0x4: off = 16; break; case 0x6: off = 16; break; case 0x8: off = 20; break;
    case 0xA: off = 24; break; case 0xC: off = 28; break; case 0xE: off = 32; break;
    }
    if (fvf & 0x10) off += 12;
    if (fvf & 0x20) off += 4;
    if (fvf & 0x40) off += 4;
    if (fvf & 0x80) off += 4;
    uint32_t nt = (fvf >> 8) & 0xF;
    for (uint32_t i = 0; i < nt && i < 8; i++) {
        uint32_t f = (fvf >> (16 + 2 * i)) & 3;
        off += f == 0 ? 8 : f == 1 ? 12 : f == 2 ? 16 : 4;
    }
    return off;
}
static uint32_t prim_count(uint32_t prim, uint32_t n) {
    switch (prim) {
    case 1: return n;
    case 2: return n / 2;
    case 3: return n ? n - 1 : 0;
    case 4: return n / 3;
    case 5: case 6: return n >= 2 ? n - 2 : 0;
    default: return 0;
    }
}
static void draw7(Device *d, uint32_t prim, uint32_t fvf, const uint8_t *verts, uint32_t nverts, const uint16_t *idx,
                  uint32_t nidx) {
    d->draws_this_frame++;
    if (!g_render->draw || !verts) return;
    d->st.vs = fvf;
    uint32_t pc = prim_count(prim, idx ? nidx : nverts);
    if (!pc) return;
    g_render->draw(d, prim, pc, 0, nverts, verts, fvf_stride(fvf), idx, 0, 0);
}
W(dv_DrawPrimitive) {
    draw7(dev7(c), ARG(1), ARG(2), HP(ARG(3)), ARG(4), NULL, 0);
    RET(6, S_OK);
}
W(dv_DrawIndexedPrimitive) {
    draw7(dev7(c), ARG(1), ARG(2), HP(ARG(3)), ARG(4), (const uint16_t *)HP(ARG(5)), ARG(6));
    RET(8, S_OK);
}
W(dv_DrawPrimitiveVB) {
    VB7 *b = com_data(ARG(2));
    if (b) draw7(dev7(c), ARG(1), b->fvf, HP(b->mem + ARG(3) * b->stride), ARG(4), NULL, 0);
    RET(6, S_OK);
}
W(dv_DrawIndexedPrimitiveVB) {
    VB7 *b = com_data(ARG(2));
    if (b) draw7(dev7(c), ARG(1), b->fvf, HP(b->mem + ARG(3) * b->stride), ARG(4), (const uint16_t *)HP(ARG(5)), ARG(6));
    RET(8, S_OK);
}
/* Strided data: D3DDRAWPRIMITIVESTRIDEDDATA = position, normal, diffuse, specular, texCoords[8], each
   { lpvData, dwStride }. Interleave into an ordinary FVF vertex array. */
static uint8_t *interleave(uint32_t fvf, uint32_t sd, uint32_t n) {
    uint32_t stride = fvf_stride(fvf);
    uint8_t *out = malloc((size_t)stride * (n ? n : 1));
    uint32_t possz = (fvf & 0xE) == 0x4 ? 16 : (fvf & 0xE) == 0x2 ? 12 : 12 + 4 * (((fvf & 0xE) - 0x4) / 2);
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *o = out + (size_t)i * stride;
        uint32_t off = 0;
        memcpy(o, HP(R32(sd) + i * R32(sd + 4)), possz); off += possz;
        if (fvf & 0x10) { memcpy(o + off, HP(R32(sd + 8) + i * R32(sd + 12)), 12); off += 12; }
        if (fvf & 0x40) { memcpy(o + off, HP(R32(sd + 16) + i * R32(sd + 20)), 4); off += 4; }
        if (fvf & 0x80) { memcpy(o + off, HP(R32(sd + 24) + i * R32(sd + 28)), 4); off += 4; }
        uint32_t nt = (fvf >> 8) & 0xF;
        for (uint32_t t = 0; t < nt && t < 8; t++) {
            uint32_t f = (fvf >> (16 + 2 * t)) & 3, sz = f == 0 ? 8 : f == 1 ? 12 : f == 2 ? 16 : 4;
            memcpy(o + off, HP(R32(sd + 32 + 8 * t) + i * R32(sd + 36 + 8 * t)), sz);
            off += sz;
        }
    }
    return out;
}
W(dv_DrawPrimitiveStrided) {
    uint8_t *v = interleave(ARG(2), ARG(3), ARG(4));
    draw7(dev7(c), ARG(1), ARG(2), v, ARG(4), NULL, 0);
    free(v);
    RET(6, S_OK);
}
W(dv_DrawIndexedPrimitiveStrided) {
    uint8_t *v = interleave(ARG(2), ARG(3), ARG(4));
    draw7(dev7(c), ARG(1), ARG(2), v, ARG(4), (const uint16_t *)HP(ARG(5)), ARG(6));
    free(v);
    RET(8, S_OK);
}

static void mat_mul(const float *a, const float *b, float *r) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a[i * 4 + k] * b[k * 4 + j];
            r[i * 4 + j] = s;
        }
}

/* Sphere visibility against the view frustum of WORLD * VIEW * PROJECTION (D3D clip space: 0 <= z <= w).
   Per sphere: D3DSTATUS_CLIPUNION* bits (low) for planes it touches, CLIPINTERSECTION* (<< 12) for planes
   it lies completely outside of. */
W(dv_ComputeSphereVisibility) {
    Device *d = dev7(c);
    uint32_t centers = ARG(1), radii = ARG(2), n = ARG(3), out = ARG(5);
    float wv[16], m[16];
    mat_mul(d->st.transform[256], d->st.transform[2], wv);
    mat_mul(wv, d->st.transform[3], m);
    /* planes from the columns of the row-vector matrix: p' = p * M */
    float col[4][4];
    for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) col[j][i] = m[i * 4 + j];
    float pl[6][4];
    for (int i = 0; i < 4; i++) {
        pl[0][i] = col[3][i] + col[0][i];   /* left:   x >= -w */
        pl[1][i] = col[3][i] - col[0][i];   /* right:  x <= w */
        pl[2][i] = col[3][i] - col[1][i];   /* top:    y <= w */
        pl[3][i] = col[3][i] + col[1][i];   /* bottom: y >= -w */
        pl[4][i] = col[2][i];               /* front:  z >= 0 */
        pl[5][i] = col[3][i] - col[2][i];   /* back:   z <= w */
    }
    for (int k = 0; k < 6; k++) {
        float l = sqrtf(pl[k][0] * pl[k][0] + pl[k][1] * pl[k][1] + pl[k][2] * pl[k][2]);
        if (l > 0) for (int i = 0; i < 4; i++) pl[k][i] /= l;
    }
    for (uint32_t s = 0; s < n; s++) {
        float x = RF32(centers + 12 * s), y = RF32(centers + 12 * s + 4), z = RF32(centers + 12 * s + 8), r = RF32(radii + 4 * s);
        uint32_t res = 0;
        for (int k = 0; k < 6; k++) {
            float dist = pl[k][0] * x + pl[k][1] * y + pl[k][2] * z + pl[k][3];
            if (dist < -r) res |= (1u << k) | (1u << (k + 12));
            else if (dist < r) res |= 1u << k;
        }
        W32(out + 4 * s, res);
    }
    RET(6, S_OK);
}

W(dv_Load) {
    DDSurf *dst = surf(ARG(1)), *src = surf(ARG(3));
    if (dst && src) {
        DDSurf *a = dst, *b = src;
        while (a && b) {
            if (a->pitch == b->pitch && a->h == b->h) memcpy(HP(a->mem), HP(b->mem), a->pitch * surf_rows(a->fmt, a->h));
            else cpu_blit(a, 0, 0, (int32_t)a->w, (int32_t)a->h, b, 0, 0, (int32_t)b->w, (int32_t)b->h, 0, 0, 0);
            a = a->mip_next ? surf(a->mip_next) : NULL;
            b = b->mip_next ? surf(b->mip_next) : NULL;
        }
        dst->has_ck = src->has_ck; dst->ck_lo = src->ck_lo; dst->ck_hi = src->ck_hi;
        touch(dst);
    }
    RET(6, S_OK);
}

static const ComMethod dev_methods[] = {
    { "QueryInterface", 2, com_QueryInterface }, { "AddRef", 0, com_AddRef }, { "Release", 0, com_Release },
    { "GetCaps", 1, dv_GetCaps }, { "EnumTextureFormats", 2, dv_EnumTextureFormats }, { "BeginScene", 0, dv_BeginScene },
    { "EndScene", 0, dv_EndScene }, { "GetDirect3D", 1, dv_GetDirect3D }, { "SetRenderTarget", 2, dv_SetRenderTarget },
    { "GetRenderTarget", 1, dv_GetRenderTarget }, { "Clear", 6, dv_Clear }, { "SetTransform", 2, dv_SetTransform },
    { "GetTransform", 2, dv_GetTransform }, { "SetViewport", 1, dv_SetViewport },
    { "MultiplyTransform", 2, dv_MultiplyTransform }, { "GetViewport", 1, dv_GetViewport },
    { "SetMaterial", 1, dv_SetMaterial }, { "GetMaterial", 1, dv_GetMaterial }, { "SetLight", 2, dv_SetLight },
    { "GetLight", 2, dv_GetLight }, { "SetRenderState", 2, dv_SetRenderState }, { "GetRenderState", 2, dv_GetRenderState },
    { "BeginStateBlock", 0, dv_BeginStateBlock }, { "EndStateBlock", 1, dv_EndStateBlock }, { "PreLoad", 1, dv_PreLoad },
    { "DrawPrimitive", 5, dv_DrawPrimitive }, { "DrawIndexedPrimitive", 7, dv_DrawIndexedPrimitive },
    { "SetClipStatus", 1, dv_SetClipStatus }, { "GetClipStatus", 1, dv_GetClipStatus },
    { "DrawPrimitiveStrided", 5, dv_DrawPrimitiveStrided },
    { "DrawIndexedPrimitiveStrided", 7, dv_DrawIndexedPrimitiveStrided }, { "DrawPrimitiveVB", 5, dv_DrawPrimitiveVB },
    { "DrawIndexedPrimitiveVB", 7, dv_DrawIndexedPrimitiveVB },
    { "ComputeSphereVisibility", 5, dv_ComputeSphereVisibility }, { "GetTexture", 2, dv_GetTexture },
    { "SetTexture", 2, dv_SetTexture }, { "GetTextureStageState", 3, dv_GetTextureStageState },
    { "SetTextureStageState", 3, dv_SetTextureStageState }, { "ValidateDevice", 1, dv_ValidateDevice },
    { "ApplyStateBlock", 1, dv_ApplyStateBlock }, { "CaptureStateBlock", 1, dv_CaptureStateBlock },
    { "DeleteStateBlock", 1, dv_DeleteStateBlock }, { "CreateStateBlock", 2, dv_CreateStateBlock },
    { "Load", 5, dv_Load }, { "LightEnable", 2, dv_LightEnable }, { "GetLightEnable", 2, dv_GetLightEnable },
    { "SetClipPlane", 2, dv_SetClipPlane }, { "GetClipPlane", 2, dv_GetClipPlane }, { "GetInfo", 3, dv_GetInfo },
};
static ComInterface IDirect3DDevice7_i = { "IDirect3DDevice7", dev_methods, 49, 0 };

/* ------------------------------------------------------------------ IDirect3DVertexBuffer7 */

W(vb_Lock) {
    VB7 *b = com_data(THIS);
    if (!b) RET(4, DDERR_INVALIDPARAMS);
    W32(ARG(2), b->mem);
    if (ARG(3)) W32(ARG(3), b->stride * b->n);
    RET(4, S_OK);
}
W(vb_Unlock) { RET(1, S_OK); }
W(vb_GetVertexBufferDesc) {
    VB7 *b = com_data(THIS);
    uint32_t p = ARG(1);
    if (!b) RET(2, DDERR_INVALIDPARAMS);
    W32(p, 16); W32(p + 4, b->caps); W32(p + 8, b->fvf); W32(p + 12, b->n);
    RET(2, S_OK);
}
W(vb_Optimize) { RET(3, S_OK); }
/* ProcessVertices(op, dst index, count, src VB, src index, device, flags): transform to screen space */
W(vb_ProcessVertices) {
    VB7 *dst = com_data(THIS), *src = com_data(ARG(4));
    Device *d = com_data(ARG(6));
    uint32_t di = ARG(2), n = ARG(3), si = ARG(5);
    if (!dst || !src || !d) RET(8, DDERR_INVALIDPARAMS);
    float wv[16], m[16];
    mat_mul(d->st.transform[256], d->st.transform[2], wv);
    mat_mul(wv, d->st.transform[3], m);
    float vx = (float)d->st.viewport.x, vy = (float)d->st.viewport.y, vw = (float)d->st.viewport.w, vh = (float)d->st.viewport.h;
    float mnz = d->st.viewport.minz, mxz = d->st.viewport.maxz;
    uint32_t ss = src->stride, ds = dst->stride;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t s = src->mem + (si + i) * ss, o = dst->mem + (di + i) * ds;
        float x = RF32(s), y = RF32(s + 4), z = RF32(s + 8);
        float cx = x * m[0] + y * m[4] + z * m[8] + m[12], cy = x * m[1] + y * m[5] + z * m[9] + m[13];
        float cz = x * m[2] + y * m[6] + z * m[10] + m[14], cw = x * m[3] + y * m[7] + z * m[11] + m[15];
        float rhw = cw != 0 ? 1.f / cw : 1.f;
        WF32(o, vx + (cx * rhw + 1.f) * 0.5f * vw);
        WF32(o + 4, vy + (1.f - cy * rhw) * 0.5f * vh);
        WF32(o + 8, mnz + cz * rhw * (mxz - mnz));
        WF32(o + 12, rhw);
        /* copy the remaining components the two formats share */
        uint32_t soff = fvf_stride(src->fvf & 0xE), doff = 16;
        if (src->fvf & 0x10) soff += 12;
        if (src->fvf & 0x20) soff += 4;
        if (dst->fvf & 0x40) { W32(o + doff, (src->fvf & 0x40) ? R32(s + soff) : 0xFFFFFFFFu); doff += 4; }
        if (src->fvf & 0x40) soff += 4;
        if (dst->fvf & 0x80) { W32(o + doff, (src->fvf & 0x80) ? R32(s + soff) : 0); doff += 4; }
        if (src->fvf & 0x80) soff += 4;
        uint32_t nt = (dst->fvf >> 8) & 0xF, ns = (src->fvf >> 8) & 0xF;
        for (uint32_t t = 0; t < nt && t < ns; t++) { W32(o + doff, R32(s + soff)); W32(o + doff + 4, R32(s + soff + 4)); doff += 8; soff += 8; }
    }
    RET(8, S_OK);
}
static void destroy_vb(void *p) { VB7 *b = p; heap_free(b->mem); free(b); }
static const ComMethod vb_methods[] = {
    { "QueryInterface", 2, com_QueryInterface }, { "AddRef", 0, com_AddRef }, { "Release", 0, com_Release },
    { "Lock", 3, vb_Lock }, { "Unlock", 0, vb_Unlock }, { "ProcessVertices", 7, vb_ProcessVertices },
    { "GetVertexBufferDesc", 1, vb_GetVertexBufferDesc }, { "Optimize", 2, vb_Optimize },
    { "ProcessVerticesStrided", 7, NULL },
};
static ComInterface IDirect3DVertexBuffer7_i = { "IDirect3DVertexBuffer7", vb_methods, 9, 0, destroy_vb };

/* ------------------------------------------------------------------ DDRAW.DLL exports */

static void w_DirectDrawCreateEx(CPU *c) {
    fprintf(stderr, "[ddraw] DirectDrawCreateEx -> DirectDraw 7 / Direct3D 7 (%s backend)\n", g_render->name);
    if (!g_dd_obj) g_dd_obj = com_new(&IDirectDraw7_i, 0, NULL);
    W32(ARG(1), g_dd_obj);
    RET(4, DD_OK);
}
/* the old entry point: games probe it (then QueryInterface for IDirectDraw7) to check the DirectX version */
static void w_DirectDrawCreate(CPU *c) {
    if (!g_dd_obj) g_dd_obj = com_new(&IDirectDraw7_i, 0, NULL);
    W32(ARG(1), g_dd_obj);
    RET(3, DD_OK);
}
static void w_DirectDrawEnumerateExA(CPU *c) {
    uint32_t cb = ARG(0), ctx = ARG(1);
    uint32_t p = heap_alloc(256, 1);
    strcpy((char *)HP(p), "Primary Display Driver");
    strcpy((char *)HP(p + 128), "display");
    uint32_t a[5] = { 0, p, p + 128, ctx, 0 };
    guest_stdcall(c, cb, 5, a);
    heap_free(p);
    RET(3, DD_OK);
}
void w_DirectInputCreateEx(CPU *c);   /* dinput.c */

static const ImportHandler dd_list[] = {
    { "DDRAW.DLL!DirectDrawCreateEx", w_DirectDrawCreateEx },
    { "DDRAW.DLL!DirectDrawEnumerateExA", w_DirectDrawEnumerateExA },
    { "DDRAW.DLL!DirectDrawCreate", w_DirectDrawCreate },
    { "DINPUT.DLL!DirectInputCreateEx", w_DirectInputCreateEx },
};
const ImportTable ddraw7_imports = { dd_list, sizeof dd_list / sizeof dd_list[0] };
