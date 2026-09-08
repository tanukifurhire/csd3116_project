#include "renderer.h"

#include <cstdio>
#include <cstring>
#include <cctype>
#include <unordered_map>

namespace
{
    struct GlyphRows
    {
        const char *r[Renderer::GLYPH_HEIGHT]; /* 7 rows, 5 chars each: '#' = on, '.' = off */
    };

    const GlyphRows *find_glyph(char c)
    {
        static const std::unordered_map<char, GlyphRows> glyphs = {
            {'A', {{".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}}},
            {'B', {{"####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."}}},
            {'C', {{".####", "#....", "#....", "#....", "#....", "#....", ".####"}}},
            {'D', {{"####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."}}},
            {'E', {{"#####", "#....", "#....", "####.", "#....", "#....", "#####"}}},
            {'F', {{"#####", "#....", "#....", "####.", "#....", "#....", "#...."}}},
            {'G', {{".####", "#....", "#....", "#.###", "#...#", "#...#", ".####"}}},
            {'H', {{"#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}}},
            {'I', {{"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "#####"}}},
            {'J', {{"..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.."}}},
            {'K', {{"#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"}}},
            {'L', {{"#....", "#....", "#....", "#....", "#....", "#....", "#####"}}},
            {'M', {{"#...#", "##.##", "#.#.#", "#...#", "#...#", "#...#", "#...#"}}},
            {'N', {{"#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#", "#...#"}}},
            {'O', {{".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}}},
            {'P', {{"####.", "#...#", "#...#", "####.", "#....", "#....", "#...."}}},
            {'Q', {{".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"}}},
            {'R', {{"####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"}}},
            {'S', {{".####", "#....", "#....", ".###.", "....#", "....#", "####."}}},
            {'T', {{"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."}}},
            {'U', {{"#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}}},
            {'V', {{"#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."}}},
            {'W', {{"#...#", "#...#", "#...#", "#.#.#", "#.#.#", "##.##", "#...#"}}},
            {'X', {{"#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"}}},
            {'Y', {{"#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."}}},
            {'Z', {{"#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"}}},
            {'0', {{".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."}}},
            {'1', {{"..#..", ".##..", "..#..", "..#..", "..#..", "..#..", "#####"}}},
            {'2', {{".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"}}},
            {'3', {{"#####", "...#.", "..#..", "...#.", "....#", "#...#", ".###."}}},
            {'4', {{"...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."}}},
            {'5', {{"#####", "#....", "####.", "....#", "....#", "#...#", ".###."}}},
            {'6', {{"..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###."}}},
            {'7', {{"#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."}}},
            {'8', {{".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."}}},
            {'9', {{".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##.."}}},
        };

        auto it = glyphs.find(c);
        return it != glyphs.end() ? &it->second : nullptr;
    }
} // namespace

/* The whole pipeline in two shaders.
 *
 * Vertex shader: takes a pixel-space position, multiplies it by an orthographic
 * projection matrix and hands GL a clip-space position. This replaces the
 * glOrtho/glMatrixMode calls in client.c.
 *
 * Fragment shader: writes the interpolated vertex colour straight out. No
 * textures, no lighting -- the game is flat 2D squares.
 */
static const char *VERTEX_SHADER_SRC =
    "#version 330 core\n"
    "layout (location = 0) in vec2 a_pos;\n"
    "layout (location = 1) in vec4 a_color;\n"
    "uniform mat4 u_projection;\n"
    "out vec4 v_color;\n"
    "void main()\n"
    "{\n"
    "    v_color = a_color;\n"
    "    gl_Position = u_projection * vec4(a_pos, 0.0, 1.0);\n"
    "}\n";

static const char *FRAGMENT_SHADER_SRC =
    "#version 330 core\n"
    "in vec4 v_color;\n"
    "out vec4 frag_color;\n"
    "void main()\n"
    "{\n"
    "    frag_color = v_color;\n"
    "}\n";

/* GLFW reports errors through a callback rather than return codes. */
static void glfw_error_callback(int code, const char *description)
{
    std::fprintf(stderr, "GLFW error %d: %s\n", code, description);
}

/* Keeps the projection and the viewport in step with the actual framebuffer,
 * which is not the same as the window size on a HiDPI display. */
static void framebuffer_size_callback(GLFWwindow *window, int width, int height)
{
    Renderer *renderer = static_cast<Renderer *>(glfwGetWindowUserPointer(window));
    if (renderer != nullptr)
    {
        glViewport(0, 0, width, height);
    }
}

GLuint Renderer::compile_shader(GLenum type, const char *source)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok == GL_FALSE)
    {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::fprintf(stderr, "%s shader failed to compile:\n%s\n",
                     type == GL_VERTEX_SHADER ? "Vertex" : "Fragment", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint Renderer::link_program(GLuint vertex_shader, GLuint fragment_shader)
{
    GLuint program = glCreateProgram();
    glAttachShader(program, vertex_shader);
    glAttachShader(program, fragment_shader);
    glLinkProgram(program);

    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok == GL_FALSE)
    {
        char log[1024];
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        std::fprintf(stderr, "Shader program failed to link:\n%s\n", log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

void Renderer::update_projection(int width, int height)
{
    if (width <= 0 || height <= 0)
    {
        return; /* minimised window */
    }

    /* Column-major orthographic projection, equivalent to
     * glOrtho(0, width, height, 0, -1, 1): x grows right, y grows DOWN, so
     * (0,0) is the top-left corner and units are pixels. */
    std::memset(m_projection, 0, sizeof(m_projection));
    m_projection[0] = 2.0f / static_cast<float>(width);
    m_projection[5] = -2.0f / static_cast<float>(height);
    m_projection[10] = -1.0f;
    m_projection[12] = -1.0f;
    m_projection[13] = 1.0f;
    m_projection[15] = 1.0f;
}

bool Renderer::init(int width, int height, const std::string &title)
{
    glfwSetErrorCallback(glfw_error_callback);

    if (glfwInit() == GLFW_FALSE)
    {
        std::fprintf(stderr, "Failed to initialise GLFW\n");
        return false;
    }

    /* Ask for a 3.3 core context. Core profile means the fixed-function calls
     * used by client.c are simply not available, which is the point: it fails
     * loudly instead of silently falling back to legacy behaviour. */
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    m_window = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
    if (m_window == nullptr)
    {
        std::fprintf(stderr, "Failed to create window (is an OpenGL 3.3 driver available?)\n");
        glfwTerminate();
        return false;
    }

    glfwMakeContextCurrent(m_window);
    glfwSwapInterval(1); /* vsync: caps the loop at the monitor refresh rate */
    glfwSetWindowUserPointer(m_window, this);
    glfwSetFramebufferSizeCallback(m_window, framebuffer_size_callback);

    /* GLEW resolves the post-1.x entry points now that a context is current.
     * glewExperimental is required with a core profile, otherwise GLEW skips
     * functions it believes the context does not expose. */
    glewExperimental = GL_TRUE;
    GLenum glew_status = glewInit();
    if (glew_status != GLEW_OK)
    {
        std::fprintf(stderr, "Failed to initialise GLEW: %s\n",
                     glewGetErrorString(glew_status));
        shutdown();
        return false;
    }
    /* glewInit() with glewExperimental leaves a bogus GL_INVALID_ENUM behind;
     * clear it so the first real error is not mistaken for this one. */
    glGetError();

    std::printf("OpenGL %s | %s\n",
                reinterpret_cast<const char *>(glGetString(GL_VERSION)),
                reinterpret_cast<const char *>(glGetString(GL_RENDERER)));

    GLuint vertex_shader = compile_shader(GL_VERTEX_SHADER, VERTEX_SHADER_SRC);
    GLuint fragment_shader = compile_shader(GL_FRAGMENT_SHADER, FRAGMENT_SHADER_SRC);
    if (vertex_shader == 0 || fragment_shader == 0)
    {
        glDeleteShader(vertex_shader);
        glDeleteShader(fragment_shader);
        shutdown();
        return false;
    }

    m_program = link_program(vertex_shader, fragment_shader);
    /* The program keeps its own copy of the compiled code once linked. */
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);
    if (m_program == 0)
    {
        shutdown();
        return false;
    }

    m_projection_loc = glGetUniformLocation(m_program, "u_projection");

    /* The VAO records how to read vertex data out of the VBO, so the layout is
     * described once here instead of before every draw call. */
    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);

    glEnableVertexAttribArray(0); /* a_pos   -> first two floats  */
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void *>(offsetof(Vertex, x)));
    glEnableVertexAttribArray(1); /* a_color -> next four floats  */
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void *>(offsetof(Vertex, r)));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    /* Alpha blending, so the 'a' argument to draw_quad actually does something. */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    int fb_width = 0;
    int fb_height = 0;
    glfwGetFramebufferSize(m_window, &fb_width, &fb_height);
    glViewport(0, 0, fb_width, fb_height);
    update_projection(width, height);

    return true;
}

void Renderer::shutdown()
{
    if (m_vbo != 0)
    {
        glDeleteBuffers(1, &m_vbo);
        m_vbo = 0;
    }
    if (m_vao != 0)
    {
        glDeleteVertexArrays(1, &m_vao);
        m_vao = 0;
    }
    if (m_program != 0)
    {
        glDeleteProgram(m_program);
        m_program = 0;
    }
    if (m_window != nullptr)
    {
        glfwDestroyWindow(m_window);
        m_window = nullptr;
    }
    glfwTerminate();
}

bool Renderer::should_close() const
{
    return m_window == nullptr || glfwWindowShouldClose(m_window) != 0;
}

void Renderer::begin_frame(float r, float g, float b)
{
    /* The window can be resized at any time, so the projection is rebuilt from
     * the current window size rather than the size passed to init(). */
    int width = 0;
    int height = 0;
    glfwGetWindowSize(m_window, &width, &height);
    update_projection(width, height);

    m_vertices.clear();

    glClearColor(r, g, b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

void Renderer::draw_quad(float x, float y, float w, float h,
                         float r, float g, float b, float a)
{
    /* Core profile has no GL_QUADS, so a rectangle is two triangles:
     *   (x,y) --- (x+w,y)      0---1     first  triangle: 0 1 2
     *     |           |        | \ |     second triangle: 0 2 3
     *   (x,y+h) - (x+w,y+h)    3---2
     */
    const Vertex top_left = {x, y, r, g, b, a};
    const Vertex top_right = {x + w, y, r, g, b, a};
    const Vertex bottom_right = {x + w, y + h, r, g, b, a};
    const Vertex bottom_left = {x, y + h, r, g, b, a};

    m_vertices.push_back(top_left);
    m_vertices.push_back(top_right);
    m_vertices.push_back(bottom_right);

    m_vertices.push_back(top_left);
    m_vertices.push_back(bottom_right);
    m_vertices.push_back(bottom_left);
}

void Renderer::draw_quad_outline(float x, float y, float w, float h,
                                 float thickness,
                                 float r, float g, float b, float a)
{
    draw_quad(x, y, w, thickness, r, g, b, a);                 /* top    */
    draw_quad(x, y + h - thickness, w, thickness, r, g, b, a); /* bottom */
    draw_quad(x, y, thickness, h, r, g, b, a);                 /* left   */
    draw_quad(x + w - thickness, y, thickness, h, r, g, b, a); /* right  */
}

void Renderer::end_frame()
{
    if (!m_vertices.empty())
    {
        const size_t bytes = m_vertices.size() * sizeof(Vertex);

        glBindVertexArray(m_vao);
        glBindBuffer(GL_ARRAY_BUFFER, m_vbo);

        /* Grow the GPU buffer only when this frame needs more space than the
         * largest previous one; otherwise overwrite what is already there. */
        if (bytes > m_vbo_capacity)
        {
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(bytes),
                         m_vertices.data(), GL_DYNAMIC_DRAW);
            m_vbo_capacity = bytes;
        }
        else
        {
            glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(bytes),
                            m_vertices.data());
        }

        glUseProgram(m_program);
        glUniformMatrix4fv(m_projection_loc, 1, GL_FALSE, m_projection);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(m_vertices.size()));

        glBindVertexArray(0);
    }

    glfwSwapBuffers(m_window);
    glfwPollEvents();
}

float Renderer::text_width(const std::string &text, float scale) const
{
    if (text.empty())
    {
        return 0.0f;
    }
    const float glyph_px = static_cast<float>(GLYPH_WIDTH) * scale;
    const float gap_px = static_cast<float>(GLYPH_SPACING) * scale;
    return static_cast<float>(text.size()) * glyph_px +
           static_cast<float>(text.size() - 1) * gap_px;
}

void Renderer::draw_text(float x, float y, const std::string &text, float scale,
                         float r, float g, float b, float a)
{
    float cursor_x = x;

    for (char raw_c : text)
    {
        const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(raw_c)));
        const GlyphRows *glyph = find_glyph(c);

        if (glyph != nullptr)
        {
            for (int row = 0; row < GLYPH_HEIGHT; row++)
            {
                const char *row_pixels = glyph->r[row];
                for (int col = 0; col < GLYPH_WIDTH; col++)
                {
                    if (row_pixels[col] == '#')
                    {
                        draw_quad(cursor_x + static_cast<float>(col) * scale,
                                 y + static_cast<float>(row) * scale,
                                 scale, scale, r, g, b, a);
                    }
                }
            }
        }
        /* Unsupported characters (and space) just advance the cursor. */

        cursor_x += static_cast<float>(GLYPH_WIDTH + GLYPH_SPACING) * scale;
    }
}
