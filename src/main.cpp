#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

// ── Shader utilities ─────────────────────────────────────────────────────────

static std::string readFile(const char* path) {
    std::ifstream f(path);
    if (!f.is_open())
        throw std::runtime_error(std::string("Cannot open shader: ") + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static GLuint compileShader(GLenum type, const std::string& src) {
    GLuint shader = glCreateShader(type);
    const char* c = src.c_str();
    glShaderSource(shader, 1, &c, nullptr);
    glCompileShader(shader);

    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        glDeleteShader(shader);
        throw std::runtime_error(std::string("Shader compile error:\n") + log);
    }
    return shader;
}

// Loads vertex + fragment sources, links them, returns the program handle.
// Throws on error so the caller can keep the previous program alive.
static GLuint buildProgram(const std::string& vertSrc, const std::string& fragSrc) {
    GLuint vert = compileShader(GL_VERTEX_SHADER,   vertSrc);
    GLuint frag = compileShader(GL_FRAGMENT_SHADER, fragSrc);

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vert);
    glAttachShader(prog, frag);
    glLinkProgram(prog);

    glDeleteShader(vert);
    glDeleteShader(frag);

    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        glDeleteProgram(prog);
        throw std::runtime_error(std::string("Program link error:\n") + log);
    }
    return prog;
}

// Hot-reload: compile new shaders, swap atomically, return old program to delete.
static GLuint reloadProgram(GLuint current,
                            const std::string& vertSrc,
                            const std::string& fragSrc) {
    GLuint next = buildProgram(vertSrc, fragSrc);
    glDeleteProgram(current);
    return next;
}

// ── Screen-quad geometry ─────────────────────────────────────────────────────

static const float kQuadVerts[] = {
    // two triangles covering NDC [-1,1]
    -1.f, -1.f,
     1.f, -1.f,
     1.f,  1.f,
    -1.f, -1.f,
     1.f,  1.f,
    -1.f,  1.f,
};

// ── GLFW callbacks ────────────────────────────────────────────────────────────

static void onError(int code, const char* msg) {
    std::fprintf(stderr, "[GLFW error %d] %s\n", code, msg);
}

static void onKey(GLFWwindow* win, int key, int /*scan*/, int action, int /*mods*/) {
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
        glfwSetWindowShouldClose(win, GLFW_TRUE);
}

// ── Entry point ───────────────────────────────────────────────────────────────

int main() {
    glfwSetErrorCallback(onError);
    if (!glfwInit()) {
        std::fprintf(stderr, "glfwInit failed\n");
        return 1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    GLFWwindow* win = glfwCreateWindow(800, 600, "Shader JIT", nullptr, nullptr);
    if (!win) {
        std::fprintf(stderr, "glfwCreateWindow failed\n");
        glfwTerminate();
        return 1;
    }

    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);                   // vsync
    glfwSetKeyCallback(win, onKey);

    if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))) {
        std::fprintf(stderr, "gladLoadGL failed\n");
        return 1;
    }

    std::printf("OpenGL %s  |  GLSL %s\n",
        glGetString(GL_VERSION),
        glGetString(GL_SHADING_LANGUAGE_VERSION));

    // Upload quad geometry
    GLuint vao, vbo;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kQuadVerts), kQuadVerts, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    glEnableVertexAttribArray(0);

    // Initial shader load
    GLuint program = 0;
    try {
        program = buildProgram(readFile("shaders/vertex.glsl"),
                               readFile("shaders/fragment.glsl"));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }

    GLint uTime = glGetUniformLocation(program, "uTime");

    double t0 = glfwGetTime();

    // ── Render loop ───────────────────────────────────────────────────────────
    while (!glfwWindowShouldClose(win)) {
        int w, h;
        glfwGetFramebufferSize(win, &w, &h);
        glViewport(0, 0, w, h);
        glClear(GL_COLOR_BUFFER_BIT);

        glUseProgram(program);
        if (uTime >= 0)
            glUniform1f(uTime, static_cast<float>(glfwGetTime() - t0));

        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glfwSwapBuffers(win);
        glfwPollEvents();
    }

    glDeleteProgram(program);
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
