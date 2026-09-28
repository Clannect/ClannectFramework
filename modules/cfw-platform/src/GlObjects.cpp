#include "cfw/platform/GlObjects.h"

#include <algorithm>
#include <vector>

namespace cfw {

// ---- GlBuffer ---------------------------------------------------------------------

bool GlBuffer::create(const GlFunctions &gl) {
    if (m_id) {
        return true;
    }
    m_gl = &gl;
    gl.GenBuffers(1, &m_id);
    return m_id != 0;
}

void GlBuffer::destroy() {
    if (m_id && m_gl) {
        m_gl->DeleteBuffers(1, &m_id);
    }
    m_id = 0;
    m_size = 0;
}

void GlBuffer::bind() {
    if (m_gl) {
        m_gl->BindBuffer(GLenum(m_type), m_id);
    }
}

void GlBuffer::release() {
    if (m_gl) {
        m_gl->BindBuffer(GLenum(m_type), 0);
    }
}

void GlBuffer::allocate(const void *data, std::size_t bytes) {
    if (m_gl) {
        m_gl->BufferData(GLenum(m_type), GLsizeiptr(bytes), data, m_usage);
        m_size = bytes;
    }
}

void GlBuffer::write(std::size_t offset, const void *data, std::size_t bytes) {
    if (m_gl) {
        m_gl->BufferSubData(GLenum(m_type), GLintptr(offset), GLsizeiptr(bytes), data);
    }
}

// ---- GlVertexArray ----------------------------------------------------------------

bool GlVertexArray::create(const GlFunctions &gl) {
    if (m_id) {
        return true;
    }
    m_gl = &gl;
    gl.GenVertexArrays(1, &m_id);
    return m_id != 0;
}

void GlVertexArray::destroy() {
    if (m_id && m_gl) {
        m_gl->DeleteVertexArrays(1, &m_id);
    }
    m_id = 0;
}

void GlVertexArray::bind() {
    if (m_gl) {
        m_gl->BindVertexArray(m_id);
    }
}

void GlVertexArray::release() {
    if (m_gl) {
        m_gl->BindVertexArray(0);
    }
}

// ---- GlShaderProgram --------------------------------------------------------------

bool GlShaderProgram::addShaderFromSourceCode(const GlFunctions &gl, Stage stage, const char *source) {
    m_gl = &gl;
    if (!m_program) {
        m_program = gl.CreateProgram();
    }
    const GLuint shader = gl.CreateShader(GLenum(stage));
    gl.ShaderSource(shader, 1, &source, nullptr);
    gl.CompileShader(shader);
    GLint ok = 0;
    gl.GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint length = 0;
        gl.GetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
        std::vector<GLchar> text(std::size_t(std::max(1, length)));
        gl.GetShaderInfoLog(shader, GLsizei(text.size()), nullptr, text.data());
        m_log = text.data();
        gl.DeleteShader(shader);
        return false;
    }
    gl.AttachShader(m_program, shader);
    gl.DeleteShader(shader); // freed with the program
    return true;
}

void GlShaderProgram::bindAttributeLocation(const char *name, GLuint location) {
    if (m_gl && m_program) {
        m_gl->BindAttribLocation(m_program, location, name);
    }
}

bool GlShaderProgram::link() {
    if (!m_gl || !m_program) {
        m_log = "no shaders";
        return false;
    }
    m_gl->LinkProgram(m_program);
    GLint ok = 0;
    m_gl->GetProgramiv(m_program, GL_LINK_STATUS, &ok);
    m_linked = ok != 0;
    m_uniforms.clear();
    if (!m_linked) {
        GLint length = 0;
        m_gl->GetProgramiv(m_program, GL_INFO_LOG_LENGTH, &length);
        std::vector<GLchar> text(std::size_t(std::max(1, length)));
        m_gl->GetProgramInfoLog(m_program, GLsizei(text.size()), nullptr, text.data());
        m_log = text.data();
    }
    return m_linked;
}

void GlShaderProgram::destroy() {
    if (m_program && m_gl) {
        m_gl->DeleteProgram(m_program);
    }
    m_program = 0;
    m_linked = false;
    m_uniforms.clear();
}

bool GlShaderProgram::bind() {
    if (!m_linked) {
        return false;
    }
    m_gl->UseProgram(m_program);
    return true;
}

void GlShaderProgram::release() {
    if (m_gl) {
        m_gl->UseProgram(0);
    }
}

GLint GlShaderProgram::uniformLocation(const char *name) {
    if (!m_linked) {
        return -1;
    }
    const auto it = m_uniforms.find(name);
    if (it != m_uniforms.end()) {
        return it->second;
    }
    const GLint location = m_gl->GetUniformLocation(m_program, name);
    m_uniforms.emplace(name, location);
    return location;
}

GLint GlShaderProgram::attributeLocation(const char *name) const {
    return m_linked ? m_gl->GetAttribLocation(m_program, name) : -1;
}

void GlShaderProgram::setUniformValue(const char *name, float value) {
    if (const GLint l = uniformLocation(name); l >= 0) m_gl->Uniform1f(l, value);
}
void GlShaderProgram::setUniformValue(const char *name, int value) {
    if (const GLint l = uniformLocation(name); l >= 0) m_gl->Uniform1i(l, value);
}
void GlShaderProgram::setUniformValue(const char *name, Vec2 v) {
    if (const GLint l = uniformLocation(name); l >= 0) m_gl->Uniform2f(l, v.x, v.y);
}
void GlShaderProgram::setUniformValue(const char *name, Vec3 v) {
    if (const GLint l = uniformLocation(name); l >= 0) m_gl->Uniform3f(l, v.x, v.y, v.z);
}
void GlShaderProgram::setUniformValue(const char *name, Vec4 v) {
    if (const GLint l = uniformLocation(name); l >= 0) m_gl->Uniform4f(l, v.x, v.y, v.z, v.w);
}
void GlShaderProgram::setUniformValue(const char *name, const Color &c) {
    if (const GLint l = uniformLocation(name); l >= 0) m_gl->Uniform4f(l, c.r, c.g, c.b, c.a);
}
void GlShaderProgram::setUniformValue(const char *name, const Mat3 &m) {
    if (const GLint l = uniformLocation(name); l >= 0) m_gl->UniformMatrix3fv(l, 1, GL_FALSE, m.data());
}
void GlShaderProgram::setUniformValue(const char *name, const Mat4 &m) {
    if (const GLint l = uniformLocation(name); l >= 0) m_gl->UniformMatrix4fv(l, 1, GL_FALSE, m.data());
}
void GlShaderProgram::setUniformValueArray(const char *name, const float *values, int count) {
    if (const GLint l = uniformLocation(name); l >= 0) m_gl->Uniform1fv(l, count, values);
}

void GlShaderProgram::enableAttributeArray(GLint location) {
    if (m_gl && location >= 0) m_gl->EnableVertexAttribArray(GLuint(location));
}
void GlShaderProgram::disableAttributeArray(GLint location) {
    if (m_gl && location >= 0) m_gl->DisableVertexAttribArray(GLuint(location));
}
void GlShaderProgram::setAttributeBuffer(GLint location, GLenum type, std::size_t offset, int tupleSize, int stride) {
    if (m_gl && location >= 0) {
        m_gl->VertexAttribPointer(GLuint(location), tupleSize, type, GL_FALSE, stride,
                                  reinterpret_cast<const void *>(offset));
    }
}

} // namespace cfw
