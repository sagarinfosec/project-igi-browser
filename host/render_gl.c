/* Second render executor: WebGL 2 (OpenGL ES 3.0), for browsers where WebGPU is missing or gives no adapter
 * (older iPhones, Android phones whose GPU Chrome keeps off WebGPU, blocklisted PC drivers: about a quarter of
 * the visitors who tried to play on 2026-10-31). It executes the same commands as render_wgpu.c (render_cmd.h)
 * on the page's thread; render_wgpu.c keeps the frame queue and the pump and hands each command here when the
 * page started this executor (rc_gl_init instead of rc_gpu_init). Web only.
 *
 * Conventions, kept identical to the WebGPU executor so the pictures match:
 *  - Every target (the back buffer and render-target textures) stores its rows top first, as Direct3D does:
 *    the vertex shader negates clip y, so viewports use Direct3D's y unchanged and render targets sample the
 *    right way up. The back buffer is blitted to the canvas flipped at present.
 *  - Clip z: WebGPU's [0, w] is turned into OpenGL's [-w, w] in the vertex shader; glDepthRangef takes the
 *    viewport's min/max z.
 *  - Front faces: Direct3D's are clockwise on screen (y down); with rows top first that is counter-clockwise in
 *    OpenGL's y-up window space, so glFrontFace(GL_CCW) and D3DCULL_CCW (3) culls GL_BACK.
 *  - Fixed function only: ps.1.x pixel shaders draw with the fixed-function stages (IGI is a Direct3D 7 game and
 *    has none). DXT textures stay compressed when the browser exposes WEBGL_compressed_texture_s3tc. */
#ifdef __EMSCRIPTEN__
#include "render.h"
#include "render_cmd.h"
#include "win32.h"

#include <emscripten.h>
#include <emscripten/html5.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int g_window_w, g_window_h;
extern int g_gpu_bc;
int g_use_gl;   /* render_wgpu.c's pump hands commands to gl_exec */
void web_set_ready(int on);   /* render_wgpu.c */

static const char *VS =
"#version 300 es\n"
"precision highp float; precision highp int;\n"
"layout(std140) uniform U { mat4 mvp; vec4 vp; uvec4 flags; uvec4 st0; uvec4 st0b; uvec4 st1; uvec4 st1b;\n"
"  vec4 tfactor; vec4 misc; vec4 mat_diffuse; vec4 ambient; vec4 psc[8]; vec4 bem[4]; vec4 bel[4]; vec4 fog; } u;\n"
"layout(location = 0) in vec4 a_pos;\n"
"layout(location = 1) in vec4 a_dif;   // D3DCOLOR bytes (b, g, r, a), normalized\n"
"layout(location = 2) in vec4 a_spc;\n"
"layout(location = 3) in vec4 a_tc0;\n"
"layout(location = 4) in vec4 a_tc1;\n"
"out vec4 v_dif; out vec4 v_spc; out vec4 v_tc0; out vec4 v_tc1;\n"
"void main() {\n"
"  vec4 p;\n"
"  if (u.flags.x == 1u) {\n"
"    float w = a_pos.w == 0.0 ? 1.0 : 1.0 / a_pos.w;\n"
"    float nx = (a_pos.x + 0.5 - u.vp.x) / u.vp.z * 2.0 - 1.0;\n"
"    float ny = 1.0 - (a_pos.y + 0.5 - u.vp.y) / u.vp.w * 2.0;\n"
"    p = vec4(nx * w, ny * w, a_pos.z * w, w);\n"
"  } else {\n"
"    p = u.mvp * vec4(a_pos.xyz, 1.0);\n"
"    if (u.flags.x == 2u) { p = a_pos; }\n"
"    p.x = p.x * u.misc.y;\n"
"    p.x = p.x + p.w / u.vp.z;\n"
"    p.y = p.y - p.w / u.vp.w;\n"
"  }\n"
"  p.z = 2.0 * p.z - p.w;   // WebGPU clip z [0, w] -> OpenGL [-w, w]\n"
"  p.y = -p.y;              // rows top first\n"
"  gl_Position = p;\n"
"  v_dif = a_dif.zyxw;\n"
"  if (u.flags.w == 1u) { v_dif = clamp(v_dif * u.mat_diffuse + u.ambient, vec4(0.0), vec4(1.0)); }\n"
"  v_spc = a_spc.zyxw;\n"
"  v_tc0 = a_tc0; v_tc1 = a_tc1;\n"
"}\n";

