/* ps3gl_vertices.c -- GL-to-RSX: vertex accumulation and ring buffer. */

#include "ps3gl.h"
#include <stdio.h>
#include <stdlib.h>   // memalign/free for the host ring below
#include <malloc.h>

/* Forward declaration from ps3gl_matrices.c */

/* Ring buffer management */

void ps3gl_vring_init(void)
{
    static const int seg_labels[PS3GL_VRING_SEGMENTS] = {
        PS3GL_LABEL_VRING_SEG0, PS3GL_LABEL_VRING_SEG1
    };

    ps3gl.vring.seg_capacity = PS3GL_VRING_SIZE / PS3GL_VRING_SEGMENTS;
    ps3gl.vring.base = (uint8_t *)rsxMemalign(128, PS3GL_VRING_SIZE);
    if (!ps3gl.vring.base) {
        ps3gl_log("[ps3gl] FATAL: failed to allocate vertex ring buffer (%u bytes)\n",
               PS3GL_VRING_SIZE);
        return;
    }
    rsxAddressToOffset(ps3gl.vring.base, &ps3gl.vring.base_off);
    ps3gl.vring.head    = 0;
    ps3gl.vring.cur_seg = 0;

    /* One backend label per segment -- RSX only writes fence_val here once the pipeline
     * fully drains. begin_frame waits on it before reusing that segment's head. */
    for (int i = 0; i < PS3GL_VRING_SEGMENTS; i++) {
        ps3gl.vring.fence_label[i] = (volatile uint32_t *)gcmGetLabelAddress(seg_labels[i]);
        ps3gl.vring.fence_val[i]   = 0;
        if (ps3gl.vring.fence_label[i])
            *ps3gl.vring.fence_label[i] = 0;
    }

    ps3gl_log("[ps3gl] Vertex ring buffer: %u bytes (%d x %u) at %p (offset 0x%08x)\n",
           PS3GL_VRING_SIZE, PS3GL_VRING_SEGMENTS, ps3gl.vring.seg_capacity,
           ps3gl.vring.base, ps3gl.vring.base_off);
}

void ps3gl_hring_init(void)
{
    uint32_t io_offset = 0;
    s32 ret;

    ps3gl.hring.seg_capacity = PS3GL_HRING_SIZE / PS3GL_VRING_SEGMENTS;
    ps3gl.hring.head   = 0;
    ps3gl.hring.mapped = 0;

    /* 1 MB alignment: that is the IO-mapping granularity the RSX uses for main
     * memory, so anything less can fail to map or map neighbouring data. */
    ps3gl.hring.base = (uint8_t *)memalign(1024 * 1024, PS3GL_HRING_SIZE);
    if (!ps3gl.hring.base) {
        ps3gl_log("[ps3gl] host ring: memalign(%u) FAILED -- studio arrays stay in BSS\n",
                  PS3GL_HRING_SIZE);
        return;
    }

    ret = gcmMapMainMemory(ps3gl.hring.base, PS3GL_HRING_SIZE, &io_offset);
    if (ret != 0) {
        ps3gl_log("[ps3gl] host ring: gcmMapMainMemory ret=%d -- studio arrays stay in BSS\n",
                  (int)ret);
        free(ps3gl.hring.base);
        ps3gl.hring.base = NULL;
        return;
    }

    ps3gl.hring.mapped = 1;
    ps3gl_log("[ps3gl] host ring: %u bytes (%d x %u) at %p (io offset 0x%08x)\n",
              PS3GL_HRING_SIZE, PS3GL_VRING_SEGMENTS, ps3gl.hring.seg_capacity,
              ps3gl.hring.base, (unsigned)io_offset);
}

void ps3gl_hring_shutdown(void)
{
    if (ps3gl.hring.base) {
        free(ps3gl.hring.base);
        ps3gl.hring.base = NULL;
        ps3gl.hring.mapped = 0;
    }
}

