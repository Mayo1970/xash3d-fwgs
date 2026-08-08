/*
 * GL/gl.h -- OpenGL type definitions and constants for ioquake3-PS3.
 *
 * Provides the minimum GL API surface needed by ioQ3's renderergl1.
 * When RSXGL is integrated, this header can be replaced by RSXGL's gl.h.
 * Until then, it defines all types and constants the renderer references.
 */

#ifndef __GL_H_PS3__
#define __GL_H_PS3__

#include <stddef.h>
#include <stdint.h>

/* ----------------------------------------------------------------
 * GL types
 * ---------------------------------------------------------------- */
typedef unsigned int    GLenum;
typedef unsigned char   GLboolean;
typedef unsigned int    GLbitfield;
typedef signed char     GLbyte;
typedef short           GLshort;
typedef int             GLint;
typedef int             GLsizei;
typedef unsigned char   GLubyte;
typedef unsigned short  GLushort;
typedef unsigned int    GLuint;
typedef float           GLfloat;
typedef float           GLclampf;
typedef double          GLdouble;
typedef double          GLclampd;
typedef void            GLvoid;

/* GL 1.5+ / GL 2.0+ types needed by qgl.h macro expansions */
typedef ptrdiff_t       GLsizeiptr;
typedef ptrdiff_t       GLintptr;
typedef char            GLchar;

/* ARB buffer-object types. Deliberately `int`, NOT ptrdiff_t -- xash's
 * ref/gl/gl_export.h declares them that way, and these must match its
 * prototypes exactly or the XASH_GL_STATIC direct-link ABI breaks. */
typedef int             GLintptrARB;
typedef int             GLsizeiptrARB;
typedef char            GLcharARB;
typedef unsigned int    GLhandleARB;

/* For function pointer declarations */
#ifndef APIENTRY
#define APIENTRY
#endif
#ifndef APIENTRYP
#define APIENTRYP APIENTRY *
#endif

typedef void ( APIENTRY *GL_DEBUG_PROC_ARB )( GLenum source, GLenum type, GLuint id,
    GLenum severity, GLsizei length, const GLcharARB *message, GLvoid *userParam );

/* ----------------------------------------------------------------
 * GL boolean values
 * ---------------------------------------------------------------- */
#define GL_FALSE    0
#define GL_TRUE     1

/* ----------------------------------------------------------------
 * GL error codes
 * ---------------------------------------------------------------- */
#define GL_NO_ERROR                     0
#define GL_INVALID_ENUM                 0x0500
#define GL_INVALID_VALUE                0x0501
#define GL_INVALID_OPERATION            0x0502
#define GL_STACK_OVERFLOW               0x0503
#define GL_STACK_UNDERFLOW              0x0504
#define GL_OUT_OF_MEMORY                0x0505

/* ----------------------------------------------------------------
 * Data types
 * ---------------------------------------------------------------- */
#define GL_BYTE                         0x1400
#define GL_UNSIGNED_BYTE                0x1401
#define GL_SHORT                        0x1402
#define GL_UNSIGNED_SHORT               0x1403
#define GL_INT                          0x1404
#define GL_UNSIGNED_INT                 0x1405
#define GL_FLOAT                        0x1406
#define GL_DOUBLE                       0x140A

/* ----------------------------------------------------------------
 * Primitives
 * ---------------------------------------------------------------- */
#define GL_POINTS                       0x0000
#define GL_LINES                        0x0001
#define GL_LINE_LOOP                    0x0002
#define GL_LINE_STRIP                   0x0003
#define GL_TRIANGLES                    0x0004
#define GL_TRIANGLE_STRIP               0x0005
#define GL_TRIANGLE_FAN                 0x0006
#define GL_QUADS                        0x0007
#define GL_QUAD_STRIP                   0x0008
#define GL_POLYGON                      0x0009

/* ----------------------------------------------------------------
 * Buffer bits
 * ---------------------------------------------------------------- */
