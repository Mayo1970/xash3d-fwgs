/* ps3gl_matrices.c -- GL-to-RSX layer: matrix stack management.
 * 32-deep modelview/projection stacks (column-major), uploaded to RSX VP constants before each draw. */

#include "ps3gl.h"
#include <math.h>

/* Helpers */

static ps3gl_matstack_t *current_stack(void)
{
    switch (ps3gl.matrix_mode) {
    case GL_PROJECTION: return &ps3gl.proj;
    default:            return &ps3gl.mv;
    }
}

static float *current_matrix(void)
{
    ps3gl_matstack_t *s = current_stack();
    return s->stack[s->depth];
}

/* Column-major 4x4 multiply: out = a * b */
static void mat4_mul(float *out, const float *a, const float *b)
{
    float tmp[16];
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 4; r++) {
            tmp[c * 4 + r] =
                a[0 * 4 + r] * b[c * 4 + 0] +
                a[1 * 4 + r] * b[c * 4 + 1] +
                a[2 * 4 + r] * b[c * 4 + 2] +
                a[3 * 4 + r] * b[c * 4 + 3];
        }
    }
    memcpy(out, tmp, sizeof(tmp));
}

/* Transpose column-major -> row-major (for RSX VP constants) */
static void mat4_transpose(float *out, const float *in)
{
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            out[r * 4 + c] = in[c * 4 + r];
}

/* Init */

void ps3gl_matrices_init(void)
{
    ps3gl.mv.depth = 0;
    ps3gl.mv.dirty = 1;
    memcpy(ps3gl.mv.stack[0], ps3gl_identity, sizeof(ps3gl_identity));

    ps3gl.proj.depth = 0;
    ps3gl.proj.dirty = 1;
    memcpy(ps3gl.proj.stack[0], ps3gl_identity, sizeof(ps3gl_identity));

    ps3gl.matrix_mode = GL_MODELVIEW;
}

/* GL functions */

void glMatrixMode(GLenum mode)
{
    ps3gl.matrix_mode = mode;
}

void glLoadIdentity(void)
{
    float *m = current_matrix();
    memcpy(m, ps3gl_identity, 64);
    current_stack()->dirty = 1;
}

void glLoadMatrixf(const GLfloat *mat)
{
    float *m = current_matrix();
    memcpy(m, mat, 64);
    current_stack()->dirty = 1;
}

void glMultMatrixf(const GLfloat *mat)
{
    float *m = current_matrix();
    mat4_mul(m, m, mat);
    current_stack()->dirty = 1;
}

void glPushMatrix(void)
{
    ps3gl_matstack_t *s = current_stack();
    if (s->depth < PS3GL_MAX_MATRIX_STACK - 1) {
        memcpy(s->stack[s->depth + 1], s->stack[s->depth], 64);
        s->depth++;
    }
}

void glPopMatrix(void)
{
    ps3gl_matstack_t *s = current_stack();
    if (s->depth > 0) {
        s->depth--;
        s->dirty = 1;
    }
}

void glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t,
                 GLdouble n, GLdouble f)
{
    float ortho[16];
    memset(ortho, 0, sizeof(ortho));
    ortho[0]  =  2.0f / (float)(r - l);
    ortho[5]  =  2.0f / (float)(t - b);
    ortho[10] = -2.0f / (float)(f - n);
    ortho[12] = -(float)(r + l) / (float)(r - l);
    ortho[13] = -(float)(t + b) / (float)(t - b);
    ortho[14] = -(float)(f + n) / (float)(f - n);
    ortho[15] =  1.0f;

    float *m = current_matrix();
    mat4_mul(m, m, ortho);
    current_stack()->dirty = 1;
}

void glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t,
                   GLdouble n, GLdouble f)
{
    float frus[16];
    memset(frus, 0, sizeof(frus));
    float nn = (float)n;
    frus[0]  = 2.0f * nn / (float)(r - l);
    frus[5]  = 2.0f * nn / (float)(t - b);
    frus[8]  = (float)(r + l) / (float)(r - l);
    frus[9]  = (float)(t + b) / (float)(t - b);
    frus[10] = -(float)(f + n) / (float)(f - n);
    frus[11] = -1.0f;
    frus[14] = -2.0f * nn * (float)f / (float)(f - n);

    float *m = current_matrix();
    mat4_mul(m, m, frus);
    current_stack()->dirty = 1;
}

