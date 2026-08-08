/* ps3gl_glapi.c -- GL entry points xash's ref_gl links against that the core
 * ps3gl implementation does not provide.
 *
 * Three groups, in descending order of "real":
 *
 *  1. Small, real VBO support plus extension-gated no-ops. VBOs back Xash's
 *     static world geometry with RSX-local memory; compressed textures, 1D/3D
 *     textures and multisample textures remain link-only. glDrawRangeElements
 *     forwards to glDrawElements, which is exactly the fallback ref_gl uses.
 *
 *  2. Trivial completions backed by existing ps3gl state.
 *
 *  3. Fog and texture-coordinate generation. State is tracked faithfully --
 *     ref_gl reads glIsEnabled(GL_FOG) and glGetFloatv(GL_FOG_COLOR) back and
 *     branches on them (gl_rmain.c:517, :662, :765) -- but neither is applied to
 *     any draw yet. Both need new offline-cgcomp'd fragment/vertex program
 *     permutations; until then fog renders as no fog and chrome/sky texgen as
 *     static texture coordinates. Visual gaps, not correctness gaps.
 */

#include "ps3gl.h"

/* ---------------------------------------------------------------- */
/* 1. GL_ARB_vertex_buffer_object                                  */
/* ---------------------------------------------------------------- */

ps3gl_buffer_t *ps3gl_buffer_lookup(GLuint name)
{
    if (!ps3gl_ptr || name == 0 || name > PS3GL_MAX_BUFFERS)
        return NULL;

    ps3gl_buffer_t *buffer = &ps3gl.buffers[name - 1];
    return buffer->name == name ? buffer : NULL;
}

static void ps3gl_defer_buffer_free(void *data)
{
    if (!data)
        return;

    if (ps3gl.pending_buffer_free_count >= (int)(PS3GL_MAX_BUFFERS * PS3GL_STREAM_BUFFER_SLOTS))
    {
        /* This only happens if a caller deletes every possible buffer before
         * the next frame fence. Keep correctness over memory pressure. */
        ps3gl_log("[ps3gl] WARNING: deferred VBO-free queue full; synchronizing RSX\n");
        rsxFinish(ps3gl_get_ctx(), 1);
        rsxFree(data);
        return;
    }

    ps3gl.pending_buffer_free[ps3gl.pending_buffer_free_count++] = data;
}

static void ps3gl_buffer_release(ps3gl_buffer_t *buffer, int deferred)
{
    if (!buffer)
        return;

    for (int i = 0; i < PS3GL_STREAM_BUFFER_SLOTS; i++)
    {
        if (buffer->data[i])
        {
            if (deferred)
                ps3gl_defer_buffer_free(buffer->data[i]);
            else
                rsxFree(buffer->data[i]);
        }
    }

    memset(buffer, 0, sizeof(*buffer));
}

void ps3gl_buffers_init(void)
{
    memset(ps3gl.buffers, 0, sizeof(ps3gl.buffers));
    ps3gl.array_buffer = 0;
    ps3gl.pending_buffer_free_count = 0;
}

void ps3gl_buffers_begin_frame(void)
{
    /* ps3gl_begin_frame calls this only after its RSX fence has proved the
     * older frame is done consuming anything we deferred.
     *
     * Slot rotation deliberately does NOT happen here: it happens in
     * glBufferDataARB, the point GL itself defines as a re-specify. Rotating
     * per frame instead would be both too coarse (a stream buffer re-specified
     * twice in one frame would still stomp itself) and too aggressive (it
     * would move a glBufferSubDataARB-updated buffer onto a slot holding
     * stale geometry). */
    for (int i = 0; i < ps3gl.pending_buffer_free_count; i++)
        rsxFree(ps3gl.pending_buffer_free[i]);
    ps3gl.pending_buffer_free_count = 0;
}