static const char *FS =
"#version 300 es\n"
"precision highp float; precision highp int;\n"
"layout(std140) uniform U { mat4 mvp; vec4 vp; uvec4 flags; uvec4 st0; uvec4 st0b; uvec4 st1; uvec4 st1b;\n"
"  vec4 tfactor; vec4 misc; vec4 mat_diffuse; vec4 ambient; vec4 psc[8]; vec4 bem[4]; vec4 bel[4]; vec4 fog; } u;\n"
"uniform sampler2D tex0; uniform sampler2D tex1;\n"
"in vec4 v_dif; in vec4 v_spc; in vec4 v_tc0; in vec4 v_tc1;\n"
"out vec4 o_color;\n"
"vec4 sat(vec4 v) { return clamp(v, vec4(0.0), vec4(1.0)); }\n"
"// no switch statements: some phone GPU drivers miscompile switch on unsigned integers\n"
"bool alpha_ok(float a) {\n"
"  float r = u.misc.x; uint f = u.flags.y;\n"
"  if (f == 1u) return false; if (f == 2u) return a < r; if (f == 3u) return abs(a - r) < 0.002;\n"
"  if (f == 4u) return a <= r; if (f == 5u) return a > r; if (f == 6u) return abs(a - r) >= 0.002;\n"
"  if (f == 7u) return a >= r; return true;\n"
"}\n"
"vec4 arg(uint a, vec4 cur, vec4 tex, vec4 dif, vec4 spc) {\n"
"  vec4 v = cur;\n"
"  uint s = a & 7u;\n"
"  if (s == 0u) v = dif; else if (s == 2u) v = tex; else if (s == 3u) v = u.tfactor; else if (s == 4u) v = spc;\n"
"  if ((a & 0x10u) != 0u) v = vec4(1.0) - v;\n"
"  if ((a & 0x20u) != 0u) v = vec4(v.a);\n"
"  return v;\n"
"}\n"
"vec4 top(uint o, vec4 a0, vec4 a1, vec4 a2, vec4 cur, vec4 tex, vec4 dif) {\n"
"  if (o == 2u) return a1;  if (o == 3u) return a2;\n"
"  if (o == 4u) return a1 * a2;  if (o == 5u) return a1 * a2 * 2.0;  if (o == 6u) return a1 * a2 * 4.0;\n"
"  if (o == 7u) return a1 + a2;  if (o == 8u) return a1 + a2 - 0.5;  if (o == 9u) return (a1 + a2 - 0.5) * 2.0;\n"
"  if (o == 10u) return a1 - a2;  if (o == 11u) return a1 + a2 - a1 * a2;\n"
"  if (o == 12u) return mix(a2, a1, dif.a);  if (o == 13u) return mix(a2, a1, tex.a);\n"
"  if (o == 14u) return mix(a2, a1, u.tfactor.a);  if (o == 15u) return a1 + a2 * (1.0 - tex.a);\n"
"  if (o == 16u) return mix(a2, a1, cur.a);\n"
"  if (o == 18u) return vec4(a1.rgb + a1.a * a2.rgb, a1.a);\n"
"  if (o == 19u) return vec4(a1.rgb * a2.rgb + vec3(a1.a), a1.a);\n"
"  if (o == 20u) return vec4((1.0 - a1.a) * a2.rgb + a1.rgb, a1.a);\n"
"  if (o == 21u) return vec4((vec3(1.0) - a1.rgb) * a2.rgb + vec3(a1.a), a1.a);\n"
"  if (o == 24u) { float d = clamp(dot(a1.rgb - 0.5, a2.rgb - 0.5) * 4.0, 0.0, 1.0); return vec4(d); }\n"
"  if (o == 25u) return a0 + a1 * a2;\n"
"  if (o == 26u) return a0 * a1 + (vec4(1.0) - a0) * a2;\n"
"  return a1;\n"
"}\n"
"vec4 stage(uvec4 s, uvec4 sb, vec4 cur, vec4 tex, vec4 dif, vec4 spc) {\n"
"  vec4 c0 = arg(sb.w & 255u, cur, tex, dif, spc);\n"
"  vec4 c = top(s.x, c0, arg(s.y, cur, tex, dif, spc), arg(s.z, cur, tex, dif, spc), cur, tex, dif);\n"
"  float a = cur.a;\n"
"  if (s.w > 1u) {\n"
"    vec4 a0 = arg(sb.w >> 8u, cur, tex, dif, spc);\n"
"    a = top(s.w, a0, arg(sb.x, cur, tex, dif, spc), arg(sb.y, cur, tex, dif, spc), cur, tex, dif).a;\n"
"  }\n"
"  return sat(vec4(c.rgb, a));\n"
"}\n"
"void main() {\n"
"  vec4 cur = v_dif;\n"
"  vec2 uv0 = u.st0b.z == 1u ? v_tc1.xy : v_tc0.xy;\n"
"  vec2 uv1 = u.st1b.z == 1u ? v_tc1.xy : v_tc0.xy;\n"
"  vec4 tx0 = texture(tex0, uv0);\n"
"  vec4 tx1 = texture(tex1, uv1);\n"
"  if (u.st0.x != 1u) {\n"
"    cur = stage(u.st0, u.st0b, cur, tx0, v_dif, v_spc);\n"
"    if (u.st1.x != 1u) { cur = stage(u.st1, u.st1b, cur, tx1, v_dif, v_spc); }\n"
"  }\n"
"  if (u.flags.z == 1u) { cur = vec4(clamp(cur.rgb + v_spc.rgb, vec3(0.0), vec3(1.0)), cur.a); }\n"
"  if (u.fog.w > 0.5) { cur = vec4(mix(u.fog.rgb, cur.rgb, clamp(v_spc.a, 0.0, 1.0)), cur.a); }\n"
"  if (!alpha_ok(cur.a)) { discard; }\n"
"  o_color = cur;\n"
"}\n";

/* Present: the back buffer drawn onto the canvas as one textured triangle (rows top first -> flipped), instead of
   glBlitFramebuffer, whose format rules some phone GPUs handle badly (Android players saw a black screen while
   the frames were being drawn, 2026-10-31). */
static const char *PVS =
"#version 300 es\n"
"out vec2 v_uv;\n"
"void main() { vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
"  v_uv = vec2(p.x, 1.0 - p.y); gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n";
static const char *PFS =
"#version 300 es\n"
"precision mediump float;\n"
"uniform sampler2D bb; in vec2 v_uv; out vec4 o;\n"
"void main() { o = vec4(texture(bb, v_uv).rgb, 1.0); }\n";

/* ------------------------------------------------------------------ state */

typedef struct { GLuint tex; uint32_t w, h, levels, fmt; } LTex;
typedef struct { GLuint tex, rb; uint32_t w, h; int depth, gpu_dirty; } LSurf;   /* color: texture; depth: renderbuffer */

static EMSCRIPTEN_WEBGL_CONTEXT_HANDLE g_ctx;
static GLuint g_prog, g_vao, g_vbo, g_ubo, g_pprog, g_pvao, g_psamp;
/* field diagnostics (sent once per session through rcGlDiag): the first GL errors and whether frames are black */
static unsigned g_diag_frames; static int g_diag_err[4], g_diag_errcmd[4], g_diag_nerr, g_diag_sent;
static char g_diag_log[200];
static GLint g_ubo_align = 256;
static uint32_t g_vpos, g_upos;
#define LVBUF_SIZE (16u << 20)
#define LUBUF_SIZE (4u << 20)
static GLuint g_samplers[1024];
/* Batching: a draw's vertices and uniforms are copied to host staging and the draw recorded; the batch goes to the
   GPU in one upload of each buffer (orphaned: fresh storage, no wait) followed by its draws, when anything other
   than a draw comes (target, clear, upload, readback, present). A glBufferSubData per draw into a buffer the GPU is
   still reading made every draw wait or copy: 1.5 frames a second at 350 draws a frame (2026-10-31). */
