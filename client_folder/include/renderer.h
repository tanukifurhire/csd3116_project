#ifndef RENDERER_H
#define RENDERER_H

/* Minimal OpenGL 3.3 core-profile renderer.
 *
 * The C client (client.c) draws with the fixed-function pipeline -- glBegin /
 * glEnd, glOrtho, glPushMatrix -- which was removed from core profiles in GL
 * 3.1. This is the shader-based equivalent for the C++ port: one vertex
 * buffer, one shader program, one draw call per frame.
 *
 * Coordinates match the old client so gameplay code ports over unchanged:
 * pixels, origin at the TOP-LEFT, +y pointing down.
 *
 *   Renderer r;
 *   r.init(640, 480, "client");
 *   while (!r.should_close()) {
 *       r.begin_frame(1.0f, 1.0f, 1.0f);          // clear to white
 *       r.draw_quad(x, y, 20, 20, 1.0f, 0, 0);    // a red 20x20 square
 *       r.end_frame();                            // flush + swap + poll
 *   }
 *   r.shutdown();
 */

#include <string>
#include <vector>

/* GLEW must be included before any GL header; GLFW would otherwise pull in
 * <GL/gl.h> on its own. GLFW_INCLUDE_NONE says "I am bringing my own loader". */
#include <GL/glew.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

class Renderer
{
public:
    /* Creates the window, makes its GL context current, loads GL through GLEW
     * and builds the shader program. Returns false (with a message on stderr)
     * if any of that fails. */
    bool init(int width, int height, const std::string &title);

    /* Destroys the GL objects and the window. Safe to call more than once. */
    void shutdown();

    bool should_close() const;
    GLFWwindow *window() const { return m_window; }

    /* Clears the screen and starts collecting geometry for this frame. */
    void begin_frame(float r, float g, float b);

    /* Appends one axis-aligned rectangle to the frame's batch. Nothing is sent
     * to the GPU until end_frame(). */
    void draw_quad(float x, float y, float w, float h,
                   float r, float g, float b, float a = 1.0f);

    /* Outline version: four thin quads, one per edge. */
    void draw_quad_outline(float x, float y, float w, float h,
                           float thickness,
                           float r, float g, float b, float a = 1.0f);

    void draw_text(float x, float y, const std::string &text, float scale,
                   float r, float g, float b, float a = 1.0f);
                   
    float text_width(const std::string &text, float scale) const;

    static constexpr int GLYPH_WIDTH = 5;
    static constexpr int GLYPH_HEIGHT = 7;
    static constexpr int GLYPH_SPACING = 1; /* blank columns between characters */

    /* Uploads the batch, issues the draw call, swaps buffers and polls input. */
    void end_frame();

private:
    /* Interleaved vertex layout: position (2 floats) + colour (4 floats).
     * Kept as a plain struct so sizeof/offsetof line up with the attribute
     * pointers set in init(). */
    struct Vertex
    {
        float x, y;
        float r, g, b, a;
    };

    /* Recomputes the pixel-space projection after a resize. */
    void update_projection(int width, int height);

    static GLuint compile_shader(GLenum type, const char *source);
    static GLuint link_program(GLuint vertex_shader, GLuint fragment_shader);

    GLFWwindow *m_window = nullptr;
    GLuint m_program = 0;
    GLuint m_vao = 0;
    GLuint m_vbo = 0;
    GLint m_projection_loc = -1;

    /* Bytes currently allocated on the GPU, so the buffer is only reallocated
     * when a frame needs more room than the last one did. */
    size_t m_vbo_capacity = 0;

    std::vector<Vertex> m_vertices;
    float m_projection[16] = {0.0f};
};

#endif /* RENDERER_H */
