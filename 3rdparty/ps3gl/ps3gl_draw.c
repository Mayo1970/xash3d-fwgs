/* GL-to-RSX layer: vertex array / glDrawElements path */

#include "ps3gl.h"
#include <stdio.h>

/* Defined in ps3gl_main.c -- see the fallback branch in glDrawElements. */
extern uint32_t ps3gl_frame_direct_binds;
extern uint32_t ps3gl_frame_fallback_binds;
extern uint32_t ps3gl_frame_fallback_verts;


/* GL vertex array functions */

/* Each *Pointer call captures the buffer bound at that moment, per GL's own
 * rule: with a buffer bound, `ptr` is a byte offset INTO that buffer, not an
 * address -- and it is legal (ref_gl does it) for one draw to mix a
 * buffer-backed array with a client-memory one. */
void glVertexPointer(GLint size, GLenum type, GLsizei stride,
                         const void *ptr)
{
    ps3gl.va_vertex.ptr    = ptr;
    ps3gl.va_vertex.size   = size;
    ps3gl.va_vertex.type   = type;
    ps3gl.va_vertex.stride = stride;
    ps3gl.va_vertex.buffer = ps3gl.array_buffer;
}

void glTexCoordPointer(GLint size, GLenum type, GLsizei stride,
                           const void *ptr)
{
    int tmu = ps3gl.client_active_tmu;
    ps3gl.va_texcoord[tmu].ptr    = ptr;
    ps3gl.va_texcoord[tmu].size   = size;
    ps3gl.va_texcoord[tmu].type   = type;
    ps3gl.va_texcoord[tmu].stride = stride;
    ps3gl.va_texcoord[tmu].buffer = ps3gl.array_buffer;
}

void glColorPointer(GLint size, GLenum type, GLsizei stride,
                        const void *ptr)
{
    ps3gl.va_color.ptr    = ptr;
    ps3gl.va_color.size   = size;
    ps3gl.va_color.type   = type;
    ps3gl.va_color.stride = stride;
    ps3gl.va_color.buffer = ps3gl.array_buffer;
}

void glEnableClientState(GLenum cap)  { (void)cap; }

void glDisableClientState(GLenum cap)
{
    switch (cap) {
    case GL_VERTEX_ARRAY:
        ps3gl.va_vertex.ptr = NULL; ps3gl.va_vertex.buffer = 0; break;
    case GL_COLOR_ARRAY:
        ps3gl.va_color.ptr  = NULL; ps3gl.va_color.buffer  = 0; break;
    case GL_TEXTURE_COORD_ARRAY:
        ps3gl.va_texcoord[ps3gl.client_active_tmu].ptr    = NULL;
        ps3gl.va_texcoord[ps3gl.client_active_tmu].buffer = 0;
        break;
    default: break;
    }
}

/* An array with a buffer bound is active even at byte offset 0 -- which is
 * exactly ref_gl's world-geometry case (offsetof(vbovertex_t,pos) == 0), so
 * the plain `ptr != NULL` test alone would silently draw nothing. */
static int ps3gl_array_active(const ps3gl_array_ptr_t *a)
{
    return a->buffer != 0 || a->ptr != NULL;
}

void glLockArraysEXT(GLint first, GLsizei count)
{
    ps3gl.va_locked     = 1;
    ps3gl.va_lock_first = first;
    ps3gl.va_lock_count = count;
}

void glUnlockArraysEXT(void)
{
    ps3gl.va_locked = 0;
}

void glArrayElement(GLint i) { (void)i; }

void glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    (void)mode; (void)first; (void)count;
}

