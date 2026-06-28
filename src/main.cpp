// stb — implementation compiled once here
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

// httplib — header-only HTTP client (plain HTTP to localhost, no TLS needed)
#include "httplib.h"

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// ── Shader utilities ──────────────────────────────────────────────────────────

static std::string readFile(const char* path) {
    std::ifstream f(path);
    if (!f.is_open())
        throw std::runtime_error(std::string("Cannot open: ") + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static GLuint compileShader(GLenum type, const std::string& src) {
    GLuint s = glCreateShader(type);
    const char* c = src.c_str();
    glShaderSource(s, 1, &c, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        glDeleteShader(s);
        throw std::runtime_error(std::string("Shader compile error:\n") + log);
    }
    return s;
}

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

// Compile new shaders, swap atomically; keeps old program alive on failure.
static GLuint reloadProgram(GLuint old,
                            const std::string& vertSrc,
                            const std::string& fragSrc) {
    GLuint next = buildProgram(vertSrc, fragSrc);
    glDeleteProgram(old);
    return next;
}

// ── Base64 encoder ────────────────────────────────────────────────────────────

static std::string base64Encode(const uint8_t* data, size_t len) {
    static const char kChars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t b = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) b |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) b |= static_cast<uint32_t>(data[i + 2]);
        out += kChars[(b >> 18) & 0x3F];
        out += kChars[(b >> 12) & 0x3F];
        out += (i + 1 < len) ? kChars[(b >>  6) & 0x3F] : '=';
        out += (i + 2 < len) ? kChars[ b        & 0x3F] : '=';
    }
    return out;
}

// ── Frame capture (must be called on the main/GL thread, before SwapBuffers) ──

static std::string captureFrameBase64(int w, int h) {
    // Read raw RGBA from the back buffer (what we just drew)
    std::vector<uint8_t> raw(static_cast<size_t>(w * h * 4));
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, raw.data());

    // OpenGL origin is bottom-left; PNG expects top-left — flip vertically
    std::vector<uint8_t> flipped(raw.size());
    const size_t rowBytes = static_cast<size_t>(w * 4);
    for (int y = 0; y < h; ++y)
        std::memcpy(flipped.data() +          y  * rowBytes,
                    raw.data()    + (h - 1 - y) * rowBytes,
                    rowBytes);

    // Encode to PNG in memory using stb
    std::vector<uint8_t> png;
    png.reserve(raw.size() / 4);            // rough initial allocation
    stbi_write_png_to_func(
        [](void* ctx, void* data, int sz) {
            auto* buf = static_cast<std::vector<uint8_t>*>(ctx);
            const auto* p = static_cast<const uint8_t*>(data);
            buf->insert(buf->end(), p, p + sz);
        },
        &png, w, h, 4, flipped.data(), w * 4);

    return base64Encode(png.data(), png.size());
}

// ── JSON extraction ───────────────────────────────────────────────────────────

// Pulls the "glsl_fragment" value from {"glsl_fragment":"..."}.
// Handles the JSON escape sequences the server may emit.
static std::string extractGlslFragment(const std::string& json) {
    const std::string key = "\"glsl_fragment\":\"";
    auto pos = json.find(key);
    if (pos == std::string::npos) return {};
    pos += key.size();
    std::string out;
    for (size_t i = pos; i < json.size(); ++i) {
        if (json[i] == '\\' && i + 1 < json.size()) {
            switch (json[++i]) {
                case 'n':  out += '\n'; break;
                case 't':  out += '\t'; break;
                case 'r':  out += '\r'; break;
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                default:   out += json[i]; break;
            }
        } else if (json[i] == '"') {
            break;
        } else {
            out += json[i];
        }
    }
    return out;
}

// ── JIT shared state ──────────────────────────────────────────────────────────
// Written by worker thread, read by main thread.
// Strings are guarded by mtx; flags are std::atomic so no lock needed to poll.

struct JitState {
    std::mutex        mtx;
    std::string       pendingGlsl;   // new fragment shader source
    std::string       errorMsg;      // human-readable error if request failed
    std::atomic<bool> hasNew{false};
    std::atomic<bool> hasError{false};
    std::atomic<bool> busy{false};
};

// ── HTTP worker (runs in its own detached thread) ─────────────────────────────

static const char kPrompt[] =
    "You are looking at the live output of a GLSL fragment shader. "
    "Generate a new, more visually complex GLSL 3.3 core fragment shader that evolves this effect. "
    "Keep the same interface: in vec2 vUV; out vec4 FragColor; uniform float uTime;. "
    "Use maths, noise, or colour tricks to make it more interesting. "
    "Return the complete shader source only.";

static void jitWorker(std::string b64, JitState* jit) {
    httplib::Client cli("127.0.0.1", 8000);
    cli.set_connection_timeout(5, 0);
    cli.set_read_timeout(60, 0);      // Cerebras can take several seconds

    // b64 contains only A-Za-z0-9+/= — safe to embed raw in JSON
    // kPrompt contains no double-quotes or backslashes — safe to embed raw
    std::string body =
        "{\"base64_image\":\"" + b64 +
        "\",\"user_prompt\":\"" + kPrompt + "\"}";

    auto res = cli.Post("/compile", body, "application/json");

    std::lock_guard<std::mutex> lock(jit->mtx);
    if (!res) {
        jit->errorMsg = "Cannot reach server — is `docker compose up` running?";
        jit->hasError = true;
    } else if (res->status != 200) {
        jit->errorMsg = "Server returned HTTP " + std::to_string(res->status)
                      + ": " + res->body;
        jit->hasError = true;
    } else {
        auto glsl = extractGlslFragment(res->body);
        if (glsl.empty()) {
            jit->errorMsg = "glsl_fragment missing from response: " + res->body;
            jit->hasError = true;
        } else {
            jit->pendingGlsl = std::move(glsl);
            jit->hasNew = true;
        }
    }
    jit->busy = false;
}