typedef struct {
    GLuint tex[2], samp[2];
    PKey k;
    GLint vx, vy; GLsizei vw, vh;
    float minz, maxz;
    GLint first; GLsizei n;
    uint32_t uoff;
    LSurf *dirty;
} DrawRec;
static uint8_t *g_vstage, *g_ustage;
static DrawRec *g_recs;
static uint32_t g_nrecs, g_caprecs;
static LTex g_lwhite, g_lblack;
static int g_lw_win, g_lh_win;          /* window (canvas) size the back buffer was made for */
static GLuint g_bb_fbo, g_bb_tex, g_bb_depth;
static int g_bb_w, g_bb_h;
static int g_frame_open_gl, g_world_frame_gl;
static uint32_t g_tw, g_th, g_tlw = 640, g_tlh = 480;   /* current target: GL size and Direct3D size */
static LSurf *g_tgs;                                     /* NULL: the back buffer */
static GLuint g_cur_fbo;
static int g_lost_gl;

/* ids -> LTex / LSurf (the ids render_rec.c hands out, shared by textures and surfaces) */
#define LOBJ_CAP 16384
static struct { uint32_t id; void *p; } g_lobjs[LOBJ_CAP];
static void *lobj_get(uint32_t id) {
    for (uint32_t i = 0, h = id * 2654435761u; i < LOBJ_CAP; i++) {
        uint32_t k = (h + i) & (LOBJ_CAP - 1);
        if (g_lobjs[k].id == id) return g_lobjs[k].p;
        if (!g_lobjs[k].id) return NULL;
    }
    return NULL;
}
static void lobj_put(uint32_t id, void *p) {
    for (uint32_t i = 0, h = id * 2654435761u; i < LOBJ_CAP; i++) {
        uint32_t k = (h + i) & (LOBJ_CAP - 1);
        if (g_lobjs[k].id == id || !g_lobjs[k].id || g_lobjs[k].id == 0xFFFFFFFFu) { g_lobjs[k].id = id; g_lobjs[k].p = p; return; }
    }
    fprintf(stderr, "[gl] object table full\n");
}
static void lobj_del(uint32_t id) {
    for (uint32_t i = 0, h = id * 2654435761u; i < LOBJ_CAP; i++) {
        uint32_t k = (h + i) & (LOBJ_CAP - 1);
        if (g_lobjs[k].id == id) { g_lobjs[k].id = 0xFFFFFFFFu; g_lobjs[k].p = NULL; return; }
        if (!g_lobjs[k].id) return;
    }
}

/* Framebuffers per (color texture, depth renderbuffer) pair, made on first use */
#define FBO_CACHE 64
static struct { GLuint color, depth, fbo; uint32_t stamp; } g_fbos[FBO_CACHE];
static uint32_t g_fbo_stamp;
static GLuint fbo_for(GLuint color, GLuint depth) {
    for (int i = 0; i < FBO_CACHE; i++)
        if (g_fbos[i].fbo && g_fbos[i].color == color && g_fbos[i].depth == depth) { g_fbos[i].stamp = ++g_fbo_stamp; return g_fbos[i].fbo; }
    int victim = 0;   /* an empty slot, else the least recently used */
    for (int i = 0; i < FBO_CACHE; i++) {
        if (!g_fbos[i].fbo) { victim = i; break; }
        if (g_fbos[i].stamp < g_fbos[victim].stamp) victim = i;
    }
    if (g_fbos[victim].fbo) glDeleteFramebuffers(1, &g_fbos[victim].fbo);
    GLuint f;
    static unsigned made; if (++made % 500 == 0) fprintf(stderr, "[gl] %u framebuffers made\n", made);
    glGenFramebuffers(1, &f);
    glBindFramebuffer(GL_FRAMEBUFFER, f);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
    if (depth) glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) { static int n; if (n++ < 5) fprintf(stderr, "[gl] framebuffer incomplete: %x\n", st); }
    g_fbos[victim].color = color; g_fbos[victim].depth = depth; g_fbos[victim].fbo = f; g_fbos[victim].stamp = ++g_fbo_stamp;
    g_cur_fbo = f;
    return f;
}
static void fbo_forget(GLuint color, GLuint depth) {
    for (int i = 0; i < FBO_CACHE; i++)
        if (g_fbos[i].fbo && ((color && g_fbos[i].color == color) || (depth && g_fbos[i].depth == depth))) {
            if (g_cur_fbo == g_fbos[i].fbo) { glBindFramebuffer(GL_FRAMEBUFFER, g_bb_fbo); g_cur_fbo = g_bb_fbo; }
            glDeleteFramebuffers(1, &g_fbos[i].fbo);
            memset(&g_fbos[i], 0, sizeof g_fbos[i]);
        }
}
static void bind_fbo(GLuint f) { if (g_cur_fbo != f) { glBindFramebuffer(GL_FRAMEBUFFER, f); g_cur_fbo = f; } }

/* ------------------------------------------------------------------ setup */

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048]; glGetShaderInfoLog(s, sizeof log, NULL, log); fprintf(stderr, "[gl] shader error: %s\n", log);
        if (!g_diag_log[0]) snprintf(g_diag_log, sizeof g_diag_log, "shader: %.180s", log);
    }
    return s;
}

static void make_backbuffer(void) {
    g_lw_win = g_window_w > 0 ? g_window_w : 1024;
    g_lh_win = g_window_h > 0 ? g_window_h : 768;
    if (g_bb_fbo) { fbo_forget(g_bb_tex, g_bb_depth); glDeleteFramebuffers(1, &g_bb_fbo); glDeleteTextures(1, &g_bb_tex); glDeleteRenderbuffers(1, &g_bb_depth); }
    g_bb_w = g_lw_win; g_bb_h = g_lh_win;
    glGenTextures(1, &g_bb_tex);
    glBindTexture(GL_TEXTURE_2D, g_bb_tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, g_bb_w, g_bb_h);
    glGenRenderbuffers(1, &g_bb_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, g_bb_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, g_bb_w, g_bb_h);
    glGenFramebuffers(1, &g_bb_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_bb_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_bb_tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, g_bb_depth);
    g_cur_fbo = g_bb_fbo;
    g_tw = (uint32_t)g_bb_w; g_th = (uint32_t)g_bb_h; g_tgs = NULL;
}

static void make_solid_gl(LTex *t, uint8_t v) {
    uint8_t px[4] = { v, v, v, 255 };
    glGenTextures(1, &t->tex);
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 1, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    t->w = t->h = t->levels = 1;
}

static EM_BOOL on_context_lost(int type, const void *e, void *ud) {
    (void)type; (void)e; (void)ud;
    fprintf(stderr, "[gl] WebGL context lost\n");
    g_lost_gl = 1;
    web_set_ready(0);
    EM_ASM({ if (typeof rcRecoverFailed === 'function') rcRecoverFailed(1); });
    return EM_TRUE;
}

