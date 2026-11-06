// gl_loader.cpp -- see gl_loader.h.
//
// Runtime OpenGL function loader. The list of required entry points lives in
// gl_loader.h as the X-macro GFX_GL_FUNCTIONS(X); this file expands it twice:
// once to define one function pointer per entry (all null until loaded) and
// once in loadGL() to fetch each pointer through glfwGetProcAddress.
// Needs a current GL context (glfwMakeContextCurrent) before loadGL() is called.
// GLFW_INCLUDE_NONE stops GLFW from pulling in a system GL header of its own.
#include "gl_loader.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <cstdio>

namespace gfxgl {
// Define every pointer declared in gl_loader.h (extern there), initialised to nullptr.
#define GFX_GL_DEFINE(type, name) type name = nullptr;
GFX_GL_FUNCTIONS(GFX_GL_DEFINE)
#undef GFX_GL_DEFINE

// Resolve all GL entry points via glfwGetProcAddress. Returns true only if every
// function was found; each missing one is reported on stderr (by name) so all
// failures show up in a single run rather than just the first.
bool loadGL()
{
    bool ok = true;
// Per-entry expansion: look up the symbol by its stringified name, cast to the
// matching function-pointer type, and flag failure if the driver returns null.
#define GFX_GL_LOAD(type, name)                                                \
    name = reinterpret_cast<type>(glfwGetProcAddress(#name));                  \
    if (!name)                                                                 \
    {                                                                          \
        std::fprintf(stderr, "[gfx] missing GL function %s\n", #name);         \
        ok = false;                                                            \
    }
    GFX_GL_FUNCTIONS(GFX_GL_LOAD)
#undef GFX_GL_LOAD
    return ok;
}
} // namespace gfxgl