void *ps3gl_hring_reserve(uint32_t bytes)
{
    if (!ps3gl.hring.mapped || bytes == 0)
        return NULL;

    if (ps3gl.hring.head + bytes > ps3gl.hring.seg_capacity) {
        /* Same refuse-to-wrap policy as the vring: wrapping would stomp data
         * the RSX has not fetched yet. The caller falls back to BSS for this
         * one run, which is correct but slow, so this is counted and reported. */
        ps3gl.hring.frame_drops++;
        ps3gl.hring.total_drops++;
        return NULL;
    }

    return ps3gl.hring.base
         + (uint32_t)ps3gl.vring.cur_seg * ps3gl.hring.seg_capacity
         + ps3gl.hring.head;
}

void ps3gl_hring_commit(uint32_t bytes)
{
    if (!ps3gl.hring.mapped)
        return;

    ps3gl.hring.head = (ps3gl.hring.head + bytes + 127u) & ~127u;
    if (ps3gl.hring.head > ps3gl.hring.peak_head)
        ps3gl.hring.peak_head = ps3gl.hring.head;
}

void ps3gl_vring_shutdown(void)
{
    if (ps3gl.vring.base) {
        rsxFree(ps3gl.vring.base);
        ps3gl.vring.base = NULL;
    }
}

ps3gl_vertex_t *ps3gl_vring_alloc(int count, uint32_t *out_offset)
{
    uint32_t needed = (uint32_t)(count * PS3GL_VERTEX_SIZE);

    if (ps3gl.vring.head + needed > ps3gl.vring.seg_capacity) {
        /* Segment full for this frame -- refuse to wrap rather than stomp data the GPU hasn't
         * fetched yet (submission runs ahead of GPU execution). Dropping the batch means
         * visibly missing geometry, so this must stay loud: it is a sizing bug in
         * PS3GL_VRING_SIZE, never something to live with. ps3gl_end_frame reports the
         * per-frame tally rather than warning once per process, which is how the
         * original once-only warning hid how often it was really happening. */
        ps3gl.vring.frame_drops++;
        ps3gl.vring.total_drops++;
        return NULL;
    }

    uint32_t seg_base = (uint32_t)ps3gl.vring.cur_seg * ps3gl.vring.seg_capacity;
    ps3gl_vertex_t *ptr = (ps3gl_vertex_t *)(ps3gl.vring.base + seg_base + ps3gl.vring.head);
    *out_offset = ps3gl.vring.base_off + seg_base + ps3gl.vring.head;

    /* Round up to 16-byte alignment -- every carve must self-align its own start or
     * attribute sub-offsets (TC0/TC1/COLOR) drift and RSX vertex fetch wedges the whole console. */
    ps3gl.vring.head = (ps3gl.vring.head + needed + 15u) & ~15u;

    if (ps3gl.vring.head > ps3gl.vring.peak_head)
        ps3gl.vring.peak_head = ps3gl.vring.head;

    return ptr;
}

void *ps3gl_vring_alloc_raw(uint32_t bytes, uint32_t *out_offset)
{
    if (bytes == 0 || !ps3gl.vring.base)
        return NULL;

    if (ps3gl.vring.head + bytes > ps3gl.vring.seg_capacity) {
        /* Same policy as ps3gl_vring_alloc: drop rather than wrap onto data
         * the GPU has not fetched yet, and count it so end_frame reports it. */
        ps3gl.vring.frame_drops++;
        ps3gl.vring.total_drops++;
        return NULL;
    }

    uint32_t seg_base = (uint32_t)ps3gl.vring.cur_seg * ps3gl.vring.seg_capacity;
    void *ptr = ps3gl.vring.base + seg_base + ps3gl.vring.head;
    *out_offset = ps3gl.vring.base_off + seg_base + ps3gl.vring.head;

    ps3gl.vring.head = (ps3gl.vring.head + bytes + 15u) & ~15u;

    if (ps3gl.vring.head > ps3gl.vring.peak_head)
        ps3gl.vring.peak_head = ps3gl.vring.head;

    return ptr;
}

/* GL primitive type -> GCM primitive type */

