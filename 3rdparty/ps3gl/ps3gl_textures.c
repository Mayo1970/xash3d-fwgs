/* ps3gl_textures.c -- GL-to-RSX: texture allocation, format conversion, binding. */

#include "ps3gl.h"
#include <stdio.h>
#include <stdlib.h>

/* Texture-upload telemetry counters, declared in ps3gl_main.c -- same
 * "declare just what we need" local-extern convention as
 * ps3gl_vertices.c's ps3gl_frame_flush_count. Reported once per 5s window
 * in ps3gl_end_frame. */
extern uint32_t ps3gl_frame_tex_alloc_count;
extern uint32_t ps3gl_frame_subimage_count;
extern uint32_t ps3gl_frame_subimage_pixels;
extern uint32_t ps3gl_frame_defer_overflow_count;
extern uint32_t ps3gl_frame_mip_uploads;
extern uint32_t ps3gl_frame_mipped_binds;
extern uint32_t ps3gl_frame_tex_binds;

/* Debug-only visual proof that mip levels are actually reaching the RSX
 * sampler: tints level>=1 rows with a flat per-level colour, and forces a
 * nonzero minlod so nearby surfaces show a tinted level too, without
 * needing to find a long sightline. MUST be 0 in any real build -- ship as
 * a separate, clearly-labelled debug pkg only. */
#ifndef PS3GL_MIP_DEBUG
#define PS3GL_MIP_DEBUG 0
#endif
#define PS3GL_MIP_DEBUG_MINLOD (2 << 8)

/* Renderer options pushed down from ref_gl once per frame (ps3gl has no access
 * to the cvar system). Both are consulted at texture-upload time, so changing
 * either takes effect on the next map load / vid_restart -- which is what makes
 * a same-session A/B possible on real hardware without a rebuild.
 *
 * swizzle defaults ON: it is a pure memory-layout change (identical sampled
 * pixels) that turns a bilinear tap's two texel rows from `width*4` bytes apart
 * into neighbours in the same RSX texture-cache line. mipmap defaults OFF: it
 * genuinely changes the image (distant surfaces get filtered rather than
 * aliased), so it stays opt-in. */
static int g_swizzle_enabled = 1;
static int g_mipmap_enabled  = 0;
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

/* Staging buffer for swizzled full-level uploads. Swizzling writes texels in
 * scattered order, and texture data lives in RSX local memory (GDDR3) where
 * scattered CPU stores are the documented worst case on this hardware -- the
 * same anti-pattern glTexSubImage2D's packed-store fast path exists to avoid.
 * So build the level in cached main memory, then push it to VRAM with ONE
 * sequential memcpy. Grown on demand, never shrunk, freed at shutdown.
 *
 * Declared up here rather than beside swz_staging_get() below because
 * ps3gl_textures_shutdown() (defined above both) has to release it. Same for
 * the swizzle tables' cached dimensions, which shutdown invalidates. */
static uint8_t *g_swz_staging = NULL;
static uint32_t g_swz_staging_size = 0;

/* Swizzle (Morton) address lookup tables and the level dimensions they were
 * built for. Definitions live up here because convert_pixels() below is
 * defined before swz_ensure_tables(); see that function for the layout rules. */
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

    /* Main-memory scratch, not VRAM -- safe to release unconditionally. The
     * swizzle tables are plain statics, but their cached dimensions must be
     * invalidated so a later ps3gl_init doesn't reuse stale ones. */
    if (g_swz_staging) {
        free(g_swz_staging);
        g_swz_staging = NULL;
        g_swz_staging_size = 0;
    }
    g_swz_tbl_w = 0;
    g_swz_tbl_h = 0;
}

/* Deferred texture free -- see ps3gl.h's pending_tex_free comment. The RSX
 * can still be executing draws queued against a texture that the CPU just
 * unbound, so rsxFree() here must wait for that pipeline slack to drain
 * first (ps3gl_begin_frame's vring fence proves it has), not free on the spot. */
