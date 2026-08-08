/* ps3gl.h -- OpenGL 1.1 fixed-function subset backed by RSX/GCM. */

#ifndef PS3GL_H
#define PS3GL_H

#include <stdint.h>
#include <string.h>

/* PSL1GHT's rsx/mm.h and rsx/gcm_sys.h declare several functions as foo()
 * instead of foo(void); same vendor-header wrap already used by this project's
 * in_ps3.c (io/pad.h), sys_ps3.c (net/net.h) and s_ps3.c (sys/thread.h). */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#pragma GCC diagnostic ignored "-Wold-style-definition"
#include <ppu-types.h>
#include <rsx/rsx.h>
#include <rsx/gcm_sys.h>
#pragma GCC diagnostic pop

#include "GL/gl.h"

/* Constants */
#define PS3GL_MAX_TMUS          2
#define PS3GL_MAX_MATRIX_STACK  32
#define PS3GL_MAX_TEXTURES      4096
#define PS3GL_MAX_VERTS         16384   /* per-batch immediate mode */
#define PS3GL_MAX_BATCH_RANGES  256     /* polygons per deferred GL_POLYGON batch */
#define PS3GL_BATCH_VERT_MARGIN 1024    /* headroom so no polygon truncates mid-fan */
#define PS3GL_MAX_BUFFERS       128     /* static-world VBOs plus dynamic dlight/decal buffers */
#define PS3GL_STREAM_BUFFER_SLOTS 3     /* never overwrite a stream buffer still used by RSX */

/* Vertex ring buffer size (RSX memory), split across PS3GL_VRING_SEGMENTS.
 * Each segment must hold ONE ENTIRE FRAME's vertex data -- ps3gl_vring_alloc
 * deliberately refuses to wrap mid-frame (it would stomp data the GPU has not
 * fetched yet), so an undersized segment silently drops draws and geometry
 * goes missing. The inherited ioQuake3 value was 2 MB total = 1 MB/segment
 * = ~29k verts at 36 B each, which Half-Life exceeds at 1080p (hardware-
 * confirmed: "vertex ring segment exhausted ... need 35676, have 25168").
 * 32 MB total = 16 MB/segment = ~466k verts/frame, against 256 MB of GDDR3 of
 * which the 1080p framebuffers and depth take ~25 MB. */
#define PS3GL_VRING_SIZE        (32 * 1024 * 1024)

/* Identity ARGB remap. Fallback for varying PSL1GHT macro definitions. */
#define PS3GL_TEX_REMAP_IDENTITY  0x00AAE4

/* Dirty flags */
#define PS3GL_DIRTY_BLEND       0x0001u
#define PS3GL_DIRTY_ALPHA       0x0002u
#define PS3GL_DIRTY_DEPTH       0x0004u
#define PS3GL_DIRTY_STENCIL     0x0008u
#define PS3GL_DIRTY_VIEWPORT    0x0010u
#define PS3GL_DIRTY_CULL        0x0020u
#define PS3GL_DIRTY_SCISSOR     0x0040u
#define PS3GL_DIRTY_COLORMASK   0x0080u
#define PS3GL_DIRTY_POLYOFFSET  0x0100u
#define PS3GL_DIRTY_SHADE       0x0200u
#define PS3GL_DIRTY_ALL         0xFFFFu

/* Texenv mode IDs (shader key) */
#define PS3GL_TENV_DISABLED     0
#define PS3GL_TENV_MODULATE     1
#define PS3GL_TENV_REPLACE      2
#define PS3GL_TENV_DECAL        3
#define PS3GL_TENV_ADD          4
#define PS3GL_TENV_BLEND        5
#define PS3GL_TENV_MODULATE2    6   /* tex0 * tex1: dual-texture lightmap pass */
#define PS3GL_TENV_COUNT        7

/* 36-byte interleaved vertex: pos(16) + tc0(8) + tc1(8) + color(4) */
#pragma pack(push, 1)
typedef struct {
    float x, y, z, w;
    float u0, v0;
    float u1, v1;
    uint32_t color;
} ps3gl_vertex_t;
#pragma pack(pop)

#define PS3GL_VERTEX_SIZE       sizeof(ps3gl_vertex_t)

/* Vertex attribute offsets */
#define PS3GL_VATTR_POS_OFF     0
#define PS3GL_VATTR_TC0_OFF     16
#define PS3GL_VATTR_TC1_OFF     24
#define PS3GL_VATTR_COLOR_OFF   32

