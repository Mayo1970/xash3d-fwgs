/* ps3gl_states.c -- GL-to-RSX: render state + batch apply to RSX. */

#include "ps3gl.h"
#include <stdio.h>

/* GL-to-GCM conversion helpers */

/* GL comparison func values match GCM exactly (0x200..0x207) */
static inline uint32_t gl_to_gcm_cmpfunc(GLenum func)
{
    switch (func) {
    case GL_NEVER:    return GCM_NEVER;
    case GL_LESS:     return GCM_LESS;
    case GL_EQUAL:    return GCM_EQUAL;
    case GL_LEQUAL:   return GCM_LEQUAL;
    case GL_GREATER:  return GCM_GREATER;
    case GL_NOTEQUAL: return GCM_NOTEQUAL;
    case GL_GEQUAL:   return GCM_GEQUAL;
    case GL_ALWAYS:   return GCM_ALWAYS;
    default:          return GCM_ALWAYS;
    }
}

static inline uint16_t gl_to_gcm_blend(GLenum factor)
{
    switch (factor) {
    case GL_ZERO:                return GCM_ZERO;
    case GL_ONE:                 return GCM_ONE;
    case GL_SRC_COLOR:           return GCM_SRC_COLOR;
    case GL_ONE_MINUS_SRC_COLOR: return GCM_ONE_MINUS_SRC_COLOR;
    case GL_SRC_ALPHA:           return GCM_SRC_ALPHA;
    case GL_ONE_MINUS_SRC_ALPHA: return GCM_ONE_MINUS_SRC_ALPHA;
    case GL_DST_ALPHA:           return GCM_DST_ALPHA;
    case GL_ONE_MINUS_DST_ALPHA: return GCM_ONE_MINUS_DST_ALPHA;
    case GL_DST_COLOR:           return GCM_DST_COLOR;
    case GL_ONE_MINUS_DST_COLOR: return GCM_ONE_MINUS_DST_COLOR;
    case GL_SRC_ALPHA_SATURATE:  return GCM_SRC_ALPHA_SATURATE;
    default:                     return GCM_ONE;
    }
}

static inline uint32_t gl_to_gcm_stencilop(GLenum op)
{
    switch (op) {
    case GL_KEEP:    return GCM_KEEP;
    case GL_ZERO:    return GCM_ZERO;
    case GL_REPLACE: return GCM_REPLACE;
    case GL_INCR:    return GCM_INCR;
    case GL_DECR:    return GCM_DECR;
    case GL_INVERT:  return GCM_INVERT;
    default:         return GCM_KEEP;
    }
}

/* GL state setters -- record state + mark dirty */

static void enable_disable(GLenum cap, int val)
{
    switch (cap) {
    case GL_BLEND:
        ps3gl.rs.blend_enable = val;
        ps3gl.dirty |= PS3GL_DIRTY_BLEND;
        break;
    case GL_ALPHA_TEST:
        ps3gl.rs.alpha_test_enable = val;
        ps3gl.dirty |= PS3GL_DIRTY_ALPHA;
        break;
    case GL_DEPTH_TEST:
        ps3gl.rs.depth_test_enable = val;
        ps3gl.dirty |= PS3GL_DIRTY_DEPTH;
        break;
    case GL_CULL_FACE:
        ps3gl.rs.cull_enable = val;
        ps3gl.dirty |= PS3GL_DIRTY_CULL;
        break;
    case GL_SCISSOR_TEST:
        ps3gl.rs.scissor_enable = val;
        ps3gl.dirty |= PS3GL_DIRTY_SCISSOR;
        break;
    case GL_STENCIL_TEST:
        ps3gl.rs.stencil_enable = val;
        ps3gl.dirty |= PS3GL_DIRTY_STENCIL;
        break;
    case GL_POLYGON_OFFSET_FILL:
        ps3gl.rs.polyoffset_fill = val;
        ps3gl.dirty |= PS3GL_DIRTY_POLYOFFSET;
        break;
    case GL_TEXTURE_2D:
        if (ps3gl.tmu[ps3gl.active_tmu].enabled != val)
            ps3gl.tmu[ps3gl.active_tmu].dirty = 1;
        ps3gl.tmu[ps3gl.active_tmu].enabled = val;
        break;
    case GL_FOG:
        /* Tracked, not applied -- xash reads this back via glIsEnabled(GL_FOG). */
        ps3gl.fog.enabled = val;
        break;
    case GL_TEXTURE_GEN_S:
    case GL_TEXTURE_GEN_T:
    case GL_TEXTURE_GEN_R:
    case GL_TEXTURE_GEN_Q:
        {
            int bit = 1 << (int)(cap - GL_TEXTURE_GEN_S);
            if (val) ps3gl.texgen.enabled_bits[ps3gl.active_tmu] |= bit;
            else     ps3gl.texgen.enabled_bits[ps3gl.active_tmu] &= ~bit;
        }
        break;
    case GL_MULTISAMPLE:
    case GL_NORMALIZE:
    case GL_POLYGON_OFFSET_LINE:
        break;
    case GL_CLIP_PLANE0:
        ps3gl.clip_plane_enabled = val;
        break;
    default:
        break;
    }
}