void ps3gl_buffers_shutdown(void)
{
    /* R_Free_Video has already rsxFinish()'d before ps3gl_shutdown(). */
    for (int i = 0; i < ps3gl.pending_buffer_free_count; i++)
        rsxFree(ps3gl.pending_buffer_free[i]);
    ps3gl.pending_buffer_free_count = 0;

    for (int i = 0; i < PS3GL_MAX_BUFFERS; i++)
        ps3gl_buffer_release(&ps3gl.buffers[i], 0);
}

void glGenBuffersARB(GLsizei n, GLuint *buffers)
{
    if (!buffers || n <= 0)
        return;

    for (GLsizei out = 0; out < n; out++)
    {
        buffers[out] = 0;
        for (GLuint i = 0; i < PS3GL_MAX_BUFFERS; i++)
        {
            if (ps3gl.buffers[i].name == 0)
            {
                ps3gl.buffers[i].name = i + 1;
                buffers[out] = i + 1;
                break;
            }
        }

        if (buffers[out] == 0)
            ps3gl_log("[ps3gl] WARNING: VBO object table exhausted (%u slots)\n", PS3GL_MAX_BUFFERS);
    }
}

void glDeleteBuffersARB(GLsizei n, const GLuint *buffers)
{
    if (!buffers || n <= 0)
        return;

    for (GLsizei i = 0; i < n; i++)
    {
        GLuint name = buffers[i];
        ps3gl_buffer_t *buffer = ps3gl_buffer_lookup(name);
        if (!buffer)
            continue;

        if (ps3gl.array_buffer == name)
            ps3gl.array_buffer = 0;
        if (ps3gl.va_vertex.buffer == name)
            ps3gl.va_vertex.buffer = 0;
        if (ps3gl.va_color.buffer == name)
            ps3gl.va_color.buffer = 0;
        for (int tmu = 0; tmu < PS3GL_MAX_TMUS; tmu++)
            if (ps3gl.va_texcoord[tmu].buffer == name)
                ps3gl.va_texcoord[tmu].buffer = 0;

        ps3gl_buffer_release(buffer, 1);
    }
}

GLboolean glIsBufferARB(GLuint buffer)
{
    return ps3gl_buffer_lookup(buffer) ? GL_TRUE : GL_FALSE;
}

void glBindBufferARB(GLenum target, GLuint buffer)
{
    if (target != GL_ARRAY_BUFFER_ARB)
        return;

    if (buffer == 0 || ps3gl_buffer_lookup(buffer))
        ps3gl.array_buffer = buffer;
}

static int ps3gl_buffer_allocate(ps3gl_buffer_t *buffer, uint32_t size, int slots)
{
    for (int i = 0; i < slots; i++)
    {
        buffer->data[i] = rsxMemalign(128, size);
        if (!buffer->data[i] || rsxAddressToOffset(buffer->data[i], &buffer->offset[i]) != 0)
        {
            ps3gl_log("[ps3gl] ERROR: VBO allocation failed (%u bytes, slot %d/%d)\n",
                      (unsigned)size, i + 1, slots);
            ps3gl_buffer_release(buffer, 0);
            return 0;
        }
    }

    buffer->size = size;
    buffer->slots = (uint8_t)slots;
    buffer->current = 0;
    return 1;
}

