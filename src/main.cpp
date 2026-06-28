// stb — implementations compiled once here
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

// httplib — header-only HTTP client (plain HTTP to localhost, no TLS needed)
#include "httplib.h"

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// ── Timestamp helper ──────────────────────────────────────────────────────────

static std::string nowStr() {
    time_t t = time(nullptr);
    char buf[12];
    strftime(buf, sizeof(buf), "%H:%M:%S", localtime(&t));
    return std::string(buf);
}

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
    std::vector<uint8_t> raw(static_cast<size_t>(w * h * 4));
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, raw.data());

    std::vector<uint8_t> flipped(raw.size());
    const size_t rowBytes = static_cast<size_t>(w * 4);
    for (int y = 0; y < h; ++y)
        std::memcpy(flipped.data() +          y  * rowBytes,
                    raw.data()    + (h - 1 - y) * rowBytes,
                    rowBytes);

    std::vector<uint8_t> png;
    png.reserve(raw.size() / 4);
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

// ── HUD system ────────────────────────────────────────────────────────────────

static constexpr int   kAtlasW = 512;
static constexpr int   kAtlasH = 128;
static constexpr float kFontPx = 16.0f;

static stbtt_bakedchar g_cdata[96];
static GLuint g_fontTex    = 0;
static float  g_fontAscent = 0.0f;
static bool   g_fontOk     = false;

static GLuint g_textProg = 0, g_textVAO = 0, g_textVBO = 0;
static GLuint g_rectProg = 0, g_rectVAO = 0, g_rectVBO = 0;

static const char* kHudTextVS = R"GLSL(
#version 330 core
layout(location=0) in vec4 aVertex;
out vec2 vTex;
uniform mat4 uProj;
void main() { vTex = aVertex.zw; gl_Position = uProj * vec4(aVertex.xy, 0.0, 1.0); }
)GLSL";

static const char* kHudTextFS = R"GLSL(
#version 330 core
in vec2 vTex;
out vec4 FragColor;
uniform sampler2D uFont;
uniform vec3 uColor;
void main() { float a = texture(uFont, vTex).r; FragColor = vec4(uColor, a); }
)GLSL";

static const char* kHudRectVS = R"GLSL(
#version 330 core
layout(location=0) in vec2 aPos;
uniform mat4 uProj;
void main() { gl_Position = uProj * vec4(aPos, 0.0, 1.0); }
)GLSL";

static const char* kHudRectFS = R"GLSL(
#version 330 core
out vec4 FragColor;
uniform vec4 uColor;
void main() { FragColor = uColor; }
)GLSL";

static GLuint buildInlineProgram(const char* vs, const char* fs) {
    auto compile = [](GLenum t, const char* src) {
        GLuint s = glCreateShader(t);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        return s;
    };
    GLuint v = compile(GL_VERTEX_SHADER, vs);
    GLuint f = compile(GL_FRAGMENT_SHADER, fs);
    GLuint p = glCreateProgram();
    glAttachShader(p, v); glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v); glDeleteShader(f);
    return p;
}

static void buildHudShaders() {
    g_textProg = buildInlineProgram(kHudTextVS, kHudTextFS);
    glGenVertexArrays(1, &g_textVAO); glGenBuffers(1, &g_textVBO);
    glBindVertexArray(g_textVAO);
    glBindBuffer(GL_ARRAY_BUFFER, g_textVBO);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);

    g_rectProg = buildInlineProgram(kHudRectVS, kHudRectFS);
    glGenVertexArrays(1, &g_rectVAO); glGenBuffers(1, &g_rectVBO);
    glBindVertexArray(g_rectVAO);
    glBindBuffer(GL_ARRAY_BUFFER, g_rectVBO);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
}