/* page thread: start the WebGL 2 executor on <canvas id="canvas"> (instead of rc_gpu_init). 1 = ready. */
EMSCRIPTEN_KEEPALIVE int rc_gl_init(int w, int h) {
    g_window_w = w; g_window_h = h;
    EmscriptenWebGLContextAttributes at;
    emscripten_webgl_init_context_attributes(&at);
    at.majorVersion = 2; at.minorVersion = 0;
    at.alpha = 0; at.depth = 0; at.stencil = 0; at.antialias = 0; at.preserveDrawingBuffer = 0;
    at.powerPreference = EM_WEBGL_POWER_PREFERENCE_HIGH_PERFORMANCE;
    g_ctx = emscripten_webgl_create_context("#canvas", &at);
    if (g_ctx <= 0) { fprintf(stderr, "[gl] no WebGL 2 context\n"); return 0; }
    emscripten_webgl_make_context_current(g_ctx);
    emscripten_set_webglcontextlost_callback("#canvas", NULL, 0, on_context_lost);
    g_gpu_bc = emscripten_webgl_enable_extension(g_ctx, "WEBGL_compressed_texture_s3tc") && !getenv("RC_NO_BC");
    fprintf(stderr, "[gl] WebGL 2: %s / %s; BC (S3TC) textures: %s\n", (const char *)glGetString(GL_RENDERER),
            (const char *)glGetString(GL_VERSION), g_gpu_bc ? "yes" : "no (converted to RGBA8)");

    GLuint vs = compile(GL_VERTEX_SHADER, VS), fs = compile(GL_FRAGMENT_SHADER, FS);
    g_prog = glCreateProgram();
    glAttachShader(g_prog, vs); glAttachShader(g_prog, fs);
    glLinkProgram(g_prog);
    GLint ok = 0;
    glGetProgramiv(g_prog, GL_LINK_STATUS, &ok);
    if (!ok) { char log[2048]; glGetProgramInfoLog(g_prog, sizeof log, NULL, log); fprintf(stderr, "[gl] link error: %s\n", log); return 0; }
    glUseProgram(g_prog);
    glUniformBlockBinding(g_prog, glGetUniformBlockIndex(g_prog, "U"), 0);
    glUniform1i(glGetUniformLocation(g_prog, "tex0"), 0);
    glUniform1i(glGetUniformLocation(g_prog, "tex1"), 1);
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &g_ubo_align);
    if (g_ubo_align < 16) g_ubo_align = 16;

    g_vstage = malloc(LVBUF_SIZE); g_ustage = malloc(LUBUF_SIZE);
    if (!g_vstage || !g_ustage) { fprintf(stderr, "[gl] no memory for staging\n"); return 0; }
    glGenBuffers(1, &g_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, LVBUF_SIZE, NULL, GL_DYNAMIC_DRAW);
    glGenBuffers(1, &g_ubo);
    glBindBuffer(GL_UNIFORM_BUFFER, g_ubo);
    glBufferData(GL_UNIFORM_BUFFER, LUBUF_SIZE, NULL, GL_DYNAMIC_DRAW);
    glGenVertexArrays(1, &g_vao);
    glBindVertexArray(g_vao);
    const GLsizei st = (GLsizei)sizeof(CVert);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, st, (void *)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, st, (void *)16);
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, st, (void *)20);
    glEnableVertexAttribArray(3); glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, st, (void *)24);
    glEnableVertexAttribArray(4); glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, st, (void *)40);

    /* the present program */
    {
        GLuint pv = compile(GL_VERTEX_SHADER, PVS), pf = compile(GL_FRAGMENT_SHADER, PFS);
        g_pprog = glCreateProgram();
        glAttachShader(g_pprog, pv); glAttachShader(g_pprog, pf);
        glLinkProgram(g_pprog);
        glGetProgramiv(g_pprog, GL_LINK_STATUS, &ok);
        if (!ok) { char log[1024]; glGetProgramInfoLog(g_pprog, sizeof log, NULL, log); fprintf(stderr, "[gl] present link error: %s\n", log); }
        glUseProgram(g_pprog);
        glUniform1i(glGetUniformLocation(g_pprog, "bb"), 0);
        glUseProgram(g_prog);
        glGenVertexArrays(1, &g_pvao);
        glGenSamplers(1, &g_psamp);
        glSamplerParameteri(g_psamp, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glSamplerParameteri(g_psamp, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glSamplerParameteri(g_psamp, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(g_psamp, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    make_solid_gl(&g_lwhite, 255);
    make_solid_gl(&g_lblack, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glFrontFace(GL_CCW);
    make_backbuffer();
    fprintf(stderr, "[gl] renderer ready: rendering at %dx%d\n", g_bb_w, g_bb_h);
    g_use_gl = 1;
    web_set_ready(1);
    return 1;
}

/* ------------------------------------------------------------------ helpers */

static GLuint sampler_gl(uint32_t key) {
    key &= 1023;
    if (g_samplers[key]) return g_samplers[key];
    uint32_t mag = key & 1, min = (key >> 1) & 1, mip = (key >> 2) & 3, au = (key >> 4) & 7, av = (key >> 7) & 7;
    GLuint s;
    glGenSamplers(1, &s);
    glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, mag ? GL_LINEAR : GL_NEAREST);
    GLint mf = min ? GL_LINEAR : GL_NEAREST;
    if (mip == 2) mf = min ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR;
    else if (mip) mf = min ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
    glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, mf);
    GLint am[2]; uint32_t a[2] = { au, av };
    for (int i = 0; i < 2; i++) am[i] = a[i] == 2 ? GL_MIRRORED_REPEAT : (a[i] >= 3 && a[i] <= 5) ? GL_CLAMP_TO_EDGE : GL_REPEAT;
    glSamplerParameteri(s, GL_TEXTURE_WRAP_S, am[0]);
    glSamplerParameteri(s, GL_TEXTURE_WRAP_T, am[1]);
    g_samplers[key] = s;
    return s;
}
static GLenum bf_gl(uint32_t b) {
    switch (b) {
    case 1: return GL_ZERO; case 2: return GL_ONE; case 3: return GL_SRC_COLOR; case 4: return GL_ONE_MINUS_SRC_COLOR;
    case 5: return GL_SRC_ALPHA; case 6: return GL_ONE_MINUS_SRC_ALPHA; case 7: return GL_DST_ALPHA; case 8: return GL_ONE_MINUS_DST_ALPHA;
    case 9: return GL_DST_COLOR; case 10: return GL_ONE_MINUS_DST_COLOR; case 11: return GL_SRC_ALPHA_SATURATE;
    case 12: return GL_ONE_MINUS_SRC_ALPHA;   /* BOTHSRCALPHA (approximation, as render_wgpu.c) */
    case 13: return GL_SRC_ALPHA;             /* BOTHINVSRCALPHA */
    default: return GL_ONE;
    }
}
static GLenum cmp_gl(uint32_t f) {
    switch (f) { case 1: return GL_NEVER; case 2: return GL_LESS; case 3: return GL_EQUAL; case 4: return GL_LEQUAL;
                 case 5: return GL_GREATER; case 6: return GL_NOTEQUAL; case 7: return GL_GEQUAL; default: return GL_ALWAYS; }
}
static GLenum bop_gl(uint32_t o) {
    switch (o) { case 2: return GL_FUNC_SUBTRACT; case 3: return GL_FUNC_REVERSE_SUBTRACT; case 4: return GL_MIN; case 5: return GL_MAX;
                 default: return GL_FUNC_ADD; }
}
static GLenum topo_gl(uint32_t t) {
    switch (t) { case 1: return GL_POINTS; case 2: return GL_LINES; case 3: return GL_LINE_STRIP; case 5: return GL_TRIANGLE_STRIP;
                 default: return GL_TRIANGLES; }
}

static LSurf *lsurf(uint32_t id, uint32_t w, uint32_t h, int depth) {
    LSurf *g = lobj_get(id);
    if (g) return g;
    g = calloc(1, sizeof *g);
    g->w = w ? w : 1; g->h = h ? h : 1; g->depth = depth;
    if (depth) {
        glGenRenderbuffers(1, &g->rb);
        glBindRenderbuffer(GL_RENDERBUFFER, g->rb);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, (GLsizei)g->w, (GLsizei)g->h);
    } else {
        glGenTextures(1, &g->tex);
        glBindTexture(GL_TEXTURE_2D, g->tex);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, (GLsizei)g->w, (GLsizei)g->h);
    }
    lobj_put(id, g);
    return g;
}

/* depth renderbuffer for a target whose own depth surface has another size */
static struct { uint32_t w, h; GLuint rb; } g_lscratch[8];
static GLuint scratch_depth_gl(uint32_t w, uint32_t h) {
    for (int i = 0; i < 8; i++) if (g_lscratch[i].rb && g_lscratch[i].w == w && g_lscratch[i].h == h) return g_lscratch[i].rb;
    for (int i = 0; i < 8; i++)
        if (!g_lscratch[i].rb) {
            glGenRenderbuffers(1, &g_lscratch[i].rb);
            glBindRenderbuffer(GL_RENDERBUFFER, g_lscratch[i].rb);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, (GLsizei)w, (GLsizei)h);
            g_lscratch[i].w = w; g_lscratch[i].h = h;
            return g_lscratch[i].rb;
        }
    return g_lscratch[0].rb;
}