void glEnable(GLenum cap)  { enable_disable(cap, 1); }
void glDisable(GLenum cap) { enable_disable(cap, 0); }

void glBlendFunc(GLenum sfactor, GLenum dfactor)
{
    ps3gl.rs.blend_src = gl_to_gcm_blend(sfactor);
    ps3gl.rs.blend_dst = gl_to_gcm_blend(dfactor);
    ps3gl.dirty |= PS3GL_DIRTY_BLEND;
}

void glAlphaFunc(GLenum func, GLclampf ref)
{
    ps3gl.rs.alpha_func = gl_to_gcm_cmpfunc(func);
    ps3gl.rs.alpha_ref  = (uint32_t)(ref * 255.0f + 0.5f);
    if (ps3gl.rs.alpha_ref > 255) ps3gl.rs.alpha_ref = 255;
    ps3gl.dirty |= PS3GL_DIRTY_ALPHA;
}

void glDepthFunc(GLenum func)
{
    ps3gl.rs.depth_func = gl_to_gcm_cmpfunc(func);
    ps3gl.dirty |= PS3GL_DIRTY_DEPTH;
}

void glDepthMask(GLboolean flag)
{
    ps3gl.rs.depth_mask = flag ? 1 : 0;
    ps3gl.dirty |= PS3GL_DIRTY_DEPTH;
}

void glDepthRange(GLclampd n, GLclampd f)
{
    ps3gl.rs.depth_near = (float)n;
    ps3gl.rs.depth_far  = (float)f;
    ps3gl.dirty |= PS3GL_DIRTY_VIEWPORT;
}

void glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
    ps3gl.rs.color_mask_r = r;
    ps3gl.rs.color_mask_g = g;
    ps3gl.rs.color_mask_b = b;
    ps3gl.rs.color_mask_a = a;
    ps3gl.dirty |= PS3GL_DIRTY_COLORMASK;
}

void glCullFace(GLenum mode)
{
    switch (mode) {
    case GL_FRONT:          ps3gl.rs.cull_face = GCM_CULL_FRONT; break;
    case GL_BACK:           ps3gl.rs.cull_face = GCM_CULL_BACK; break;
    case GL_FRONT_AND_BACK: ps3gl.rs.cull_face = GCM_CULL_ALL; break;
    default: break;
    }
    ps3gl.dirty |= PS3GL_DIRTY_CULL;
}

void glFrontFace(GLenum mode)
{
    ps3gl.rs.front_face = (mode == GL_CW) ? GCM_FRONTFACE_CW : GCM_FRONTFACE_CCW;
    ps3gl.dirty |= PS3GL_DIRTY_CULL;
}