// ── Quad geometry ─────────────────────────────────────────────────────────────

static const float kQuadVerts[] = {
    -1.f, -1.f,  1.f, -1.f,  1.f,  1.f,
    -1.f, -1.f,  1.f,  1.f, -1.f,  1.f,
};

// ── GLFW callbacks ────────────────────────────────────────────────────────────

static void onError(int code, const char* msg) {
    std::fprintf(stderr, "[GLFW %d] %s\n", code, msg);
}

static void onKey(GLFWwindow* win, int key, int, int action, int) {
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
        glfwSetWindowShouldClose(win, GLFW_TRUE);
}

// ── Entry point ───────────────────────────────────────────────────────────────

int main() {
    glfwSetErrorCallback(onError);
    if (!glfwInit()) { std::fprintf(stderr, "glfwInit failed\n"); return 1; }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    GLFWwindow* win = glfwCreateWindow(
        800, 600, "Shader JIT  [SPACE = compile  ESC = quit]", nullptr, nullptr);
    if (!win) { glfwTerminate(); return 1; }

    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);
    glfwSetKeyCallback(win, onKey);

    if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))) {
        std::fprintf(stderr, "gladLoadGL failed\n"); return 1;
    }
    std::printf("OpenGL %s  |  GLSL %s\n",
                glGetString(GL_VERSION),
                glGetString(GL_SHADING_LANGUAGE_VERSION));

    // Quad VAO/VBO
    GLuint vao, vbo;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kQuadVerts), kQuadVerts, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    glEnableVertexAttribArray(0);

    // Load initial shaders; keep vertSrc for all future reloads
    std::string vertSrc;
    GLuint program = 0;
    try {
        vertSrc = readFile("shaders/vertex.glsl");
        program = buildProgram(vertSrc, readFile("shaders/fragment.glsl"));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what()); return 1;
    }
    GLint uTime = glGetUniformLocation(program, "uTime");

    JitState jit;
    bool spaceWasDown = false;
    double t0 = glfwGetTime();

    // ── Render loop ───────────────────────────────────────────────────────────
    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();

        // ── Hot-reload: new shader arrived from worker thread ─────────────────
        if (jit.hasNew.load()) {
            std::string newGlsl;
            { std::lock_guard<std::mutex> lk(jit.mtx);
              newGlsl = std::move(jit.pendingGlsl);
              jit.hasNew = false; }
            try {
                program = reloadProgram(program, vertSrc, newGlsl);
                uTime   = glGetUniformLocation(program, "uTime");
                t0      = glfwGetTime();   // restart uTime for the new shader
                std::printf("[JIT] Shader hot-reloaded!\n");
                glfwSetWindowTitle(win, "Shader JIT  [reloaded! SPACE = evolve again]");
            } catch (const std::exception& e) {
                std::fprintf(stderr, "[JIT] Compile error (keeping old shader):\n%s\n",
                             e.what());
                glfwSetWindowTitle(win, "Shader JIT  [compile error — see console]");
            }
        }

        // ── Error from worker thread ──────────────────────────────────────────
        if (jit.hasError.load()) {
            std::string err;
            { std::lock_guard<std::mutex> lk(jit.mtx);
              err = std::move(jit.errorMsg);
              jit.hasError = false; }
            std::fprintf(stderr, "[JIT] Error: %s\n", err.c_str());
            glfwSetWindowTitle(win, "Shader JIT  [error — see console]");
        }

        // ── Render ────────────────────────────────────────────────────────────
        int w, h;
        glfwGetFramebufferSize(win, &w, &h);
        glViewport(0, 0, w, h);
        glClear(GL_COLOR_BUFFER_BIT);

        glUseProgram(program);
        if (uTime >= 0)
            glUniform1f(uTime, static_cast<float>(glfwGetTime() - t0));
        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        // ── Space: capture current frame and launch JIT worker ────────────────
        // Capture BEFORE SwapBuffers so glReadPixels reads the back buffer
        // we just rendered into. The worker thread handles blocking network I/O.
        bool spaceDown = glfwGetKey(win, GLFW_KEY_SPACE) == GLFW_PRESS;
        if (spaceDown && !spaceWasDown && !jit.busy.load()) {
            jit.busy = true;
            std::printf("[JIT] Capturing %dx%d framebuffer...\n", w, h);
            glfwSetWindowTitle(win, "Shader JIT  [capturing frame...]");

            std::string b64 = captureFrameBase64(w, h);
            std::printf("[JIT] %.1f KB PNG → %zu B64 chars. Calling /compile...\n",
                        b64.size() * 3.0 / 4.0 / 1024.0, b64.size());
            glfwSetWindowTitle(win, "Shader JIT  [waiting for Gemma 4...]");

            // Detach: render loop keeps running while Cerebras responds
            std::thread(jitWorker, std::move(b64), &jit).detach();
        }
        spaceWasDown = spaceDown;

        glfwSwapBuffers(win);
    }

    glDeleteProgram(program);
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