void glBufferDataARB(GLenum target, GLsizeiptrARB size, const GLvoid *data, GLenum usage)
{
    if (target != GL_ARRAY_BUFFER_ARB || size <= 0 || (uint64_t)size > UINT32_MAX)
        return;

    ps3gl_buffer_t *buffer = ps3gl_buffer_lookup(ps3gl.array_buffer);
    if (!buffer)
        return;

    GLuint name = buffer->name;
    int slots = usage == GL_STREAM_DRAW_ARB ? PS3GL_STREAM_BUFFER_SLOTS : 1;

    /* Grow-only. ref_gl re-specifies its stream buffers every frame with a
     * size that tracks the touched index range, so reallocating on every size
     * change would mean an rsxMemalign + deferred rsxFree per frame -- GDDR3
     * churn and fragmentation in exchange for nothing. Capacity is what the
     * allocation must cover; `used` is what this re-specify actually holds. */
    if (!buffer->data[0] || buffer->slots != slots || buffer->size < (uint32_t)size)
    {
        ps3gl_buffer_release(buffer, 1);
        buffer->name = name;
        if (!ps3gl_buffer_allocate(buffer, (uint32_t)size, slots))
            return;
    }
    else if (buffer->slots > 1)
    {
        /* Orphan: hand the next draw a different allocation so this re-specify
         * cannot overwrite memory an already-submitted draw still fetches. */
        buffer->current = (uint8_t)((buffer->current + 1) % buffer->slots);
    }

    buffer->used = (uint32_t)size;

    /* A stream buffer re-specified more times per frame than it has slots has
     * wrapped onto its own in-flight data. Not reachable with ref_gl's current
     * usage (only the r_vbo_dlightmode buffers are STREAM), but silent
     * corruption is exactly what this would look like, so say so. */
    {
        uint32_t frame = ps3gl_frame_index();
        if (buffer->respec_frame != frame)
        {
            buffer->respec_frame = frame;
            buffer->respec_count = 0;
        }
        if (buffer->slots > 1 && ++buffer->respec_count == buffer->slots + 1)
            ps3gl_log("[ps3gl] WARNING: stream VBO %u re-specified %u times in one "
                      "frame (%u slots) -- reusing in-flight memory\n",
                      (unsigned)name, (unsigned)buffer->respec_count, (unsigned)buffer->slots);
    }

    if (data)
    {
        memcpy(buffer->data[buffer->current], data, (size_t)size);
        __asm__ volatile("sync" ::: "memory");
    }
}

void glBufferSubDataARB(GLenum target, GLintptrARB offset, GLsizeiptrARB size, const GLvoid *data)
{
    if (target != GL_ARRAY_BUFFER_ARB || offset < 0 || size <= 0 || !data)
        return;

    ps3gl_buffer_t *buffer = ps3gl_buffer_lookup(ps3gl.array_buffer);
    if (!buffer || (uint64_t)offset + (uint64_t)size > buffer->size)
        return;

    memcpy((uint8_t *)buffer->data[buffer->current] + offset, data, (size_t)size);
    __asm__ volatile("sync" ::: "memory");
}

/* GL_ARB_texture_compression */
void glCompressedTexImage1DARB(GLenum target, GLint level, GLenum internalformat,
                               GLsizei width, GLint border, GLsizei imageSize, const void *data)
{
    (void)target; (void)level; (void)internalformat; (void)width;
    (void)border; (void)imageSize; (void)data;
}

void glCompressedTexImage2DARB(GLenum target, GLint level, GLenum internalformat,
                               GLsizei width, GLsizei height, GLint border,
                               GLsizei imageSize, const void *data)
{
    (void)target; (void)level; (void)internalformat; (void)width; (void)height;
    (void)border; (void)imageSize; (void)data;
}

void glCompressedTexImage3DARB(GLenum target, GLint level, GLenum internalformat,
                               GLsizei width, GLsizei height, GLsizei depth, GLint border,
                               GLsizei imageSize, const void *data)
{
    (void)target; (void)level; (void)internalformat; (void)width; (void)height;
    (void)depth; (void)border; (void)imageSize; (void)data;
}

void glCompressedTexSubImage1DARB(GLenum target, GLint level, GLint xoffset, GLsizei width,
                                  GLenum format, GLsizei imageSize, const void *data)
{
    (void)target; (void)level; (void)xoffset; (void)width;
    (void)format; (void)imageSize; (void)data;
}

void glCompressedTexSubImage2DARB(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                                  GLsizei width, GLsizei height, GLenum format,
                                  GLsizei imageSize, const void *data)
{
    (void)target; (void)level; (void)xoffset; (void)yoffset; (void)width; (void)height;
    (void)format; (void)imageSize; (void)data;
}

void glCompressedTexSubImage3DARB(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                                  GLint zoffset, GLsizei width, GLsizei height, GLsizei depth,
                                  GLenum format, GLsizei imageSize, const void *data)
{
    (void)target; (void)level; (void)xoffset; (void)yoffset; (void)zoffset;
    (void)width; (void)height; (void)depth; (void)format; (void)imageSize; (void)data;
}