void glScissor(GLint x, GLint y, GLsizei w, GLsizei h)
{
    /* Same GL→RSX Y conversion as glViewport. */
    int rsx_y = (int)ps3gl.screen_h - y - (int)h;

    ps3gl.rs.scissor_x = (int16_t)x;
    ps3gl.rs.scissor_y = (int16_t)rsx_y;
    ps3gl.rs.scissor_w = (uint16_t)w;
    ps3gl.rs.scissor_h = (uint16_t)h;
    ps3gl.dirty |= PS3GL_DIRTY_SCISSOR;
}

void glViewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
    /* ioq3 viewport Y is GL-convention (Y=0 at bottom); RSX framebuffer has Y=0 at top --
     * convert with rsx_y = screen_h - gl_y - h. No-op for full-screen viewports (y=0, h=screen_h). */
    int rsx_y = (int)ps3gl.screen_h - y - (int)h;

    ps3gl.rs.vp_x = (int16_t)x;
    ps3gl.rs.vp_y = (int16_t)rsx_y;
    ps3gl.rs.vp_w = (uint16_t)w;
    ps3gl.rs.vp_h = (uint16_t)h;
    ps3gl.dirty |= PS3GL_DIRTY_VIEWPORT;
}

void glShadeModel(GLenum mode)
{
    ps3gl.rs.shade_model = (mode == GL_FLAT) ? GCM_SHADE_MODEL_FLAT
                                             : GCM_SHADE_MODEL_SMOOTH;
    ps3gl.dirty |= PS3GL_DIRTY_SHADE;
}

void glPolygonOffset(GLfloat factor, GLfloat units)
{
    ps3gl.rs.polyoffset_factor = factor;
    ps3gl.rs.polyoffset_units  = units;
    ps3gl.dirty |= PS3GL_DIRTY_POLYOFFSET;
}

void glPolygonMode(GLenum face, GLenum mode)
{
    (void)face; (void)mode;
    /* RSX supports polygon modes but Q3 only uses GL_FILL */
}

void glStencilFunc(GLenum func, GLint ref, GLuint mask)
{
    ps3gl.rs.stencil_func = gl_to_gcm_cmpfunc(func);
    ps3gl.rs.stencil_ref  = (uint32_t)ref;
    ps3gl.rs.stencil_mask = mask;
    ps3gl.dirty |= PS3GL_DIRTY_STENCIL;
}

void glStencilMask(GLuint mask)
{
    ps3gl.rs.stencil_writemask = mask;
    ps3gl.dirty |= PS3GL_DIRTY_STENCIL;
}

void glStencilOp(GLenum fail, GLenum zfail, GLenum zpass)
{
    ps3gl.rs.stencil_fail  = gl_to_gcm_stencilop(fail);
    ps3gl.rs.stencil_zfail = gl_to_gcm_stencilop(zfail);
    ps3gl.rs.stencil_zpass = gl_to_gcm_stencilop(zpass);
    ps3gl.dirty |= PS3GL_DIRTY_STENCIL;
}

void glLineWidth(GLfloat width)  { (void)width; }

void glClipPlane(GLenum plane, const GLdouble *eq)
{
    /* Intentional no-op: Q3's portal renderer uses ps3gl_SetWorldClipPlane
     * instead, which stores the plane in world space for correct evaluation. */
    (void)plane; (void)eq;
}

void ps3gl_SetWorldClipPlane(float nx, float ny, float nz, float dist)
{
    ps3gl.clip_plane[0] = nx;
    ps3gl.clip_plane[1] = ny;
    ps3gl.clip_plane[2] = nz;
    ps3gl.clip_plane[3] = dist;
}

/* Clear */

void glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a)
{
    uint32_t ri = (uint32_t)(r * 255.0f);
    uint32_t gi = (uint32_t)(g * 255.0f);
    uint32_t bi = (uint32_t)(b * 255.0f);
    uint32_t ai = (uint32_t)(a * 255.0f);
    ps3gl.rs.clear_color = (ai << 24) | (ri << 16) | (gi << 8) | bi;
}

void glClearDepth(GLclampd depth)
{
    ps3gl.rs.clear_depth = (float)depth;
}

void glClearStencil(GLint s)
{
    ps3gl.rs.clear_stencil = (uint32_t)s;
}

