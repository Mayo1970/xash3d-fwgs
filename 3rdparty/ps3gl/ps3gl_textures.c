/* ps3gl_textures.c -- GL-to-RSX: texture allocation, format conversion, binding. */

#include "ps3gl.h"
#include <stdio.h>
#include <stdlib.h>

/* Texture-upload telemetry, defined in ps3gl_main.c (local-extern convention, like ps3gl_vertices.c's
 * ps3gl_frame_flush_count). Reported once per 5s window in ps3gl_end_frame. */
extern uint32_t ps3gl_frame_tex_alloc_count;
extern uint32_t ps3gl_frame_subimage_count;
extern uint32_t ps3gl_frame_subimage_pixels;
extern uint32_t ps3gl_frame_defer_overflow_count;
extern uint32_t ps3gl_frame_mip_uploads;
extern uint32_t ps3gl_frame_mipped_binds;
extern uint32_t ps3gl_frame_tex_binds;

/* Debug-only proof mips reach the sampler: flat per-level tint on level>=1, plus a nonzero minlod so
 * nearby surfaces show it too. MUST be 0 in real builds; ship only as a clearly-labelled debug pkg. */
#ifndef PS3GL_MIP_DEBUG
#define PS3GL_MIP_DEBUG 0
#endif
#define PS3GL_MIP_DEBUG_MINLOD (2 << 8)

/* Pushed from ref_gl per frame (no cvar access here) and read at upload time, so a change applies on
 * the next map load / vid_restart: a same-session A/B on HW without a rebuild. */
static int g_swizzle_enabled = 1; /* layout-only: a bilinear tap's 2 rows share an RSX cache line */
static int g_mipmap_enabled  = 0; /* opt-in: changes the image (distant surfaces filtered, not aliased) */
static int g_texture_upload_probe_armed = 0;

void ps3gl_set_texture_options(int swizzle_enabled, int mipmap_enabled)
{
    g_swizzle_enabled = swizzle_enabled;
    g_mipmap_enabled  = mipmap_enabled;
}

void ps3gl_arm_texture_upload_probe(void)
{
    g_texture_upload_probe_armed = 1;
}

/* Swizzle staging: scattered CPU stores to GDDR3 are this HW's worst case, so build a level in cached RAM,
 * then ONE sequential memcpy to VRAM. Grown on demand; declared up here since shutdown frees it. */
static uint8_t *g_swz_staging = NULL;
static uint32_t g_swz_staging_size = 0;

/* Morton lookup tables + the level dims they were built for. Up here because convert_pixels() uses them
 * before swz_ensure_tables() (layout rules there) is defined; shutdown resets the dims. */
#define PS3GL_MAX_TEX_DIM 4096

static uint32_t g_swz_x[PS3GL_MAX_TEX_DIM];
static uint32_t g_swz_y[PS3GL_MAX_TEX_DIM];
static uint32_t g_swz_tbl_w = 0;
static uint32_t g_swz_tbl_h = 0;

/* Init / Shutdown */

void ps3gl_textures_init(void)
{
    for (int i = 0; i < PS3GL_MAX_TEXTURES; i++) {
        ps3gl.textures[i].glname = -1;
        ps3gl.textures[i].data   = NULL;
    }
    ps3gl.tex_next_name = 1;
}

void ps3gl_textures_shutdown(void)
{
    /* Caller (R_Free_Video) already rsxFinish()'d, so the GPU is idle --
     * safe to free pending and live textures directly here. */
    ps3gl_flush_deferred_texture_frees();

    for (int i = 0; i < PS3GL_MAX_TEXTURES; i++) {
        if (ps3gl.textures[i].data) {
            rsxFree(ps3gl.textures[i].data);
            ps3gl.textures[i].data = NULL;
        }
        ps3gl.textures[i].glname = -1;
    }

    /* Main-memory scratch, not VRAM: safe to free now. Reset the tables' cached dims so a later
     * ps3gl_init doesn't reuse stale ones. */
    if (g_swz_staging) {
        free(g_swz_staging);
        g_swz_staging = NULL;
        g_swz_staging_size = 0;
    }
    g_swz_tbl_w = 0;
    g_swz_tbl_h = 0;
}

/* See ps3gl.h's pending_tex_free: the RSX may still run draws queued against a just-unbound texture,
 * so rsxFree() waits until ps3gl_begin_frame's vring fence proves that slack drained. */
void ps3gl_defer_texture_free(void *data)
{
    if (!data) return;
    if (ps3gl.pending_tex_free_count >= PS3GL_MAX_TEXTURES) {
        /* Pathological churn only: flush now rather than drop the entry. NOTE: skips begin_frame's GPU
         * fence wait; telemetry-only for now, see ps3gl_frame_defer_overflow_count in the window log. */
        ps3gl_frame_defer_overflow_count++;
        ps3gl_flush_deferred_texture_frees();
    }
    ps3gl.pending_tex_free[ps3gl.pending_tex_free_count++] = data;
}

void ps3gl_flush_deferred_texture_frees(void)
{
    for (int i = 0; i < ps3gl.pending_tex_free_count; i++)
        rsxFree(ps3gl.pending_tex_free[i]);
    ps3gl.pending_tex_free_count = 0;
}