#define GL_DEPTH_BUFFER_BIT             0x00000100
#define GL_STENCIL_BUFFER_BIT           0x00000400
#define GL_COLOR_BUFFER_BIT             0x00004000

/* ----------------------------------------------------------------
 * Enable/Disable caps
 * ---------------------------------------------------------------- */
#define GL_TEXTURE_2D                   0x0DE1
#define GL_BLEND                        0x0BE2
#define GL_DEPTH_TEST                   0x0B71
#define GL_CULL_FACE                    0x0B44
#define GL_ALPHA_TEST                   0x0BC0
#define GL_SCISSOR_TEST                 0x0C11
#define GL_STENCIL_TEST                 0x0B90
#define GL_FOG                          0x0B60
#define GL_POLYGON_OFFSET_FILL          0x8037
#define GL_POLYGON_OFFSET_LINE          0x2A02
#define GL_CLIP_PLANE0                  0x3000
#define GL_MULTISAMPLE                  0x809D
#define GL_NORMALIZE                    0x0BA1

/* ----------------------------------------------------------------
 * Alpha / depth / stencil function values
 * ---------------------------------------------------------------- */
#define GL_NEVER                        0x0200
#define GL_LESS                         0x0201
#define GL_EQUAL                        0x0202
#define GL_LEQUAL                       0x0203
#define GL_GREATER                      0x0204
#define GL_NOTEQUAL                     0x0205
#define GL_GEQUAL                       0x0206
#define GL_ALWAYS                       0x0207

/* ----------------------------------------------------------------
 * Blend factors
 * ---------------------------------------------------------------- */
#define GL_ZERO                         0
#define GL_ONE                          1
#define GL_SRC_COLOR                    0x0300
#define GL_ONE_MINUS_SRC_COLOR          0x0301
#define GL_SRC_ALPHA                    0x0302
#define GL_ONE_MINUS_SRC_ALPHA          0x0303
#define GL_DST_ALPHA                    0x0304
#define GL_ONE_MINUS_DST_ALPHA          0x0305
#define GL_DST_COLOR                    0x0306
#define GL_ONE_MINUS_DST_COLOR          0x0307
#define GL_SRC_ALPHA_SATURATE           0x0308

/* ----------------------------------------------------------------
 * Texture targets / parameters
 * ---------------------------------------------------------------- */
#define GL_TEXTURE_MAG_FILTER           0x2800
#define GL_TEXTURE_MIN_FILTER           0x2801
#define GL_TEXTURE_WRAP_S               0x2802
#define GL_TEXTURE_WRAP_T               0x2803
#define GL_NEAREST                      0x2600
#define GL_LINEAR                       0x2601
#define GL_NEAREST_MIPMAP_NEAREST       0x2700
#define GL_LINEAR_MIPMAP_NEAREST        0x2701
#define GL_NEAREST_MIPMAP_LINEAR        0x2702
#define GL_LINEAR_MIPMAP_LINEAR         0x2703
#define GL_REPEAT                       0x2901
#define GL_CLAMP                        0x2900
#define GL_CLAMP_TO_EDGE                0x812F
#define GL_TEXTURE_MAX_ANISOTROPY_EXT   0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF

/* ----------------------------------------------------------------
 * Internal texture formats
 * ---------------------------------------------------------------- */
#define GL_ALPHA                        0x1906
#define GL_RGB                          0x1907
#define GL_RGBA                         0x1908
#define GL_LUMINANCE                    0x1909
#define GL_LUMINANCE_ALPHA              0x190A
#define GL_RGB8                         0x8051
#define GL_RGBA8                        0x8058
#define GL_RGBA4                        0x8056
#define GL_RGB5                         0x8050
#define GL_RGB5_A1                      0x8057

/* Compressed texture formats */
#define GL_COMPRESSED_RGB_S3TC_DXT1_EXT  0x83F0
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3

/* ----------------------------------------------------------------
 * Matrix modes
 * ---------------------------------------------------------------- */