/* Texture slot */
typedef struct {
    int             glname;     /* -1 = free */
    uint8_t        *data;
    uint32_t        offset;     /* RSX offset */
    uint16_t        width;      /* level 0 (base) dimensions */
    uint16_t        height;
    uint8_t         bpp;
    uint8_t         num_levels; /* contiguous mip levels uploaded so far, >=1 */
    uint8_t         swizzled;   /* Morton-ordered, tightly packed levels, no pitch. */
    uint8_t         alloc_levels; /* mip levels `data` was actually sized for, >=1 */
                                /* swizzled/alloc_levels are both decided ONCE, when
                                 * `data` is allocated, and fixed for its lifetime --
                                 * the buffer size depends on both, and the cvars
                                 * behind them (gl_ps3_swizzle/gl_ps3_mipmap) can be
                                 * toggled between two uploads of the same texture. */
    uint8_t         wrap_s, wrap_t;
    uint8_t         min_filter, mag_filter;
    gcmTexture      gcm_tex;
    int             dirty;
    int             content_dirty; /* RSX-local texels changed; cache invalidate pending */
} ps3gl_texture_t;

/* TMU state */
typedef struct {
    ps3gl_texture_t *bound;
    int              enabled;
    int              texenv;
    int              dirty;
} ps3gl_tmu_t;

/* Matrix stack (column-major 4x4) */
typedef struct {
    float   stack[PS3GL_MAX_MATRIX_STACK][16];
    int     depth;
    int     dirty;
} ps3gl_matstack_t;

/* Vertex array pointers (glDrawElements path) */
typedef struct {
	const void *ptr;
	GLint       size;
	GLenum      type;
	GLsizei     stride;
	GLuint      buffer; /* GL_ARRAY_BUFFER_ARB bound when ptr was specified */
} ps3gl_array_ptr_t;

/* RSX-local backing for the small ARB VBO subset ref_gl uses.
 *
 * GL_STATIC_DRAW and GL_DYNAMIC_DRAW get ONE allocation: both are re-specified
 * rarely (static world geometry once per map) or updated in place with
 * glBufferSubDataARB (ref_gl's decal buffer), and glBufferSubDataARB only ever
 * writes the current slot -- rotating a sub-data buffer would leave the other
 * slots holding stale geometry.
 *
 * GL_STREAM_DRAW rotates through PS3GL_STREAM_BUFFER_SLOTS allocations, one
 * step per glBufferDataARB (the GL orphan point), so a re-specify never stomps
 * memory the RSX is still fetching from a draw submitted earlier this frame.
 * ref_gl only uses STREAM for the two r_vbo_dlightmode buffers, and always
 * re-specifies them whole. */
typedef struct {
	void       *data[PS3GL_STREAM_BUFFER_SLOTS];
	uint32_t    offset[PS3GL_STREAM_BUFFER_SLOTS];
	uint32_t    size;     /* bytes actually allocated per slot (capacity) */
	uint32_t    used;     /* bytes the last glBufferDataARB asked for */
	uint32_t    respec_frame; /* frame of the last glBufferDataARB, for the wrap warning */
	uint8_t     respec_count; /* re-specifies this frame; > slots means a slot was reused */
	uint8_t     slots;
	uint8_t     current;
	GLuint      name;
} ps3gl_buffer_t;

/* Double-buffered: command submission runs ahead of GPU execution, so without this a
 * same-frame wrap would stomp data the GPU hasn't fetched yet. Labels <64 are system-reserved; 65 is saved for an unimplemented tess-arena fence -- don't reuse it. */
#define PS3GL_VRING_SEGMENTS    2
#define PS3GL_LABEL_VRING_SEG0  64
#define PS3GL_LABEL_VRING_SEG1  66

/* Vertex ring buffer in RSX memory */
typedef struct {
    uint8_t    *base;
    uint32_t    base_off;
    uint32_t    seg_capacity;  /* bytes per segment */
    uint32_t    head;          /* offset within the current segment */
    int         cur_seg;       /* 0 or 1 */
    volatile uint32_t *fence_label[PS3GL_VRING_SEGMENTS]; /* GCM label addr; RSX writes here when pipeline drains */
    uint32_t    fence_val[PS3GL_VRING_SEGMENTS];           /* last value written to each label */

    /* Sizing telemetry. peak_head is the high-water mark across all frames, so
     * PS3GL_VRING_SIZE can be set from measurement instead of guesswork;
     * frame_drops counts draws lost to exhaustion in the current frame. */
    uint32_t    peak_head;
    uint32_t    frame_drops;
    uint32_t    total_drops;
} ps3gl_vring_t;

