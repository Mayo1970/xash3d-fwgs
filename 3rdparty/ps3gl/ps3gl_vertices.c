/* ps3gl_vertices.c -- GL-to-RSX: vertex accumulation and ring buffer. */

#include "ps3gl.h"
#include <stdio.h>
#include <stdlib.h>   // memalign/free for the host ring below
#include <malloc.h>
#include <unistd.h>   // usleep, for ps3gl_wait_idle's fence poll

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
        /* Same refuse-to-wrap policy as the vring. The caller falls back to BSS for this run,
         * correct but slow, so it is counted. */
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
        /* Refuse to wrap onto data the GPU has not fetched. A drop is missing geometry, i.e. a
         * PS3GL_VRING_SIZE bug; end_frame reports it per frame (a once-only warning hid the rate). */
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

/* The real per-frame draw counters (defined in ps3gl_main.c): every glBegin/glEnd draw goes
 * through flush_immediate, unlike ps3gl_frame_draw_count below. */
extern uint32_t ps3gl_frame_flush_count;
extern uint32_t ps3gl_frame_flush_vertex_count;
/* GL_POLYGON draws merged into a batch this frame: tells "batching works" apart from "the
 * state-clean gate never passes". */
extern uint32_t ps3gl_frame_batched_poly_count;

/* One vring copy and bind, then one draw per range (the binding persists). apply_state must be 0
 * for a batch flush: its state is already in the FIFO, and re-applying drew it one state late. */
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

/* Must run before anything else writes to the FIFO or ends the frame: glBegin (non-GL_POLYGON),
 * glClear, glDrawElements and ps3gl_end_frame call it. */
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

/* Must mirror the exact conditions of ps3gl_apply_states/matrices/textures/shader, or batching
 * merges polygons across a real state change. Keep it in lockstep with them. */
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
    /* A non-GL_POLYGON primitive or low batch headroom flushes the batch now; the imm.count
     * reset below would otherwise drop it. */
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
        /* Other primitives draw at once; this glBegin already flushed any pending batch. */
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

    /* State changed: flush the batch under its own state (count cut to its vertices), then draw
     * this polygon alone, which applies the new state. Nothing stays pending. */
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

/* Backend label with a fresh value per call: it lands only once every earlier draw has
 * finished, texture reads included. Callable without ps3gl initialized (ref_soft). */
void ps3gl_wait_idle(gcmContextData *ctx)
{
    static volatile uint32_t *label = NULL;
    static uint32_t value = 0;
    int waited = 0;

    if (!ctx) return;

    /* A deferred GL_POLYGON batch is not in the FIFO yet. */
    if (ps3gl_ptr)
        ps3gl_flush_polygon_batch();

    if (!label) {
        label = (volatile uint32_t *)gcmGetLabelAddress(PS3GL_LABEL_WAIT_IDLE);
        if (!label) {
            rsxFlushBuffer(ctx);
            return;
        }
        *label = 0;
    }

    value++;
    rsxSetWriteBackendLabel(ctx, PS3GL_LABEL_WAIT_IDLE, value);
    rsxFlushBuffer(ctx);

    while (*label != value && waited < 20000) {
        usleep(10);
        waited++;
    }
    if (*label != value)
        ps3gl_log("[ps3gl] WARNING: wait-idle fence timed out (val=%u label=%u)\n",
                  (unsigned)value, (unsigned)*label);
}

void glFinish(void)
{
    ps3gl_wait_idle(ps3gl_get_ctx());
    ps3gl_frame_draw_count = 0;
}
void ps3gl_inc_draw_count(void) { ps3gl_frame_draw_count++; }
void glFlush(void) {}
void glDrawBuffer(GLenum mode) { (void)mode; }

/* Defined in vid_ps3.c; declared by hand since no shared header crosses into ps3gl
 * (same convention as ps3gl_report_flip_wait). */
extern void *PS3_RSX_GetColorBuffer(uint32_t *pitch, uint32_t *width, uint32_t *height);

/* Only for VID_ScreenShot's saveshots. The RSX surface is A,R,G,B bytes with row 0 at the top;
 * GL returns row 0 at the bottom and the caller flips back via IMAGE_FLIP_Y, so flip here too. */
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