#define GL_MODELVIEW                    0x1700
#define GL_PROJECTION                   0x1701
#define GL_TEXTURE                      0x1702

/* ----------------------------------------------------------------
 * Vertex array types
 * ---------------------------------------------------------------- */
#define GL_VERTEX_ARRAY                 0x8074
#define GL_COLOR_ARRAY                  0x8076
#define GL_TEXTURE_COORD_ARRAY          0x8078

/* ----------------------------------------------------------------
 * glGet parameters
 * ---------------------------------------------------------------- */
#define GL_MAX_TEXTURE_SIZE             0x0D33
#define GL_MAX_TEXTURE_UNITS_ARB        0x84E2
#define GL_STENCIL_BITS                 0x0D57
#define GL_STENCIL_INDEX                0x1901
#define GL_DEPTH_BITS                   0x0D56
#define GL_VENDOR                       0x1F00
#define GL_RENDERER                     0x1F01
#define GL_VERSION                      0x1F02
#define GL_EXTENSIONS                   0x1F03
#define GL_NUM_EXTENSIONS               0x821D  /* GL 3.0 */
#define GL_PACK_ALIGNMENT               0x0D05
#define GL_UNPACK_ALIGNMENT             0x0CF5

/* GL_ARB_vertex_buffer_object */
#define GL_ARRAY_BUFFER_ARB             0x8892
#define GL_ELEMENT_ARRAY_BUFFER_ARB     0x8893
#define GL_STATIC_DRAW_ARB              0x88E4
#define GL_DYNAMIC_DRAW_ARB             0x88E8
#define GL_STREAM_DRAW_ARB              0x88E0

/* ----------------------------------------------------------------
 * Cull face / front face
 * ---------------------------------------------------------------- */
#define GL_CW                           0x0900
#define GL_CCW                          0x0901
#define GL_FRONT                        0x0404
#define GL_BACK                         0x0405
#define GL_FRONT_AND_BACK               0x0408

/* ----------------------------------------------------------------
 * Shade model
 * ---------------------------------------------------------------- */
#define GL_FLAT                         0x1D00
#define GL_SMOOTH                       0x1D01

/* ----------------------------------------------------------------
 * Draw buffer
 * ---------------------------------------------------------------- */
#define GL_NONE                         0
#define GL_FRONT_LEFT                   0x0400
#define GL_FRONT_RIGHT                  0x0401
#define GL_BACK_LEFT                    0x0402
#define GL_BACK_RIGHT                   0x0403

/* ----------------------------------------------------------------
 * Polygon mode
 * ---------------------------------------------------------------- */
#define GL_POINT                        0x1B00
#define GL_LINE                         0x1B01
#define GL_FILL                         0x1B02

/* ----------------------------------------------------------------
 * Stencil operations
 * ---------------------------------------------------------------- */
#define GL_KEEP                         0x1E00
#define GL_REPLACE                      0x1E01
#define GL_INCR                         0x1E02
#define GL_DECR                         0x1E03
#define GL_INVERT                       0x150A
#define GL_INCR_WRAP                    0x8507
#define GL_DECR_WRAP                    0x8508

/* ----------------------------------------------------------------
 * TexEnv
 * ---------------------------------------------------------------- */
#define GL_TEXTURE_ENV                  0x2300
#define GL_TEXTURE_ENV_MODE             0x2200
#define GL_TEXTURE_ENV_COLOR            0x2201
#define GL_MODULATE                     0x2100
#define GL_DECAL                        0x2101
#define GL_ADD                          0x0104
#define GL_REPLACE_TEXENV               0x1E01  /* same as GL_REPLACE but for texenv */

/* ----------------------------------------------------------------
 * ARB multitexture
 * ---------------------------------------------------------------- */
#define GL_TEXTURE0_ARB                 0x84C0
#define GL_TEXTURE1_ARB                 0x84C1
#define GL_TEXTURE2_ARB                 0x84C2
#define GL_TEXTURE3_ARB                 0x84C3