uint32_t ps3gl_gl_to_gcm_prim(GLenum mode)
{
    switch (mode) {
    case GL_POINTS:         return GCM_TYPE_POINTS;
    case GL_LINES:          return GCM_TYPE_LINES;
    case GL_LINE_LOOP:      return GCM_TYPE_LINE_LOOP;
    case GL_LINE_STRIP:     return GCM_TYPE_LINE_STRIP;
    case GL_TRIANGLES:      return GCM_TYPE_TRIANGLES;
    case GL_TRIANGLE_STRIP: return GCM_TYPE_TRIANGLE_STRIP;
    case GL_TRIANGLE_FAN:   return GCM_TYPE_TRIANGLE_FAN;
    case GL_QUADS:          return GCM_TYPE_QUADS;
    case GL_QUAD_STRIP:     return GCM_TYPE_QUAD_STRIP;
    case GL_POLYGON:        return GCM_TYPE_TRIANGLE_FAN;
    default:                return GCM_TYPE_TRIANGLES;
    }
}

/* Submit accumulated vertices to RSX */

/* Defined in ps3gl_main.c -- the real per-frame draw-call counter. Unlike
 * ps3gl_frame_draw_count below (only ever fed from glDrawElements, which
 * xash never calls), flush_immediate is the actual draw path every real
 * glBegin/glEnd triangle goes through. */
extern uint32_t ps3gl_frame_flush_count;
extern uint32_t ps3gl_frame_flush_vertex_count;
/* Per-frame count of GL_POLYGON draws merged into a batch instead of
 * flushed individually -- see ps3gl_main.c's telemetry window. The only way
 * to tell "batching is working" from "the state-clean gate never passes"
 * without this is to stare at flush-count deltas and guess. */
extern uint32_t ps3gl_frame_batched_poly_count;

/* One vring_alloc + memcpy + attribute-bind, followed by `nranges` draw
 * calls against that single binding. Multiple rsxDrawVertexArray calls
 * against one unchanged attribute binding are legal -- the binding is
 * persistent GCM register state, DRAW_ARRAYS is a separate method -- so
 * this is what lets ps3gl_flush_polygon_batch() submit N polygons' worth
 * of still-intact triangle fans without any CPU-side fan-to-triangle
 * expansion. nranges==1 with first[0]==0 reproduces flush_immediate's
 * original single-draw FIFO output exactly.
 *
 * apply_state must be 0 for a deferred GL_POLYGON batch flush: those
 * vertices were admitted only while batch_state_is_clean() held, i.e. every
 * bit of state they need was already emitted to the FIFO by an earlier
 * submit_ranges call -- the FIFO is append-only, so re-running apply_*
 * here would emit whatever's current NOW (the next texture chain's bound
 * texture, R_BlendLightmaps' blend mode, ...) immediately before drawing
 * OLD vertices that were recorded under different state. That was the
 * actual bug behind "textures and polygons glitched": every batch drew
 * one state-change late, every single chain, every frame. */
