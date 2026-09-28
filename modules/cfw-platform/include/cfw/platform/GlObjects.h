#pragma once

// Thin owners of GL objects, shaped like the Qt classes the engine's
// viewport used (QOpenGLBuffer, QOpenGLVertexArrayObject,
// QOpenGLShaderProgram) so porting it is mechanical. Each takes the
// GlFunctions of the current context; objects must be destroyed while a
// context sharing them is current (destroy() does it explicitly).
//
// Threads: the thread whose context is current.

#include <string>
#include <unordered_map>

#include "cfw/core/Color.h"
#include "cfw/core/Mat3.h"
#include "cfw/core/Mat4.h"
#include "cfw/core/String.h"
#include "cfw/core/Vec2.h"
#include "cfw/core/Vec3.h"
#include "cfw/core/Vec4.h"
#include "cfw/platform/Gl.h"

namespace cfw {

class GlBuffer {
public:
    enum class Type : GLenum { Vertex = GL_ARRAY_BUFFER, Index = GL_ELEMENT_ARRAY_BUFFER };
    explicit GlBuffer(Type type = Type::Vertex) noexcept : m_type(type) {}
    ~GlBuffer() = default; // GL objects are released by destroy(), which needs the functions
    GlBuffer(const GlBuffer &) = delete;
    GlBuffer &operator=(const GlBuffer &) = delete;

    bool create(const GlFunctions &gl);
    void destroy();
    [[nodiscard]] bool isCreated() const noexcept { return m_id != 0; }
    void bind();
    void release();
    // Replaces the contents (the buffer must be bound).
    void allocate(const void *data, std::size_t bytes);
    void allocate(std::size_t bytes) { allocate(nullptr, bytes); }
    void write(std::size_t offset, const void *data, std::size_t bytes);
    void setUsage(GLenum usage) noexcept { m_usage = usage; }
    [[nodiscard]] std::size_t size() const noexcept { return m_size; }
    [[nodiscard]] GLuint id() const noexcept { return m_id; }

private:
    const GlFunctions *m_gl = nullptr;
    Type m_type;
    GLenum m_usage = GL_STATIC_DRAW;
    GLuint m_id = 0;
    std::size_t m_size = 0;
};

class GlVertexArray {
public:
    GlVertexArray() = default;
    GlVertexArray(const GlVertexArray &) = delete;
    GlVertexArray &operator=(const GlVertexArray &) = delete;

    bool create(const GlFunctions &gl);
    void destroy();
    [[nodiscard]] bool isCreated() const noexcept { return m_id != 0; }
    void bind();
    void release();
    [[nodiscard]] GLuint id() const noexcept { return m_id; }

    // Binds for a scope (QOpenGLVertexArrayObject::Binder).
    class Binder {
    public:
        explicit Binder(GlVertexArray &array) : m_array(array) { m_array.bind(); }
        ~Binder() { m_array.release(); }
        Binder(const Binder &) = delete;
        Binder &operator=(const Binder &) = delete;

    private:
        GlVertexArray &m_array;
    };

private:
    const GlFunctions *m_gl = nullptr;
    GLuint m_id = 0;
};

class GlShaderProgram {
public:
    enum class Stage : GLenum { Vertex = GL_VERTEX_SHADER, Fragment = GL_FRAGMENT_SHADER, Geometry = GL_GEOMETRY_SHADER };
    GlShaderProgram() = default;
    GlShaderProgram(const GlShaderProgram &) = delete;
    GlShaderProgram &operator=(const GlShaderProgram &) = delete;

    // Compiles a stage; false with the compiler's message in log().
    bool addShaderFromSourceCode(const GlFunctions &gl, Stage stage, const char *source);
    void bindAttributeLocation(const char *name, GLuint location);
    bool link();
    void destroy();
    [[nodiscard]] bool isLinked() const noexcept { return m_linked; }
    bool bind();
    void release();
    [[nodiscard]] const String &log() const noexcept { return m_log; }
    [[nodiscard]] GLuint programId() const noexcept { return m_program; }

    // Locations are looked up once and cached; -1 (not in the program, or
    // optimised away) is accepted silently by the setters, as in Qt.
    [[nodiscard]] GLint uniformLocation(const char *name);
    [[nodiscard]] GLint attributeLocation(const char *name) const;

    void setUniformValue(const char *name, float value);
    void setUniformValue(const char *name, int value);
    void setUniformValue(const char *name, bool value) { setUniformValue(name, value ? 1 : 0); }
    void setUniformValue(const char *name, Vec2 value);
    void setUniformValue(const char *name, Vec3 value);
    void setUniformValue(const char *name, Vec4 value);
    void setUniformValue(const char *name, const Color &value); // as a vec4 (r, g, b, a)
    void setUniformValue(const char *name, const Mat3 &value);
    void setUniformValue(const char *name, const Mat4 &value);
    void setUniformValueArray(const char *name, const float *values, int count);

    // Vertex attributes from the bound GL_ARRAY_BUFFER (offsets in bytes).
    void enableAttributeArray(GLint location);
    void disableAttributeArray(GLint location);
    void setAttributeBuffer(GLint location, GLenum type, std::size_t offset, int tupleSize, int stride = 0);

private:
    const GlFunctions *m_gl = nullptr;
    GLuint m_program = 0;
    bool m_linked = false;
    String m_log;
    std::unordered_map<std::string, GLint> m_uniforms;
};

} // namespace cfw