static bool initFont() {
    const char* candidates[] = {
        "C:/Windows/Fonts/consola.ttf",
        "C:/Windows/Fonts/cour.ttf",
        "C:/Windows/Fonts/lucon.ttf",
        "C:/Windows/Fonts/arial.ttf",
    };
    std::vector<uint8_t> ttf;
    for (auto* path : candidates) {
        FILE* f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        size_t sz = (size_t)ftell(f); rewind(f);
        ttf.resize(sz);
        fread(ttf.data(), 1, sz, f);
        fclose(f);
        std::printf("[HUD] Font: %s\n", path);
        break;
    }
    if (ttf.empty()) {
        std::fprintf(stderr, "[HUD] No system font — text overlay disabled\n");
        return false;
    }

    stbtt_fontinfo info;
    stbtt_InitFont(&info, ttf.data(), 0);
    int asc, desc, lg;
    stbtt_GetFontVMetrics(&info, &asc, &desc, &lg);
    g_fontAscent = asc * stbtt_ScaleForPixelHeight(&info, kFontPx);

    std::vector<uint8_t> atlas(kAtlasW * kAtlasH, 0);
    if (stbtt_BakeFontBitmap(ttf.data(), 0, kFontPx,
                              atlas.data(), kAtlasW, kAtlasH,
                              32, 96, g_cdata) <= 0) {
        std::fprintf(stderr, "[HUD] Font bake failed\n");
        return false;
    }

    glGenTextures(1, &g_fontTex);
    glBindTexture(GL_TEXTURE_2D, g_fontTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, kAtlasW, kAtlasH,
                 0, GL_RED, GL_UNSIGNED_BYTE, atlas.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);
    return true;
}

static void hudOrtho(GLuint prog, float W, float H) {
    float m[16] = {
        2.f/W,  0,      0, 0,
        0,     -2.f/H,  0, 0,
        0,      0,      1, 0,
       -1.f,    1.f,    0, 1
    };
    glUniformMatrix4fv(glGetUniformLocation(prog, "uProj"), 1, GL_FALSE, m);
}

static void drawRect(float x, float y, float w, float h,
                     float r, float g, float b, float a,
                     float W, float H) {
    float v[12] = { x,y, x+w,y, x+w,y+h, x,y, x+w,y+h, x,y+h };
    glBindVertexArray(g_rectVAO);
    glBindBuffer(GL_ARRAY_BUFFER, g_rectVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_DYNAMIC_DRAW);
    glUseProgram(g_rectProg);
    hudOrtho(g_rectProg, W, H);
    glUniform4f(glGetUniformLocation(g_rectProg, "uColor"), r, g, b, a);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
}

static void drawText(const char* text, float x, float y,
                     float cr, float cg, float cb,
                     float W, float H) {
    if (!g_fontOk || !text || !*text) return;
    float cx = x, cy = y + g_fontAscent;
    std::vector<float> v;
    v.reserve(512);
    for (const char* p = text; *p; ++p) {
        if (*p < 32 || *p >= 128) { cx += 6.f; continue; }
        stbtt_aligned_quad q;
        stbtt_GetBakedQuad(g_cdata, kAtlasW, kAtlasH, *p - 32, &cx, &cy, &q, 1);
        v.insert(v.end(), {q.x0,q.y0,q.s0,q.t0, q.x1,q.y0,q.s1,q.t0, q.x1,q.y1,q.s1,q.t1,
                            q.x0,q.y0,q.s0,q.t0, q.x1,q.y1,q.s1,q.t1, q.x0,q.y1,q.s0,q.t1});
    }
    if (v.empty()) return;
    glBindVertexArray(g_textVAO);
    glBindBuffer(GL_ARRAY_BUFFER, g_textVBO);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(v.size()*sizeof(float)), v.data(), GL_DYNAMIC_DRAW);
    glUseProgram(g_textProg);
    hudOrtho(g_textProg, W, H);
    glUniform3f(glGetUniformLocation(g_textProg, "uColor"), cr, cg, cb);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_fontTex);
    glUniform1i(glGetUniformLocation(g_textProg, "uFont"), 0);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(v.size()/4));
    glBindVertexArray(0);
}

enum class AgentStatus { Active, Processing, Fixing };