/* Host-memory (XDR) ring, IO-mapped so the RSX can fetch vertex data straight
 * out of it over FlexIO. This exists for exactly one reason: glDrawElements
 * below direct-binds client arrays when rsxAddressToOffset resolves them and
 * otherwise falls back to a scalar per-vertex interleave-copy into the vring.
 * ref_gl's studio arrays live in BSS, which is not IO-mapped, so every studio
 * mesh took the copy -- measured at 2.66ms/frame (`submit`) on c4a2.
 *
 * Segments and fencing are NOT independent of the vring: it rotates on
 * ps3gl.vring.cur_seg and is covered by the same per-segment fence, because
 * both hold data for the same frame's commands and are retired together.
 *
 * reserve/commit rather than a plain alloc: a caller filling a variable-length
 * run (a studio submodel) knows only an upper bound up front, and permanently
 * consuming that bound would exhaust the ring in a handful of models. */
/* 2 x 1 MB. Do NOT size this from the measured peak alone: ps3gl_hring_reserve
 * bound-checks against the WORST CASE a caller might write, and the studio
 * reserve is MAXSTUDIOVERTS * 24 = 384 KB. So a segment must hold
 * peak + 384 KB, not peak -- at 512 KB/segment (sized off the 291 KB measured
 * peak) every reserve past head=128 KB fails and silently falls back to BSS,
 * which reads as "the feature does nothing" with no drop counted, because a
 * caller with a fallback is not a dropped draw.
 * 291 KB peak + 384 KB reserve = 675 KB, so 1 MB/segment clears it. */
#define PS3GL_HRING_SIZE        (2 * 1024 * 1024)

typedef struct {
    uint8_t    *base;
    uint32_t    seg_capacity;
    uint32_t    head;
    uint32_t    peak_head;
    uint32_t    frame_drops;
    uint32_t    total_drops;
    int         mapped;       /* gcmMapMainMemory succeeded */
} ps3gl_hring_t;

/* Shader program pair (vertex + fragment) */
typedef struct {
    rsxVertexProgram   *vp;
    void               *vp_ucode;
    uint32_t            vp_ucode_size;
    rsxProgramConst    *mvp_const;
    rsxProgramConst    *clip_plane_const; /* world-space clip plane uniform; NULL if not in binary */

    rsxFragmentProgram *fp;
    void               *fp_ucode;
    uint32_t            fp_ucode_size;
    uint32_t            fp_offset;
} ps3gl_shader_t;