static void submit_ranges(int total_verts, const int *first, const int *cnt,
                           int nranges, uint32_t gcm_prim, int apply_state)
{
    if (total_verts <= 0 || nranges <= 0) return;

    if (apply_state) {
        ps3gl_apply_states();
        ps3gl_apply_matrices();
        ps3gl_apply_textures();
        ps3gl_apply_shader();
    }

    /* Copy vertices into ring buffer */
    uint32_t vb_offset;
    ps3gl_vertex_t *dst = ps3gl_vring_alloc(total_verts, &vb_offset);
    if (!dst) return;
    memcpy(dst, ps3gl.imm.buf, (size_t)total_verts * PS3GL_VERTEX_SIZE);

    gcmContextData *ctx = ps3gl_get_ctx();
    if (!ctx) return;

    /* Bind vertex attributes from ring buffer */
    /* Position: attr 0, 4 floats */
    rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_POS, 0,
                             vb_offset + PS3GL_VATTR_POS_OFF,
                             PS3GL_VERTEX_SIZE, 4,
                             GCM_VERTEX_DATA_TYPE_F32,
                             GCM_LOCATION_RSX);
    /* Texcoord 0: attr 8, 2 floats */
    rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_TEX0, 0,
                             vb_offset + PS3GL_VATTR_TC0_OFF,
                             PS3GL_VERTEX_SIZE, 2,
                             GCM_VERTEX_DATA_TYPE_F32,
                             GCM_LOCATION_RSX);
    /* Texcoord 1: attr 9, 2 floats */
    rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_TEX1, 0,
                             vb_offset + PS3GL_VATTR_TC1_OFF,
                             PS3GL_VERTEX_SIZE, 2,
                             GCM_VERTEX_DATA_TYPE_F32,
                             GCM_LOCATION_RSX);
    /* Color: attr 3, 4 unsigned bytes */
    rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_COLOR0, 0,
                             vb_offset + PS3GL_VATTR_COLOR_OFF,
                             PS3GL_VERTEX_SIZE, 4,
                             GCM_VERTEX_DATA_TYPE_U8,
                             GCM_LOCATION_RSX);

    /* Draw -- one call per range, all against the single binding above */
    for (int i = 0; i < nranges; i++)
        rsxDrawVertexArray(ctx, gcm_prim, first[i], cnt[i]);
}

static void flush_immediate(void)
{
    int n = ps3gl.imm.count;
    if (n == 0) return;
    ps3gl_frame_flush_count++;
    ps3gl_frame_flush_vertex_count += (uint32_t)n;

    int first0 = 0;
    submit_ranges(n, &first0, &n, 1, ps3gl_gl_to_gcm_prim(ps3gl.imm.prim), 1);
}

/* Force-submits a pending deferred GL_POLYGON batch, if any. Must run
 * before anything else writes to the GCM FIFO or ends the frame -- see
 * glBegin (non-GL_POLYGON), glClear, glDrawElements, and ps3gl_end_frame's
 * hooks. */
void ps3gl_flush_polygon_batch(void)
{
    if (!ps3gl.imm.batch_active) return;

    ps3gl_frame_flush_count++;
    ps3gl_frame_flush_vertex_count += (uint32_t)ps3gl.imm.count;
    submit_ranges(ps3gl.imm.count, ps3gl.imm.batch_first, ps3gl.imm.batch_count,
                  ps3gl.imm.batch_nranges, GCM_TYPE_TRIANGLE_FAN, /*apply_state=*/0);

    ps3gl.imm.batch_active  = 0;
    ps3gl.imm.batch_nranges = 0;
    ps3gl.imm.count         = 0;
}

/* Is it safe to merge the polygon that just ended into the pending batch?
 * Must mirror exactly what ps3gl_apply_states/apply_matrices/apply_textures/
 * apply_shader would each do if called right now -- not a proxy, the actual
 * conditions, since apply_shader in particular has no dirty flag of its own
 * (ps3gl_shaders.c) and apply_textures' per-TMU condition (ps3gl_textures.c)
 * is not simply "tmu->dirty". If this drifts from those functions, batching
 * either never engages (harmless, just measure-and-fix) or merges polygons
 * across a real state change (a visible bug) -- keep it in lockstep with them. */
static int batch_state_is_clean(void)
{
    if (ps3gl.dirty != 0) return 0;
    if (ps3gl.mv.dirty || ps3gl.proj.dirty) return 0;

    for (int i = 0; i < PS3GL_MAX_TMUS; i++) {
        ps3gl_tmu_t *tmu = &ps3gl.tmu[i];
        ps3gl_texture_t *t = tmu->bound;
        if (!tmu->enabled || !t || !t->data) {
            if (tmu->dirty) return 0;
        } else {
            if (tmu->dirty || t->dirty) return 0;
        }
    }

    int key = ps3gl_shader_key();
    if (key < 0 || key >= PS3GL_TENV_COUNT) key = PS3GL_TENV_DISABLED;
    if (key != ps3gl.active_shader) return 0;
    if (ps3gl.shaders[key].vp != ps3gl.active_vp) return 0;
    if (ps3gl_get_mvp_generation() != ps3gl.mvp_uploaded_gen) return 0;

    return 1;
}