void glClear(GLbitfield mask)
{
    /* A real FIFO write -- a pending deferred GL_POLYGON batch (ps3gl_vertices.c)
     * must not be reordered across it either way (survive an erase it shouldn't,
     * or get wiped along with it). */
    ps3gl_flush_polygon_batch();

    gcmContextData *ctx = ps3gl_get_ctx();
    if (!ctx) return;
    uint32_t gcm_mask = 0;

    if (mask & GL_COLOR_BUFFER_BIT) {
        rsxSetClearColor(ctx, ps3gl.rs.clear_color);
        gcm_mask |= GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A;
    }
    if (mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) {
        /* Z24S8: upper 24 bits = depth, lower 8 = stencil */
        uint32_t z24 = (uint32_t)(ps3gl.rs.clear_depth * 0xFFFFFF);
        uint32_t ds  = (z24 << 8) | (ps3gl.rs.clear_stencil & 0xFF);
        rsxSetClearDepthStencil(ctx, ds);
        if (mask & GL_DEPTH_BUFFER_BIT)   gcm_mask |= GCM_CLEAR_Z;
        if (mask & GL_STENCIL_BUFFER_BIT) gcm_mask |= GCM_CLEAR_S;
    }

    if (gcm_mask)
        rsxClearSurface(ctx, gcm_mask);
}

/* Query */

void glGetIntegerv(GLenum pname, GLint *params)
{
    if (!params) return;
    switch (pname) {
    case GL_MAX_TEXTURE_SIZE:      *params = 2048; break;
    case GL_MAX_TEXTURE_UNITS_ARB: *params = 2; break;
    case GL_STENCIL_BITS:          *params = 8; break;
    case GL_DEPTH_BITS:            *params = 24; break;
    default:                       *params = 0; break;
    }
}

const GLubyte *glGetString(GLenum name)
{
    switch (name) {
    case GL_VENDOR:     return (const GLubyte *)"Sony";
    case GL_RENDERER:   return (const GLubyte *)"RSX Reality Synthesizer";
    case GL_VERSION:    return (const GLubyte *)"1.1 PSL1GHT";
    /* Only extensions ps3gl really implements. GL_ARB_vertex_buffer_object is
     * backed by RSX-local memory (ps3gl_glapi.c) and consumed by the
     * glDrawElements path in ps3gl_draw.c -- ref_gl's GL_CheckExtension greps
     * this string, so adding a name here is what turns that path on. */
    case GL_EXTENSIONS: return (const GLubyte *)"GL_ARB_multitexture GL_ARB_vertex_buffer_object";
    default:            return (const GLubyte *)"";
    }
}

GLenum glGetError(void) { return GL_NO_ERROR; }

void glGetBooleanv(GLenum pname, GLboolean *params)
{
    if (!params) return;
    switch (pname) {
    case GL_COLOR_WRITEMASK:
        /* ioq3 tr_shadows.c reads rgba[4] */
        params[0] = ps3gl.rs.color_mask_r ? GL_TRUE : GL_FALSE;
        params[1] = ps3gl.rs.color_mask_g ? GL_TRUE : GL_FALSE;
        params[2] = ps3gl.rs.color_mask_b ? GL_TRUE : GL_FALSE;
        params[3] = ps3gl.rs.color_mask_a ? GL_TRUE : GL_FALSE;
        break;
    case GL_DEPTH_WRITEMASK:
        params[0] = ps3gl.rs.depth_mask ? GL_TRUE : GL_FALSE;
        break;
    default:
        params[0] = GL_FALSE;
        break;
    }
}

/* Diagnostic: dump everything that decides how a draw composites.
 *
 * The lightmap passes in ref_gl set blend + texenv + vertex color from three
 * different places and never read any of it back, so a wrong composite has no
 * observable cause from the ref_gl side alone. fp_key is the important field:
 * ps3gl_shader_key() picks MODULATE2 (tex0*tex1, vertex color DISCARDED)
 * whenever both TMUs happen to be enabled and bound, regardless of the texenv
 * that was requested. */
