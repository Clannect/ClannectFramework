// OpenGL through CFW: a core 3.3 context on a real window, the function
// table, clearing and drawing with a shader program, vertex array and
// buffer, render-to-texture, reading pixels back (from GL and from the
// window system after swapBuffers), and shader error logs. Skips without a
// display or OpenGL.

#include <chrono>
#include <cstdio>

#include "cfw/image/Image.h"
#include "cfw/platform/GlContext.h"
#include "cfw/platform/GlObjects.h"
#include "cfw/platform/Window.h"
#include "cfw/test/Check.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

const char *kVertex = R"(#version 330 core
layout(location = 0) in vec2 aPosition;
void main() { gl_Position = vec4(aPosition, 0.0, 1.0); }
)";

const char *kFragment = R"(#version 330 core
uniform vec4 uColor;
out vec4 fragColor;
void main() { fragColor = uColor; }
)";

bool near(const std::uint8_t *p, int r, int g, int b) {
    return std::abs(p[0] - r) <= 2 && std::abs(p[1] - g) <= 2 && std::abs(p[2] - b) <= 2;
}

} // namespace

int main() {
    auto window = Window::create({"CFW GL test", {128, 96}, true, true});
    if (!window) {
        std::printf("GlTest: skipped (%s)\n", window.error().message().c_str());
        return cfw::test::finish("GlTest");
    }
    auto created = GlContext::create(*window.value());
    if (!created) {
        std::printf("GlTest: skipped (%s)\n", created.error().message().c_str());
        check(created.error().code() == ErrorCode::Unsupported, "no OpenGL is reported as Unsupported");
        return cfw::test::finish("GlTest");
    }
    std::unique_ptr<GlContext> context = std::move(created).value();
    std::printf("  OpenGL %d.%d on %s\n", context->majorVersion(), context->minorVersion(), context->renderer().c_str());
    check(context->majorVersion() * 10 + context->minorVersion() >= 33, "at least OpenGL 3.3");

    GlFunctions gl;
    const char *missing = nullptr;
    check(gl.load(*context, &missing), "every function loads");
    if (missing) {
        std::printf("  missing: %s\n", missing);
        return cfw::test::finish("GlTest");
    }
    for (int i = 0; i < 20; ++i) {
        processEvents(std::chrono::milliseconds(5)); // let the window map
    }
    const Vec2i size = window.value()->pixelSize();
    gl.Viewport(0, 0, size.x, size.y);

    // Clear.
    gl.ClearColor(0.2f, 0.4f, 0.6f, 1.0f);
    gl.Clear(GL_COLOR_BUFFER_BIT);
    std::uint8_t pixel[4] = {};
    gl.ReadPixels(5, 5, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    check(near(pixel, 51, 102, 153), "clearing fills the framebuffer");

    // A triangle over the right half: program, buffer, vertex array.
    GlShaderProgram program;
    check(program.addShaderFromSourceCode(gl, GlShaderProgram::Stage::Vertex, kVertex), "the vertex shader compiles");
    check(program.addShaderFromSourceCode(gl, GlShaderProgram::Stage::Fragment, kFragment),
          "the fragment shader compiles");
    check(program.link(), "the program links");
    GlVertexArray vao;
    GlBuffer buffer;
    check(vao.create(gl) && buffer.create(gl), "objects are created");
    {
        GlVertexArray::Binder bound(vao);
        buffer.bind();
        const float triangle[] = {0.0f, -3.0f, 3.0f, 1.0f, 0.0f, 3.0f}; // covers x > 0
        buffer.allocate(triangle, sizeof triangle);
        program.enableAttributeArray(0);
        program.setAttributeBuffer(0, GL_FLOAT, 0, 2);
    }
    program.bind();
    program.setUniformValue("uColor", Color{1.0f, 0.5f, 0.0f, 1.0f});
    program.setUniformValue("uMissing", 1.0f); // silently ignored, as in Qt
    {
        GlVertexArray::Binder bound(vao);
        gl.DrawArrays(GL_TRIANGLES, 0, 3);
    }
    gl.ReadPixels(size.x * 3 / 4, size.y / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    check(near(pixel, 255, 128, 0), "the triangle is drawn on the right");
    gl.ReadPixels(size.x / 4, size.y / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    check(near(pixel, 51, 102, 153), "and not on the left");
    checkEqual(gl.GetError(), GL_NO_ERROR, "no GL errors");

    // What the window system shows after swapping.
    context->swapBuffers();
    gl.Finish();
    processEvents(std::chrono::milliseconds(20));
    auto shot = window.value()->capture();
    if (shot) {
        const Image &image = shot.value();
        check(near(image.row(std::uint32_t(size.y / 2)).data() + std::size_t(size.x * 3 / 4) * 4, 255, 128, 0),
              "after swapBuffers the window shows the frame");
    }

    // Render to a texture (the viewport's shadow map and picking use this).
    GLuint texture = 0;
    GLuint framebuffer = 0;
    gl.GenTextures(1, &texture);
    gl.BindTexture(GL_TEXTURE_2D, texture);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GLint(GL_RGBA8), 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GLint(GL_NEAREST));
    gl.GenFramebuffers(1, &framebuffer);
    gl.BindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    checkEqual(gl.CheckFramebufferStatus(GL_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE, "a texture framebuffer is complete");
    gl.Viewport(0, 0, 16, 16);
    gl.ClearColor(0.0f, 1.0f, 0.0f, 1.0f);
    gl.Clear(GL_COLOR_BUFFER_BIT);
    gl.ReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    check(near(pixel, 0, 255, 0), "rendering goes to the texture");
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl.DeleteFramebuffers(1, &framebuffer);
    gl.DeleteTextures(1, &texture);

    // Errors are reported with the compiler's log.
    GlShaderProgram broken;
    check(!broken.addShaderFromSourceCode(gl, GlShaderProgram::Stage::Fragment, "#version 330 core\nvoid main() { oops; }"),
          "a broken shader fails");
    check(!broken.log().empty(), "with the compiler's message");
    broken.destroy();

    program.destroy();
    buffer.destroy();
    vao.destroy();
    context->doneCurrent();
    context.reset();
    return cfw::test::finish("GlTest");
}