/* Global GL state */
typedef struct {
    gcmContextData     *ctx;

    uint32_t            screen_w;
    uint32_t            screen_h;

    uint32_t            dirty;

    struct {
        int     blend_enable;
        uint16_t blend_src, blend_dst;
        int     alpha_test_enable;
        uint32_t alpha_func, alpha_ref;
        int     depth_test_enable;
        int     depth_mask;
        uint32_t depth_func;
        int     cull_enable;
        uint32_t cull_face, front_face;
        int     scissor_enable;
        int16_t scissor_x, scissor_y;
        uint16_t scissor_w, scissor_h;
        int16_t vp_x, vp_y;
        uint16_t vp_w, vp_h;
        float    depth_near, depth_far;
        int     color_mask_r, color_mask_g, color_mask_b, color_mask_a;
        int     polyoffset_fill;
        float   polyoffset_factor, polyoffset_units;
        uint32_t shade_model;
        int     stencil_enable;
        uint32_t stencil_func, stencil_ref, stencil_mask;
        uint32_t stencil_fail, stencil_zfail, stencil_zpass;
        uint32_t stencil_writemask;
        uint32_t clear_color;
        float    clear_depth;
        uint32_t clear_stencil;
    } rs;

    /* GL_CLIP_PLANE0: world-space plane (normal.xyz, dist) for portal/mirror clipping.
     * Must stay world-space -- eye-space flips the sign and blacks out the mirror. Software-evaluated in DrawElements. */
    int                 clip_plane_enabled;
    float               clip_plane[4];     /* (nx,ny,nz,dist): dot(n,v)>=dist keeps */

    /* Fog: state is tracked faithfully so glIsEnabled(GL_FOG)/glGetFloatv(GL_FOG_COLOR)
     * answer correctly (xash's gl_rmain.c branches on both), but NOT yet applied to
     * any draw -- see ps3gl_glapi.c. */
    struct {
        int     enabled;
        GLenum  mode;              /* GL_EXP / GL_EXP2 / GL_LINEAR */
        float   density;
        float   start, end;
        float   color[4];
    } fog;

    /* Texture coordinate generation: tracked per TMU per coord (bit 0=S,1=T,2=R,3=Q),
     * likewise not yet applied. */
    struct {
        int     enabled_bits[PS3GL_MAX_TMUS];
        GLenum  mode[PS3GL_MAX_TMUS][4];
    } texgen;

    int                 active_tmu;
    ps3gl_tmu_t         tmu[PS3GL_MAX_TMUS];
    int                 client_active_tmu;

    GLenum              matrix_mode;
    ps3gl_matstack_t    mv;
    ps3gl_matstack_t    proj;

    struct {
        ps3gl_vertex_t  buf[PS3GL_MAX_VERTS];
        int             count;
        GLenum          prim;
        float           u0, v0;
        float           u1, v1;
        uint32_t        color;

        /* Deferred GL_POLYGON batch: consecutive same-state polygons (one
         * BSP surface each, from ref/gl/gl_rsurf.c's DrawGLPoly) accumulate
         * into buf[] across multiple glBegin/glEnd pairs instead of each
         * paying its own vring_alloc+bind+draw. Each range below is one
         * polygon's still-intact triangle fan -- multiple rsxDrawVertexArray
         * calls against a single unchanged attribute binding are legal (the
         * binding is persistent GCM register state), so no vertex expansion
         * is needed, just one bind followed by N draws. See ps3gl_vertices.c. */
        int             batch_active;
        int             batch_nranges;
        int             batch_first[PS3GL_MAX_BATCH_RANGES];
        int             batch_count[PS3GL_MAX_BATCH_RANGES];
        int             vert_start;   /* buf[] index where this glBegin started */
    } imm;

    ps3gl_array_ptr_t   va_vertex;
    ps3gl_array_ptr_t   va_color;
	ps3gl_array_ptr_t   va_texcoord[PS3GL_MAX_TMUS];
	int                 va_locked;
	GLint               va_lock_first;
	GLsizei             va_lock_count;
	GLuint              array_buffer;
	ps3gl_buffer_t      buffers[PS3GL_MAX_BUFFERS];
	void               *pending_buffer_free[PS3GL_MAX_BUFFERS * PS3GL_STREAM_BUFFER_SLOTS];
	int                 pending_buffer_free_count;

    ps3gl_vring_t       vring;
    ps3gl_hring_t       hring;

    ps3gl_texture_t     textures[PS3GL_MAX_TEXTURES];
    GLuint              tex_next_name;

    /* Textures unbound this frame can still be sampled by draws the RSX has
     * queued but not yet executed (submission runs up to one frame ahead --
     * same slack PS3GL_VRING_SEGMENTS relies on). rsxFree()'ing them
     * immediately races the GPU and can wedge/blank the display with no CPU
     * exception (see ps3gl_defer_texture_free()). Deferred frees are flushed
     * from ps3gl_begin_frame() right after the vring fence wait proves the
     * GPU has drained the frame(s) that could still reference them. */
    void               *pending_tex_free[PS3GL_MAX_TEXTURES];
    int                 pending_tex_free_count;

    ps3gl_shader_t      shaders[PS3GL_TENV_COUNT];
    int                 active_shader;
    rsxVertexProgram   *active_vp;      /* physical VP currently loaded on RSX; all shader slots share one VP, so this is tracked separately from active_shader (the FP key) */
    uint32_t            mvp_uploaded_gen; /* last ps3gl_get_mvp_generation() value uploaded to active_vp */
    float               clip_plane_uploaded[4]; /* last clip_plane patched into the VP; re-sent on any VP reload */

} ps3gl_state_t;

/* Heap-allocated singleton. Macro allows `ps3gl.field` access. */
extern ps3gl_state_t *ps3gl_ptr;
#define ps3gl (*ps3gl_ptr)

gcmContextData *ps3gl_get_ctx(void);

/* Routes to gEngfuncs.Con_Printf, which mirrors to the UDP debug sink. */
void ps3gl_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Module init/shutdown. Returns 1 on success, 0 on failure. */
int ps3gl_init(gcmContextData *ctx, uint32_t w, uint32_t h);
void ps3gl_shutdown(void);
void ps3gl_begin_frame(void);
void ps3gl_end_frame(void);

