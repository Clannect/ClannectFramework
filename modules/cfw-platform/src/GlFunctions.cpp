#include "cfw/platform/Gl.h"
#include "cfw/platform/GlContext.h"

namespace cfw {

bool GlFunctions::load(const GlContext &context, const char **missing) {
    bool ok = true;
#define CFW_GL_LOAD(ret, name, params)                                                                         \
    name = reinterpret_cast<ret(CFW_GL_API *) params>(context.procAddress("gl" #name));                        \
    if (!name && ok) {                                                                                         \
        ok = false;                                                                                            \
        if (missing) {                                                                                         \
            *missing = "gl" #name;                                                                             \
        }                                                                                                      \
    }
    CFW_GL_FUNCTIONS(CFW_GL_LOAD)
#undef CFW_GL_LOAD
    return ok;
}

GlContext::~GlContext() = default;

void GlContext::describe() {
    using GetIntegerv = void(CFW_GL_API *)(GLenum, GLint *);
    using GetString = const GLubyte *(CFW_GL_API *)(GLenum);
    const auto getIntegerv = reinterpret_cast<GetIntegerv>(procAddress("glGetIntegerv"));
    const auto getString = reinterpret_cast<GetString>(procAddress("glGetString"));
    if (getIntegerv) {
        getIntegerv(GL_MAJOR_VERSION, &m_major);
        getIntegerv(GL_MINOR_VERSION, &m_minor);
    }
    if (getString) {
        if (const GLubyte *renderer = getString(GL_RENDERER)) {
            m_renderer = reinterpret_cast<const char *>(renderer);
        }
    }
}

} // namespace cfw