static void clear_target(uint32_t flags, uint32_t color, float z, uint32_t stencil, int x, int y, int w, int h, int scissor) {
    GLbitfield m = 0;
    if (flags & 1) { m |= GL_COLOR_BUFFER_BIT; glColorMask(1, 1, 1, 1);
        glClearColor(((color >> 16) & 255) / 255.f, ((color >> 8) & 255) / 255.f, (color & 255) / 255.f, (color >> 24) / 255.f); }
    if (flags & 2) { m |= GL_DEPTH_BUFFER_BIT; glDepthMask(1); glClearDepthf(z); }
    if (flags & 4) { m |= GL_STENCIL_BUFFER_BIT; glStencilMask(0xFF); glClearStencil((GLint)stencil); }
    if (!m) return;
    if (scissor) { glEnable(GL_SCISSOR_TEST); glScissor(x, y, w, h); } else glDisable(GL_SCISSOR_TEST);
    glClear(m);
    glDisable(GL_SCISSOR_TEST);
}

static void begin_frame_gl(void) {
    if (g_frame_open_gl) return;
    if (g_window_w != g_lw_win || g_window_h != g_lh_win) make_backbuffer();
    g_frame_open_gl = 1;
    g_world_frame_gl = 0;
    if (!g_tgs) { bind_fbo(g_bb_fbo); clear_target(1, 0, 1.f, 0, 0, 0, 0, 0, 0); }   /* the bars start black every frame */
}

/* ------------------------------------------------------------------ commands */

static void gl_target(const CmdTarget *c) {
    GLuint color, depth;
    LSurf *gs = NULL;
    uint32_t w, h;
    if (c->color_id) { gs = lsurf(c->color_id, c->color_w, c->color_h, 0); color = gs->tex; w = gs->w; h = gs->h; }
    else { color = g_bb_tex; w = (uint32_t)g_bb_w; h = (uint32_t)g_bb_h; }
    if (!c->color_id && !c->depth_id) depth = g_bb_depth;
    else if (c->depth_id) {
        LSurf *gd = lsurf(c->depth_id, c->depth_w, c->depth_h, 1);
        depth = gd->w == w && gd->h == h ? gd->rb : scratch_depth_gl(w, h);
    } else depth = scratch_depth_gl(w, h);
    bind_fbo(color == g_bb_tex && depth == g_bb_depth ? g_bb_fbo : fbo_for(color, depth));
    g_tw = w; g_th = h; g_tlw = c->lw; g_tlh = c->lh; g_tgs = gs;
}

static void use_backbuffer_gl(void) {
    bind_fbo(g_bb_fbo);
    g_tgs = NULL; g_tw = (uint32_t)g_bb_w; g_th = (uint32_t)g_bb_h;
}

static void gl_clear(const CmdClear *c) {
    begin_frame_gl();
    clear_target(c->flags, c->color, c->z, c->stencil, 0, 0, 0, 0, 0);
    if (g_tgs) g_tgs->gpu_dirty = 1;
}