/* Lookup / Allocate */

ps3gl_texture_t *ps3gl_texture_find(GLuint name)
{
    /* Simple linear scan -- 4096 slots is small enough */
    for (int i = 0; i < PS3GL_MAX_TEXTURES; i++) {
        if (ps3gl.textures[i].glname == (int)name)
            return &ps3gl.textures[i];
    }
    return NULL;
}

ps3gl_texture_t *ps3gl_texture_alloc(GLuint name)
{
    for (int i = 0; i < PS3GL_MAX_TEXTURES; i++) {
        if (ps3gl.textures[i].glname == -1) {
            ps3gl_texture_t *t = &ps3gl.textures[i];
            memset(t, 0, sizeof(*t));
            t->glname     = (int)name;
            t->wrap_s     = GCM_TEXTURE_CLAMP_TO_EDGE;
            t->wrap_t     = GCM_TEXTURE_CLAMP_TO_EDGE;
            t->min_filter = GCM_TEXTURE_LINEAR;
            t->mag_filter = GCM_TEXTURE_LINEAR;
            t->dirty      = 1;
            return t;
        }
    }
    ps3gl_log("[ps3gl] WARNING: texture pool exhausted\n");
    return NULL;
}

/* GL functions */

void glGenTextures(GLsizei n, GLuint *textures)
{
    if (!textures || n <= 0) return;
    for (int i = 0; i < n; i++) {
        textures[i] = ps3gl.tex_next_name++;
    }
}

void glDeleteTextures(GLsizei n, const GLuint *textures)
{
    if (!textures) return;
    for (int i = 0; i < n; i++) {
        ps3gl_texture_t *t = ps3gl_texture_find(textures[i]);
        if (t) {
            for (int j = 0; j < PS3GL_MAX_TMUS; j++) {
                if (ps3gl.tmu[j].bound == t) {
                    ps3gl.tmu[j].bound = NULL;
                    ps3gl.tmu[j].dirty = 1;
                }
            }
            if (t->data) {
                ps3gl_defer_texture_free(t->data);
                t->data = NULL;
            }
            t->glname = -1;
        }
    }
}

void glBindTexture(GLenum target, GLuint texture)
{
    (void)target; /* only GL_TEXTURE_2D supported */
    if (texture == 0) {
        if (ps3gl.tmu[ps3gl.active_tmu].bound != NULL)
            ps3gl.tmu[ps3gl.active_tmu].dirty = 1;
        ps3gl.tmu[ps3gl.active_tmu].bound = NULL;
        return;
    }

    ps3gl_texture_t *t = ps3gl_texture_find(texture);
    if (!t) {
        t = ps3gl_texture_alloc(texture);
    }

    if (ps3gl.tmu[ps3gl.active_tmu].bound != t)
        ps3gl.tmu[ps3gl.active_tmu].dirty = 1;
    ps3gl.tmu[ps3gl.active_tmu].bound = t;
}

/* Pixel format helpers */

/* BGR/BGRA are required: unlike ioQuake3 (RGBA only), gl_image.c passes RF_BGRA/RF_BGR through
 * (gl_image.c:849-853), common for WAD/BMP. Without them GL_BGR fell to 4 bpp and desynced every row. */
static int gl_format_bpp(GLenum format)
{
    switch (format) {
    case GL_ALPHA:
    case GL_LUMINANCE:
    case GL_RED:
        return 1;
    case GL_LUMINANCE_ALPHA:
        return 2;
    case GL_RGB:
    case GL_RGB8:
    case GL_BGR:
        return 3;
    case GL_RGBA:
    case GL_RGBA8:
    case GL_BGRA:
    default:
        return 4;
    }
}

/* Do the source bytes arrive as B,G,R[,A] rather than R,G,B[,A]? */
static int gl_format_is_bgr(GLenum format)
{
    return format == GL_BGR || format == GL_BGRA;
}

/* Alpha OR-mask for an internal format; RGB ones sample alpha as 1 in GL. 8-bit BMPs carry
 * palette alpha 0 (Decay's btns_main.bmp), so without this they draw invisible under SRC_ALPHA. */
static uint32_t gl_internal_alpha_mask(GLint internalformat)
{
    switch (internalformat) {
    case 3:
    case GL_RGB:
    case GL_RGB5:
    case GL_RGB8:
        return 0xFF000000u;
    default:
        return 0;
    }
}

/* One pixel -> A8R8G8B8 word; big-endian, it lands as bytes A,R,G,B as RSX wants (same as the old byte
 * stores). Shared by every converter so format handling can't drift between linear and swizzled paths. */