/* GL immediate mode functions */

void glBegin(GLenum mode)
{
    /* A pending batch may only ever hold polygons under state that's
     * already been applied to the FIFO (see the invariant comment on
     * ps3gl_flush_polygon_batch's declaration in ps3gl.h) -- so any
     * non-GL_POLYGON primitive, or running low on batch headroom, forces
     * it out now rather than risk it getting silently discarded by the
     * unconditional imm.count reset below. */
    if (ps3gl.imm.batch_active &&
        (mode != GL_POLYGON ||
         ps3gl.imm.batch_nranges >= PS3GL_MAX_BATCH_RANGES ||
         ps3gl.imm.count > PS3GL_MAX_VERTS - PS3GL_BATCH_VERT_MARGIN))
        ps3gl_flush_polygon_batch();

    if (!ps3gl.imm.batch_active)
        ps3gl.imm.count = 0;
    ps3gl.imm.vert_start = ps3gl.imm.count;
    ps3gl.imm.prim = mode;
}

void glEnd(void)
{
    int n = ps3gl.imm.count - ps3gl.imm.vert_start;

    if (ps3gl.imm.prim != GL_POLYGON) {
        /* Every other primitive: unchanged from before batching existed.
         * Any pending batch was already force-flushed by this glBegin
         * (above), so there is nothing to merge or protect here. */
        flush_immediate();
        ps3gl.imm.count = 0;
        return;
    }
    if (n <= 0) return;

    if (batch_state_is_clean()) {
        ps3gl.imm.batch_first[ps3gl.imm.batch_nranges] = ps3gl.imm.vert_start;
        ps3gl.imm.batch_count[ps3gl.imm.batch_nranges] = n;
        ps3gl.imm.batch_nranges++;
        ps3gl.imm.batch_active = 1;
        ps3gl_frame_batched_poly_count++;
        /* Deliberately does NOT reset imm.count -- these vertices stay in
         * buf[] so the next GL_POLYGON glBegin can keep appending after them. */
        return;
    }

    /* State changed since the pending batch was applied. Flush it under
     * its own already-applied state first (truncate count to just the
     * batch's own vertices so submit_ranges doesn't also copy this new,
     * not-yet-merged polygon), then submit this polygon alone via the
     * normal single-draw path, which applies the new state and clears the
     * dirty flags. Leaves nothing pending, satisfying the invariant. */
    ps3gl.imm.count = ps3gl.imm.vert_start;
    ps3gl_flush_polygon_batch();
    memmove(ps3gl.imm.buf, &ps3gl.imm.buf[ps3gl.imm.vert_start],
            (size_t)n * PS3GL_VERTEX_SIZE);
    ps3gl.imm.count = n;
    flush_immediate();
    ps3gl.imm.count = 0;
}

void glVertex2f(GLfloat x, GLfloat y)
{
    if (ps3gl.imm.count >= PS3GL_MAX_VERTS) return;
    ps3gl_vertex_t *v = &ps3gl.imm.buf[ps3gl.imm.count++];
    v->x  = x;  v->y  = y;  v->z = 0.0f; v->w = 1.0f;
    v->u0 = ps3gl.imm.u0; v->v0 = ps3gl.imm.v0;
    v->u1 = ps3gl.imm.u1; v->v1 = ps3gl.imm.v1;
    v->color = ps3gl.imm.color;
}

void glVertex3f(GLfloat x, GLfloat y, GLfloat z)
{
    if (ps3gl.imm.count >= PS3GL_MAX_VERTS) return;
    ps3gl_vertex_t *v = &ps3gl.imm.buf[ps3gl.imm.count++];
    v->x  = x;  v->y  = y;  v->z = z; v->w = 1.0f;
    v->u0 = ps3gl.imm.u0; v->v0 = ps3gl.imm.v0;
    v->u1 = ps3gl.imm.u1; v->v1 = ps3gl.imm.v1;
    v->color = ps3gl.imm.color;
}