static void gl_upload_tex(const CmdUploadTex *c) {
    LTex *g = lobj_get(c->id);
    static const GLenum cf[4] = { GL_RGBA8, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT };
    uint32_t bs = c->fmt == TEXFMT_BC1 ? 8 : 16;
    if (g && (g->w != c->w[0] || g->h != c->h_[0] || g->levels != c->levels || g->fmt != c->fmt)) {
        glDeleteTextures(1, &g->tex); free(g); lobj_del(c->id); g = NULL;
    }
    if (!g) {
        g = calloc(1, sizeof *g);
        glGenTextures(1, &g->tex);
        glBindTexture(GL_TEXTURE_2D, g->tex);
        glTexStorage2D(GL_TEXTURE_2D, (GLsizei)c->levels, cf[c->fmt & 3], (GLsizei)c->w[0], (GLsizei)c->h_[0]);
        g->w = c->w[0]; g->h = c->h_[0]; g->levels = c->levels; g->fmt = c->fmt;
        lobj_put(c->id, g);
    } else glBindTexture(GL_TEXTURE_2D, g->tex);
    const uint8_t *px = (const uint8_t *)(c + 1);
    for (uint32_t l = 0; l < c->levels; l++) {
        if (c->fmt) {
            uint32_t n = ((c->w[l] + 3) / 4) * ((c->h_[l] + 3) / 4) * bs;
            glCompressedTexSubImage2D(GL_TEXTURE_2D, (GLint)l, 0, 0, (GLsizei)c->w[l], (GLsizei)c->h_[l], cf[c->fmt & 3], (GLsizei)n, px);
            px += n;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, (GLint)l, 0, 0, (GLsizei)c->w[l], (GLsizei)c->h_[l], GL_RGBA, GL_UNSIGNED_BYTE, px);
            px += (size_t)c->w[l] * c->h_[l] * 4;
        }
    }
}

static void gl_destroy(uint32_t id, int surface) {
    void *p = lobj_get(id);
    if (!p) return;
    if (surface) {
        LSurf *g = p;
        if (g == g_tgs) use_backbuffer_gl();
        fbo_forget(g->tex, g->rb);
        if (g->tex) glDeleteTextures(1, &g->tex);
        if (g->rb) glDeleteRenderbuffers(1, &g->rb);
    } else {
        LTex *g = p;
        glDeleteTextures(1, &g->tex);
    }
    free(p);
    lobj_del(id);
}

static void gl_upload_surf(const CmdUploadSurf *c) {
    LSurf *gs = lsurf(c->id, c->w, c->h_, 0);
    glBindTexture(GL_TEXTURE_2D, gs->tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)gs->w, (GLsizei)gs->h, GL_RGBA, GL_UNSIGNED_BYTE, c + 1);
    gs->gpu_dirty = 0;
}

static void gl_flush(void);
static void gl_diag_frame(void);
static void gl_draw(const CmdDraw *c) {
    begin_frame_gl();
    uint32_t n = c->nverts, bytes = n * (uint32_t)sizeof(CVert);
    if (!n || bytes > LVBUF_SIZE) return;
    GLuint views[2];
    for (int s = 0; s < 2; s++) {
        views[s] = g_lwhite.tex;   /* fixed function: an empty stage reads white */
        if (c->slot_kind[s] == TS_TEX) { LTex *g = lobj_get(c->slot_id[s]); if (g) views[s] = g->tex; }
        else if (c->slot_kind[s] == TS_RTSURF) {
            LSurf *gs = lobj_get(c->slot_id[s]);
            views[s] = gs && !gs->depth && gs != g_tgs ? gs->tex : g_lblack.tex;
        }
    }
    /* the placement on the target: the same rules as render_wgpu.c exec_draw */
    int pre = c->u.flags[0] == 1, bb = !g_tgs && !c->full_viewport;
    float vx, vy, vw, vh;
    if (c->full_viewport) {
        vx = vy = 0; vw = (float)g_tw; vh = (float)g_th;
        if (!g_tgs && !c->wide) {
            float W = (float)g_tw, H = (float)g_th, lh = (float)g_tlh, pw = lh * 4.f / 3.f;
            float s = fminf(W / pw, H / lh);
            vx = (W - pw * s) / 2; vy = (H - lh * s) / 2; vw = pw * s; vh = lh * s;
        }
    } else if (!bb) {
        float sx = (float)g_tw / (float)g_tlw, sy = (float)g_th / (float)g_tlh;
        vx = c->vp[0] * sx; vy = c->vp[1] * sy; vw = c->vp[2] * sx; vh = c->vp[3] * sy;
    } else {
        float W = (float)g_tw, H = (float)g_th, lw = (float)g_tlw, lh = (float)g_tlh;
        float box = lh * 4.f / 3.f, pw = c->wide ? lw : box, s = fminf(W / pw, H / lh);
        float sx = s * pw / lw, sy = s, ox = (W - pw * s) / 2, oy = (H - lh * s) / 2;
        if (!pre) g_world_frame_gl = 1;
        else if (c->wide && !c->key.zen) {
            int full = 0;
            if (n <= 8 && g_world_frame_gl) {
                const CVert *v = (const CVert *)(c + 1);
                float x0 = 1e9f, x1 = -1e9f;
                for (uint32_t i = 0; i < n; i++) { x0 = fminf(x0, v[i].x); x1 = fmaxf(x1, v[i].x); }
                full = x0 <= 1.f && x1 >= lw - 1.f;
            }
            if (!full) { sx = s * box / lw; ox = (W - box * s) / 2; }
        }
        vx = ox + c->vp[0] * sx; vy = oy + c->vp[1] * sy; vw = c->vp[2] * sx; vh = c->vp[3] * sy;
    }
    if (vx < 0) { vw += vx; vx = 0; }
    if (vy < 0) { vh += vy; vy = 0; }
    if (vx + vw > g_tw) vw = g_tw - vx;
    if (vy + vh > g_th) vh = g_th - vy;
    if (vw <= 0 || vh <= 0) return;

    /* vertex and uniform data: rings in two buffers, restarted (orphaned) when full */
    uint32_t ustep = ((uint32_t)sizeof(Uniforms) + (uint32_t)g_ubo_align - 1) / (uint32_t)g_ubo_align * (uint32_t)g_ubo_align;
    if (g_vpos + bytes > LVBUF_SIZE || g_upos + ustep > LUBUF_SIZE) gl_flush();
    if (g_nrecs == g_caprecs) { g_caprecs = g_caprecs ? g_caprecs * 2 : 1024; g_recs = realloc(g_recs, g_caprecs * sizeof *g_recs); }
    DrawRec *r = &g_recs[g_nrecs++];
    memcpy(g_vstage + g_vpos, c + 1, bytes);
    Uniforms *u = (Uniforms *)(g_ustage + g_upos);
    memcpy(u, &c->u, sizeof *u);
    u->misc[1] = 1.f;
    for (int s = 0; s < 2; s++) { r->tex[s] = views[s]; r->samp[s] = sampler_gl(c->samp_key[s]); }
    r->k = c->key;
    r->vx = (GLint)lroundf(vx); r->vy = (GLint)lroundf(vy); r->vw = (GLsizei)lroundf(vw); r->vh = (GLsizei)lroundf(vh);
    r->minz = c->minz; r->maxz = c->maxz;
    r->first = (GLint)(g_vpos / sizeof(CVert)); r->n = (GLsizei)n;
    r->uoff = g_upos;
    r->dirty = g_tgs;
    g_vpos += (bytes + (uint32_t)sizeof(CVert) - 1) / (uint32_t)sizeof(CVert) * (uint32_t)sizeof(CVert);
    g_upos += ustep;
}