/* ---------------------------------------------------------------- *
 * Buffer-object draw path (ref_gl's r_vbo world geometry)
 *
 * Split from the legacy path below rather than folded into it, on purpose:
 * the legacy path's fallback CPU-interleaves every vertex by dereferencing
 * va_*.ptr, and with a buffer bound that "pointer" is a byte offset into
 * GDDR3 -- dereferencing it would read a wild address, and even a corrected
 * version would be a PPE read of video memory (~16 MB/s, the platform's
 * single worst access pattern). So when any array is buffer-backed, nothing
 * here may touch vertex data on the CPU: buffer-backed arrays bind straight
 * from RSX memory, and a client-memory array mixed into the same draw
 * (ref_gl does this for dlight texcoords) is staged into the vertex ring,
 * never merged with the VBO.
 * ---------------------------------------------------------------- */

enum { PS3GL_ATTR_POS = 0, PS3GL_ATTR_TC0, PS3GL_ATTR_TC1, PS3GL_ATTR_COL, PS3GL_ATTR_COUNT };

static int ps3gl_index_range(const void *indices, GLsizei count, int idx32,
                             uint32_t *out_min, uint32_t *out_max)
{
    uint32_t lo = 0xFFFFFFFFu, hi = 0;

    if (!indices) return 0;

    if (idx32) {
        const uint32_t *idx = (const uint32_t *)indices;
        for (GLsizei i = 0; i < count; i++) {
            if (idx[i] < lo) lo = idx[i];
            if (idx[i] > hi) hi = idx[i];
        }
    } else {
        const uint16_t *idx = (const uint16_t *)indices;
        for (GLsizei i = 0; i < count; i++) {
            if (idx[i] < lo) lo = idx[i];
            if (idx[i] > hi) hi = idx[i];
        }
    }

    *out_min = lo;
    *out_max = hi;
    return lo <= hi;
}

/* A stride-0 attribute makes every vertex fetch the same element -- the way
 * to feed a constant down an attribute the vertex program always reads
 * (q3_vp.vcg consumes COLOR0 and both texcoords unconditionally) without
 * materializing one copy per vertex. Leaving the attribute unbound instead
 * would let the previous draw's binding stand, which is stale memory at a
 * stale stride. */
static int ps3gl_bind_constant_attrib(gcmContextData *ctx, uint8_t attr,
                                      const void *value, uint32_t bytes,
                                      uint8_t elems, uint8_t dtype)
{
    uint32_t offset;
    void *dst = ps3gl_vring_alloc_raw(bytes, &offset);
    if (!dst) return 0;

    memcpy(dst, value, bytes);
    rsxBindVertexArrayAttrib(ctx, attr, 0, offset, 0, elems, dtype, GCM_LOCATION_RSX);
    return 1;
}