void ps3gl_defer_texture_free(void *data)
{
    if (!data) return;
    if (ps3gl.pending_tex_free_count >= PS3GL_MAX_TEXTURES) {
        /* Pool-exhaustion levels of churn in one frame -- flush now rather
         * than drop the entry, this is only ever reached under pathological load.
         * NOTE: this flush skips the GPU fence wait ps3gl_begin_frame normally
         * provides before a deferred free is safe -- telemetry-only for now,
         * see ps3gl_frame_defer_overflow_count in ps3gl_end_frame's window log. */
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
            /* Unbind from any TMU */
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

/* BGR/BGRA are NOT optional here. This layer came from the ioQuake3 port, whose
 * renderer only ever uploads GL_RGBA -- but xash's gl_image.c maps rgbdata's
 * RF_BGRA/RF_BGR straight through (gl_image.c:849-853), which is the common
 * case for GoldSrc WAD/BMP content. Without these cases GL_BGR fell to the
 * default of 4 bytes/pixel and desynced every row. */
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

/* One source pixel -> A8R8G8B8 packed as a uint32. Stored as a 32-bit word on
 * this big-endian target that lands as bytes A,R,G,B -- exactly what RSX
 * A8R8G8B8 wants, and byte-identical to the four separate byte stores this
 * replaced. Shared by every converter below so format handling cannot drift
 * between the linear and swizzled paths. */
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

/* Convert a full level's source pixels to ARGB8888 for RSX. src_bpp: 1=L/A,
 * 2=LA, 3=RGB, 4=RGBA.
 *
 * swizzled==0: row-major, dst_stride bytes apart. dst_stride can exceed
 *   width*4 -- linear RSX mip levels share the base level's pitch, so smaller
 *   levels get padded rows instead of tight packing.
 * swizzled==1: Morton order, dst_stride ignored, dst tightly packed. Caller
 *   must have called swz_ensure_tables(width, height) first, and dst must be
 *   the main-memory staging buffer, never VRAM directly. */
static void convert_pixels(uint8_t *dst, uint32_t dst_stride, const uint8_t *src,
                           int width, int height, int src_bpp,
                           GLenum src_format, int swizzled)
{
    const int bgr = gl_format_is_bgr(src_format);
    /* Straight RGBA in needs no channel shuffling, so it gets a tighter inner
     * loop. Both branches below store the same packed word; the split is only
     * to keep the per-pixel format switch out of the common path. */
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
                         (uint32_t)srow[col * 4 + 2];
                }
            } else {
                for (int col = 0; col < width; col++)
                    d[yo | g_swz_x[col]] = fetch_pixel(srow, col, src_bpp, src_format, bgr);
            }
        } else {
            uint32_t *drow = (uint32_t *)(dst + (uint32_t)row * dst_stride);

            if (fast) {
                for (int col = 0; col < width; col++) {
                    drow[col] =
                        ((uint32_t)srow[col * 4 + 3] << 24) |
                        ((uint32_t)srow[col * 4 + 0] << 16) |
                        ((uint32_t)srow[col * 4 + 1] << 8)  |
                         (uint32_t)srow[col * 4 + 2];
                }
            } else {
                for (int col = 0; col < width; col++)
                    drow[col] = fetch_pixel(srow, col, src_bpp, src_format, bgr);
            }
        }
    }
}

/* Mip chain helpers.
 *
 * Two layouts, chosen per texture by t->swizzled:
 *
 *   LINEAR: RSX describes the whole chain from ONE width/height/pitch/offset --
 *     every level uses the BASE level's pitch, only row count shrinks. NOT
 *     tightly packed, don't "fix" it into being so.
 *   SWIZZLED: no pitch at all. Each level is Morton-ordered independently and
 *     levels are packed tight (w*h*4 each), which is both smaller (1.33x base
 *     vs the linear chain's 2x) and the layout the RSX texture cache is built
 *     for. */

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

/* Levels to actually allocate and accept uploads for. With mipmapping off the
 * pyramid collapses to the base level, so num_levels stays 1, maxlod stays 0,
 * and every mip code path below is inert -- today's image, unchanged. */
static int tex_alloc_levels(uint32_t w0, uint32_t h0)
{
    return g_mipmap_enabled ? mip_num_levels(w0, h0) : 1;
}

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