/* Called once per frame from ref/gl/gl_rsurf.c (declared there via a local
 * extern, since that file can't include this header across the ref_gl/ps3gl
 * module boundary -- see the call site's comment). Feeds the dlight-cost
 * telemetry in ps3gl_end_frame's window report. */
void ps3gl_report_dlight_cost(uint32_t surface_count, uint32_t luxel_count);

/* Called from vid_ps3.c's PS3_RSX_WaitForFreeBuffer. Accumulated per frame and
 * reported in the same 5s window -- this is the CPU-vs-GPU split that the
 * vring fence wait alone cannot give (that fence is waited on AFTER this one,
 * so it reads ~0% whenever the GPU is the real bottleneck). */
void ps3gl_report_flip_wait(double seconds, uint32_t sleeps);

/* Coarse CPU phase split, fed once per frame from engine/common/host.c.
 * `render` still includes the flip wait; the window report subtracts it. */
void ps3gl_report_frame_phases(double input, double server, double client,
                               double render, double sound, double total);

/* Breakdown of the render phase, fed from ref_gl's R_RenderScene (accumulated
 * across passes -- mirrors/portals re-run the whole scene). */
void ps3gl_report_scene_phases(double setup, double world, double entities,
                               double water);

/* PPE time-base clock for the fine-grained phase timers in ref_gl. Deliberately
 * NOT Platform_DoubleTime(): that is gettimeofday() on this platform (an LV2
 * syscall at 1us granularity, see engine/platform/posix/sys_posix.c), which is
 * fine for the ~8 coarse seams per frame above but would both quantize and
 * inflate the entity/studio seams below, which fire hundreds of times a frame.
 * ps3gl_ticks() is a single mftb instruction. */
uint64_t ps3gl_ticks(void);
double ps3gl_ticks_to_sec(uint64_t ticks);

/* Split of the `entities` figure above by what is actually being drawn, fed
 * from ref_gl's R_DrawEntitiesOnList. That phase is NOT just studio models --
 * it also runs brush/sprite entities, CL_DrawEFX twice, the client DLL's two
 * triangle callbacks and the viewmodel, and no counter distinguished them. */
void ps3gl_report_entity_phases(uint64_t studio, uint64_t brush, uint64_t sprite,
                                uint64_t efx, uint64_t clienttri, uint64_t viewmodel);

/* Split of the studio-model cost itself, fed from ref_gl's gl_studio.c.
 * `client` is the client DLL's own studio work (bone setup, gait, state churn),
 * derived by subtraction at the R_StudioDrawModelInternal funnel because that
 * code lives in the game DLL, not here. `fill`/`submit` are sub-items OF
 * `build`, not additional to it. `models` replaces r_stats.c_studio_models_drawn,
 * which is only zeroed inside GL_BackendEndFrame's `r_speeds > 0` early-return
 * guard and therefore prints a running total in a default build. */
void ps3gl_report_studio_phases(uint64_t client, uint64_t entlight, uint64_t skin,
                                uint64_t light, uint64_t build, uint64_t fill,
                                uint64_t submit, uint32_t models);

/* Sub-split of `client` above, fed from the game DLL's own studio renderer
 * (hlsdk-portable/cl_dll/StudioModelRenderer.cpp). Sub-items OF client, not
 * additional to it. Needed because `client` is derived by subtraction in ref_gl
 * and is therefore one opaque number -- the code it measures lives in a
 * different module entirely. */
void ps3gl_report_studio_client(uint64_t xform, uint64_t bones, uint64_t events);

/* Renderer cvars pushed down from ref_gl once per frame (ps3gl cannot reach
 * the cvar system itself). Both are read at texture-upload time, so a change
 * takes effect on the next map load / vid_restart -- which is what makes a
 * same-session A/B on real hardware possible without a rebuild. */
void ps3gl_set_texture_options(int swizzle_enabled, int mipmap_enabled);

/* One-shot diagnostic used by ref_gl's dynamic-light atlas dump. The next
 * TexSubImage upload verifies the RSX-memory byte layout after conversion. */
void ps3gl_arm_texture_upload_probe(void);

/* Logs the GL-to-RSX state the NEXT draw would be submitted under: which
 * fragment program the texenv/TMU combination selects, the immediate-mode
 * vertex color that program multiplies by, and the ROP blend/depth state.
 * Call sites decide when to fire it -- it prints unconditionally. */