/* the recorded draws to the GPU (see DrawRec) */
static void gl_flush(void) {
    if (!g_nrecs) return;
    glUseProgram(g_prog);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, g_vpos, g_vstage, GL_STREAM_DRAW);
    glBindBuffer(GL_UNIFORM_BUFFER, g_ubo);
    glBufferData(GL_UNIFORM_BUFFER, g_upos, g_ustage, GL_STREAM_DRAW);
    GLuint bt[2] = { 0, 0 }, bs[2] = { 0, 0 };
    PKey last; int have = 0;
    for (uint32_t i = 0; i < g_nrecs; i++) {
        const DrawRec *r = &g_recs[i];
        glBindBufferRange(GL_UNIFORM_BUFFER, 0, g_ubo, r->uoff, sizeof(Uniforms));
        for (int s = 0; s < 2; s++) {
            if (bt[s] != r->tex[s]) { glActiveTexture(GL_TEXTURE0 + s); glBindTexture(GL_TEXTURE_2D, r->tex[s]); bt[s] = r->tex[s]; }
            if (bs[s] != r->samp[s]) { glBindSampler((GLuint)s, r->samp[s]); bs[s] = r->samp[s]; }
        }
        const PKey *k = &r->k;
        if (!have || memcmp(&last, k, sizeof last)) {
            if (k->blend) { glEnable(GL_BLEND); glBlendFunc(bf_gl(k->src), bf_gl(k->dst)); glBlendEquation(bop_gl(k->op)); }
            else glDisable(GL_BLEND);
            glColorMask(k->cmask & 1, (k->cmask >> 1) & 1, (k->cmask >> 2) & 1, (k->cmask >> 3) & 1);
            if (k->zen) { glEnable(GL_DEPTH_TEST); glDepthFunc(cmp_gl(k->zfunc)); glDepthMask(k->zwrite ? 1 : 0); }
            else { glDisable(GL_DEPTH_TEST); glDepthMask(0); }
            if (k->cull == 3 || k->cull == 2) { glEnable(GL_CULL_FACE); glCullFace(k->cull == 3 ? GL_BACK : GL_FRONT); }
            else glDisable(GL_CULL_FACE);
            last = *k; have = 1;
        }
        glViewport(r->vx, r->vy, r->vw, r->vh);
        glDepthRangef(r->minz, r->maxz);
        glDrawArrays(topo_gl(k->topo), r->first, r->n);
        if (r->dirty) r->dirty->gpu_dirty = 1;
    }
    g_nrecs = 0; g_vpos = 0; g_upos = 0;
}

/* one RGBA8 pixel -> a Direct3D format (as render_wgpu.c pack_pixel, from RGBA) */
static uint32_t pack_rgba(uint32_t fmt, const uint8_t *rgba, uint8_t *o) {
    uint32_t r = rgba[0], g = rgba[1], b = rgba[2], a = rgba[3];
    switch (fmt) {
    case FMT_A8R8G8B8: case FMT_X8R8G8B8: o[0] = (uint8_t)b; o[1] = (uint8_t)g; o[2] = (uint8_t)r; o[3] = fmt == FMT_X8R8G8B8 ? 255 : (uint8_t)a; return 4;
    case FMT_R5G6B5: { uint16_t v = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3); memcpy(o, &v, 2); return 2; }
    case FMT_X1R5G5B5: case FMT_A1R5G5B5: {
        uint16_t v = (uint16_t)((a >= 128 ? 0x8000 : 0) | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3); memcpy(o, &v, 2); return 2; }
    case FMT_A4R4G4B4: { uint16_t v = (uint16_t)((a >> 4) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | b >> 4); memcpy(o, &v, 2); return 2; }
    default: return 0;
    }
}

static void gl_readback(const CmdReadback *c) {
    LSurf *gs = c->id ? lobj_get(c->id) : NULL;
    if (!(c->id && (!gs || gs->depth || !gs->gpu_dirty))) {
        uint8_t tmp[4];
        uint32_t gw = c->id ? gs->w : (uint32_t)g_bb_w, gh = c->id ? gs->h : (uint32_t)g_bb_h;
        if (pack_rgba(c->fmt, (const uint8_t[4]){ 0, 0, 0, 0 }, tmp)) {
            uint8_t *px = malloc((size_t)gw * gh * 4);
            if (px) {
                GLuint keep = g_cur_fbo;
                bind_fbo(c->id ? fbo_for(gs->tex, 0) : g_bb_fbo);
                glReadPixels(0, 0, (GLsizei)gw, (GLsizei)gh, GL_RGBA, GL_UNSIGNED_BYTE, px);
                bind_fbo(keep);
                for (uint32_t y = 0; y < c->h_; y++) {   /* rows are stored top first: no flip */
                    const uint8_t *row = px + (size_t)(y * gh / c->h_) * gw * 4;
                    uint8_t *o = HP(c->guest_mem + y * c->pitch);
                    for (uint32_t x = 0; x < c->w; x++) o += pack_rgba(c->fmt, row + 4 * (x * gw / c->w), o);
                }
                free(px);
            }
        }
        if (gs) gs->gpu_dirty = 0;
    }
    __atomic_store_n((volatile int *)(uintptr_t)c->done_ptr, 1, __ATOMIC_RELEASE);
}

/* Once per session, ~150 frames in: the GPU's name, the first GL errors, and how bright the back buffer's middle is
   (0 = black: the drawing fails; >0 with a black screen: the presenting fails). The page sends it to crash.php. */