/* Swizzle (Morton order) addressing.
 *
 * RSX/NV swizzle interleaves the x and y bits, x's bit first at the LSB,
 * alternating while both dimensions still have bits left; once the smaller
 * dimension runs out, the larger dimension's remaining bits occupy the high
 * positions (so non-square power-of-two textures work). Example, 4x4:
 * bit layout y1 x1 y0 x0. Example, 8x2: x2 x1 y0 x0.
 *
 * Encoded as two lookup tables so the converter's inner loop is a single OR
 * per texel instead of a per-bit loop. Tables are shared and rebuilt only when
 * the level dimensions change -- consecutive uploads of the same size (the
 * common case: a whole mip chain, or many same-size world textures) reuse them.
 * The tables themselves are declared near the top of this file, because
 * convert_pixels() above is defined before this point and consumes them. */

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

/* Build gcmTexture descriptor */
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
    /* Hardware ignores pitch for swizzled textures (the address comes entirely
     * from the Morton interleave), so the linear value is left in place rather
     * than special-cased. If a first hardware test shows scrambled texels or
     * diagonal shearing on swizzled textures, try 0 here BEFORE touching
     * swz_ensure_tables -- the interleave is the far more likely-correct part. */
    t->gcm_tex.pitch     = t->width * 4;
    t->gcm_tex.offset    = t->offset;
}

#if PS3GL_MIP_DEBUG
/* Flat per-level tint, level>=1 only. Applied to the level's data block after
 * conversion, whichever layout it is in -- a flat fill is layout-agnostic, so
 * correctly ordered colour bands on screen are evidence the LEVEL SELECTION is
 * reaching the sampler. (Layout correctness shows up as scrambled texels in
 * the normal, untinted build; these two checks are deliberately separate.) */
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
    (void)border; (void)type; (void)internalformat;

    ps3gl_texture_t *t;

    if (level < 0) return;

    t = ps3gl.tmu[ps3gl.active_tmu].bound;
    if (!t) return;

    if (level == 0) {
        /* Free old data if base dimensions changed */
        if (t->data && (t->width != (uint16_t)width || t->height != (uint16_t)height)) {
            ps3gl_defer_texture_free(t->data);
            t->data = NULL;
        }

        t->width      = (uint16_t)width;
        t->height     = (uint16_t)height;
        t->bpp        = 4; /* always store as ARGB8888 */
        t->num_levels = 0; /* reset here; growth logic below (level==0, or
                             * level==num_levels for level>0) rebuilds this
                             * as levels actually land -- see build_gcm_texture()
                             * and ps3gl_apply_textures()'s maxlod, both of
                             * which were silently pinned to 1 level/maxlod=0
                             * forever because nothing used to grow this. */

        /* Layout is decided ONCE, at allocation, and then fixed for the life of
         * that buffer -- its size depends on the layout (a linear mip chain is
         * ~2x the base level, a swizzled one ~1.33x), so re-deciding under a
         * live allocation could size-mismatch it. That is reachable in practice:
         * GL_UpdateTexture re-uploads an existing texture in place at the same
         * dimensions, and gl_ps3_swizzle may have been toggled in between.
         *
         * Non-power-of-two falls back to linear -- RSX swizzle is only defined
         * for power-of-two dimensions. That fallback is near-dead code, since
         * ps3gl advertises only GL_ARB_multitexture, so GL_ARB_TEXTURE_NPOT_EXT
         * is off and gl_image.c rescales every texture to power-of-two before
         * upload -- but a silently mis-sampled texture is a bad failure mode to
         * leave open, so the check is real. */
        if (!t->data) {
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
        /* Beyond what this buffer was sized for. Also the mipmapping-off path:
         * levels above 0 are simply dropped, exactly as before this feature
         * existed. Checked against the texture's own recorded level count, not
         * a fresh tex_alloc_levels() call -- gl_ps3_mipmap could have been
         * turned on since this buffer was allocated at one level. */
        return;
    }
    if (width != (GLsizei)mip_level_dim(t->width, level) ||
        height != (GLsizei)mip_level_dim(t->height, level)) {
        /* Mismatched level dims -- refuse rather than scribble past this
         * level's allocated block. Never fires today: gl_image.c always passes
         * exact per-level dims. */
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
                           width, height, gl_format_bpp(format), format, 1);
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
                           width, height, gl_format_bpp(format), format, 0);
        } else {
            memset(lvl_data, 0, lvl_bytes);
        }