void ps3gl_log_draw_state(const char *tag);

/* Subsystem init */
void ps3gl_states_init(void);
void ps3gl_matrices_init(void);
void ps3gl_textures_init(void);
void ps3gl_vring_init(void);
void ps3gl_shaders_init(void);
void ps3gl_buffers_init(void);
void ps3gl_buffers_begin_frame(void);

void ps3gl_states_shutdown(void);
void ps3gl_textures_shutdown(void);
void ps3gl_vring_shutdown(void);
void ps3gl_shaders_shutdown(void);
void ps3gl_buffers_shutdown(void);

ps3gl_buffer_t *ps3gl_buffer_lookup(GLuint name);

/* State application (before draw) */
void ps3gl_apply_states(void);
void ps3gl_apply_matrices(void);
void ps3gl_apply_textures(void);
void ps3gl_apply_shader(void);

/* Vertex ring buffer */
ps3gl_vertex_t *ps3gl_vring_alloc(int count, uint32_t *out_offset);

/* Raw carve out of the same ring, for data that is not a ps3gl_vertex_t:
 * client-side vertex arrays staged into RSX memory so they can be bound
 * alongside a real VBO (glDrawElements' mixed path), and index arrays. Same
 * per-frame fencing and same refuse-to-wrap policy as ps3gl_vring_alloc. */
void *ps3gl_vring_alloc_raw(uint32_t bytes, uint32_t *out_offset);

/* Host (XDR) ring -- see ps3gl_hring_t. reserve() bound-checks and returns a
 * CPU pointer WITHOUT consuming; commit() consumes what was actually written.
 * Callers get a plain pointer and nothing else: the whole point is that it
 * looks like an ordinary client array to ref_gl, and glDrawElements then
 * resolves it with rsxAddressToOffset like any other. Returns NULL if the
 * mapping failed or the segment is full -- callers MUST have a BSS fallback. */
void ps3gl_hring_init(void);
void ps3gl_hring_shutdown(void);
void *ps3gl_hring_reserve(uint32_t bytes);
void ps3gl_hring_commit(uint32_t bytes);

/* Frame counter, incremented by ps3gl_begin_frame. Only used to detect a
 * stream buffer re-specified more times in one frame than it has slots. */
uint32_t ps3gl_frame_index(void);

uint32_t ps3gl_gl_to_gcm_prim(GLenum mode);

/* Force-submits a pending deferred GL_POLYGON batch (see the imm struct's
 * comment above), if one exists. Must be called before anything else writes
 * to the GCM FIFO or ends the frame -- glClear, glDrawElements, and
 * ps3gl_end_frame (before its fence-label write) all call this first. */
void ps3gl_flush_polygon_batch(void);

/* Texture helpers */
ps3gl_texture_t *ps3gl_texture_find(GLuint name);
ps3gl_texture_t *ps3gl_texture_alloc(GLuint name);
void ps3gl_defer_texture_free(void *data);
void ps3gl_flush_deferred_texture_frees(void);

/* Shader helpers */
int ps3gl_shader_key(void);
void ps3gl_shader_select(int key);

/* Cross-module internals. Previously re-declared by hand in each consumer
 * (ps3gl_draw.c, ps3gl_shaders.c, ps3gl_vertices.c); centralized here so the
 * definitions in ps3gl_matrices.c/ps3gl_vertices.c see a prototype. */
const float *ps3gl_get_mvp(void);
uint32_t ps3gl_get_mvp_generation(void);
void ps3gl_inc_draw_count(void);

/* GL function implementations */
void glEnable(GLenum cap);
void glDisable(GLenum cap);
void glBlendFunc(GLenum sfactor, GLenum dfactor);
void glAlphaFunc(GLenum func, GLclampf ref);
void glDepthFunc(GLenum func);
void glDepthMask(GLboolean flag);
void glDepthRange(GLclampd n, GLclampd f);
void glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a);
void glCullFace(GLenum mode);
void glFrontFace(GLenum mode);
void glScissor(GLint x, GLint y, GLsizei w, GLsizei h);
void glViewport(GLint x, GLint y, GLsizei w, GLsizei h);
void glShadeModel(GLenum mode);
void glPolygonOffset(GLfloat factor, GLfloat units);
void glPolygonMode(GLenum face, GLenum mode);
void glStencilFunc(GLenum func, GLint ref, GLuint mask);
void glStencilMask(GLuint mask);
void glStencilOp(GLenum fail, GLenum zfail, GLenum zpass);
void glClear(GLbitfield mask);
void glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a);
void glClearDepth(GLclampd depth);
void glClearStencil(GLint s);
void glLineWidth(GLfloat width);
void glClipPlane(GLenum plane, const GLdouble *equation);