void ps3gl_log_draw_state(const char *tag)
{
    ps3gl_log("[ps3gl] %s: fp_key=%d tmu0(en=%d tex=%d) tmu1(en=%d tex=%d) "
              "color=%08x blend=%d src=0x%04x dst=0x%04x "
              "depth(test=%d func=0x%04x mask=%d) alpha_test=%d\n",
              tag ? tag : "draw-state",
              ps3gl_shader_key(),
              ps3gl.tmu[0].enabled,
              ps3gl.tmu[0].bound ? ps3gl.tmu[0].bound->glname : -1,
              ps3gl.tmu[1].enabled,
              ps3gl.tmu[1].bound ? ps3gl.tmu[1].bound->glname : -1,
              (unsigned)ps3gl.imm.color,
              ps3gl.rs.blend_enable,
              (unsigned)ps3gl.rs.blend_src,
              (unsigned)ps3gl.rs.blend_dst,
              ps3gl.rs.depth_test_enable,
              (unsigned)ps3gl.rs.depth_func,
              ps3gl.rs.depth_mask,
              ps3gl.rs.alpha_test_enable);
}

/* Apply states to RSX before draw */

void ps3gl_states_init(void)
{
    /* Push full state to RSX on first frame */
    ps3gl.dirty = PS3GL_DIRTY_ALL;
}

void ps3gl_states_shutdown(void) {}