static inline uint32_t fetch_pixel(const uint8_t *srow, int col, int src_bpp,
                                   GLenum src_format, int bgr)
{
    uint8_t r, g, b, a;

    switch (src_bpp) {
    case 1:
        if (src_format == GL_ALPHA) {
            r = g = b = 255;
            a = srow[col];
        } else {
            /* Luminance */
            r = g = b = srow[col];
            a = 255;
        }
        break;
    case 2:
        r = g = b = srow[col * 2];
        a = srow[col * 2 + 1];
        break;
    case 3:
        r = srow[col * 3 + (bgr ? 2 : 0)];
        g = srow[col * 3 + 1];
        b = srow[col * 3 + (bgr ? 0 : 2)];
        a = 255;
        break;
    case 4:
    default:
        r = srow[col * 4 + (bgr ? 2 : 0)];
        g = srow[col * 4 + 1];
        b = srow[col * 4 + (bgr ? 0 : 2)];
        a = srow[col * 4 + 3];
        break;
    }

    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

/* Level -> ARGB8888 (src_bpp 1=L/A 2=LA 3=RGB 4=RGBA). Linear: rows dst_stride apart, can exceed width*4
 * (mips share base pitch). Swizzled: tight Morton, needs swz_ensure_tables() first, dst = staging, never VRAM. */
static void convert_pixels(uint8_t *dst, uint32_t dst_stride, const uint8_t *src,
                           int width, int height, int src_bpp,
                           GLenum src_format, int swizzled, uint32_t amask)
{
    const int bgr = gl_format_is_bgr(src_format);
    /* Straight RGBA needs no channel shuffle; both branches store the same word, the split only keeps
     * the per-pixel format switch out of the common path. */
    const int fast = (src_bpp == 4 && !bgr);

    for (int row = 0; row < height; row++) {
        const uint8_t *srow = src + (size_t)row * width * src_bpp;

        if (swizzled) {
            uint32_t *d = (uint32_t *)dst;
            const uint32_t yo = g_swz_y[row];

            if (fast) {
                for (int col = 0; col < width; col++) {
                    d[yo | g_swz_x[col]] =
                        ((uint32_t)srow[col * 4 + 3] << 24) |
                        ((uint32_t)srow[col * 4 + 0] << 16) |
                        ((uint32_t)srow[col * 4 + 1] << 8)  |
                         (uint32_t)srow[col * 4 + 2] | amask;
                }
            } else {
                for (int col = 0; col < width; col++)
                    d[yo | g_swz_x[col]] = fetch_pixel(srow, col, src_bpp, src_format, bgr) | amask;
            }
        } else {
            uint32_t *drow = (uint32_t *)(dst + (uint32_t)row * dst_stride);

            if (fast) {
                for (int col = 0; col < width; col++) {
                    drow[col] =
                        ((uint32_t)srow[col * 4 + 3] << 24) |
                        ((uint32_t)srow[col * 4 + 0] << 16) |
                        ((uint32_t)srow[col * 4 + 1] << 8)  |
                         (uint32_t)srow[col * 4 + 2] | amask;
                }
            } else {
                for (int col = 0; col < width; col++)
                    drow[col] = fetch_pixel(srow, col, src_bpp, src_format, bgr) | amask;
            }
        }
    }
}

/* Mip layout per t->swizzled. LINEAR: RSX describes the chain from ONE width/height/pitch/offset, every
 * level uses the BASE pitch and only rows shrink -- NOT tightly packed, don't "fix" it. */

static uint32_t mip_level_dim(uint32_t base, int level)
{
    uint32_t d = base >> level;
    return d ? d : 1;
}

static int mip_num_levels(uint32_t w0, uint32_t h0)
{
    uint32_t maxdim = (w0 > h0) ? w0 : h0;
    int levels = 1;
    while (maxdim > 1) { maxdim >>= 1; levels++; }
    return levels;
}

/* Levels to allocate/accept. Mipmapping off collapses to the base level (num_levels 1, maxlod 0),
 * so every mip path below is inert and the image is unchanged. */
static int tex_alloc_levels(uint32_t w0, uint32_t h0)
{
    return g_mipmap_enabled ? mip_num_levels(w0, h0) : 1;
}

/* SWIZZLED: no pitch, each level Morton-ordered alone and packed tight (w*h*4): smaller (1.33x base vs
 * linear's 2x) and the layout the RSX texture cache is built for. */
static uint32_t tex_level_bytes(uint32_t w0, uint32_t h0, int level, int swizzled)
{
    if (swizzled)
        return mip_level_dim(w0, level) * mip_level_dim(h0, level) * 4;
    return (w0 * 4) * mip_level_dim(h0, level); /* base pitch, shrinking rows */
}

static uint32_t tex_level_offset(uint32_t w0, uint32_t h0, int level, int swizzled)
{
    uint32_t off = 0;
    for (int i = 0; i < level; i++)
        off += tex_level_bytes(w0, h0, i, swizzled);
    return off;
}

static uint32_t tex_total_size(uint32_t w0, uint32_t h0, int levels, int swizzled)
{
    return tex_level_offset(w0, h0, levels, swizzled);
}

/* RSX/NV swizzle interleaves x,y bits from the LSB, x first, while both have bits left; the larger dim's
 * leftover bits go high, so non-square pow2 works. 4x4: y1 x1 y0 x0. 8x2: x2 x1 y0 x0. */

static int is_pow2(uint32_t v)
{
    return v != 0 && (v & (v - 1)) == 0;
}

static int log2u(uint32_t v)
{
    int l = 0;
    while ((1u << l) < v) l++;
    return l;
}

/* Two lookup tables make the inner loop one OR per texel. Rebuilt only when level dims change, so same-size
 * uploads (a mip chain, many world textures) reuse them; declared at the top for convert_pixels(). */
static void swz_ensure_tables(uint32_t w, uint32_t h)
{
    if (w == g_swz_tbl_w && h == g_swz_tbl_h) return;
    if (w > PS3GL_MAX_TEX_DIM || h > PS3GL_MAX_TEX_DIM) return;

    const int lw = log2u(w), lh = log2u(h);
    int xpos[32], ypos[32];
    int nx = 0, ny = 0, shift = 0;

    while (nx < lw || ny < lh) {
        if (nx < lw) xpos[nx++] = shift++;
        if (ny < lh) ypos[ny++] = shift++;
    }

    for (uint32_t x = 0; x < w; x++) {
        uint32_t o = 0;
        for (int b = 0; b < lw; b++)
            if (x & (1u << b)) o |= 1u << xpos[b];
        g_swz_x[x] = o;
    }
    for (uint32_t y = 0; y < h; y++) {
        uint32_t o = 0;
        for (int b = 0; b < lh; b++)
            if (y & (1u << b)) o |= 1u << ypos[b];
        g_swz_y[y] = o;
    }

    g_swz_tbl_w = w;
    g_swz_tbl_h = h;
}

static uint8_t *swz_staging_get(uint32_t size)
{
    if (size > g_swz_staging_size) {
        uint8_t *p = (uint8_t *)realloc(g_swz_staging, size);
        if (!p) return NULL;
        g_swz_staging      = p;
        g_swz_staging_size = size;
    }
    return g_swz_staging;
}

static void build_gcm_texture(ps3gl_texture_t *t)
{
    memset(&t->gcm_tex, 0, sizeof(t->gcm_tex));
    /* Swizzled is the absence of GCM_TEXTURE_FORMAT_LIN (the SZ variant is 0). */
    t->gcm_tex.format    = GCM_TEXTURE_FORMAT_A8R8G8B8 |
                           (t->swizzled ? 0 : GCM_TEXTURE_FORMAT_LIN);
    t->gcm_tex.mipmap    = t->num_levels ? t->num_levels : 1;
    t->gcm_tex.dimension = GCM_TEXTURE_DIMS_2D;
    t->gcm_tex.cubemap   = GCM_FALSE;
    t->gcm_tex.remap     = PS3GL_TEX_REMAP_IDENTITY;
    t->gcm_tex.width     = t->width;
    t->gcm_tex.height    = t->height;
    t->gcm_tex.depth     = 1;
    t->gcm_tex.location  = GCM_LOCATION_RSX;
    /* HW ignores pitch when swizzled (address is pure Morton), so the linear value stays. If swizzled texels
     * look scrambled/sheared on HW, try 0 here BEFORE touching swz_ensure_tables, the likelier-correct part. */
    t->gcm_tex.pitch     = t->width * 4;
    t->gcm_tex.offset    = t->offset;
}

#if PS3GL_MIP_DEBUG
/* Flat tint on level>=1 after conversion; layout-agnostic, so ordered colour bands prove LEVEL SELECTION
 * reaches the sampler. Layout bugs show separately, as scrambled texels in the untinted build. */
static void ps3gl_mip_debug_tint(uint8_t *level_data, uint32_t level_bytes, int level)
{
    static const uint32_t colors[] = {
        0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFF00,
        0xFFFF00FF, 0xFF00FFFF, 0xFFFFFFFF,
    };
    if (level < 1) return;
    uint32_t color = colors[(level - 1) % (sizeof(colors) / sizeof(colors[0]))];
    uint32_t *d = (uint32_t *)level_data;
    for (uint32_t i = 0; i < level_bytes / 4; i++)
        d[i] = color;
}
#endif

void glTexImage2D(GLenum target, GLint level, GLint internalformat,
                      GLsizei width, GLsizei height, GLint border,
                      GLenum format, GLenum type, const void *pixels)
{
    (void)border; (void)type;

    ps3gl_texture_t *t;
    const uint32_t amask = gl_internal_alpha_mask(internalformat);

    if (level < 0) return;

    t = ps3gl.tmu[ps3gl.active_tmu].bound;
    if (!t) return;

    if (level == 0) {
        t->opaque = (uint8_t)(amask != 0);

        if (t->data && (t->width != (uint16_t)width || t->height != (uint16_t)height)) {
            ps3gl_defer_texture_free(t->data);
            t->data = NULL;
        }

        t->width      = (uint16_t)width;
        t->height     = (uint16_t)height;
        t->bpp        = 4; /* always store as ARGB8888 */
        t->num_levels = 0; /* regrown below as levels land; nothing grew it once, which pinned
                             * build_gcm_texture() and apply_textures' maxlod to 1 level/maxlod=0 */

        /* Layout is fixed at allocation: buffer size depends on it (linear chain ~2x base, swizzled ~1.33x),
         * and GL_UpdateTexture re-uploads in place after gl_ps3_swizzle may have toggled. */
        if (!t->data) {
            /* NPOT falls back to linear (RSX swizzle is pow2-only). Near-dead, gl_image.c rescales to pow2
             * as no NPOT ext is advertised, but a silently mis-sampled texture is too bad to leave open. */
            t->swizzled = (uint8_t)(g_swizzle_enabled &&
                                    is_pow2((uint32_t)width) && is_pow2((uint32_t)height) &&
                                    width <= PS3GL_MAX_TEX_DIM && height <= PS3GL_MAX_TEX_DIM);

            t->alloc_levels = (uint8_t)tex_alloc_levels((uint32_t)width, (uint32_t)height);

            uint32_t size = tex_total_size((uint32_t)width, (uint32_t)height,
                                           t->alloc_levels, t->swizzled);
            t->data = (uint8_t *)rsxMemalign(128, size);
            if (!t->data) {
                ps3gl_log("[ps3gl] WARNING: failed to allocate %u bytes for texture %d\n",
                       size, t->glname);
                return;
            }
            rsxAddressToOffset(t->data, &t->offset);
            ps3gl_frame_tex_alloc_count++;
        }
    }

    if (!t->data) { /* level>0 arrived before level 0 -- ignore */
        return;
    }
    if (level >= (int)(t->alloc_levels ? t->alloc_levels : 1)) {
        /* Beyond what this buffer was sized for; also drops levels >0 with mipmapping off. Uses the recorded
         * alloc_levels, not tex_alloc_levels(): gl_ps3_mipmap may have turned on since allocation. */
        return;
    }
    if (width != (GLsizei)mip_level_dim(t->width, level) ||
        height != (GLsizei)mip_level_dim(t->height, level)) {
        /* Mismatched level dims: refuse rather than scribble past this level's block. Never fires today,
         * gl_image.c always passes exact per-level dims. */
        return;
    }

    const uint32_t pitch      = (uint32_t)t->width * 4;
    const uint32_t lvl_off    = tex_level_offset(t->width, t->height, level, t->swizzled);
    const uint32_t lvl_bytes  = tex_level_bytes(t->width, t->height, level, t->swizzled);
    uint8_t       *lvl_data   = t->data + lvl_off;

    if (t->swizzled) {
        /* Never scatter-write texels straight into GDDR3: convert into cached
         * main memory, then one sequential memcpy to VRAM. */
        uint8_t *stage = swz_staging_get(lvl_bytes);
        if (!stage) {
            ps3gl_log("[ps3gl] WARNING: swizzle staging alloc failed (%u bytes), texture %d\n",
                      lvl_bytes, t->glname);
            return;
        }

        swz_ensure_tables((uint32_t)width, (uint32_t)height);

        if (pixels) {
            convert_pixels(stage, 0, (const uint8_t *)pixels,
                           width, height, gl_format_bpp(format), format, 1, amask);
        } else {
            memset(stage, 0, lvl_bytes);
        }
#if PS3GL_MIP_DEBUG
        ps3gl_mip_debug_tint(stage, lvl_bytes, level);
#endif
        memcpy(lvl_data, stage, lvl_bytes);
    } else {
        if (pixels) {
            convert_pixels(lvl_data, pitch, (const uint8_t *)pixels,
                           width, height, gl_format_bpp(format), format, 0, amask);
        } else {
            memset(lvl_data, 0, lvl_bytes);
        }
#if PS3GL_MIP_DEBUG
        ps3gl_mip_debug_tint(lvl_data, lvl_bytes, level);
#endif
    }

    /* Contiguous-only growth (gl_image.c uploads 0,1,2,... in order). A gap leaves data written but uncounted:
     * the sampler just sees fewer levels, instead of growing past it onto uninitialized rows. */
    if (level == 0) {
        t->num_levels = 1;
    } else if (level == t->num_levels && t->num_levels < 15) {
        t->num_levels = (uint8_t)(level + 1);
        ps3gl_frame_mip_uploads++;
    }

    build_gcm_texture(t);
    t->dirty = 1;
    t->content_dirty = 1;
}

void glTexSubImage2D(GLenum target, GLint level, GLint xoff, GLint yoff,
                          GLsizei w, GLsizei h, GLenum format, GLenum type,
                          const void *pixels)
{
    const int upload_probe = g_texture_upload_probe_armed;
    g_texture_upload_probe_armed = 0;

    (void)target; (void)type;
    if (level < 0 || !pixels) {
        if (upload_probe)
            ps3gl_log("[ps3gl] dlight upload probe: skipped (level=%d pixels=%p)\n",
                      level, pixels);
        return;
    }

    ps3gl_texture_t *t = ps3gl.tmu[ps3gl.active_tmu].bound;
    if (!t || !t->data || level >= (int)t->num_levels) {
        if (upload_probe)
            ps3gl_log("[ps3gl] dlight upload probe: skipped (texture unavailable, level=%d)\n",
                      level);
        return;
    }

    /* Via the level helpers, level 0 yields byte-identical addresses to the old level-0-only code in BOTH
     * layouts, so the HW-validated packed-store fast path below is unchanged for its lightmap call sites. */
    const uint32_t pitch = (uint32_t)t->width * 4;
    const int lw = (int)mip_level_dim(t->width, level);
    const int lh = (int)mip_level_dim(t->height, level);
    uint8_t *level_data = t->data + tex_level_offset(t->width, t->height, level, t->swizzled);

    int src_bpp = gl_format_bpp(format);
    /* Same BGR handling as convert_pixels -- lightmap updates come through here. */
    const int bgr = gl_format_is_bgr(format);
    const uint32_t amask = t->opaque ? 0xFF000000u : 0;
    const uint8_t *src = (const uint8_t *)pixels;

    ps3gl_frame_subimage_count++;
    ps3gl_frame_subimage_pixels += (uint32_t)(w > 0 && h > 0 ? w * h : 0);

    /* Clip once up front, not per pixel: every store below is an uncached CPU write to GDDR3, which
     * dwarfs a branch, but hoisting it out of the inner loop is still worth it. */
    int row0 = (yoff < 0) ? -yoff : 0;
    int row1 = (yoff + h > lh) ? (lh - yoff) : h;
    int col0 = (xoff < 0) ? -xoff : 0;
    int col1 = (xoff + w > lw) ? (lw - xoff) : w;
    const int full_rect = (xoff == 0 && yoff == 0 && w == lw && h == lh);

    if (t->swizzled && full_rect) {
        /* Whole-level refresh (GL_UpdateTexture re-upload): same volume as glTexImage2D, so the same
         * staging route instead of scatter-writing a full level into VRAM. */
        uint32_t lvl_bytes = tex_level_bytes(t->width, t->height, level, 1);
        uint8_t *stage = swz_staging_get(lvl_bytes);
        if (!stage) return;

        swz_ensure_tables((uint32_t)lw, (uint32_t)lh);
        convert_pixels(stage, 0, src, lw, lh, src_bpp, format, 1, amask);
        memcpy(level_data, stage, lvl_bytes);
    } else if (t->swizzled) {
        /* Partial swizzled rect goes straight to VRAM: dynamic lightmaps are only a few hundred to ~2k px/frame
         * (~41k px over ~225 frames) and Morton keeps a small rect clustered; staging + memcpy would cost more. */
        swz_ensure_tables((uint32_t)lw, (uint32_t)lh);
        uint32_t *d = (uint32_t *)level_data;

        for (int row = row0; row < row1; row++) {
            const uint8_t *srow = src + (size_t)row * w * src_bpp;
            const uint32_t yo = g_swz_y[yoff + row];
            for (int col = col0; col < col1; col++)
                d[yo | g_swz_x[xoff + col]] =
                    fetch_pixel(srow, col, src_bpp, format, bgr) | amask;
        }
    } else if (src_bpp == 4 && !bgr) {
        /* THE hot linear path (both gl_rsurf.c lightmap uploads pass GL_RGBA/UNSIGNED_BYTE): one packed BE word
         * per pixel, as 4 byte stores defeat write-combining on RSX-local memory. Mirrors convert_pixels(). */
        for (int row = row0; row < row1; row++) {
            int dy = yoff + row;
            uint32_t *drow = (uint32_t *)(level_data + (uint32_t)dy * pitch);
            const uint8_t *srow = src + (size_t)row * w * 4;
            for (int col = col0; col < col1; col++) {
                int dx = xoff + col;
                uint8_t r = srow[col * 4 + 0];
                uint8_t g = srow[col * 4 + 1];
                uint8_t b = srow[col * 4 + 2];
                uint8_t a = srow[col * 4 + 3];
                drow[dx] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b | amask;
            }
        }
    } else {
        for (int row = row0; row < row1; row++) {
            int dy = yoff + row;
            uint32_t *drow = (uint32_t *)(level_data + (uint32_t)dy * pitch);
            const uint8_t *srow = src + (size_t)row * w * src_bpp;
            for (int col = col0; col < col1; col++)
                drow[xoff + col] = fetch_pixel(srow, col, src_bpp, format, bgr) | amask;
        }
    }

    if (upload_probe) {
        uint32_t mismatches = 0;
        uint32_t compared = 0;
        int first_row = -1, first_col = -1;
        uint8_t first_src[4] = { 0, 0, 0, 0 };
        uint8_t first_dst[4] = { 0, 0, 0, 0 };

        /* RSX A8R8G8B8 expects bytes A,R,G,B; comparing bytes (not words) makes this a real endian/layout
         * check: a little-endian packed store would read back B,G,R,A and fail. */
        __asm__ volatile("sync" ::: "memory");
        if (t->swizzled)
            swz_ensure_tables((uint32_t)lw, (uint32_t)lh);

        for (int row = row0; row < row1; row++) {
            const uint8_t *srow = src + (size_t)row * w * src_bpp;
            for (int col = col0; col < col1; col++) {
                const uint32_t packed = fetch_pixel(srow, col, src_bpp, format, bgr) | amask;
                const uint8_t expected[4] = {
                    (uint8_t)(packed >> 24), (uint8_t)(packed >> 16),
                    (uint8_t)(packed >> 8), (uint8_t)packed
                };
                const uint8_t *actual;

                if (t->swizzled) {
                    const uint32_t texel = g_swz_y[yoff + row] | g_swz_x[xoff + col];
                    actual = level_data + texel * 4;
                } else {
                    actual = level_data + (uint32_t)(yoff + row) * pitch +
                             (uint32_t)(xoff + col) * 4;
                }

                compared++;
                if (actual[0] != expected[0] || actual[1] != expected[1] ||
                    actual[2] != expected[2] || actual[3] != expected[3]) {
                    mismatches++;
                    if (first_row < 0) {
                        first_row = row;
                        first_col = col;
                        first_src[0] = expected[0]; first_src[1] = expected[1];
                        first_src[2] = expected[2]; first_src[3] = expected[3];
                        first_dst[0] = actual[0]; first_dst[1] = actual[1];
                        first_dst[2] = actual[2]; first_dst[3] = actual[3];
                    }
                }
            }
        }

        ps3gl_log("[ps3gl] dlight upload probe: %dx%d at %d,%d texture=%d "
                  "swizzled=%d pitch=%u byte mismatches=%u/%u\n",
                  w, h, xoff, yoff, t->glname, (int)t->swizzled,
                  (unsigned)t->gcm_tex.pitch, (unsigned)mismatches,
                  (unsigned)compared);
        if (mismatches) {
            ps3gl_log("[ps3gl] dlight upload probe: first mismatch row=%d col=%d "
                      "expected ARGB=%02x,%02x,%02x,%02x got=%02x,%02x,%02x,%02x\n",
                      first_row, first_col,
                      first_src[0], first_src[1], first_src[2], first_src[3],
                      first_dst[0], first_dst[1], first_dst[2], first_dst[3]);
        }
    }

    /* Full-rect refresh regrows the chain like glTexImage2D, so GL_UpdateTexture can't leave stale higher
     * levels live under a shrunk count. Partial rects never touch it: lightmaps are TF_NOMIPMAP, stays 1. */
    if (full_rect) {
        if (level == 0) {
            t->num_levels = 1;
        } else if (level == t->num_levels && t->num_levels < 15) {
            t->num_levels = (uint8_t)(level + 1);
            ps3gl_frame_mip_uploads++;
        }
    }

    build_gcm_texture(t);
    t->dirty = 1;
    t->content_dirty = 1;
}

/* Texture parameters */

static uint8_t gl_to_gcm_wrap(GLint param)
{
    switch (param) {
    case GL_REPEAT:        return GCM_TEXTURE_REPEAT;
    case GL_CLAMP:         return GCM_TEXTURE_CLAMP;
    case GL_CLAMP_TO_EDGE: return GCM_TEXTURE_CLAMP_TO_EDGE;
    default:               return GCM_TEXTURE_REPEAT;
    }
}

/* Faithful 1:1. A probe clamping *_MIPMAP_LINEAR to *_NEAREST (did trilinear offset the padded-pitch mip win?)
 * is superseded (mips now tight + swizzled, off by default) and was reverted: it changed the image. */
static uint8_t gl_to_gcm_filter(GLint param)
{
    switch (param) {
    case GL_NEAREST:                return GCM_TEXTURE_NEAREST;
    case GL_LINEAR:                 return GCM_TEXTURE_LINEAR;
    case GL_NEAREST_MIPMAP_NEAREST: return GCM_TEXTURE_NEAREST_MIPMAP_NEAREST;
    case GL_LINEAR_MIPMAP_NEAREST:  return GCM_TEXTURE_LINEAR_MIPMAP_NEAREST;
    case GL_NEAREST_MIPMAP_LINEAR:  return GCM_TEXTURE_NEAREST_MIPMAP_LINEAR;
    case GL_LINEAR_MIPMAP_LINEAR:   return GCM_TEXTURE_LINEAR_MIPMAP_LINEAR;
    default:                        return GCM_TEXTURE_LINEAR;
    }
}

static void tex_param(GLenum pname, GLint param)
{
    ps3gl_texture_t *t = ps3gl.tmu[ps3gl.active_tmu].bound;
    if (!t) return;

    switch (pname) {
    case GL_TEXTURE_WRAP_S:     t->wrap_s     = gl_to_gcm_wrap(param); break;
    case GL_TEXTURE_WRAP_T:     t->wrap_t     = gl_to_gcm_wrap(param); break;
    case GL_TEXTURE_MIN_FILTER: t->min_filter = gl_to_gcm_filter(param); break;
    case GL_TEXTURE_MAG_FILTER: t->mag_filter = gl_to_gcm_filter(param); break;
    case GL_TEXTURE_MAX_ANISOTROPY_EXT:
    case GL_GENERATE_MIPMAP:
        break;
    default: break;
    }
    t->dirty = 1;
}

void glTexParameterf(GLenum target, GLenum pname, GLfloat param)
{
    (void)target;
    tex_param(pname, (GLint)param);
}

void glTexParameteri(GLenum target, GLenum pname, GLint param)
{
    (void)target;
    tex_param(pname, param);
}

/* TexEnv */

void glTexEnvf(GLenum target, GLenum pname, GLfloat param)
{
    (void)target;
    if (pname != GL_TEXTURE_ENV_MODE) return;

    int mode;
    GLenum p = (GLenum)(int)param;
    switch (p) {
    case GL_MODULATE:       mode = PS3GL_TENV_MODULATE; break;
    case GL_REPLACE:        mode = PS3GL_TENV_REPLACE; break;
    case GL_DECAL:          mode = PS3GL_TENV_DECAL; break;
    case GL_ADD:            mode = PS3GL_TENV_ADD; break;
    default:                mode = PS3GL_TENV_MODULATE; break;
    }
    if (ps3gl.tmu[ps3gl.active_tmu].texenv != mode)
        ps3gl.tmu[ps3gl.active_tmu].dirty = 1;
    ps3gl.tmu[ps3gl.active_tmu].texenv = mode;
}

void glTexEnvi(GLenum target, GLenum pname, GLint param)
{
    glTexEnvf(target, pname, (GLfloat)param);
}

/* Multitexture */

void glActiveTextureARB(GLenum texture)
{
    int tmu = (int)(texture - GL_TEXTURE0_ARB);
    if (tmu >= 0 && tmu < PS3GL_MAX_TMUS)
        ps3gl.active_tmu = tmu;
}

void glClientActiveTextureARB(GLenum texture)
{
    int tmu = (int)(texture - GL_TEXTURE0_ARB);
    if (tmu >= 0 && tmu < PS3GL_MAX_TMUS)
        ps3gl.client_active_tmu = tmu;
}

void glPixelStorei(GLenum pname, GLint param)
{
    (void)pname; (void)param;
}

void glCopyTexSubImage2D(GLenum target, GLint level, GLint xoff,
                              GLint yoff, GLint x, GLint y,
                              GLsizei w, GLsizei h)
{
    (void)target; (void)level; (void)xoff; (void)yoff;
    (void)x; (void)y; (void)w; (void)h;
}

/* Apply textures to RSX before draw */

void ps3gl_apply_textures(void)
{
    gcmContextData *ctx = ps3gl_get_ctx();
    if (!ctx) return;

    /* Uploads write texels straight into RSX-local memory: sync orders those PPE stores before the FIFO,
     * then the texture cache is invalidated right before a draw can sample them. */
    int invalidate_texture_cache = 0;
    /* Not t->dirty, which also covers sampler-only (filter/wrap) changes that don't stale texels. One invalidate
     * covers both TMUs; an unbound texture keeps content_dirty until an apply pass can sample it. */
    for (int i = 0; i < PS3GL_MAX_TMUS; i++) {
        ps3gl_tmu_t *tmu = &ps3gl.tmu[i];
        ps3gl_texture_t *t = tmu->bound;
        if (tmu->enabled && t && t->data && t->content_dirty) {
            invalidate_texture_cache = 1;
            break;
        }
    }
    if (invalidate_texture_cache) {
        __asm__ volatile("sync" ::: "memory");
        rsxInvalidateTextureCache(ctx, GCM_INVALIDATE_TEXTURE);
    }

    for (int i = 0; i < PS3GL_MAX_TMUS; i++) {
        ps3gl_tmu_t *tmu = &ps3gl.tmu[i];
        ps3gl_texture_t *t = tmu->bound;

        if (!tmu->enabled || !t || !t->data) {
            if (tmu->dirty) {
                rsxTextureControl(ctx, i, GCM_FALSE, 0, 0, 0);
                tmu->dirty = 0;
            }
            continue;
        }

        if (!tmu->dirty && !t->dirty) continue;

        rsxLoadTexture(ctx, i, &t->gcm_tex);
        /* maxlod is 4.8 fixed point, NOT an integer level count -- get this wrong and the
         * texture binds but renders nothing. maxlod = (num_levels-1)<<8 so the sampler can reach every uploaded mip. */
        {
            uint16_t maxlod = (uint16_t)(((t->num_levels ? t->num_levels : 1) - 1) << 8);
            uint16_t minlod = 0;
#if PS3GL_MIP_DEBUG
            /* Debug-only: force a mip level regardless of distance so the tinted levels show up close too. */
            if (t->num_levels > 1)
                minlod = PS3GL_MIP_DEBUG_MINLOD;
#endif
            rsxTextureControl(ctx, i, GCM_TRUE, minlod, maxlod, GCM_TEXTURE_MAX_ANISO_1);
            ps3gl_frame_tex_binds++;
            if (t->num_levels > 1)
                ps3gl_frame_mipped_binds++;
        }
        rsxTextureFilter(ctx, i, 0, t->min_filter, t->mag_filter,
                         GCM_TEXTURE_CONVOLUTION_QUINCUNX);
        rsxTextureWrapMode(ctx, i, t->wrap_s, t->wrap_t,
                           GCM_TEXTURE_CLAMP_TO_EDGE,
                           GCM_TEXTURE_UNSIGNED_REMAP_NORMAL,
                           GCM_TEXTURE_ZFUNC_NEVER, 0);

        tmu->dirty = 0;
        t->dirty = 0;
        t->content_dirty = 0;
    }
}