/* PS3-specific: sets the clip plane directly in world space (normal.xyz, dist),
 * skipping glClipPlane's eye-space transform. Called from tr_backend.c before the portal/mirror pass. */
void ps3gl_SetWorldClipPlane(float nx, float ny, float nz, float dist);
void glGetIntegerv(GLenum pname, GLint *params);
void glGetBooleanv(GLenum pname, GLboolean *params);
const GLubyte *glGetString(GLenum name);
GLenum glGetError(void);

void glMatrixMode(GLenum mode);
void glLoadIdentity(void);
void glLoadMatrixf(const GLfloat *m);
void glMultMatrixf(const GLfloat *m);
void glPushMatrix(void);
void glPopMatrix(void);
void glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t,
                 GLdouble n, GLdouble f);
void glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t,
                   GLdouble n, GLdouble f);
void glTranslatef(GLfloat x, GLfloat y, GLfloat z);
void glScalef(GLfloat x, GLfloat y, GLfloat z);
void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z);
void glGetFloatv(GLenum pname, GLfloat *params);

void glBegin(GLenum mode);
void glEnd(void);
void glVertex2f(GLfloat x, GLfloat y);
void glVertex3f(GLfloat x, GLfloat y, GLfloat z);
void glVertex3fv(const GLfloat *v);
void glTexCoord2f(GLfloat s, GLfloat t);
void glTexCoord2fv(const GLfloat *v);
void glMultiTexCoord2fARB(GLenum target, GLfloat s, GLfloat t);

void glColor3f(GLfloat r, GLfloat g, GLfloat b);
void glColor3fv(const GLfloat *v);
void glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void glColor4fv(const GLfloat *v);
void glColor4ubv(const GLubyte *v);
void glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a);
void glColor3ubv(const GLubyte *v);

void glBindTexture(GLenum target, GLuint texture);
void glGenTextures(GLsizei n, GLuint *textures);
void glDeleteTextures(GLsizei n, const GLuint *textures);
void glTexImage2D(GLenum target, GLint level, GLint internalformat,
                      GLsizei width, GLsizei height, GLint border,
                      GLenum format, GLenum type, const void *pixels);
void glTexSubImage2D(GLenum target, GLint level, GLint xoff, GLint yoff,
                         GLsizei w, GLsizei h, GLenum format, GLenum type,
                         const void *pixels);
void glTexParameterf(GLenum target, GLenum pname, GLfloat param);
void glTexParameteri(GLenum target, GLenum pname, GLint param);
void glTexEnvf(GLenum target, GLenum pname, GLfloat param);
void glTexEnvi(GLenum target, GLenum pname, GLint param);
void glActiveTextureARB(GLenum texture);
void glClientActiveTextureARB(GLenum texture);
void glPixelStorei(GLenum pname, GLint param);
void glCopyTexSubImage2D(GLenum target, GLint level, GLint xoff,
                              GLint yoff, GLint x, GLint y,
                              GLsizei w, GLsizei h);

void glVertexPointer(GLint size, GLenum type, GLsizei stride,
                         const void *ptr);
void glTexCoordPointer(GLint size, GLenum type, GLsizei stride,
                           const void *ptr);
void glColorPointer(GLint size, GLenum type, GLsizei stride,
                        const void *ptr);
void glEnableClientState(GLenum cap);
void glDisableClientState(GLenum cap);
void glLockArraysEXT(GLint first, GLsizei count);
void glUnlockArraysEXT(void);
void glDrawElements(GLenum mode, GLsizei count, GLenum type,
                        const void *indices);
void glDrawArrays(GLenum mode, GLint first, GLsizei count);
void glArrayElement(GLint i);

/* No-ops */
void glFinish(void);
void glFlush(void);
void glDrawBuffer(GLenum mode);
void glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h,
                      GLenum format, GLenum type, void *pixels);

/* ps3gl_glapi.c -- entry points ref_gl links against beyond the core above.
 * See that file's header comment for which are real and which are link-only. */