#if PS3GL_MIP_DEBUG
        ps3gl_mip_debug_tint(lvl_data, lvl_bytes, level);
#endif
    }

    /* Contiguous-only growth: gl_image.c's mip-build loop uploads levels
     * 0,1,2,... in order. A gap (level arrives ahead of num_levels) leaves
     * its data written but uncounted -- safe (sampler just sees fewer
     * levels than are actually in VRAM), unlike exposing uninitialized rows
     * to the sampler by growing past a gap. */
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

    /* Deriving lw/lh/level_data through the level helpers makes level==0
     * generate byte-identical addresses to the old level-0-only code
     * (lw==t->width, lh==t->height, level_data==t->data) in BOTH layouts, so
     * the already-hardware-validated packed-store fast path below is unchanged
     * for the lightmap call sites that are its whole reason to exist. */
    const uint32_t pitch = (uint32_t)t->width * 4;
    const int lw = (int)mip_level_dim(t->width, level);
    const int lh = (int)mip_level_dim(t->height, level);
    uint8_t *level_data = t->data + tex_level_offset(t->width, t->height, level, t->swizzled);

    int src_bpp = gl_format_bpp(format);
    /* Same BGR handling as convert_pixels -- lightmap updates come through here. */
    const int bgr = gl_format_is_bgr(format);
    const uint8_t *src = (const uint8_t *)pixels;

    ps3gl_frame_subimage_count++;
    ps3gl_frame_subimage_pixels += (uint32_t)(w > 0 && h > 0 ? w * h : 0);

    /* Clip to the destination rect once, up front, instead of a per-pixel
     * bounds check -- t->data lives in RSX local memory (GDDR3), and every
     * store below is an uncached CPU write to VRAM; branches in the inner
     * loop are cheap by comparison to that, but still worth hoisting. */
    int row0 = (yoff < 0) ? -yoff : 0;
    int row1 = (yoff + h > lh) ? (lh - yoff) : h;
    int col0 = (xoff < 0) ? -xoff : 0;
    int col1 = (xoff + w > lw) ? (lw - xoff) : w;
    const int full_rect = (xoff == 0 && yoff == 0 && w == lw && h == lh);

    if (t->swizzled && full_rect) {
        /* Whole-level refresh (GL_UpdateTexture re-uploading an existing
         * texture): same volume as a glTexImage2D, so it takes the same
         * staging-buffer route rather than scatter-writing a full level
         * straight into VRAM. */
        uint32_t lvl_bytes = tex_level_bytes(t->width, t->height, level, 1);
        uint8_t *stage = swz_staging_get(lvl_bytes);
        if (!stage) return;

        swz_ensure_tables((uint32_t)lw, (uint32_t)lh);
        convert_pixels(stage, 0, src, lw, lh, src_bpp, format, 1);
        memcpy(level_data, stage, lvl_bytes);
    } else if (t->swizzled) {
        /* Partial rect into a swizzled level -- written straight to VRAM, no
         * staging. Scattered VRAM stores are normally the anti-pattern to
         * avoid, but this path's real volume is tiny: the dynamic-lightmap
         * updates that dominate it measure a few hundred to a couple thousand
         * pixels PER FRAME in existing telemetry (window sums of ~41k px over
         * ~225 frames). Morton order also keeps a small rect's addresses
         * clustered rather than pitch-strided. Staging + a full-level memcpy
         * here would cost far more than it saved. */
        swz_ensure_tables((uint32_t)lw, (uint32_t)lh);
        uint32_t *d = (uint32_t *)level_data;

        for (int row = row0; row < row1; row++) {
            const uint8_t *srow = src + (size_t)row * w * src_bpp;
            const uint32_t yo = g_swz_y[yoff + row];
            for (int col = col0; col < col1; col++)
                d[yo | g_swz_x[xoff + col]] =
                    fetch_pixel(srow, col, src_bpp, format, bgr);
        }
    } else if (src_bpp == 4 && !bgr) {
        /* Fast path, mirrors convert_pixels(): straight RGBA in, so one
         * packed 32-bit store matches PS3's big-endian byte order for free.
         * This is THE hot path for linear textures -- both GoldSrc lightmap-
         * upload call sites (gl_rsurf.c) pass GL_RGBA/GL_UNSIGNED_BYTE. Four
         * separate byte stores per pixel (the code this replaced) defeats
         * write-combining on RSX-local memory; one aligned word store does not. */
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
                drow[dx] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
            }
        }
    } else {
        for (int row = row0; row < row1; row++) {
            int dy = yoff + row;
            uint32_t *drow = (uint32_t *)(level_data + (uint32_t)dy * pitch);
            const uint8_t *srow = src + (size_t)row * w * src_bpp;
            for (int col = col0; col < col1; col++)
                drow[xoff + col] = fetch_pixel(srow, col, src_bpp, format, bgr);
        }
    }

    if (upload_probe) {
        uint32_t mismatches = 0;
        uint32_t compared = 0;
        int first_row = -1, first_col = -1;
        uint8_t first_src[4] = { 0, 0, 0, 0 };
        uint8_t first_dst[4] = { 0, 0, 0, 0 };

        /* The RSX A8R8G8B8 texture expects bytes A,R,G,B in memory. Comparing
         * bytes (not uint32 values) makes this a real endian/layout check: a
         * little-endian packed store would read back B,G,R,A and fail here. */
        __asm__ volatile("sync" ::: "memory");
        if (t->swizzled)
            swz_ensure_tables((uint32_t)lw, (uint32_t)lh);

        for (int row = row0; row < row1; row++) {
            const uint8_t *srow = src + (size_t)row * w * src_bpp;
            for (int col = col0; col < col1; col++) {
                const uint32_t packed = fetch_pixel(srow, col, src_bpp, format, bgr);
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

    /* A full-rect refresh of a level rebuilds the contiguous chain the same
     * way glTexImage2D's growth logic does, so a re-upload sequence
     * (GL_UpdateTexture, which sub-images an already-uploaded texture's
     * full mip chain rather than calling glTexImage2D again) restores
     * num_levels correctly instead of leaving stale higher levels live
     * under a shrunk count. Partial-rect updates (the lightmap case) never
     * touch num_levels -- lightmaps are TF_NOMIPMAP, so it's already 1 and
     * stays 1 either way. */
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

/* Faithful 1:1 mapping. An earlier probe clamped *_MIPMAP_LINEAR down to
 * *_MIPMAP_NEAREST to halve texture fetches; it was testing the theory that
 * trilinear was offsetting the (padded-pitch) mip win, and it is superseded --
 * mip levels are now tightly packed and swizzled, and mipmapping ships off by
 * default anyway. Reverted rather than left in: it changed the image. */
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

    /* glTexImage2D/glTexSubImage2D write texels directly into RSX-local
     * memory. Order those PPE stores before the FIFO command, then invalidate
     * the global texture cache immediately before a draw can sample them.
     *
     * Keep this separate from t->dirty: that flag also covers sampler-only
     * changes (filter/wrap), which do not stale cached texels. One invalidate
     * covers every enabled dirty texture and both TMUs; bound textures are
     * cleared below, while an updated-but-unbound texture keeps the flag until
     * the first apply pass that can actually sample it. */
    int invalidate_texture_cache = 0;
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

        /* Skip re-bind if nothing changed on this TMU */
        if (!tmu->dirty && !t->dirty) continue;

        rsxLoadTexture(ctx, i, &t->gcm_tex);
        /* maxlod is 4.8 fixed point, NOT an integer level count -- get this wrong and the
         * texture binds but renders nothing. maxlod = (num_levels-1)<<8 so the sampler can reach every uploaded mip. */
        {
            uint16_t maxlod = (uint16_t)(((t->num_levels ? t->num_levels : 1) - 1) << 8);
            uint16_t minlod = 0;
#if PS3GL_MIP_DEBUG
            /* Force every bound texture onto a mip level regardless of
             * distance, so the tinted levels are visible up close too --
             * debug-only, never in a real build. */
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