/* ----------------------------------------------------------------
 * Pixel format
 * ---------------------------------------------------------------- */
#define GL_COLOR_INDEX                  0x1900
#define GL_RED                          0x1903
#define GL_BGR                          0x80E0
#define GL_BGRA                         0x80E1
#define GL_GREEN                        0x1904
#define GL_BLUE                         0x1905

/* ----------------------------------------------------------------
 * GetBoolean / state query
 * ---------------------------------------------------------------- */
#define GL_COLOR_WRITEMASK              0x0C23
#define GL_DEPTH_WRITEMASK              0x0B72

/* ----------------------------------------------------------------
 * Depth / stencil pixel formats
 * ---------------------------------------------------------------- */
#define GL_DEPTH_COMPONENT              0x1902

/* ----------------------------------------------------------------
 * Luminance sized internal formats
 * ---------------------------------------------------------------- */
#define GL_LUMINANCE8                   0x8040
#define GL_LUMINANCE8_ALPHA8            0x8045

/* ----------------------------------------------------------------
 * S3TC (texture compression)
 * ---------------------------------------------------------------- */
#define GL_RGB4_S3TC                    0x83A1

/* ----------------------------------------------------------------
 * EXT_texture_sRGB
 * ---------------------------------------------------------------- */
#define GL_SRGB_EXT                     0x8C40
#define GL_SRGB8_EXT                    0x8C41
#define GL_SRGB_ALPHA_EXT               0x8C42
#define GL_SRGB8_ALPHA8_EXT             0x8C43
#define GL_SLUMINANCE_ALPHA_EXT         0x8C44
#define GL_SLUMINANCE8_ALPHA8_EXT       0x8C45
#define GL_SLUMINANCE_EXT               0x8C46
#define GL_SLUMINANCE8_EXT              0x8C47
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT  0x8C4D
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT  0x8C4F

/* ----------------------------------------------------------------
 * EXT_texture_compression_latc / ARB_texture_compression_bptc
 * ---------------------------------------------------------------- */
#define GL_COMPRESSED_LUMINANCE_ALPHA_LATC2_EXT 0x8C72
#define GL_COMPRESSED_RGBA_BPTC_UNORM_ARB       0x8E8C
#define GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM_ARB 0x8E8D

/* ----------------------------------------------------------------
 * Misc
 * ---------------------------------------------------------------- */
#define GL_GENERATE_MIPMAP              0x8191
#define GL_GENERATE_MIPMAP_HINT         0x8192

/* ----------------------------------------------------------------
 * Fog (values must match ref/gl/gl_export.h)
 * ---------------------------------------------------------------- */
#define GL_FOG_INDEX                    0x0B61
#define GL_FOG_DENSITY                  0x0B62
#define GL_FOG_START                    0x0B63
#define GL_FOG_END                      0x0B64
#define GL_FOG_MODE                     0x0B65
#define GL_FOG_COLOR                    0x0B66
#define GL_FOG_HINT                     0x0C54
#define GL_EXP                          0x0800
#define GL_EXP2                         0x0801

/* ----------------------------------------------------------------
 * Texture coordinate generation
 * ---------------------------------------------------------------- */
#define GL_S                            0x2000
#define GL_T                            0x2001
#define GL_R                            0x2002
#define GL_Q                            0x2003
#define GL_TEXTURE_GEN_S                0x0C60
#define GL_TEXTURE_GEN_T                0x0C61
#define GL_TEXTURE_GEN_R                0x0C62
#define GL_TEXTURE_GEN_Q                0x0C63
#define GL_TEXTURE_GEN_MODE             0x2500
#define GL_EYE_LINEAR                   0x2400
#define GL_OBJECT_LINEAR                0x2401
#define GL_SPHERE_MAP                   0x2402
#define GL_NORMAL_MAP_ARB               0x8511
#define GL_REFLECTION_MAP_ARB           0x8512

#endif /* __GL_H_PS3__ */