void glGenBuffersARB(GLsizei n, GLuint *buffers);
void glDeleteBuffersARB(GLsizei n, const GLuint *buffers);
void glBindBufferARB(GLenum target, GLuint buffer);
GLboolean glIsBufferARB(GLuint buffer);
void glBufferDataARB(GLenum target, GLsizeiptrARB size, const GLvoid *data, GLenum usage);
void glBufferSubDataARB(GLenum target, GLintptrARB offset, GLsizeiptrARB size, const GLvoid *data);
void glCompressedTexImage1DARB(GLenum target, GLint level, GLenum internalformat,
                               GLsizei width, GLint border, GLsizei imageSize, const void *data);
void glCompressedTexImage2DARB(GLenum target, GLint level, GLenum internalformat,
                               GLsizei width, GLsizei height, GLint border,
                               GLsizei imageSize, const void *data);
void glCompressedTexImage3DARB(GLenum target, GLint level, GLenum internalformat,
                               GLsizei width, GLsizei height, GLsizei depth, GLint border,
                               GLsizei imageSize, const void *data);
void glCompressedTexSubImage1DARB(GLenum target, GLint level, GLint xoffset, GLsizei width,
                                  GLenum format, GLsizei imageSize, const void *data);
void glCompressedTexSubImage2DARB(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                                  GLsizei width, GLsizei height, GLenum format,
                                  GLsizei imageSize, const void *data);
void glCompressedTexSubImage3DARB(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                                  GLint zoffset, GLsizei width, GLsizei height, GLsizei depth,
                                  GLenum format, GLsizei imageSize, const void *data);
void glTexImage1D(GLenum target, GLint level, GLint internalformat, GLsizei width,
                  GLint border, GLenum format, GLenum type, const GLvoid *pixels);
void glTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLsizei width,
                     GLenum format, GLenum type, const GLvoid *pixels);
void glTexImage3D(GLenum target, GLint level, GLenum internalFormat, GLsizei width,
                  GLsizei height, GLsizei depth, GLint border, GLenum format,
                  GLenum type, const GLvoid *pixels);
void glTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset,
                     GLsizei width, GLsizei height, GLsizei depth, GLenum format,
                     GLenum type, const GLvoid *pixels);
void glTexImage2DMultisample(GLenum target, GLsizei samples, GLenum internalformat,
                             GLsizei width, GLsizei height, GLboolean fixedsamplelocations);
void glDebugMessageCallbackARB(GL_DEBUG_PROC_ARB callback, void *userParam);
void glDebugMessageControlARB(GLenum source, GLenum type, GLenum severity, GLsizei count,
                              const GLuint *ids, GLboolean enabled);
void glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count,
                         GLenum type, const GLvoid *indices);
void glMultiTexCoord2f(GLenum target, GLfloat s, GLfloat t);
void glNormal3fv(const GLfloat *v);
void glPointSize(GLfloat size);
void glHint(GLenum target, GLenum mode);
void glTexCoord3f(GLfloat s, GLfloat t, GLfloat r);
void glTexEnvfv(GLenum target, GLenum pname, const GLfloat *params);
void glTexParameterfv(GLenum target, GLenum pname, const GLfloat *params);
GLboolean glIsTexture(GLuint texture);
GLboolean glIsEnabled(GLenum cap);
void glFogf(GLenum pname, GLfloat param);
void glFogi(GLenum pname, GLint param);
void glFogfv(GLenum pname, const GLfloat *params);
void glTexGeni(GLenum coord, GLenum pname, GLint param);

/* Pack RGBA floats to uint32 (big-endian RGBA byte order for RSX) */
static inline uint32_t ps3gl_pack_color(float r, float g, float b, float a)
{
    uint32_t ri = (uint32_t)(r * 255.0f + 0.5f);
    uint32_t gi = (uint32_t)(g * 255.0f + 0.5f);
    uint32_t bi = (uint32_t)(b * 255.0f + 0.5f);
    uint32_t ai = (uint32_t)(a * 255.0f + 0.5f);
    if (ri > 255) ri = 255;
    if (gi > 255) gi = 255;
    if (bi > 255) bi = 255;
    if (ai > 255) ai = 255;
    return (ri << 24) | (gi << 16) | (bi << 8) | ai;
}

/* Pack RGBA bytes to uint32 */
static inline uint32_t ps3gl_pack_color_ub(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    return ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | a;
}

/* Identity matrix */
static const float ps3gl_identity[16] = {
    1, 0, 0, 0,
    0, 1, 0, 0,
    0, 0, 1, 0,
    0, 0, 0, 1
};

#endif /* PS3GL_H */