/* 1D / 3D / multisample textures -- RSX has no 1D target, and ref_gl only
 * reaches the 3D paths behind GL_TEXTURE_3D_EXT / GL_TEXTURE_ARRAY_EXT. */
void glTexImage1D(GLenum target, GLint level, GLint internalformat, GLsizei width,
                  GLint border, GLenum format, GLenum type, const GLvoid *pixels)
{
    (void)target; (void)level; (void)internalformat; (void)width;
    (void)border; (void)format; (void)type; (void)pixels;
}

void glTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLsizei width,
                     GLenum format, GLenum type, const GLvoid *pixels)
{
    (void)target; (void)level; (void)xoffset; (void)width;
    (void)format; (void)type; (void)pixels;
}

void glTexImage3D(GLenum target, GLint level, GLenum internalFormat, GLsizei width,
                  GLsizei height, GLsizei depth, GLint border, GLenum format,
                  GLenum type, const GLvoid *pixels)
{
    (void)target; (void)level; (void)internalFormat; (void)width; (void)height;
    (void)depth; (void)border; (void)format; (void)type; (void)pixels;
}

void glTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset,
                     GLsizei width, GLsizei height, GLsizei depth, GLenum format,
                     GLenum type, const GLvoid *pixels)
{
    (void)target; (void)level; (void)xoffset; (void)yoffset; (void)zoffset;
    (void)width; (void)height; (void)depth; (void)format; (void)type; (void)pixels;
}

void glTexImage2DMultisample(GLenum target, GLsizei samples, GLenum internalformat,
                             GLsizei width, GLsizei height, GLboolean fixedsamplelocations)
{
    (void)target; (void)samples; (void)internalformat;
    (void)width; (void)height; (void)fixedsamplelocations;
}

/* GL_ARB_debug_output -- only bound when glw_state.extended, which the PS3
 * context never sets. */
void glDebugMessageCallbackARB(GL_DEBUG_PROC_ARB callback, void *userParam)
{
    (void)callback; (void)userParam;
}

void glDebugMessageControlARB(GLenum source, GLenum type, GLenum severity, GLsizei count,
                              const GLuint *ids, GLboolean enabled)
{
    (void)source; (void)type; (void)severity; (void)count; (void)ids; (void)enabled;
}

/* GL_EXT_draw_range_elements. The [start,end] hint exists purely to let a driver
 * pre-transform a vertex sub-range; ps3gl walks the index list anyway, so
 * dropping it is exactly the unextended fallback ref_gl would otherwise take. */
void glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count,
                         GLenum type, const GLvoid *indices)
{
    (void)start; (void)end;
    glDrawElements(mode, count, type, indices);
}

/* ---------------------------------------------------------------- */
/* 2. Trivial completions                                           */
/* ---------------------------------------------------------------- */

void glMultiTexCoord2f(GLenum target, GLfloat s, GLfloat t)
{
    glMultiTexCoord2fARB(target, s, t);
}

/* ps3gl's vertex format carries no normal, and xash lights everything itself
 * (GL_LIGHTING is never enabled), so normals have no consumer. */
void glNormal3fv(const GLfloat *v) { (void)v; }

/* RSX point size is a render state ps3gl does not track; ref_gl only sets it
 * for r_showtree/debug point rendering. */
void glPointSize(GLfloat size) { (void)size; }

/* Quality hints have no RSX equivalent. */
void glHint(GLenum target, GLenum mode) { (void)target; (void)mode; }

/* The third coordinate is dropped: ps3gl's vertex has 2D texcoords per TMU and
 * ref_gl only calls this from the triapi passthrough. */
void glTexCoord3f(GLfloat s, GLfloat t, GLfloat r)
{
    (void)r;
    glTexCoord2f(s, t);
}

/* Vector forms of the scalar setters ps3gl already has. glTexEnvfv is only used
 * for GL_TEXTURE_ENV_COLOR, which belongs to the GL_COMBINE_ARB path ps3gl
 * folds into plain MODULATE (see glTexEnvf). */