static void renderHUD(int winW, int winH,
                      AgentStatus status, double latencyMs,
                      bool baselineActive, double baselineElapsed) {
    const float pad  = 12.f;
    const float panW = 260.f;
    const float lh   = 21.f;
    const float panH = pad + lh * 5.f + pad;
    const float panX = (float)winW - panW - 12.f;
    const float panY = 12.f;
    const float W    = (float)winW;
    const float H    = (float)winH;

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);

    // Subtle border glow
    drawRect(panX-1.f, panY-1.f, panW+2.f, panH+2.f, 0.3f,0.55f,1.f, 0.18f, W, H);
    // Dark panel
    drawRect(panX, panY, panW, panH, 0.04f,0.04f,0.09f, 0.88f, W, H);

    if (g_fontOk) {
        float tx = panX + pad;
        float ty = panY + pad;
        char buf[128];

        if (baselineActive) {
            drawText("TRADITIONAL INFERENCE", tx, ty, 1.f,0.55f,0.f, W, H); ty += lh;
            drawText("Simulating local GPU pipeline...", tx, ty, 0.65f,0.65f,0.65f, W, H); ty += lh;

            snprintf(buf, sizeof(buf), "Loading...  %.1f s / 5.0 s", baselineElapsed);
            drawText(buf, tx, ty, 1.f,1.f,1.f, W, H); ty += lh;

            // ASCII progress bar
            int filled = (int)(baselineElapsed / 5.0 * 22);
            std::string bar = "[";
            for (int i = 0; i < 22; ++i) bar += (i < filled) ? '#' : '-';
            bar += "]";
            drawText(bar.c_str(), tx, ty, 1.f,0.45f,0.f, W, H); ty += lh;

            drawText("[B] exit  |  Cerebras JIT: ~4s", tx, ty, 0.45f,0.45f,0.45f, W, H);
        } else {
            drawText("GEMMA 4-31B", tx, ty, 0.35f,0.75f,1.f, W, H); ty += lh;
            drawText("Cerebras Inference Engine", tx, ty, 0.5f,0.5f,0.5f, W, H); ty += lh;

            float sr = 0.3f, sg = 1.f, sb = 0.5f;
            const char* sl = "Active";
            if (status == AgentStatus::Processing) { sl="Processing..."; sr=1.f; sg=0.85f; sb=0.2f; }
            else if (status == AgentStatus::Fixing) { sl="Error (holding)"; sr=1.f; sg=0.35f; sb=0.2f; }
            snprintf(buf, sizeof(buf), "Status:   %s", sl);
            drawText(buf, tx, ty, sr, sg, sb, W, H); ty += lh;

            if (latencyMs > 0.0)
                snprintf(buf, sizeof(buf), "Latency:  %.0f ms", latencyMs);
            else
                snprintf(buf, sizeof(buf), "Latency:  --");
            drawText(buf, tx, ty, 1.f,1.f,1.f, W, H); ty += lh;

            drawText("[SPACE] evolve  [B] baseline", tx, ty, 0.42f,0.42f,0.42f, W, H);
        }
    }

    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
}

// ── JIT shared state ──────────────────────────────────────────────────────────

struct JitState {
    std::mutex        mtx;
    std::string       pendingGlsl;
    std::string       errorMsg;
    std::atomic<bool> hasNew{false};
    std::atomic<bool> hasError{false};
    std::atomic<bool> busy{false};
};

// ── HTTP worker (runs in its own detached thread) ─────────────────────────────

static const char kPrompt[] =
    "You are looking at the live output of a GLSL fragment shader with a dark fantasy aesthetic. "
    "Generate a new, more visually stunning GLSL 3.3 core fragment shader that evolves this effect "
    "in the style of Elden Ring or Bloodborne — think swirling dark mist, blood-red fog, eldritch runes, "
    "glowing embers, ghostly apparitions, or cosmic horror. "
    "Use domain-warped fbm noise, polar coordinates, or signed-distance functions for complexity. "
    "Keep the same interface: in vec2 vUV; out vec4 FragColor; uniform float uTime;. "
    "Palette: deep blacks, crimson reds, dark violets, occasional amber/ember highlights. "
    "Return ONLY the complete GLSL 3.3 core shader source, no markdown.";

