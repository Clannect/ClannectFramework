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
inline constexpr GLboolean GL_FALSE = 0;
inline constexpr GLboolean GL_TRUE = 1;
inline constexpr GLenum GL_NONE = 0;
inline constexpr GLenum GL_NO_ERROR = 0;
// Buffers to clear
inline constexpr GLbitfield GL_DEPTH_BUFFER_BIT = 0x00000100;
inline constexpr GLbitfield GL_STENCIL_BUFFER_BIT = 0x00000400;
inline constexpr GLbitfield GL_COLOR_BUFFER_BIT = 0x00004000;
// Primitives
inline constexpr GLenum GL_POINTS = 0x0000;
inline constexpr GLenum GL_LINES = 0x0001;
inline constexpr GLenum GL_LINE_LOOP = 0x0002;
inline constexpr GLenum GL_LINE_STRIP = 0x0003;
inline constexpr GLenum GL_TRIANGLES = 0x0004;
inline constexpr GLenum GL_TRIANGLE_STRIP = 0x0005;
inline constexpr GLenum GL_TRIANGLE_FAN = 0x0006;
// Depth and blending
inline constexpr GLenum GL_NEVER = 0x0200;
inline constexpr GLenum GL_LESS = 0x0201;
inline constexpr GLenum GL_EQUAL = 0x0202;
inline constexpr GLenum GL_LEQUAL = 0x0203;
inline constexpr GLenum GL_GREATER = 0x0204;
inline constexpr GLenum GL_NOTEQUAL = 0x0205;
inline constexpr GLenum GL_GEQUAL = 0x0206;
inline constexpr GLenum GL_ALWAYS = 0x0207;
inline constexpr GLenum GL_ZERO = 0;
inline constexpr GLenum GL_ONE = 1;
inline constexpr GLenum GL_SRC_COLOR = 0x0300;
inline constexpr GLenum GL_ONE_MINUS_SRC_COLOR = 0x0301;
inline constexpr GLenum GL_SRC_ALPHA = 0x0302;
inline constexpr GLenum GL_ONE_MINUS_SRC_ALPHA = 0x0303;
inline constexpr GLenum GL_DST_ALPHA = 0x0304;
inline constexpr GLenum GL_ONE_MINUS_DST_ALPHA = 0x0305;
inline constexpr GLenum GL_DST_COLOR = 0x0306;
inline constexpr GLenum GL_ONE_MINUS_DST_COLOR = 0x0307;
// Faces
inline constexpr GLenum GL_FRONT = 0x0404;
inline constexpr GLenum GL_BACK = 0x0405;
inline constexpr GLenum GL_FRONT_AND_BACK = 0x0408;
inline constexpr GLenum GL_CW = 0x0900;
inline constexpr GLenum GL_CCW = 0x0901;
// Capabilities
inline constexpr GLenum GL_CULL_FACE = 0x0B44;
inline constexpr GLenum GL_DEPTH_TEST = 0x0B71;
inline constexpr GLenum GL_STENCIL_TEST = 0x0B90;
inline constexpr GLenum GL_BLEND = 0x0BE2;
inline constexpr GLenum GL_SCISSOR_TEST = 0x0C11;
inline constexpr GLenum GL_POLYGON_OFFSET_FILL = 0x8037;
inline constexpr GLenum GL_MULTISAMPLE = 0x809D;
inline constexpr GLenum GL_LINE_SMOOTH = 0x0B20;
inline constexpr GLenum GL_FRAMEBUFFER_SRGB = 0x8DB9;
inline constexpr GLenum GL_PROGRAM_POINT_SIZE = 0x8642;
// Queries
inline constexpr GLenum GL_VIEWPORT = 0x0BA2;
inline constexpr GLenum GL_VENDOR = 0x1F00;
inline constexpr GLenum GL_RENDERER = 0x1F01;
inline constexpr GLenum GL_VERSION = 0x1F02;
inline constexpr GLenum GL_SHADING_LANGUAGE_VERSION = 0x8B8C;
inline constexpr GLenum GL_MAJOR_VERSION = 0x821B;
inline constexpr GLenum GL_MINOR_VERSION = 0x821C;
inline constexpr GLenum GL_MAX_TEXTURE_SIZE = 0x0D33;
inline constexpr GLenum GL_MAX_SAMPLES = 0x8D57;
// Types
inline constexpr GLenum GL_BYTE = 0x1400;
inline constexpr GLenum GL_UNSIGNED_BYTE = 0x1401;
inline constexpr GLenum GL_SHORT = 0x1402;
inline constexpr GLenum GL_UNSIGNED_SHORT = 0x1403;
inline constexpr GLenum GL_INT = 0x1404;
inline constexpr GLenum GL_UNSIGNED_INT = 0x1405;
inline constexpr GLenum GL_FLOAT = 0x1406;
inline constexpr GLenum GL_HALF_FLOAT = 0x140B;
inline constexpr GLenum GL_UNSIGNED_INT_24_8 = 0x84FA;
// Pixel formats
inline constexpr GLenum GL_DEPTH_COMPONENT = 0x1902;
inline constexpr GLenum GL_RED = 0x1903;
inline constexpr GLenum GL_ALPHA = 0x1906;
inline constexpr GLenum GL_RGB = 0x1907;
inline constexpr GLenum GL_RGBA = 0x1908;
inline constexpr GLenum GL_BGRA = 0x80E1;
inline constexpr GLenum GL_RG = 0x8227;
inline constexpr GLenum GL_R8 = 0x8229;
inline constexpr GLenum GL_RGB8 = 0x8051;
inline constexpr GLenum GL_RGBA8 = 0x8058;
inline constexpr GLenum GL_SRGB8_ALPHA8 = 0x8C43;
inline constexpr GLenum GL_RGBA16F = 0x881A;
inline constexpr GLenum GL_DEPTH_COMPONENT16 = 0x81A5;
inline constexpr GLenum GL_DEPTH_COMPONENT24 = 0x81A6;
inline constexpr GLenum GL_DEPTH_COMPONENT32F = 0x8CAC;
inline constexpr GLenum GL_DEPTH_STENCIL = 0x84F9;
inline constexpr GLenum GL_DEPTH24_STENCIL8 = 0x88F0;
inline constexpr GLenum GL_UNPACK_ALIGNMENT = 0x0CF5;
inline constexpr GLenum GL_PACK_ALIGNMENT = 0x0D05;
inline constexpr GLenum GL_UNPACK_ROW_LENGTH = 0x0CF2;
// Textures
inline constexpr GLenum GL_TEXTURE_2D = 0x0DE1;
inline constexpr GLenum GL_TEXTURE_2D_MULTISAMPLE = 0x9100;
inline constexpr GLenum GL_TEXTURE_CUBE_MAP = 0x8513;
inline constexpr GLenum GL_TEXTURE_MAG_FILTER = 0x2800;
inline constexpr GLenum GL_TEXTURE_MIN_FILTER = 0x2801;
inline constexpr GLenum GL_TEXTURE_WRAP_S = 0x2802;
inline constexpr GLenum GL_TEXTURE_WRAP_T = 0x2803;
inline constexpr GLenum GL_TEXTURE_WRAP_R = 0x8072;
inline constexpr GLenum GL_TEXTURE_BORDER_COLOR = 0x1004;
inline constexpr GLenum GL_TEXTURE_COMPARE_MODE = 0x884C;
inline constexpr GLenum GL_TEXTURE_COMPARE_FUNC = 0x884D;
inline constexpr GLenum GL_COMPARE_REF_TO_TEXTURE = 0x884E;
inline constexpr GLenum GL_TEXTURE_MAX_LEVEL = 0x813D;
inline constexpr GLenum GL_NEAREST = 0x2600;
inline constexpr GLenum GL_LINEAR = 0x2601;
inline constexpr GLenum GL_NEAREST_MIPMAP_NEAREST = 0x2700;
inline constexpr GLenum GL_LINEAR_MIPMAP_NEAREST = 0x2701;
inline constexpr GLenum GL_NEAREST_MIPMAP_LINEAR = 0x2702;
inline constexpr GLenum GL_LINEAR_MIPMAP_LINEAR = 0x2703;
inline constexpr GLenum GL_REPEAT = 0x2901;
inline constexpr GLenum GL_CLAMP_TO_EDGE = 0x812F;
inline constexpr GLenum GL_CLAMP_TO_BORDER = 0x812D;
inline constexpr GLenum GL_MIRRORED_REPEAT = 0x8370;
inline constexpr GLenum GL_TEXTURE0 = 0x84C0;
// Buffers
inline constexpr GLenum GL_ARRAY_BUFFER = 0x8892;
inline constexpr GLenum GL_ELEMENT_ARRAY_BUFFER = 0x8893;
inline constexpr GLenum GL_UNIFORM_BUFFER = 0x8A11;
inline constexpr GLenum GL_STREAM_DRAW = 0x88E0;
inline constexpr GLenum GL_STATIC_DRAW = 0x88E4;
inline constexpr GLenum GL_DYNAMIC_DRAW = 0x88E8;
// Shaders
inline constexpr GLenum GL_FRAGMENT_SHADER = 0x8B30;
inline constexpr GLenum GL_VERTEX_SHADER = 0x8B31;
inline constexpr GLenum GL_GEOMETRY_SHADER = 0x8DD9;
inline constexpr GLenum GL_COMPILE_STATUS = 0x8B81;
inline constexpr GLenum GL_LINK_STATUS = 0x8B82;
inline constexpr GLenum GL_INFO_LOG_LENGTH = 0x8B84;
// Framebuffers
inline constexpr GLenum GL_FRAMEBUFFER = 0x8D40;
inline constexpr GLenum GL_READ_FRAMEBUFFER = 0x8CA8;
inline constexpr GLenum GL_DRAW_FRAMEBUFFER = 0x8CA9;
inline constexpr GLenum GL_RENDERBUFFER = 0x8D41;
inline constexpr GLenum GL_COLOR_ATTACHMENT0 = 0x8CE0;
inline constexpr GLenum GL_DEPTH_ATTACHMENT = 0x8D00;
inline constexpr GLenum GL_STENCIL_ATTACHMENT = 0x8D20;
inline constexpr GLenum GL_DEPTH_STENCIL_ATTACHMENT = 0x821A;
inline constexpr GLenum GL_FRAMEBUFFER_COMPLETE = 0x8CD5;
inline constexpr GLenum GL_FRONT_LEFT = 0x0400;
inline constexpr GLenum GL_BACK_LEFT = 0x0402;

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
    [[nodiscard]] bool loaded() const noexcept { return Clear != nullptr; }
};

} // namespace cfw