void ps3gl_apply_states(void)
{
    gcmContextData *ctx = ps3gl_get_ctx();
    uint32_t d = ps3gl.dirty;
    if (d == 0) return;
    if (!ctx) return;

    if (d & PS3GL_DIRTY_BLEND) {
        rsxSetBlendEnable(ctx, ps3gl.rs.blend_enable);
        if (ps3gl.rs.blend_enable) {
            rsxSetBlendFunc(ctx, ps3gl.rs.blend_src, ps3gl.rs.blend_dst,
                            ps3gl.rs.blend_src, ps3gl.rs.blend_dst);
            rsxSetBlendEquation(ctx, GCM_FUNC_ADD, GCM_FUNC_ADD);
        }
    }

    if (d & PS3GL_DIRTY_ALPHA) {
        rsxSetAlphaTestEnable(ctx, ps3gl.rs.alpha_test_enable);
        if (ps3gl.rs.alpha_test_enable) {
            rsxSetAlphaFunc(ctx, ps3gl.rs.alpha_func, ps3gl.rs.alpha_ref);
        }
    }

    if (d & PS3GL_DIRTY_DEPTH) {
        rsxSetDepthTestEnable(ctx, ps3gl.rs.depth_test_enable);
        rsxSetDepthFunc(ctx, ps3gl.rs.depth_func);
        rsxSetDepthWriteEnable(ctx, ps3gl.rs.depth_mask);
    }

    if (d & PS3GL_DIRTY_CULL) {
        rsxSetCullFaceEnable(ctx, ps3gl.rs.cull_enable);
        if (ps3gl.rs.cull_enable) {
            rsxSetCullFace(ctx, ps3gl.rs.cull_face);
            rsxSetFrontFace(ctx, ps3gl.rs.front_face);
        }
    }

    if (d & PS3GL_DIRTY_SCISSOR) {
        if (ps3gl.rs.scissor_enable) {
            /* Clamp scissor to screen bounds (origin can be negative from
             * sub-viewport rendering, e.g. Player Setup model preview). */
            int sx = ps3gl.rs.scissor_x;
            int sy = ps3gl.rs.scissor_y;
            int sw = ps3gl.rs.scissor_w;
            int sh = ps3gl.rs.scissor_h;
            if (sx < 0) { sw += sx; sx = 0; }
            if (sy < 0) { sh += sy; sy = 0; }
            if (sw < 1) sw = 1;
            if (sh < 1) sh = 1;
            if (sx + sw > (int)ps3gl.screen_w) sw = (int)ps3gl.screen_w - sx;
            if (sy + sh > (int)ps3gl.screen_h) sh = (int)ps3gl.screen_h - sy;
            if (sw < 1) sw = 1;
            if (sh < 1) sh = 1;
            rsxSetScissor(ctx, (uint16_t)sx, (uint16_t)sy,
                          (uint16_t)sw, (uint16_t)sh);
        } else {
            rsxSetScissor(ctx, 0, 0, ps3gl.screen_w, ps3gl.screen_h);
        }
    }

    if (d & PS3GL_DIRTY_VIEWPORT) {
        /* Viewport origin can be negative (sub-viewport); clamp rect to screen bounds. */
        int vx = ps3gl.rs.vp_x;
        int vy = ps3gl.rs.vp_y;
        int vw = ps3gl.rs.vp_w;
        int vh = ps3gl.rs.vp_h;
        float zn = ps3gl.rs.depth_near;
        float zf = ps3gl.rs.depth_far;

        /* Scale/offset computed from the FULL viewport (including the
         * off-screen portion) so the projection maps correctly. */
        float scale[4], offset[4];
        scale[0]  = vw * 0.5f;
        scale[1]  = vh * -0.5f;
        scale[2]  = (zf - zn) * 0.5f;
        scale[3]  = 0.0f;
        offset[0] = vx + vw * 0.5f;
        offset[1] = vy + vh * 0.5f;
        offset[2] = (zf + zn) * 0.5f;
        offset[3] = 0.0f;

        /* Clamp the hardware viewport rect to screen bounds.
         * The scissor test will further clip to the visible region. */
        int cx = vx, cy = vy, cw = vw, ch = vh;
        if (cx < 0) { cw += cx; cx = 0; }
        if (cy < 0) { ch += cy; cy = 0; }
        if (cw < 1) cw = 1;
        if (ch < 1) ch = 1;
        if (cx + cw > (int)ps3gl.screen_w) cw = (int)ps3gl.screen_w - cx;
        if (cy + ch > (int)ps3gl.screen_h) ch = (int)ps3gl.screen_h - cy;
        if (cw < 1) cw = 1;
        if (ch < 1) ch = 1;

        rsxSetViewport(ctx, (uint16_t)cx, (uint16_t)cy,
                       (uint16_t)cw, (uint16_t)ch,
                       zn, zf, scale, offset);
        rsxSetViewportClip(ctx, 0, ps3gl.screen_w, ps3gl.screen_h);
    }

    if (d & PS3GL_DIRTY_COLORMASK) {
        uint32_t mask = 0;
        if (ps3gl.rs.color_mask_b) mask |= GCM_COLOR_MASK_B;
        if (ps3gl.rs.color_mask_g) mask |= GCM_COLOR_MASK_G;
        if (ps3gl.rs.color_mask_r) mask |= GCM_COLOR_MASK_R;
        if (ps3gl.rs.color_mask_a) mask |= GCM_COLOR_MASK_A;
        rsxSetColorMask(ctx, mask);
    }

    if (d & PS3GL_DIRTY_POLYOFFSET) {
        rsxSetPolygonOffsetFillEnable(ctx, ps3gl.rs.polyoffset_fill);
        if (ps3gl.rs.polyoffset_fill) {
            rsxSetPolygonOffset(ctx, ps3gl.rs.polyoffset_factor,
                                ps3gl.rs.polyoffset_units);
        }
    }

    if (d & PS3GL_DIRTY_SHADE) {
        rsxSetShadeModel(ctx, ps3gl.rs.shade_model);
    }

    if (d & PS3GL_DIRTY_STENCIL) {
        rsxSetStencilTestEnable(ctx, ps3gl.rs.stencil_enable);
        if (ps3gl.rs.stencil_enable) {
            rsxSetStencilFunc(ctx, ps3gl.rs.stencil_func,
                              ps3gl.rs.stencil_ref, ps3gl.rs.stencil_mask);
            rsxSetStencilOp(ctx, ps3gl.rs.stencil_fail,
                            ps3gl.rs.stencil_zfail, ps3gl.rs.stencil_zpass);
            rsxSetStencilMask(ctx, ps3gl.rs.stencil_writemask);
        }
    }

    ps3gl.dirty = 0;
}