static void jitWorker(std::string b64, JitState* jit) {
    std::printf("[%s] [Critic] Dispatching %.1f KB frame to Gemma 4-31B...\n",
                nowStr().c_str(), b64.size() * 3.0 / 4.0 / 1024.0);

    httplib::Client cli("127.0.0.1", 8000);
    cli.set_connection_timeout(5, 0);
    cli.set_read_timeout(60, 0);

    std::string body =
        "{\"base64_image\":\"" + b64 +
        "\",\"user_prompt\":\"" + kPrompt + "\"}";

    auto res = cli.Post("/compile", body, "application/json");

    std::lock_guard<std::mutex> lock(jit->mtx);
    if (!res) {
        std::fprintf(stderr, "[%s] [Critic] Network error — is the backend running?\n",
                     nowStr().c_str());
        jit->errorMsg = "Cannot reach server — is `uvicorn` running?";
        jit->hasError = true;
    } else if (res->status != 200) {
        std::fprintf(stderr, "[%s] [Critic] Backend HTTP %d — check API key\n",
                     nowStr().c_str(), res->status);
        jit->errorMsg = "HTTP " + std::to_string(res->status) + ": " + res->body;
        jit->hasError = true;
    } else {
        auto glsl = extractGlslFragment(res->body);
        if (glsl.empty()) {
            std::fprintf(stderr, "[%s] [Critic] Model returned empty shader\n",
                         nowStr().c_str());
            jit->errorMsg = "glsl_fragment missing: " + res->body;
            jit->hasError = true;
        } else {
            std::printf("[%s] [Critic] Shader received (%zu chars) — queuing hot-reload\n",
                        nowStr().c_str(), glsl.size());
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
        800, 600, "Shader JIT  [SPACE = compile  B = baseline  ESC = quit]",
        nullptr, nullptr);
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

    // HUD init
    buildHudShaders();
    g_fontOk = initFont();

    // Quad VAO/VBO
    GLuint vao, vbo;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kQuadVerts), kQuadVerts, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    glEnableVertexAttribArray(0);

    // Load initial shaders
    std::string vertSrc;
    GLuint program = 0;
    try {
        vertSrc = readFile("shaders/vertex.glsl");
        program = buildProgram(vertSrc, readFile("shaders/fragment.glsl"));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what()); return 1;
    }
    GLint uTime = glGetUniformLocation(program, "uTime");

    // State
    JitState jit;
    AgentStatus agentStatus  = AgentStatus::Active;
    double jitStartTime      = 0.0;
    double lastLatencyMs     = 0.0;
    bool   baselineActive    = false;
    double baselineStart     = 0.0;
    bool   spaceWasDown      = false;
    bool   bWasDown          = false;
    double t0                = glfwGetTime();

    std::printf("[%s] [Critic] Engine ready — SPACE to evolve, B for baseline compare\n",
                nowStr().c_str());

    // ── Render loop ───────────────────────────────────────────────────────────
    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();

        // ── B key: toggle baseline comparison mode ────────────────────────────
        bool bDown = glfwGetKey(win, GLFW_KEY_B) == GLFW_PRESS;
        if (bDown && !bWasDown) {
            baselineActive = !baselineActive;
            if (baselineActive) {
                baselineStart = glfwGetTime();
                std::printf("[%s] [Baseline] Traditional inference queued — simulating 5s GPU latency\n",
                            nowStr().c_str());
                glfwSetWindowTitle(win, "Shader JIT  [BASELINE: traditional inference...]");
            } else {
                std::printf("[%s] [Baseline] Exited — Cerebras was %.1fx faster\n",
                            nowStr().c_str(),
                            lastLatencyMs > 0.0 ? 5000.0 / lastLatencyMs : 0.0);
                glfwSetWindowTitle(win, "Shader JIT  [SPACE = evolve  B = baseline]");
            }
        }
        bWasDown = bDown;

        // Auto-exit baseline after 5 s
        if (baselineActive && glfwGetTime() - baselineStart >= 5.0) {
            baselineActive = false;
            std::printf("[%s] [Baseline] 5000ms elapsed — demo complete\n"
                        "[%s] [Baseline] Cerebras latency: %.0f ms  |  Speedup: %.1fx\n",
                        nowStr().c_str(), nowStr().c_str(),
                        lastLatencyMs,
                        lastLatencyMs > 0.0 ? 5000.0 / lastLatencyMs : 0.0);
            glfwSetWindowTitle(win, "Shader JIT  [SPACE = evolve  B = baseline]");
        }

        // ── Hot-reload: new shader arrived ────────────────────────────────────
        if (jit.hasNew.load()) {
            std::string newGlsl;
            { std::lock_guard<std::mutex> lk(jit.mtx);
              newGlsl = std::move(jit.pendingGlsl);
              jit.hasNew = false; }

            lastLatencyMs = (glfwGetTime() - jitStartTime) * 1000.0;
            std::printf("[%s] [Critic] Inference: %.0f ms — compiling into GL context\n",
                        nowStr().c_str(), lastLatencyMs);
            try {
                program = reloadProgram(program, vertSrc, newGlsl);
                uTime   = glGetUniformLocation(program, "uTime");
                t0      = glfwGetTime();
                agentStatus = AgentStatus::Active;
                std::printf("[%s] [Critic] Shader hot-reloaded OK\n", nowStr().c_str());
                glfwSetWindowTitle(win, "Shader JIT  [reloaded! SPACE = evolve again]");
            } catch (const std::exception& e) {
                agentStatus = AgentStatus::Fixing;
                std::fprintf(stderr, "[%s] [Critic] GLSL compile error — retaining previous shader:\n%s\n",
                             nowStr().c_str(), e.what());
                glfwSetWindowTitle(win, "Shader JIT  [compile error — see console]");
            }
        }

        // ── Error from worker ─────────────────────────────────────────────────
        if (jit.hasError.load()) {
            std::string err;
            { std::lock_guard<std::mutex> lk(jit.mtx);
              err = std::move(jit.errorMsg);
              jit.hasError = false; }
            agentStatus = AgentStatus::Fixing;
            std::fprintf(stderr, "[%s] [Critic] Error: %s\n", nowStr().c_str(), err.c_str());
            glfwSetWindowTitle(win, "Shader JIT  [error — see console]");
        }

        // ── Render scene ──────────────────────────────────────────────────────
        int w, h;
        glfwGetFramebufferSize(win, &w, &h);
        glViewport(0, 0, w, h);
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);

        glUseProgram(program);
        // Freeze uTime in baseline mode to simulate stalled pipeline
        if (!baselineActive && uTime >= 0)
            glUniform1f(uTime, static_cast<float>(glfwGetTime() - t0));
        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        // ── Space: capture frame + launch JIT worker ──────────────────────────
        bool spaceDown = glfwGetKey(win, GLFW_KEY_SPACE) == GLFW_PRESS;
        if (spaceDown && !spaceWasDown && !jit.busy.load() && !baselineActive) {
            jit.busy      = true;
            jitStartTime  = glfwGetTime();
            agentStatus   = AgentStatus::Processing;

            std::printf("[%s] [Critic] Analyzing framebuffer — shader at t=%.2fs\n",
                        nowStr().c_str(), glfwGetTime() - t0);

            std::string b64 = captureFrameBase64(w, h);
            glfwSetWindowTitle(win, "Shader JIT  [waiting for Gemma 4...]");

            std::thread(jitWorker, std::move(b64), &jit).detach();
        }
        spaceWasDown = spaceDown;

        // ── Render HUD overlay ────────────────────────────────────────────────
        double baselineElapsed = baselineActive ? (glfwGetTime() - baselineStart) : 0.0;
        renderHUD(w, h, agentStatus, lastLatencyMs, baselineActive, baselineElapsed);

        glfwSwapBuffers(win);
    }

    // Cleanup
    glDeleteProgram(program);
    glDeleteProgram(g_textProg);
    glDeleteProgram(g_rectProg);
    glDeleteBuffers(1, &vbo);
    glDeleteBuffers(1, &g_textVBO);
    glDeleteBuffers(1, &g_rectVBO);
    glDeleteVertexArrays(1, &vao);
    glDeleteVertexArrays(1, &g_textVAO);
    glDeleteVertexArrays(1, &g_rectVAO);
    if (g_fontTex) glDeleteTextures(1, &g_fontTex);
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