void glVertex3fv(const GLfloat *vv)
{
    glVertex3f(vv[0], vv[1], vv[2]);
}

void glTexCoord2f(GLfloat s, GLfloat t)
{
    ps3gl.imm.u0 = s;
    ps3gl.imm.v0 = t;
}

void glTexCoord2fv(const GLfloat *v)
{
    ps3gl.imm.u0 = v[0];
    ps3gl.imm.v0 = v[1];
}

void glMultiTexCoord2fARB(GLenum target, GLfloat s, GLfloat t)
{
    int tmu = (int)(target - GL_TEXTURE0_ARB);
    if (tmu == 0) {
        ps3gl.imm.u0 = s;
        ps3gl.imm.v0 = t;
    } else if (tmu == 1) {
        ps3gl.imm.u1 = s;
        ps3gl.imm.v1 = t;
    }
}

/* Frame draw count tracking */
static int ps3gl_frame_draw_count = 0;

void glFinish(void) { ps3gl_frame_draw_count = 0; }
void ps3gl_inc_draw_count(void) { ps3gl_frame_draw_count++; }
void glFlush(void) {}
void glDrawBuffer(GLenum mode) { (void)mode; }

/* Declared in engine/platform/ps3/vid_ps3.c; not in a shared header because
 * that file can't be included across the ref_gl/ps3gl module boundary (same
 * convention as ps3gl_report_flip_wait etc, see ps3gl.h). */
extern void *PS3_RSX_GetColorBuffer(uint32_t *pitch, uint32_t *width, uint32_t *height);

/* Only caller is VID_ScreenShot's savegame/levelshot capture (ref/gl/gl_backend.c),
 * always with x=0,y=0,GL_RGBA,GL_UNSIGNED_BYTE -- this was previously a total
 * no-op, so every saveshot .bmp was written from an uninitialized buffer
 * (garbage in the load-game preview). The RSX color surface is A8R8G8B8,
 * stored on this big-endian target as bytes A,R,G,B per word (see
 * ps3gl_textures.c's fetch_pixel comment) with row 0 = top of screen. Real GL
 * glReadPixels returns row 0 = bottom of screen; the caller already flips
 * back via IMAGE_FLIP_Y, so this must flip too or the two cancel out wrong. */
void glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h,
                      GLenum format, GLenum type, void *pixels)
{
    uint32_t pitch, sw, sh;
    const uint8_t *base;
    uint8_t *dst = (uint8_t *)pixels;

    if (format != GL_RGBA || type != GL_UNSIGNED_BYTE)
        return;

    base = (const uint8_t *)PS3_RSX_GetColorBuffer(&pitch, &sw, &sh);
    if (!base)
        return;

    for (GLint row = 0; row < h; row++) {
        GLint sy = (GLint)sh - 1 - (y + row);
        uint8_t *drow = dst + (size_t)row * w * 4;

        if (sy < 0 || sy >= (GLint)sh) {
            memset(drow, 0, (size_t)w * 4);
            continue;
        }

        const uint32_t *srow = (const uint32_t *)(base + (size_t)sy * pitch);

        for (GLint col = 0; col < w; col++) {
            GLint sx = x + col;

            if (sx < 0 || sx >= (GLint)sw) {
                drow[col * 4 + 0] = drow[col * 4 + 1] = drow[col * 4 + 2] = drow[col * 4 + 3] = 0;
                continue;
            }

            uint32_t argb = srow[sx];
            drow[col * 4 + 0] = (uint8_t)(argb >> 16); /* R */
            drow[col * 4 + 1] = (uint8_t)(argb >> 8);  /* G */
            drow[col * 4 + 2] = (uint8_t)argb;         /* B */
            drow[col * 4 + 3] = (uint8_t)(argb >> 24); /* A */
        }
    }
}