static void ps3gl_draw_elements_vbo(gcmContextData *ctx, GLenum mode, GLsizei count,
                                    GLenum type, const void *indices)
{
    static const uint8_t gcm_attr[PS3GL_ATTR_COUNT] = {
        GCM_VERTEX_ATTRIB_POS, GCM_VERTEX_ATTRIB_TEX0,
        GCM_VERTEX_ATTRIB_TEX1, GCM_VERTEX_ATTRIB_COLOR0
    };
    const ps3gl_array_ptr_t *arrays[PS3GL_ATTR_COUNT] = {
        &ps3gl.va_vertex, &ps3gl.va_texcoord[0],
        &ps3gl.va_texcoord[1], &ps3gl.va_color
    };
    const int idx32 = (type == GL_UNSIGNED_INT);

    /* Indices address the buffer absolutely. That is already what the RSX
     * wants for a buffer-backed attribute, so the common all-VBO draw needs
     * no index scan at all -- the scan only exists to size the staging copy
     * of a client-memory array, and to rebase indices onto it. */
    int need_scan = idx32;
    for (int i = 0; i < PS3GL_ATTR_COUNT; i++)
        if (ps3gl_array_active(arrays[i]) && arrays[i]->buffer == 0)
            need_scan = 1;

    uint32_t idx_min = 0, idx_max = 0;
    if (need_scan) {
        if (!ps3gl_index_range(indices, count, idx32, &idx_min, &idx_max))
            return;
        if (idx_max - idx_min > 0xFFFFu) {
            static int warned = 0;
            if (!warned) {
                ps3gl_log("[ps3gl] WARNING: VBO draw index range %u..%u exceeds 16 bits "
                          "-- skipping draw\n", (unsigned)idx_min, (unsigned)idx_max);
                warned = 1;
            }
            return;
        }
    }
    const uint32_t rebase = idx_min;

    for (int i = 0; i < PS3GL_ATTR_COUNT; i++) {
        const ps3gl_array_ptr_t *a = arrays[i];
        const int is_color = (i == PS3GL_ATTR_COL);
        uint8_t elems, dtype;
        uint32_t esz;

        if (is_color) {
            elems = (uint8_t)(a->size >= 4 ? 4 : 3);
            dtype = GCM_VERTEX_DATA_TYPE_U8;
            esz   = elems;
        } else {
            int size = a->size;
            if (i == PS3GL_ATTR_POS) size = size >= 4 ? 4 : (size >= 3 ? 3 : 2);
            else                     size = size >= 2 ? 2 : 1;
            elems = (uint8_t)size;
            dtype = GCM_VERTEX_DATA_TYPE_F32;
            esz   = (uint32_t)elems * 4u;
        }

        if (!ps3gl_array_active(a)) {
            /* q3_vp.vcg reads all three of COLOR0/TEXCOORD0/TEXCOORD1
             * unconditionally; feed the inactive ones a constant. */
            if (is_color) {
                if (!ps3gl_bind_constant_attrib(ctx, gcm_attr[i], &ps3gl.imm.color,
                                                4, 4, GCM_VERTEX_DATA_TYPE_U8))
                    return;
            } else {
                static const float zero_tc[2] = { 0.0f, 0.0f };
                if (!ps3gl_bind_constant_attrib(ctx, gcm_attr[i], zero_tc,
                                                sizeof(zero_tc), 2, GCM_VERTEX_DATA_TYPE_F32))
                    return;
            }
            continue;
        }

        uint32_t src_stride = a->stride ? (uint32_t)a->stride : esz;

        if (a->buffer) {
            ps3gl_buffer_t *b = ps3gl_buffer_lookup(a->buffer);
            uint32_t base = (uint32_t)(uintptr_t)a->ptr;

            /* rsxBindVertexArrayAttrib's stride is a u8 -- a wider one cannot
             * be expressed at all, and silently binding a wrong stride would
             * wedge RSX vertex fetch. ref_gl's vbovertex_t is 28 bytes. */
            if (!b || !b->data[b->current] || src_stride > 255 || base >= b->size) {
                static int warned = 0;
                if (!warned) {
                    ps3gl_log("[ps3gl] WARNING: unbindable VBO attrib %d "
                              "(buffer %u, base %u, stride %u) -- skipping draw\n",
                              i, (unsigned)a->buffer, (unsigned)base, (unsigned)src_stride);
                    warned = 1;
                }
                return;
            }

            rsxBindVertexArrayAttrib(ctx, gcm_attr[i], 0,
                                     b->offset[b->current] + base + rebase * src_stride,
                                     (uint8_t)src_stride, elems, dtype, GCM_LOCATION_RSX);
        } else {
            /* Client memory: stage the referenced range into the ring,
             * repacked tight so the RSX stride always fits in a u8. */
            uint32_t n = idx_max - idx_min + 1;
            uint32_t offset;
            uint8_t *dst = (uint8_t *)ps3gl_vring_alloc_raw(n * esz, &offset);
            if (!dst) return;

            const uint8_t *src = (const uint8_t *)a->ptr + (size_t)idx_min * src_stride;
            if (src_stride == esz) {
                memcpy(dst, src, (size_t)n * esz);
            } else {
                for (uint32_t e = 0; e < n; e++)
                    memcpy(dst + e * esz, src + (size_t)e * src_stride, esz);
            }

            rsxBindVertexArrayAttrib(ctx, gcm_attr[i], 0, offset,
                                     (uint8_t)esz, elems, dtype, GCM_LOCATION_RSX);
        }
    }

    /* Indices go to RSX memory rather than inline into the FIFO: a world
     * texture chain can be tens of thousands of indices, and inlining those
     * would push hundreds of KB through the command buffer every frame. */
    uint32_t idx_offset;
    uint16_t *dst = (uint16_t *)ps3gl_vring_alloc_raw((uint32_t)count * 2u, &idx_offset);
    if (!dst) return;

    if (idx32) {
        const uint32_t *src = (const uint32_t *)indices;
        for (GLsizei i = 0; i < count; i++)
            dst[i] = (uint16_t)(src[i] - rebase);
    } else if (rebase == 0) {
        memcpy(dst, indices, (size_t)count * 2u);
    } else {
        const uint16_t *src = (const uint16_t *)indices;
        for (GLsizei i = 0; i < count; i++)
            dst[i] = (uint16_t)((uint32_t)src[i] - rebase);
    }

    /* Order the attribute/index stores ahead of the draw the RSX will read
     * them for -- same barrier the buffer writes in ps3gl_glapi.c use. */
    __asm__ volatile("sync" ::: "memory");

    rsxDrawIndexArray(ctx, ps3gl_gl_to_gcm_prim(mode), idx_offset, (uint32_t)count,
                      GCM_INDEX_TYPE_16B, GCM_LOCATION_RSX);
}