void glTranslatef(GLfloat x, GLfloat y, GLfloat z)
{
    float tr[16];
    memcpy(tr, ps3gl_identity, sizeof(tr));
    tr[12] = x;
    tr[13] = y;
    tr[14] = z;

    float *m = current_matrix();
    mat4_mul(m, m, tr);
    current_stack()->dirty = 1;
}

void glScalef(GLfloat x, GLfloat y, GLfloat z)
{
    float sc[16];
    memset(sc, 0, sizeof(sc));
    sc[0]  = x;
    sc[5]  = y;
    sc[10] = z;
    sc[15] = 1.0f;

    float *m = current_matrix();
    mat4_mul(m, m, sc);
    current_stack()->dirty = 1;
}

void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z)
{
    float rad = angle * (float)(M_PI / 180.0);
    float c = cosf(rad), s = sinf(rad);
    float len = sqrtf(x*x + y*y + z*z);
    if (len < 1e-6f) return;
    x /= len; y /= len; z /= len;
    float ic = 1.0f - c;

    float rot[16];
    rot[0]  = x*x*ic + c;    rot[4]  = x*y*ic - z*s;  rot[8]  = x*z*ic + y*s;  rot[12] = 0;
    rot[1]  = y*x*ic + z*s;  rot[5]  = y*y*ic + c;    rot[9]  = y*z*ic - x*s;  rot[13] = 0;
    rot[2]  = z*x*ic - y*s;  rot[6]  = z*y*ic + x*s;  rot[10] = z*z*ic + c;    rot[14] = 0;
    rot[3]  = 0;              rot[7]  = 0;              rot[11] = 0;              rot[15] = 1;

    float *m = current_matrix();
    mat4_mul(m, m, rot);
    current_stack()->dirty = 1;
}

void glGetFloatv(GLenum pname, GLfloat *params)
{
    if (!params) return;
    switch (pname) {
    case GL_MODELVIEW:
    case 0x0BA6: /* GL_MODELVIEW_MATRIX */
        memcpy(params, ps3gl.mv.stack[ps3gl.mv.depth], 64);
        break;
    case GL_PROJECTION:
    case 0x0BA7: /* GL_PROJECTION_MATRIX */
        memcpy(params, ps3gl.proj.stack[ps3gl.proj.depth], 64);
        break;
    case GL_FOG_COLOR:
        memcpy(params, ps3gl.fog.color, sizeof(ps3gl.fog.color));
        break;
    case GL_FOG_DENSITY:
        params[0] = ps3gl.fog.density;
        break;
    case GL_FOG_START:
        params[0] = ps3gl.fog.start;
        break;
    case GL_FOG_END:
        params[0] = ps3gl.fog.end;
        break;
    default:
        break;
    }
}

/* Upload MVP matrix to RSX VP constants via ps3gl_apply_shader(). */

/* MVP scratch space, transposed for RSX (row-major for VP constants) */
static float s_mvp[16];

/* Bumped only when s_mvp is actually recomputed below, so callers can skip
 * re-uploading the RSX vertex-program constant when nothing has changed. */
static uint32_t s_mvp_generation = 1;

const float *ps3gl_get_mvp(void)
{
    return s_mvp;
}

uint32_t ps3gl_get_mvp_generation(void)
{
    return s_mvp_generation;
}

void ps3gl_apply_matrices(void)
{
    if (!ps3gl.mv.dirty && !ps3gl.proj.dirty) return;

    /* Compute MVP = projection * modelview */
    float mvp[16];
    mat4_mul(mvp, ps3gl.proj.stack[ps3gl.proj.depth],
                  ps3gl.mv.stack[ps3gl.mv.depth]);

    /* RSX VP expects constants in row-major order */
    mat4_transpose(s_mvp, mvp);
    s_mvp_generation++;

    ps3gl.mv.dirty  = 0;
    ps3gl.proj.dirty = 0;
}