static void gl_diag_frame(void) {
    if (g_diag_sent || ++g_diag_frames < 150) return;
    g_diag_sent = 1;
    uint8_t px[32 * 32 * 4];
    bind_fbo(g_bb_fbo);
    glReadPixels(g_bb_w / 2 - 16, g_bb_h / 2 - 16, 32, 32, GL_RGBA, GL_UNSIGNED_BYTE, px);
    unsigned sum = 0, maxv = 0;
    for (int i = 0; i < 32 * 32; i++) { unsigned v = px[4 * i] + px[4 * i + 1] + px[4 * i + 2]; sum += v; if (v > maxv) maxv = v; }
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    const char *ren = (const char *)glGetString(GL_RENDERER);
    char *dbg = (char *)EM_ASM_PTR({ return stringToNewUTF8(typeof rcGlRenderer === 'function' ? rcGlRenderer() : ''); });
    char json[512];
    snprintf(json, sizeof json, "{\"gpu\":\"%.80s\",\"ren\":\"%.40s\",\"avg\":%u,\"max\":%u,\"fbo\":%u,\"bc\":%d,\"errs\":\"%x/%d %x/%d %x/%d\",\"n\":%d,\"log\":\"%.120s\",\"win\":\"%dx%d\"}",
             dbg ? dbg : "", ren ? ren : "", sum / (32 * 32), maxv, (unsigned)st, g_gpu_bc, g_diag_err[0], g_diag_errcmd[0], g_diag_err[1], g_diag_errcmd[1],
             g_diag_err[2], g_diag_errcmd[2], g_diag_nerr, g_diag_log, g_bb_w, g_bb_h);
    for (char *q = json; *q; q++) if (*q == '\n' || *q == '\r') *q = ' ';
    free(dbg);
    fprintf(stderr, "[gl] diag %s\n", json);
    EM_ASM({ if (typeof rcGlDiag === 'function') rcGlDiag(UTF8ToString($0)); }, json);
}

static void gl_present(const CmdPresent *c) {
    (void)c;
    begin_frame_gl();
    use_backbuffer_gl();
    g_frame_open_gl = 0;
    gl_diag_frame();
    /* the back buffer (rows top first) onto the canvas: one textured triangle, flipped (see PVS) */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    g_cur_fbo = 0;
    glViewport(0, 0, g_bb_w, g_bb_h);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
    glColorMask(1, 1, 1, 1);
    glUseProgram(g_pprog);
    glBindVertexArray(g_pvao);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_bb_tex);
    glBindSampler(0, g_psamp);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindSampler(0, 0);
    glBindVertexArray(g_vao);
    glUseProgram(g_prog);
    bind_fbo(g_bb_fbo);
    static unsigned frames;
    if (++frames % 120 == 0) {
        GLenum e = glGetError();
        if (e != GL_NO_ERROR) { g_watch.gpu_errors++; static int n; if (n++ < 10) fprintf(stderr, "[gl] error %x\n", e); }
    }
}

/* render_wgpu.c's pump: one command */
static double g_prof_t[16]; static unsigned g_prof_n[16];
void gl_exec_inner(const CmdHdr *h);
void gl_exec(const CmdHdr *h) {
    static int prof = -1; if (prof < 0) prof = getenv("RC_GL_PROF") != NULL;
    if (!g_diag_sent) {   /* the first frames: note GL errors and which command raised them */
        gl_exec_inner(h);
        GLenum e = glGetError();
        if (e != GL_NO_ERROR) { if (g_diag_nerr < 4) { g_diag_err[g_diag_nerr] = (int)e; g_diag_errcmd[g_diag_nerr] = (int)h->type; } g_diag_nerr++; }
        return;
    }
    if (!prof) { gl_exec_inner(h); return; }
    double t = emscripten_get_now();
    gl_exec_inner(h);
    unsigned k = h->type & 15; g_prof_t[k] += emscripten_get_now() - t; g_prof_n[k]++;
    if (h->type == CMD_PRESENT && g_prof_n[CMD_PRESENT] % 30 == 0) {
        fprintf(stderr, "[glprof] per 30 frames: draw %u %.1fms, tex %u %.1fms, target %u %.1fms, clear %u %.1fms, surf %u %.1fms, rb %u %.1fms, present %.1fms\n",
                g_prof_n[CMD_DRAW], g_prof_t[CMD_DRAW], g_prof_n[CMD_UPLOAD_TEX], g_prof_t[CMD_UPLOAD_TEX], g_prof_n[CMD_TARGET], g_prof_t[CMD_TARGET],
                g_prof_n[CMD_CLEAR], g_prof_t[CMD_CLEAR], g_prof_n[CMD_UPLOAD_SURF], g_prof_t[CMD_UPLOAD_SURF], g_prof_n[CMD_READBACK], g_prof_t[CMD_READBACK], g_prof_t[CMD_PRESENT]);
        memset(g_prof_t, 0, sizeof g_prof_t); memset(g_prof_n, 0, sizeof g_prof_n);
    }
}
void gl_exec_inner(const CmdHdr *h) {
    if (h->type != CMD_DRAW) gl_flush();   /* the batch goes first: everything else depends on its results or changes its inputs */
    if (g_lost_gl) {   /* a lost context draws nothing, but readbacks must still let the game go on */
        if (h->type == CMD_READBACK) __atomic_store_n((volatile int *)(uintptr_t)((const CmdReadback *)h)->done_ptr, 1, __ATOMIC_RELEASE);
        return;
    }
    switch (h->type) {
    case CMD_TARGET: gl_target((const CmdTarget *)h); break;
    case CMD_CLEAR: gl_clear((const CmdClear *)h); break;
    case CMD_DRAW: gl_draw((const CmdDraw *)h); break;
    case CMD_UPLOAD_TEX: gl_upload_tex((const CmdUploadTex *)h); break;
    case CMD_DESTROY_TEX: gl_destroy(((const CmdDestroy *)h)->id, 0); break;
    case CMD_UPLOAD_SURF: gl_upload_surf((const CmdUploadSurf *)h); break;
    case CMD_DESTROY_SURF: gl_destroy(((const CmdDestroy *)h)->id, 1); break;
    case CMD_READBACK: gl_readback((const CmdReadback *)h); break;
    case CMD_PRESENT: gl_present((const CmdPresent *)h); break;
    }
}
#endif
