// gl_loader.h -- tiny GL 3.3 core function loader (no GLAD/GLEW installed).
//
// Only gfx/ includes this. Function pointers live in namespace gfxgl with the
// usual gl* names, so `using namespace gfxgl;` makes ordinary-looking GL code
// work without clashing with libGL's exported C symbols. Include this instead
// of any other GL header, and define GLFW_INCLUDE_NONE before <GLFW/glfw3.h>.
#pragma once

#include <GL/glcorearb.h>

#define GFX_GL_FUNCTIONS(X)                                                   \
    X(PFNGLGETERRORPROC, glGetError)                                          \
    X(PFNGLGETSTRINGPROC, glGetString)                                        \
    X(PFNGLGETINTEGERVPROC, glGetIntegerv)                                    \
    X(PFNGLENABLEPROC, glEnable)                                              \
    X(PFNGLDISABLEPROC, glDisable)                                            \
    X(PFNGLCLEARPROC, glClear)                                                \
    X(PFNGLCLEARCOLORPROC, glClearColor)                                      \
    X(PFNGLCLEARDEPTHPROC, glClearDepth)                                      \
    X(PFNGLVIEWPORTPROC, glViewport)                                          \
    X(PFNGLSCISSORPROC, glScissor)                                            \
    X(PFNGLDEPTHFUNCPROC, glDepthFunc)                                        \
    X(PFNGLDEPTHMASKPROC, glDepthMask)                                        \
    X(PFNGLCULLFACEPROC, glCullFace)                                          \
    X(PFNGLFRONTFACEPROC, glFrontFace)                                        \
    X(PFNGLBLENDFUNCPROC, glBlendFunc)                                        \
    X(PFNGLBLENDFUNCSEPARATEPROC, glBlendFuncSeparate)                        \
    X(PFNGLCOLORMASKPROC, glColorMask)                                        \
    X(PFNGLPOLYGONOFFSETPROC, glPolygonOffset)                                \
    X(PFNGLPIXELSTOREIPROC, glPixelStorei)                                    \
    X(PFNGLREADPIXELSPROC, glReadPixels)                                      \
    X(PFNGLREADBUFFERPROC, glReadBuffer)                                      \
    X(PFNGLDRAWBUFFERPROC, glDrawBuffer)                                      \
    X(PFNGLFINISHPROC, glFinish)                                              \
    X(PFNGLGENTEXTURESPROC, glGenTextures)                                    \
    X(PFNGLDELETETEXTURESPROC, glDeleteTextures)                              \
    X(PFNGLBINDTEXTUREPROC, glBindTexture)                                    \
    X(PFNGLTEXIMAGE2DPROC, glTexImage2D)                                      \
    X(PFNGLTEXPARAMETERIPROC, glTexParameteri)                                \
    X(PFNGLTEXPARAMETERFPROC, glTexParameterf)                                \
    X(PFNGLTEXPARAMETERFVPROC, glTexParameterfv)                              \
    X(PFNGLACTIVETEXTUREPROC, glActiveTexture)                                \
    X(PFNGLGENERATEMIPMAPPROC, glGenerateMipmap)                              \
    X(PFNGLGENBUFFERSPROC, glGenBuffers)                                      \
    X(PFNGLDELETEBUFFERSPROC, glDeleteBuffers)                                \
    X(PFNGLBINDBUFFERPROC, glBindBuffer)                                      \
    X(PFNGLBUFFERDATAPROC, glBufferData)                                      \
    X(PFNGLBUFFERSUBDATAPROC, glBufferSubData)                                \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays)                            \
    X(PFNGLDELETEVERTEXARRAYSPROC, glDeleteVertexArrays)                      \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray)                            \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray)            \
    X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer)                    \
    X(PFNGLDRAWELEMENTSPROC, glDrawElements)                                  \
    X(PFNGLDRAWARRAYSPROC, glDrawArrays)                                      \
    X(PFNGLCREATESHADERPROC, glCreateShader)                                  \
    X(PFNGLSHADERSOURCEPROC, glShaderSource)                                  \
    X(PFNGLCOMPILESHADERPROC, glCompileShader)                                \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv)                                    \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog)                          \
    X(PFNGLDELETESHADERPROC, glDeleteShader)                                  \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram)                                \
    X(PFNGLATTACHSHADERPROC, glAttachShader)                                  \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram)                                    \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv)                                  \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog)                        \
    X(PFNGLDELETEPROGRAMPROC, glDeleteProgram)                                \
    X(PFNGLUSEPROGRAMPROC, glUseProgram)                                      \
    X(PFNGLBINDATTRIBLOCATIONPROC, glBindAttribLocation)                      \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation)                      \
    X(PFNGLUNIFORM1IPROC, glUniform1i)                                        \
    X(PFNGLUNIFORM1FPROC, glUniform1f)                                        \
    X(PFNGLUNIFORM2FPROC, glUniform2f)                                        \
    X(PFNGLUNIFORM3FPROC, glUniform3f)                                        \
    X(PFNGLUNIFORM4FPROC, glUniform4f)                                        \
    X(PFNGLUNIFORMMATRIX4FVPROC, glUniformMatrix4fv)                          \
    X(PFNGLUNIFORMMATRIX3FVPROC, glUniformMatrix3fv)                          \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers)                            \
    X(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers)                      \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer)                            \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D)                  \
    X(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer)            \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus)              \
    X(PFNGLGENRENDERBUFFERSPROC, glGenRenderbuffers)                          \
    X(PFNGLDELETERENDERBUFFERSPROC, glDeleteRenderbuffers)                    \
    X(PFNGLBINDRENDERBUFFERPROC, glBindRenderbuffer)                          \
    X(PFNGLRENDERBUFFERSTORAGEPROC, glRenderbufferStorage)                    \
    X(PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC, glRenderbufferStorageMultisample) \
    X(PFNGLBLITFRAMEBUFFERPROC, glBlitFramebuffer)

namespace gfxgl {
#define GFX_GL_DECLARE(type, name) extern type name;
GFX_GL_FUNCTIONS(GFX_GL_DECLARE)
#undef GFX_GL_DECLARE

// Resolves every function above through glfwGetProcAddress. Needs a current
// context. Returns false (and logs the missing names) if any is missing.
bool loadGL();
} // namespace gfxgl
