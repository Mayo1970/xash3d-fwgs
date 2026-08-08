/* ps3gl_colors.c -- GL-to-RSX layer: vertex color management. */

#include "ps3gl.h"

void glColor3f(GLfloat r, GLfloat g, GLfloat b)
{
    ps3gl.imm.color = ps3gl_pack_color(r, g, b, 1.0f);
}

void glColor3fv(const GLfloat *v)
{
    ps3gl.imm.color = ps3gl_pack_color(v[0], v[1], v[2], 1.0f);
}

void glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{
    ps3gl.imm.color = ps3gl_pack_color(r, g, b, a);
}

void glColor4fv(const GLfloat *v)
{
    ps3gl.imm.color = ps3gl_pack_color(v[0], v[1], v[2], v[3]);
}

void glColor4ubv(const GLubyte *v)
{
    ps3gl.imm.color = ps3gl_pack_color_ub(v[0], v[1], v[2], v[3]);
}

void glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a)
{
    ps3gl.imm.color = ps3gl_pack_color_ub(r, g, b, a);
}

void glColor3ubv(const GLubyte *v)
{
    ps3gl.imm.color = ps3gl_pack_color_ub(v[0], v[1], v[2], 255);
}