void glTexEnvfv(GLenum target, GLenum pname, const GLfloat *params)
{
    if (!params) return;
    if (pname == GL_TEXTURE_ENV_MODE)
        glTexEnvf(target, pname, params[0]);
}

void glTexParameterfv(GLenum target, GLenum pname, const GLfloat *params)
{
    if (!params) return;
    glTexParameterf(target, pname, params[0]);
}

GLboolean glIsTexture(GLuint texture)
{
    if (!ps3gl_ptr || texture == 0) return GL_FALSE;
    return ps3gl_texture_find(texture) ? GL_TRUE : GL_FALSE;
}

GLboolean glIsEnabled(GLenum cap)
{
    if (!ps3gl_ptr) return GL_FALSE;

    switch (cap) {
    case GL_BLEND:                return ps3gl.rs.blend_enable ? GL_TRUE : GL_FALSE;
    case GL_ALPHA_TEST:           return ps3gl.rs.alpha_test_enable ? GL_TRUE : GL_FALSE;
    case GL_DEPTH_TEST:           return ps3gl.rs.depth_test_enable ? GL_TRUE : GL_FALSE;
    case GL_CULL_FACE:            return ps3gl.rs.cull_enable ? GL_TRUE : GL_FALSE;
    case GL_SCISSOR_TEST:         return ps3gl.rs.scissor_enable ? GL_TRUE : GL_FALSE;
    case GL_STENCIL_TEST:         return ps3gl.rs.stencil_enable ? GL_TRUE : GL_FALSE;
    case GL_POLYGON_OFFSET_FILL:  return ps3gl.rs.polyoffset_fill ? GL_TRUE : GL_FALSE;
    case GL_TEXTURE_2D:           return ps3gl.tmu[ps3gl.active_tmu].enabled ? GL_TRUE : GL_FALSE;
    case GL_CLIP_PLANE0:          return ps3gl.clip_plane_enabled ? GL_TRUE : GL_FALSE;
    case GL_FOG:                  return ps3gl.fog.enabled ? GL_TRUE : GL_FALSE;
    case GL_TEXTURE_GEN_S:
    case GL_TEXTURE_GEN_T:
    case GL_TEXTURE_GEN_R:
    case GL_TEXTURE_GEN_Q:
        return (ps3gl.texgen.enabled_bits[ps3gl.active_tmu]
                & (1 << (int)(cap - GL_TEXTURE_GEN_S))) ? GL_TRUE : GL_FALSE;
    default:
        return GL_FALSE;
    }
}

/* ---------------------------------------------------------------- */
/* 3. Fog and texgen -- tracked, not applied                        */
/* ---------------------------------------------------------------- */

void glFogf(GLenum pname, GLfloat param)
{
    if (!ps3gl_ptr) return;

    switch (pname) {
    case GL_FOG_DENSITY: ps3gl.fog.density = param; break;
    case GL_FOG_START:   ps3gl.fog.start   = param; break;
    case GL_FOG_END:     ps3gl.fog.end     = param; break;
    case GL_FOG_MODE:    ps3gl.fog.mode    = (GLenum)(int)param; break;
    default: break;
    }
}

void glFogi(GLenum pname, GLint param)
{
    glFogf(pname, (GLfloat)param);
}

void glFogfv(GLenum pname, const GLfloat *params)
{
    if (!ps3gl_ptr || !params) return;

    if (pname == GL_FOG_COLOR) {
        /* ref_gl passes a vec4_t whose alpha it documents as ignored
         * (gl_rsurf.c:328), but copy all four so a read-back round-trips. */
        for (int i = 0; i < 4; i++) ps3gl.fog.color[i] = params[i];
        return;
    }
    glFogf(pname, params[0]);
}

void glTexGeni(GLenum coord, GLenum pname, GLint param)
{
    if (!ps3gl_ptr || pname != GL_TEXTURE_GEN_MODE) return;
    if (coord < GL_S || coord > GL_Q) return;

    ps3gl.texgen.mode[ps3gl.active_tmu][coord - GL_S] = (GLenum)param;
}
