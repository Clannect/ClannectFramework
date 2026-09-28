#pragma once

// OpenGL 3.3 core for CFW: the types, the constants the engine uses, and a
// table of function pointers loaded from a GlContext. Written for CFW, so no
// third-party GL headers or loaders are needed; the functions come from the
// system's OpenGL driver at run time.
//
//     GlFunctions gl;
//     if (!gl.load(context)) { ... }
//     gl.ClearColor(0, 0, 0, 1);
//     gl.Clear(GL_COLOR_BUFFER_BIT);
//
// Threads: a table is valid on any thread where its context (or one sharing
// with it) is current.

#include <cstddef>
#include <cstdint>
#include <functional>

#if defined(_WIN32)
#define CFW_GL_API __stdcall
#else
#define CFW_GL_API
#endif

namespace cfw {

class GlContext;

using GLenum = unsigned int;
using GLboolean = unsigned char;
using GLbitfield = unsigned int;
using GLbyte = std::int8_t;
using GLubyte = std::uint8_t;
using GLshort = std::int16_t;
using GLushort = std::uint16_t;
using GLint = int;
using GLuint = unsigned int;
using GLsizei = int;
using GLfloat = float;
using GLclampf = float;
using GLdouble = double;
using GLchar = char;
using GLintptr = std::ptrdiff_t;
using GLsizeiptr = std::ptrdiff_t;

// ---- Constants ----------------------------------------------------------------------
// Each is skipped when a system GL header (or a toolkit's, such as Qt's) has
// already defined it as a macro: the values are fixed by the OpenGL
// specification, so both spellings agree.
#ifndef GL_FALSE
inline constexpr GLboolean GL_FALSE = 0;
#endif
#ifndef GL_TRUE
inline constexpr GLboolean GL_TRUE = 1;
#endif
#ifndef GL_NONE
inline constexpr GLenum GL_NONE = 0;
#endif
#ifndef GL_NO_ERROR
inline constexpr GLenum GL_NO_ERROR = 0;
#endif
// Buffers to clear
#ifndef GL_DEPTH_BUFFER_BIT
inline constexpr GLbitfield GL_DEPTH_BUFFER_BIT = 0x00000100;
#endif
#ifndef GL_STENCIL_BUFFER_BIT
inline constexpr GLbitfield GL_STENCIL_BUFFER_BIT = 0x00000400;
#endif
#ifndef GL_COLOR_BUFFER_BIT
inline constexpr GLbitfield GL_COLOR_BUFFER_BIT = 0x00004000;
#endif
// Primitives
#ifndef GL_POINTS
inline constexpr GLenum GL_POINTS = 0x0000;
#endif
#ifndef GL_LINES
inline constexpr GLenum GL_LINES = 0x0001;
#endif
#ifndef GL_LINE_LOOP
inline constexpr GLenum GL_LINE_LOOP = 0x0002;
#endif
#ifndef GL_LINE_STRIP
inline constexpr GLenum GL_LINE_STRIP = 0x0003;
#endif
#ifndef GL_TRIANGLES
inline constexpr GLenum GL_TRIANGLES = 0x0004;
#endif
#ifndef GL_TRIANGLE_STRIP
inline constexpr GLenum GL_TRIANGLE_STRIP = 0x0005;
#endif
#ifndef GL_TRIANGLE_FAN
inline constexpr GLenum GL_TRIANGLE_FAN = 0x0006;
#endif
// Depth and blending
#ifndef GL_NEVER
inline constexpr GLenum GL_NEVER = 0x0200;
#endif
#ifndef GL_LESS
inline constexpr GLenum GL_LESS = 0x0201;
#endif
#ifndef GL_EQUAL
inline constexpr GLenum GL_EQUAL = 0x0202;
#endif
#ifndef GL_LEQUAL
inline constexpr GLenum GL_LEQUAL = 0x0203;
#endif
#ifndef GL_GREATER
inline constexpr GLenum GL_GREATER = 0x0204;
#endif
#ifndef GL_NOTEQUAL
inline constexpr GLenum GL_NOTEQUAL = 0x0205;
#endif
#ifndef GL_GEQUAL
inline constexpr GLenum GL_GEQUAL = 0x0206;
#endif
#ifndef GL_ALWAYS
inline constexpr GLenum GL_ALWAYS = 0x0207;
#endif
#ifndef GL_ZERO
inline constexpr GLenum GL_ZERO = 0;
#endif
#ifndef GL_ONE
inline constexpr GLenum GL_ONE = 1;
#endif
#ifndef GL_SRC_COLOR
inline constexpr GLenum GL_SRC_COLOR = 0x0300;
#endif
#ifndef GL_ONE_MINUS_SRC_COLOR
inline constexpr GLenum GL_ONE_MINUS_SRC_COLOR = 0x0301;
#endif
#ifndef GL_SRC_ALPHA
inline constexpr GLenum GL_SRC_ALPHA = 0x0302;
#endif
#ifndef GL_ONE_MINUS_SRC_ALPHA
inline constexpr GLenum GL_ONE_MINUS_SRC_ALPHA = 0x0303;
#endif
#ifndef GL_DST_ALPHA
inline constexpr GLenum GL_DST_ALPHA = 0x0304;
#endif
#ifndef GL_ONE_MINUS_DST_ALPHA
inline constexpr GLenum GL_ONE_MINUS_DST_ALPHA = 0x0305;
#endif
#ifndef GL_DST_COLOR
inline constexpr GLenum GL_DST_COLOR = 0x0306;
#endif
#ifndef GL_ONE_MINUS_DST_COLOR
inline constexpr GLenum GL_ONE_MINUS_DST_COLOR = 0x0307;
#endif
// Faces
#ifndef GL_FRONT
inline constexpr GLenum GL_FRONT = 0x0404;
#endif
#ifndef GL_BACK
inline constexpr GLenum GL_BACK = 0x0405;
#endif
#ifndef GL_FRONT_AND_BACK
inline constexpr GLenum GL_FRONT_AND_BACK = 0x0408;
#endif
#ifndef GL_CW
inline constexpr GLenum GL_CW = 0x0900;
#endif
#ifndef GL_CCW
inline constexpr GLenum GL_CCW = 0x0901;
#endif
// Capabilities
#ifndef GL_CULL_FACE
inline constexpr GLenum GL_CULL_FACE = 0x0B44;
#endif
#ifndef GL_DEPTH_TEST
inline constexpr GLenum GL_DEPTH_TEST = 0x0B71;
#endif
#ifndef GL_STENCIL_TEST
inline constexpr GLenum GL_STENCIL_TEST = 0x0B90;
#endif
#ifndef GL_BLEND
inline constexpr GLenum GL_BLEND = 0x0BE2;
#endif
#ifndef GL_SCISSOR_TEST
inline constexpr GLenum GL_SCISSOR_TEST = 0x0C11;
#endif
#ifndef GL_POLYGON_OFFSET_FILL
inline constexpr GLenum GL_POLYGON_OFFSET_FILL = 0x8037;
#endif
#ifndef GL_MULTISAMPLE
inline constexpr GLenum GL_MULTISAMPLE = 0x809D;
#endif
#ifndef GL_LINE_SMOOTH
inline constexpr GLenum GL_LINE_SMOOTH = 0x0B20;
#endif
#ifndef GL_FRAMEBUFFER_SRGB
inline constexpr GLenum GL_FRAMEBUFFER_SRGB = 0x8DB9;
#endif
#ifndef GL_PROGRAM_POINT_SIZE
inline constexpr GLenum GL_PROGRAM_POINT_SIZE = 0x8642;
#endif
// Queries
#ifndef GL_VIEWPORT
inline constexpr GLenum GL_VIEWPORT = 0x0BA2;
#endif
#ifndef GL_VENDOR
inline constexpr GLenum GL_VENDOR = 0x1F00;
#endif
#ifndef GL_RENDERER
inline constexpr GLenum GL_RENDERER = 0x1F01;
#endif
#ifndef GL_VERSION
inline constexpr GLenum GL_VERSION = 0x1F02;
#endif
#ifndef GL_SHADING_LANGUAGE_VERSION
inline constexpr GLenum GL_SHADING_LANGUAGE_VERSION = 0x8B8C;
#endif
#ifndef GL_MAJOR_VERSION
inline constexpr GLenum GL_MAJOR_VERSION = 0x821B;
#endif
#ifndef GL_MINOR_VERSION
inline constexpr GLenum GL_MINOR_VERSION = 0x821C;
#endif
#ifndef GL_MAX_TEXTURE_SIZE
inline constexpr GLenum GL_MAX_TEXTURE_SIZE = 0x0D33;
#endif
#ifndef GL_MAX_SAMPLES
inline constexpr GLenum GL_MAX_SAMPLES = 0x8D57;
#endif
// Types
#ifndef GL_BYTE
inline constexpr GLenum GL_BYTE = 0x1400;
#endif
#ifndef GL_UNSIGNED_BYTE
inline constexpr GLenum GL_UNSIGNED_BYTE = 0x1401;
#endif
#ifndef GL_SHORT
inline constexpr GLenum GL_SHORT = 0x1402;
#endif
#ifndef GL_UNSIGNED_SHORT
inline constexpr GLenum GL_UNSIGNED_SHORT = 0x1403;
#endif
#ifndef GL_INT
inline constexpr GLenum GL_INT = 0x1404;
#endif
#ifndef GL_UNSIGNED_INT
inline constexpr GLenum GL_UNSIGNED_INT = 0x1405;
#endif
#ifndef GL_FLOAT
inline constexpr GLenum GL_FLOAT = 0x1406;
#endif
#ifndef GL_HALF_FLOAT
inline constexpr GLenum GL_HALF_FLOAT = 0x140B;
#endif
#ifndef GL_UNSIGNED_INT_24_8
inline constexpr GLenum GL_UNSIGNED_INT_24_8 = 0x84FA;
#endif
// Pixel formats
#ifndef GL_DEPTH_COMPONENT
inline constexpr GLenum GL_DEPTH_COMPONENT = 0x1902;
#endif
#ifndef GL_RED
inline constexpr GLenum GL_RED = 0x1903;
#endif
#ifndef GL_ALPHA
inline constexpr GLenum GL_ALPHA = 0x1906;
#endif
#ifndef GL_RGB
inline constexpr GLenum GL_RGB = 0x1907;
#endif
#ifndef GL_RGBA
inline constexpr GLenum GL_RGBA = 0x1908;
#endif
#ifndef GL_BGRA
inline constexpr GLenum GL_BGRA = 0x80E1;
#endif
#ifndef GL_RG
inline constexpr GLenum GL_RG = 0x8227;
#endif
#ifndef GL_R8
inline constexpr GLenum GL_R8 = 0x8229;
#endif
#ifndef GL_RGB8
inline constexpr GLenum GL_RGB8 = 0x8051;
#endif
#ifndef GL_RGBA8
inline constexpr GLenum GL_RGBA8 = 0x8058;
#endif
#ifndef GL_SRGB8_ALPHA8
inline constexpr GLenum GL_SRGB8_ALPHA8 = 0x8C43;
#endif
#ifndef GL_RGBA16F
inline constexpr GLenum GL_RGBA16F = 0x881A;
#endif
#ifndef GL_DEPTH_COMPONENT16
inline constexpr GLenum GL_DEPTH_COMPONENT16 = 0x81A5;
#endif
#ifndef GL_DEPTH_COMPONENT24
inline constexpr GLenum GL_DEPTH_COMPONENT24 = 0x81A6;
#endif
#ifndef GL_DEPTH_COMPONENT32F
inline constexpr GLenum GL_DEPTH_COMPONENT32F = 0x8CAC;
#endif
#ifndef GL_DEPTH_STENCIL
inline constexpr GLenum GL_DEPTH_STENCIL = 0x84F9;
#endif
#ifndef GL_DEPTH24_STENCIL8
inline constexpr GLenum GL_DEPTH24_STENCIL8 = 0x88F0;
#endif
#ifndef GL_UNPACK_ALIGNMENT
inline constexpr GLenum GL_UNPACK_ALIGNMENT = 0x0CF5;
#endif
#ifndef GL_PACK_ALIGNMENT
inline constexpr GLenum GL_PACK_ALIGNMENT = 0x0D05;
#endif
#ifndef GL_UNPACK_ROW_LENGTH
inline constexpr GLenum GL_UNPACK_ROW_LENGTH = 0x0CF2;
#endif
// Textures
#ifndef GL_TEXTURE_2D
inline constexpr GLenum GL_TEXTURE_2D = 0x0DE1;
#endif
#ifndef GL_TEXTURE_2D_MULTISAMPLE
inline constexpr GLenum GL_TEXTURE_2D_MULTISAMPLE = 0x9100;
#endif
#ifndef GL_TEXTURE_CUBE_MAP
inline constexpr GLenum GL_TEXTURE_CUBE_MAP = 0x8513;
#endif
#ifndef GL_TEXTURE_MAG_FILTER
inline constexpr GLenum GL_TEXTURE_MAG_FILTER = 0x2800;
#endif
#ifndef GL_TEXTURE_MIN_FILTER
inline constexpr GLenum GL_TEXTURE_MIN_FILTER = 0x2801;
#endif
#ifndef GL_TEXTURE_WRAP_S
inline constexpr GLenum GL_TEXTURE_WRAP_S = 0x2802;
#endif
#ifndef GL_TEXTURE_WRAP_T
inline constexpr GLenum GL_TEXTURE_WRAP_T = 0x2803;
#endif
#ifndef GL_TEXTURE_WRAP_R
inline constexpr GLenum GL_TEXTURE_WRAP_R = 0x8072;
#endif
#ifndef GL_TEXTURE_BORDER_COLOR
inline constexpr GLenum GL_TEXTURE_BORDER_COLOR = 0x1004;
#endif
#ifndef GL_TEXTURE_COMPARE_MODE
inline constexpr GLenum GL_TEXTURE_COMPARE_MODE = 0x884C;
#endif
#ifndef GL_TEXTURE_COMPARE_FUNC
inline constexpr GLenum GL_TEXTURE_COMPARE_FUNC = 0x884D;
#endif
#ifndef GL_COMPARE_REF_TO_TEXTURE
inline constexpr GLenum GL_COMPARE_REF_TO_TEXTURE = 0x884E;
#endif
#ifndef GL_TEXTURE_MAX_LEVEL
inline constexpr GLenum GL_TEXTURE_MAX_LEVEL = 0x813D;
#endif
#ifndef GL_NEAREST
inline constexpr GLenum GL_NEAREST = 0x2600;
#endif
#ifndef GL_LINEAR
inline constexpr GLenum GL_LINEAR = 0x2601;
#endif
#ifndef GL_NEAREST_MIPMAP_NEAREST
inline constexpr GLenum GL_NEAREST_MIPMAP_NEAREST = 0x2700;
#endif
#ifndef GL_LINEAR_MIPMAP_NEAREST
inline constexpr GLenum GL_LINEAR_MIPMAP_NEAREST = 0x2701;
#endif
#ifndef GL_NEAREST_MIPMAP_LINEAR
inline constexpr GLenum GL_NEAREST_MIPMAP_LINEAR = 0x2702;
#endif
#ifndef GL_LINEAR_MIPMAP_LINEAR
inline constexpr GLenum GL_LINEAR_MIPMAP_LINEAR = 0x2703;
#endif
#ifndef GL_REPEAT
inline constexpr GLenum GL_REPEAT = 0x2901;
#endif
#ifndef GL_CLAMP_TO_EDGE
inline constexpr GLenum GL_CLAMP_TO_EDGE = 0x812F;
#endif
#ifndef GL_CLAMP_TO_BORDER
inline constexpr GLenum GL_CLAMP_TO_BORDER = 0x812D;
#endif
#ifndef GL_MIRRORED_REPEAT
inline constexpr GLenum GL_MIRRORED_REPEAT = 0x8370;
#endif
#ifndef GL_TEXTURE0
inline constexpr GLenum GL_TEXTURE0 = 0x84C0;
#endif
// Buffers
#ifndef GL_ARRAY_BUFFER
inline constexpr GLenum GL_ARRAY_BUFFER = 0x8892;
#endif
#ifndef GL_ELEMENT_ARRAY_BUFFER
inline constexpr GLenum GL_ELEMENT_ARRAY_BUFFER = 0x8893;
#endif
#ifndef GL_UNIFORM_BUFFER
inline constexpr GLenum GL_UNIFORM_BUFFER = 0x8A11;
#endif
#ifndef GL_STREAM_DRAW
inline constexpr GLenum GL_STREAM_DRAW = 0x88E0;
#endif
#ifndef GL_STATIC_DRAW
inline constexpr GLenum GL_STATIC_DRAW = 0x88E4;
#endif
#ifndef GL_DYNAMIC_DRAW
inline constexpr GLenum GL_DYNAMIC_DRAW = 0x88E8;
#endif
// Shaders
#ifndef GL_FRAGMENT_SHADER
inline constexpr GLenum GL_FRAGMENT_SHADER = 0x8B30;
#endif
#ifndef GL_VERTEX_SHADER
inline constexpr GLenum GL_VERTEX_SHADER = 0x8B31;
#endif
#ifndef GL_GEOMETRY_SHADER
inline constexpr GLenum GL_GEOMETRY_SHADER = 0x8DD9;
#endif
#ifndef GL_COMPILE_STATUS
inline constexpr GLenum GL_COMPILE_STATUS = 0x8B81;
#endif
#ifndef GL_LINK_STATUS
inline constexpr GLenum GL_LINK_STATUS = 0x8B82;
#endif
#ifndef GL_INFO_LOG_LENGTH
inline constexpr GLenum GL_INFO_LOG_LENGTH = 0x8B84;
#endif
// Framebuffers
#ifndef GL_FRAMEBUFFER
inline constexpr GLenum GL_FRAMEBUFFER = 0x8D40;
#endif
#ifndef GL_READ_FRAMEBUFFER
inline constexpr GLenum GL_READ_FRAMEBUFFER = 0x8CA8;
#endif
#ifndef GL_DRAW_FRAMEBUFFER
inline constexpr GLenum GL_DRAW_FRAMEBUFFER = 0x8CA9;
#endif
#ifndef GL_RENDERBUFFER
inline constexpr GLenum GL_RENDERBUFFER = 0x8D41;
#endif
#ifndef GL_COLOR_ATTACHMENT0
inline constexpr GLenum GL_COLOR_ATTACHMENT0 = 0x8CE0;
#endif
#ifndef GL_DEPTH_ATTACHMENT
inline constexpr GLenum GL_DEPTH_ATTACHMENT = 0x8D00;
#endif
#ifndef GL_STENCIL_ATTACHMENT
inline constexpr GLenum GL_STENCIL_ATTACHMENT = 0x8D20;
#endif
#ifndef GL_DEPTH_STENCIL_ATTACHMENT
inline constexpr GLenum GL_DEPTH_STENCIL_ATTACHMENT = 0x821A;
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
inline constexpr GLenum GL_FRAMEBUFFER_COMPLETE = 0x8CD5;
#endif
#ifndef GL_FRONT_LEFT
inline constexpr GLenum GL_FRONT_LEFT = 0x0400;
#endif
#ifndef GL_BACK_LEFT
inline constexpr GLenum GL_BACK_LEFT = 0x0402;
#endif

// ---- Functions ----------------------------------------------------------------------
// X(return type, name without the "gl" prefix, parameter list)
#define CFW_GL_FUNCTIONS(X)                                                                                     \
    X(void, ActiveTexture, (GLenum texture))                                                                   \
    X(void, AttachShader, (GLuint program, GLuint shader))                                                     \
    X(void, BindAttribLocation, (GLuint program, GLuint index, const GLchar *name))                            \
    X(void, BindBuffer, (GLenum target, GLuint buffer))                                                        \
    X(void, BindFramebuffer, (GLenum target, GLuint framebuffer))                                              \
    X(void, BindRenderbuffer, (GLenum target, GLuint renderbuffer))                                            \
    X(void, BindTexture, (GLenum target, GLuint texture))                                                      \
    X(void, BindVertexArray, (GLuint array))                                                                   \
    X(void, BlendFunc, (GLenum sfactor, GLenum dfactor))                                                       \
    X(void, BlendFuncSeparate, (GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha))               \
    X(void, BlitFramebuffer, (GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0,    \
                              GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter))                       \
    X(void, BufferData, (GLenum target, GLsizeiptr size, const void *data, GLenum usage))                      \
    X(void, BufferSubData, (GLenum target, GLintptr offset, GLsizeiptr size, const void *data))                \
    X(GLenum, CheckFramebufferStatus, (GLenum target))                                                         \
    X(void, Clear, (GLbitfield mask))                                                                          \
    X(void, ClearColor, (GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha))                             \
    X(void, ClearDepth, (GLdouble depth))                                                                      \
    X(void, ColorMask, (GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha))                      \
    X(void, CompileShader, (GLuint shader))                                                                    \
    X(GLuint, CreateProgram, ())                                                                               \
    X(GLuint, CreateShader, (GLenum type))                                                                     \
    X(void, CullFace, (GLenum mode))                                                                           \
    X(void, DeleteBuffers, (GLsizei n, const GLuint *buffers))                                                 \
    X(void, DeleteFramebuffers, (GLsizei n, const GLuint *framebuffers))                                       \
    X(void, DeleteProgram, (GLuint program))                                                                   \
    X(void, DeleteRenderbuffers, (GLsizei n, const GLuint *renderbuffers))                                     \
    X(void, DeleteShader, (GLuint shader))                                                                     \
    X(void, DeleteTextures, (GLsizei n, const GLuint *textures))                                               \
    X(void, DeleteVertexArrays, (GLsizei n, const GLuint *arrays))                                             \
    X(void, DepthFunc, (GLenum func))                                                                          \
    X(void, DepthMask, (GLboolean flag))                                                                       \
    X(void, Disable, (GLenum cap))                                                                             \
    X(void, DisableVertexAttribArray, (GLuint index))                                                          \
    X(void, DrawArrays, (GLenum mode, GLint first, GLsizei count))                                             \
    X(void, DrawArraysInstanced, (GLenum mode, GLint first, GLsizei count, GLsizei instances))                 \
    X(void, DrawBuffer, (GLenum buf))                                                                          \
    X(void, DrawBuffers, (GLsizei n, const GLenum *bufs))                                                      \
    X(void, DrawElements, (GLenum mode, GLsizei count, GLenum type, const void *indices))                      \
    X(void, DrawElementsInstanced, (GLenum mode, GLsizei count, GLenum type, const void *indices,              \
                                    GLsizei instances))                                                        \
    X(void, Enable, (GLenum cap))                                                                              \
    X(void, EnableVertexAttribArray, (GLuint index))                                                           \
    X(void, Finish, ())                                                                                        \
    X(void, Flush, ())                                                                                         \
    X(void, FramebufferRenderbuffer, (GLenum target, GLenum attachment, GLenum rbtarget, GLuint rb))           \
    X(void, FramebufferTexture2D, (GLenum target, GLenum attachment, GLenum textarget, GLuint texture,         \
                                   GLint level))                                                               \
    X(void, FrontFace, (GLenum mode))                                                                          \
    X(void, GenBuffers, (GLsizei n, GLuint *buffers))                                                          \
    X(void, GenFramebuffers, (GLsizei n, GLuint *framebuffers))                                                \
    X(void, GenRenderbuffers, (GLsizei n, GLuint *renderbuffers))                                              \
    X(void, GenTextures, (GLsizei n, GLuint *textures))                                                        \
    X(void, GenVertexArrays, (GLsizei n, GLuint *arrays))                                                      \
    X(void, GenerateMipmap, (GLenum target))                                                                   \
    X(GLint, GetAttribLocation, (GLuint program, const GLchar *name))                                          \
    X(GLenum, GetError, ())                                                                                    \
    X(void, GetFloatv, (GLenum pname, GLfloat *data))                                                          \
    X(void, GetIntegerv, (GLenum pname, GLint *data))                                                          \
    X(void, GetProgramInfoLog, (GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog))            \
    X(void, GetProgramiv, (GLuint program, GLenum pname, GLint *params))                                       \
    X(void, GetShaderInfoLog, (GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog))              \
    X(void, GetShaderiv, (GLuint shader, GLenum pname, GLint *params))                                         \
    X(const GLubyte *, GetString, (GLenum name))                                                               \
    X(GLint, GetUniformLocation, (GLuint program, const GLchar *name))                                         \
    X(void, LineWidth, (GLfloat width))                                                                        \
    X(void, LinkProgram, (GLuint program))                                                                     \
    X(void, PixelStorei, (GLenum pname, GLint param))                                                          \
    X(void, PolygonOffset, (GLfloat factor, GLfloat units))                                                    \
    X(void, ReadBuffer, (GLenum src))                                                                          \
    X(void, ReadPixels, (GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type,          \
                         void *pixels))                                                                        \
    X(void, RenderbufferStorage, (GLenum target, GLenum internalformat, GLsizei width, GLsizei height))        \
    X(void, RenderbufferStorageMultisample, (GLenum target, GLsizei samples, GLenum internalformat,            \
                                             GLsizei width, GLsizei height))                                   \
    X(void, Scissor, (GLint x, GLint y, GLsizei width, GLsizei height))                                        \
    X(void, ShaderSource, (GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length))    \
    X(void, TexImage2D, (GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,      \
                         GLint border, GLenum format, GLenum type, const void *pixels))                        \
    X(void, TexParameterf, (GLenum target, GLenum pname, GLfloat param))                                      \
    X(void, TexParameterfv, (GLenum target, GLenum pname, const GLfloat *params))                              \
    X(void, TexParameteri, (GLenum target, GLenum pname, GLint param))                                         \
    X(void, TexSubImage2D, (GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,          \
                            GLsizei height, GLenum format, GLenum type, const void *pixels))                   \
    X(void, Uniform1f, (GLint location, GLfloat v0))                                                           \
    X(void, Uniform1i, (GLint location, GLint v0))                                                             \
    X(void, Uniform2f, (GLint location, GLfloat v0, GLfloat v1))                                               \
    X(void, Uniform3f, (GLint location, GLfloat v0, GLfloat v1, GLfloat v2))                                   \
    X(void, Uniform4f, (GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3))                       \
    X(void, Uniform1fv, (GLint location, GLsizei count, const GLfloat *value))                                 \
    X(void, Uniform3fv, (GLint location, GLsizei count, const GLfloat *value))                                 \
    X(void, Uniform4fv, (GLint location, GLsizei count, const GLfloat *value))                                 \
    X(void, UniformMatrix3fv, (GLint location, GLsizei count, GLboolean transpose, const GLfloat *value))      \
    X(void, UniformMatrix4fv, (GLint location, GLsizei count, GLboolean transpose, const GLfloat *value))      \
    X(void, UseProgram, (GLuint program))                                                                      \
    X(void, VertexAttribDivisor, (GLuint index, GLuint divisor))                                               \
    X(void, VertexAttribIPointer, (GLuint index, GLint size, GLenum type, GLsizei stride, const void *pointer))\
    X(void, VertexAttribPointer, (GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, \
                                  const void *pointer))                                                        \
    X(void, Viewport, (GLint x, GLint y, GLsizei width, GLsizei height))

// The functions of one context.
struct GlFunctions {
#define CFW_GL_DECLARE(ret, name, params) ret(CFW_GL_API *name) params = nullptr;
    CFW_GL_FUNCTIONS(CFW_GL_DECLARE)
#undef CFW_GL_DECLARE

    // Loads every function from `context` (which must be current). Returns
    // false, naming the first missing function in `missing`, if the driver
    // lacks one.
    bool load(const GlContext &context, const char **missing = nullptr);
    // From any resolver of GL entry points ("glClear" -> address): a context
    // made by another toolkit (QOpenGLContext::getProcAddress during the
    // editor's port), an embedding, a test.
    bool load(const std::function<void *(const char *name)> &resolve, const char **missing = nullptr);
    [[nodiscard]] bool loaded() const noexcept { return Clear != nullptr; }
};

} // namespace cfw