void glDrawElements(GLenum mode, GLsizei count, GLenum type,
                        const void *indices)
{
    if (count <= 0) return;
    if (!ps3gl_array_active(&ps3gl.va_vertex)) return;
    /* Real FIFO writes below -- force out any pending deferred GL_POLYGON
     * batch (ps3gl_vertices.c) first so it can't get reordered past this draw. */
    ps3gl_flush_polygon_batch();
    ps3gl_inc_draw_count();

    /* Apply all deferred state */
    ps3gl_apply_states();
    ps3gl_apply_matrices();
    ps3gl_apply_textures();
    ps3gl_apply_shader();

    gcmContextData *ctx = ps3gl_get_ctx();
    if (!ctx) return;

    /* Any buffer-backed array takes the VBO path wholesale. Everything below
     * this point is the original client-array path, unchanged: with gl_vbo 0
     * no buffer is ever bound, so nothing about the immediate/studio-model
     * behaviour moves. */
    if (ps3gl.va_vertex.buffer || ps3gl.va_color.buffer ||
        ps3gl.va_texcoord[0].buffer || ps3gl.va_texcoord[1].buffer) {
        ps3gl_draw_elements_vbo(ctx, mode, count, type, indices);
        return;
    }

    /* Determine vertex count from glLockArraysEXT or scan indices.
     * locked_base is the source-array offset a lock implies: indices are
     * still absolute (e.g. a studio model's Nth mesh references vertex 500
     * of the whole submodel), but a locked range only needs *this* mesh's
     * own slice copied, not everything from vertex 0 up to its high-water
     * mark -- see the fallback-copy and index-rebase below. */
    int num_verts;
    int locked = (ps3gl.va_locked && ps3gl.va_lock_count > 0);
    int locked_base = locked ? ps3gl.va_lock_first : 0;
    if (locked) {
        num_verts = ps3gl.va_lock_count;
    } else {
        /* Fallback: scan indices for max (shouldn't happen in practice) */
        int max_idx = 0;
        const uint16_t *idx = (const uint16_t *)indices;
        for (int i = 0; i < count; i++) {
            if ((int)idx[i] > max_idx) max_idx = (int)idx[i];
        }
        num_verts = max_idx + 1;
    }

    const uint8_t *vp  = (const uint8_t *)ps3gl.va_vertex.ptr;
    int vs = ps3gl.va_vertex.stride;
    if (vs == 0) vs = ps3gl.va_vertex.size * sizeof(float);

    const uint8_t *tp0 = (const uint8_t *)ps3gl.va_texcoord[0].ptr;
    int ts0 = ps3gl.va_texcoord[0].stride;
    if (ts0 == 0) ts0 = 2 * sizeof(float);

    const uint8_t *tp1 = (const uint8_t *)ps3gl.va_texcoord[1].ptr;
    int ts1 = ps3gl.va_texcoord[1].stride;
    if (ts1 == 0) ts1 = 2 * sizeof(float);

    const uint8_t *cp  = (const uint8_t *)ps3gl.va_color.ptr;
    int cs = ps3gl.va_color.stride;
    if (cs == 0) cs = 4;

    const int has_z = (ps3gl.va_vertex.size >= 3);

    /* Direct-bind XDR arrays to RSX, or copy to vring on failure. Hoisted
     * out of the block below (unlike upstream) so the index-submission code
     * after it can tell which case ran: direct-bind reads straight from the
     * original array at absolute offsets (indices stay absolute, no rebase
     * needed), but the vring-copy fallback below only copies this call's
     * own `[locked_base, locked_base+num_verts)` slice into a freshly and
     * locally 0-indexed block -- indices submitted against it must be
     * rebased by locked_base. */
    int direct;
    {
        uint32_t off_pos  = 0;
        uint32_t off_tc0  = 0;
        uint32_t off_tc1  = 0;
        uint32_t off_col  = 0;

        direct =
            (rsxAddressToOffset((void *)vp,  &off_pos) == 0) &&
            (!tp0 || rsxAddressToOffset((void *)tp0, &off_tc0) == 0) &&
            (!tp1 || rsxAddressToOffset((void *)tp1, &off_tc1) == 0) &&
            (!cp  || rsxAddressToOffset((void *)cp,  &off_col) == 0);

        if (direct) {
            ps3gl_frame_direct_binds++;
            /* Bind Q3's arrays straight from main memory — zero PPE copy. */
            /* The component count must be the array's real one: a size-3
             * position array bound as 4 would fetch the next attribute's
             * first float as w. (Fewer than 4 components is fine -- the RSX
             * fills the rest with (0,0,0,1), which is what q3_vp.vcg's
             * mul(mvp, position) needs.) */
            rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_POS, 0,
                                     off_pos, vs,
                                     (uint8_t)(ps3gl.va_vertex.size >= 4 ? 4 :
                                               ps3gl.va_vertex.size >= 3 ? 3 : 2),
                                     GCM_VERTEX_DATA_TYPE_F32,
                                     GCM_LOCATION_CELL);
            if (tp0)
                rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_TEX0, 0,
                                         off_tc0, ts0, 2,
                                         GCM_VERTEX_DATA_TYPE_F32,
                                         GCM_LOCATION_CELL);
            if (tp1)
                rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_TEX1, 0,
                                         off_tc1, ts1, 2,
                                         GCM_VERTEX_DATA_TYPE_F32,
                                         GCM_LOCATION_CELL);
            if (cp)
                rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_COLOR0, 0,
                                         off_col, cs, 4,
                                         GCM_VERTEX_DATA_TYPE_U8,
                                         GCM_LOCATION_CELL);
        } else {
            /* Fallback: interleave-copy into RSX ring buffer. This is the
             * expensive branch -- a scalar per-vertex gather from up to four
             * separate client arrays. The counters distinguish "the direct path
             * exists but never triggers" from "it triggers and is still slow",
             * which nothing else could tell apart. */
            ps3gl_frame_fallback_binds++;
            ps3gl_frame_fallback_verts += (uint32_t)num_verts;
            uint32_t vb_offset;
            ps3gl_vertex_t *verts = ps3gl_vring_alloc(num_verts, &vb_offset);
            if (!verts) return;

            const uint32_t imm_c = ps3gl.imm.color;

            if (tp0 && tp1 && cp) {
                for (int i = 0; i < num_verts; i++) {
                    int j = locked_base + i;
                    const float   *pos = (const float *)(vp  + j * vs);
                    const float   *tc0 = (const float *)(tp0 + j * ts0);
                    const float   *tc1 = (const float *)(tp1 + j * ts1);
                    const uint8_t *c   = cp + j * cs;
                    ps3gl_vertex_t *v  = &verts[i];
                    v->x = pos[0]; v->y = pos[1]; v->z = has_z ? pos[2] : 0.0f; v->w = 1.0f;
                    v->u0 = tc0[0]; v->v0 = tc0[1];
                    v->u1 = tc1[0]; v->v1 = tc1[1];
                    v->color = ps3gl_pack_color_ub(c[0], c[1], c[2], c[3]);
                }
            } else if (tp0 && cp) {
                for (int i = 0; i < num_verts; i++) {
                    int j = locked_base + i;
                    const float   *pos = (const float *)(vp  + j * vs);
                    const float   *tc0 = (const float *)(tp0 + j * ts0);
                    const uint8_t *c   = cp + j * cs;
                    ps3gl_vertex_t *v  = &verts[i];
                    v->x = pos[0]; v->y = pos[1]; v->z = has_z ? pos[2] : 0.0f; v->w = 1.0f;
                    v->u0 = tc0[0]; v->v0 = tc0[1];
                    v->u1 = 0.0f;  v->v1 = 0.0f;
                    v->color = ps3gl_pack_color_ub(c[0], c[1], c[2], c[3]);
                }
            } else {
                for (int i = 0; i < num_verts; i++) {
                    int j = locked_base + i;
                    const float   *pos = (const float *)(vp + j * vs);
                    ps3gl_vertex_t *v  = &verts[i];
                    v->x = pos[0]; v->y = pos[1]; v->z = has_z ? pos[2] : 0.0f; v->w = 1.0f;
                    if (tp0) { const float *tc = (const float *)(tp0 + j * ts0); v->u0 = tc[0]; v->v0 = tc[1]; }
                    else     { v->u0 = 0.0f; v->v0 = 0.0f; }
                    if (tp1) { const float *tc = (const float *)(tp1 + j * ts1); v->u1 = tc[0]; v->v1 = tc[1]; }
                    else     { v->u1 = 0.0f; v->v1 = 0.0f; }
                    if (cp)  { const uint8_t *c = cp + j * cs; v->color = ps3gl_pack_color_ub(c[0], c[1], c[2], c[3]); }
                    else     { v->color = imm_c; }
                }
            }

            rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_POS, 0,
                                     vb_offset + PS3GL_VATTR_POS_OFF,
                                     PS3GL_VERTEX_SIZE, 4,
                                     GCM_VERTEX_DATA_TYPE_F32,
                                     GCM_LOCATION_RSX);
            rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_TEX0, 0,
                                     vb_offset + PS3GL_VATTR_TC0_OFF,
                                     PS3GL_VERTEX_SIZE, 2,
                                     GCM_VERTEX_DATA_TYPE_F32,
                                     GCM_LOCATION_RSX);
            rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_TEX1, 0,
                                     vb_offset + PS3GL_VATTR_TC1_OFF,
                                     PS3GL_VERTEX_SIZE, 2,
                                     GCM_VERTEX_DATA_TYPE_F32,
                                     GCM_LOCATION_RSX);
            rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_COLOR0, 0,
                                     vb_offset + PS3GL_VATTR_COLOR_OFF,
                                     PS3GL_VERTEX_SIZE, 4,
                                     GCM_VERTEX_DATA_TYPE_U8,
                                     GCM_LOCATION_RSX);
        }
    }

    /* Submit indexed draw with inline indices. */
    uint32_t gcm_prim = GCM_TYPE_TRIANGLES; /* Q3 always uses GL_TRIANGLES here */
    (void)mode; /* mode is always GL_TRIANGLES in practice */

    /* RSX wants 16-bit indices; static 65536 buffer avoids a stack alloc. Q3's own
     * tessellator never gets this big (SHADER_MAX_VERTEXES caps it well below), so this is a safety net -- if it ever fires, warn loudly, don't silently eat triangles. */
    static uint16_t idx16[65536];
    if (count > 65536) {
        static int warned = 0;
        if (!warned) {
            ps3gl_log("[ps3gl] WARNING: DrawElements count %d exceeds 65536 -- "
                   "truncating, geometry will be missing\n", count);
            warned = 1;
        }
    }
    int n = (count > 65536) ? 65536 : count;

    /* Indices are always absolute into the original array (e.g. a studio
     * model's Nth mesh references vertex 500 of the whole submodel). The
     * direct-bind path above reads that same original array at absolute
     * offsets, so its indices stay as-is. The vring-copy fallback only
     * copied this call's own [locked_base, locked_base+num_verts) slice
     * into a freshly, locally 0-indexed block -- indices submitted against
     * it must be rebased by locked_base, or they'll read past the small
     * block into whatever follows it in the vring. */
    int idx_rebase = (!direct && locked) ? locked_base : 0;

    /* Software clip plane: cull triangles where all 3 verts are behind plane.
     * clip_plane_enabled is never set anywhere in ref/gl today (no glClipPlane
     * caller exists), so this whole block is currently dead code -- rebasing
     * is applied here too, for consistency with the path above, so it isn't
     * a landmine if mirror/portal clipping is ever wired up later. */
    if (ps3gl.clip_plane_enabled && n >= 3) {
        float nx   = ps3gl.clip_plane[0];
        float ny   = ps3gl.clip_plane[1];
        float nz   = ps3gl.clip_plane[2];
        float dist = ps3gl.clip_plane[3];

        /* World-space clip test: BSP surfaces, no modelview transform needed.
         * clip_dist is sized for the immediate-mode batch cap (PS3GL_MAX_VERTS),
         * but num_verts and raw index values here come from a separate,
         * unrelated path (glLockArraysEXT count or index-buffer scan) and can
         * exceed it -- clamp the write and skip any triangle referencing an
         * index we didn't fill in, rather than reading/writing out of bounds. */
        static float clip_dist[PS3GL_MAX_VERTS];
        int clip_n = (num_verts > PS3GL_MAX_VERTS) ? PS3GL_MAX_VERTS : num_verts;
        for (int i = 0; i < clip_n; i++) {
            const float *pos = (const float *)(vp + (locked_base + i) * vs);
            float wx = pos[0], wy = pos[1], wz = has_z ? pos[2] : 0.0f;
            clip_dist[i] = nx*wx + ny*wy + nz*wz - dist;
        }

        int out = 0;
        const uint16_t *src16 = (const uint16_t *)indices;
        for (int i = 0; i + 2 < n; i += 3) {
            int i0 = (int)src16[i]   - idx_rebase;
            int i1 = (int)src16[i+1] - idx_rebase;
            int i2 = (int)src16[i+2] - idx_rebase;
            if (i0 < 0 || i0 >= clip_n || i1 < 0 || i1 >= clip_n || i2 < 0 || i2 >= clip_n)
                continue;
            if (clip_dist[i0] < 0.0f && clip_dist[i1] < 0.0f && clip_dist[i2] < 0.0f)
                continue;
            idx16[out++] = (uint16_t)i0;
            idx16[out++] = (uint16_t)i1;
            idx16[out++] = (uint16_t)i2;
        }
        if (out > 0)
            rsxDrawInlineIndexArray16(ctx, gcm_prim, 0, out, idx16);
        return;
    }

    if (idx_rebase == 0) {
        /* Q3 uses 16-bit indices; direct memcpy, no conversion needed. */
        memcpy(idx16, indices, n * sizeof(uint16_t));
    } else {
        const uint16_t *src16 = (const uint16_t *)indices;
        for (int i = 0; i < n; i++)
            idx16[i] = (uint16_t)((int)src16[i] - idx_rebase);
    }
    rsxDrawInlineIndexArray16(ctx, gcm_prim, 0, n, idx16);
}
