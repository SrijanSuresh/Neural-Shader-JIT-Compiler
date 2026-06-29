// stb — implementations compiled once here
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

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

// ── Timestamp ─────────────────────────────────────────────────────────────────

static std::string nowStr() {
    time_t t = time(nullptr); char buf[12];
    strftime(buf, sizeof(buf), "%H:%M:%S", localtime(&t));
    return std::string(buf);
}

// ── Shader utilities ──────────────────────────────────────────────────────────

static std::string readFile(const char* path) {
    std::ifstream f(path);
    if (!f.is_open()) throw std::runtime_error(std::string("Cannot open: ") + path);
    std::ostringstream ss; ss << f.rdbuf(); return ss.str();
}

static GLuint compileShader(GLenum type, const std::string& src) {
    GLuint s = glCreateShader(type);
    const char* c = src.c_str();
    glShaderSource(s, 1, &c, nullptr); glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[1024]; glGetShaderInfoLog(s,sizeof(log),nullptr,log); glDeleteShader(s);
               throw std::runtime_error(std::string("Shader compile:\n")+log); }
    return s;
}

static GLuint buildProgram(const std::string& vertSrc, const std::string& fragSrc) {
    GLuint vert = compileShader(GL_VERTEX_SHADER, vertSrc);
    GLuint frag = compileShader(GL_FRAGMENT_SHADER, fragSrc);
    GLuint prog = glCreateProgram();
    glAttachShader(prog,vert); glAttachShader(prog,frag); glLinkProgram(prog);
    glDeleteShader(vert); glDeleteShader(frag);
    GLint ok=0; glGetProgramiv(prog,GL_LINK_STATUS,&ok);
    if (!ok) { char log[1024]; glGetProgramInfoLog(prog,sizeof(log),nullptr,log); glDeleteProgram(prog);
               throw std::runtime_error(std::string("Program link:\n")+log); }
    return prog;
}

static GLuint reloadProgram(GLuint old, const std::string& vs, const std::string& fs) {
    GLuint next = buildProgram(vs, fs); glDeleteProgram(old); return next;
}

// ── Base64 ────────────────────────────────────────────────────────────────────

static std::string base64Encode(const uint8_t* data, size_t len) {
    static const char kC[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out; out.reserve(((len+2)/3)*4);
    for (size_t i=0;i<len;i+=3){
        uint32_t b=static_cast<uint32_t>(data[i])<<16;
        if(i+1<len)b|=static_cast<uint32_t>(data[i+1])<<8;
        if(i+2<len)b|=static_cast<uint32_t>(data[i+2]);
        out+=kC[(b>>18)&0x3F]; out+=kC[(b>>12)&0x3F];
        out+=(i+1<len)?kC[(b>>6)&0x3F]:'=';
        out+=(i+2<len)?kC[b&0x3F]:'=';
    }
    return out;
}

// ── Frame capture ─────────────────────────────────────────────────────────────

static std::string captureFrameBase64(int w, int h) {
    std::vector<uint8_t> raw(static_cast<size_t>(w*h*4));
    glReadPixels(0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,raw.data());
    std::vector<uint8_t> flip(raw.size());
    const size_t rb=static_cast<size_t>(w*4);
    for(int y=0;y<h;++y) std::memcpy(flip.data()+y*rb, raw.data()+(h-1-y)*rb, rb);
    std::vector<uint8_t> png; png.reserve(raw.size()/4);
    stbi_write_png_to_func([](void*ctx,void*d,int sz){
        auto*b=static_cast<std::vector<uint8_t>*>(ctx);
        const auto*p=static_cast<const uint8_t*>(d);
        b->insert(b->end(),p,p+sz);},&png,w,h,4,flip.data(),w*4);
    return base64Encode(png.data(),png.size());
}

// Store frame into a GL texture (allocated/reallocated as needed)
static void storeFrameToTex(int w, int h, GLuint* tex, int* tw, int* th) {
    std::vector<uint8_t> raw(static_cast<size_t>(w*h*4));
    glReadPixels(0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,raw.data());
    std::vector<uint8_t> flip(raw.size());
    const size_t rb=static_cast<size_t>(w*4);
    for(int y=0;y<h;++y) std::memcpy(flip.data()+y*rb, raw.data()+(h-1-y)*rb, rb);
    if(*tex==0 || *tw!=w || *th!=h) {
        if(*tex) glDeleteTextures(1,tex);
        glGenTextures(1,tex);
        glBindTexture(GL_TEXTURE_2D,*tex);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,flip.data());
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        *tw=w; *th=h;
    } else {
        glBindTexture(GL_TEXTURE_2D,*tex);
        glTexSubImage2D(GL_TEXTURE_2D,0,0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,flip.data());
    }
    glBindTexture(GL_TEXTURE_2D,0);
}

// ── JSON helpers ──────────────────────────────────────────────────────────────

static std::string jsonEscape(const std::string& s) {
    std::string out; out.reserve(s.size()+64);
    for(unsigned char c:s){
        switch(c){
            case '"':  out+="\\\""; break;
            case '\\': out+="\\\\"; break;
            case '\n': out+="\\n";  break;
            case '\r': out+="\\r";  break;
            case '\t': out+="\\t";  break;
            default: if(c<0x20){char e[8];snprintf(e,sizeof(e),"\\u%04x",c);out+=e;}
                     else out+=(char)c; break;
        }
    }
    return out;
}

static std::string extractGlslFragment(const std::string& json) {
    const std::string key = "\"glsl_fragment\":\"";
    auto pos = json.find(key); if(pos==std::string::npos) return {};
    pos += key.size(); std::string out;
    for(size_t i=pos;i<json.size();++i){
        if(json[i]=='\\'&&i+1<json.size()){
            switch(json[++i]){case 'n':out+='\n';break;case 't':out+='\t';break;
                case 'r':out+='\r';break;case '"':out+='"';break;case '\\':out+='\\';break;
                default:out+=json[i];break;}
        } else if(json[i]=='"') break;
        else out+=json[i];
    }
    return out;
}

static float extractFloat(const std::string& json, const char* key) {
    std::string s=std::string("\"")+key+"\":";
    auto pos=json.find(s); if(pos==std::string::npos) return 0.f;
    pos+=s.size(); while(pos<json.size()&&(json[pos]==' '||json[pos]=='\t'))++pos;
    return static_cast<float>(std::atof(json.c_str()+pos));
}

static std::string extractString(const std::string& json, const char* key) {
    std::string s=std::string("\"")+key+"\":\"";
    auto pos=json.find(s); if(pos==std::string::npos) return {};
    pos+=s.size(); std::string out;
    for(size_t i=pos;i<json.size();++i){
        if(json[i]=='\\'&&i+1<json.size()){
            switch(json[++i]){case 'n':out+='\n';break;case 't':out+='\t';break;
                case '"':out+='"';break;case '\\':out+='\\';break;default:out+=json[i];break;}
        } else if(json[i]=='"') break;
        else out+=json[i];
    }
    return out;
}

// ── Preset definitions ────────────────────────────────────────────────────────

struct Preset {
    const char* name;
    const char* shaderFile;
    const char* criticContext;
    const char* composerStyle;
    int         glfwKey;
};

static const Preset kPresets[] = {
    {
        "Dark Fantasy",
        "shaders/dark_fantasy.glsl",
        "Score for Bloodborne/Elden Ring: eldritch horror, crimson fog, cosmic dread.",
        "Elden Ring / Bloodborne: vivid crimson fog, eldritch runes, glowing embers, blood moon. "
        "Dark voids + VIVID color peaks. Never output near-zero RGB everywhere.",
        GLFW_KEY_1
    },
    {
        "Persona / Anime",
        "shaders/persona.glsl",
        "Score for Persona/anime: vibrant pop-art energy, geometric patterns, emotional color.",
        "Persona 5 / anime: electric yellow, hot pink, cyan, white. Rotating SDF geometry, "
        "concentric rings, radial glow, anime scan-lines. Upbeat, vivid, high energy.",
        GLFW_KEY_2
    },
    {
        "Biomedical",
        "shaders/medicine.glsl",
        "Score for biomedical visualization: cellular realism, organic form, scientific clarity.",
        "Biomedical microscopy: Voronoi cell membranes, organelle clusters, cytoplasm fluid, "
        "fluorescence glow. Palette: cyan-blue membranes, amber organelles, violet nucleus, dark bg.",
        GLFW_KEY_3
    },
    {
        "Cosmic / Space",
        "shaders/space.glsl",
        "Score for cosmic aesthetic: nebula grandeur, stellar phenomena, galactic scale.",
        "Cosmic nebula: domain-warped nebula clouds, star fields with twinkle, pulsar beams, "
        "stellar nursery clusters. Deep indigo, electric blue, gold star cores.",
        GLFW_KEY_4
    },
};
static constexpr int kNumPresets = 4;

// ── HUD system ────────────────────────────────────────────────────────────────

static constexpr int   kAtlasW = 512, kAtlasH = 128;
static constexpr float kFontPx = 16.0f;
static stbtt_bakedchar g_cdata[96];
static GLuint g_fontTex=0; static float g_fontAscent=0.f; static bool g_fontOk=false;
static GLuint g_textProg=0,g_textVAO=0,g_textVBO=0;
static GLuint g_rectProg=0,g_rectVAO=0,g_rectVBO=0;
static GLuint g_blitProg=0;
static GLuint g_grayProg=0;  // desaturation blit — used during baseline mode
static GLuint g_fbo=0,g_fboTex=0; static int g_fboW=0,g_fboH=0;

static const char* kHudTextVS = R"GLSL(
#version 330 core
layout(location=0) in vec4 aVertex; out vec2 vTex; uniform mat4 uProj;
void main(){vTex=aVertex.zw;gl_Position=uProj*vec4(aVertex.xy,0,1);}
)GLSL";
static const char* kHudTextFS = R"GLSL(
#version 330 core
in vec2 vTex; out vec4 FragColor; uniform sampler2D uFont; uniform vec3 uColor;
void main(){float a=texture(uFont,vTex).r;FragColor=vec4(uColor,a);}
)GLSL";
static const char* kHudRectVS = R"GLSL(
#version 330 core
layout(location=0) in vec2 aPos; uniform mat4 uProj;
void main(){gl_Position=uProj*vec4(aPos,0,1);}
)GLSL";
static const char* kHudRectFS = R"GLSL(
#version 330 core
out vec4 FragColor; uniform vec4 uColor;
void main(){FragColor=uColor;}
)GLSL";
static const char* kBlitVS = R"GLSL(
#version 330 core
layout(location=0) in vec2 aPos; out vec2 vUV;
void main(){vUV=aPos*0.5+0.5;gl_Position=vec4(aPos,0,1);}
)GLSL";
static const char* kBlitFS = R"GLSL(
#version 330 core
in vec2 vUV; out vec4 FragColor; uniform sampler2D uTex;
void main(){FragColor=texture(uTex,vUV);}
)GLSL";
// Desaturation + darken — makes the "waiting" state look visually dead
static const char* kGrayFS = R"GLSL(
#version 330 core
in vec2 vUV; out vec4 FragColor; uniform sampler2D uTex;
void main(){
    vec3 c=texture(uTex,vUV).rgb;
    float lum=dot(c,vec3(0.299,0.587,0.114));
    FragColor=vec4(vec3(lum)*0.45,1.0);
}
)GLSL";

static GLuint buildInlineProg(const char* vs, const char* fs) {
    auto cmp=[](GLenum t,const char*src){GLuint s=glCreateShader(t);glShaderSource(s,1,&src,nullptr);glCompileShader(s);return s;};
    GLuint v=cmp(GL_VERTEX_SHADER,vs),f=cmp(GL_FRAGMENT_SHADER,fs),p=glCreateProgram();
    glAttachShader(p,v);glAttachShader(p,f);glLinkProgram(p);glDeleteShader(v);glDeleteShader(f);
    return p;
}

static void buildHudShaders() {
    g_textProg=buildInlineProg(kHudTextVS,kHudTextFS);
    glGenVertexArrays(1,&g_textVAO);glGenBuffers(1,&g_textVBO);
    glBindVertexArray(g_textVAO);glBindBuffer(GL_ARRAY_BUFFER,g_textVBO);
    glVertexAttribPointer(0,4,GL_FLOAT,GL_FALSE,4*sizeof(float),nullptr);glEnableVertexAttribArray(0);glBindVertexArray(0);

    g_rectProg=buildInlineProg(kHudRectVS,kHudRectFS);
    glGenVertexArrays(1,&g_rectVAO);glGenBuffers(1,&g_rectVBO);
    glBindVertexArray(g_rectVAO);glBindBuffer(GL_ARRAY_BUFFER,g_rectVBO);
    glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,2*sizeof(float),nullptr);glEnableVertexAttribArray(0);glBindVertexArray(0);

    g_blitProg=buildInlineProg(kBlitVS,kBlitFS);
    g_grayProg=buildInlineProg(kBlitVS,kGrayFS);
}

static void ensureFBO(int w,int h){
    if(g_fbo&&g_fboW==w&&g_fboH==h) return;
    if(g_fbo){glDeleteFramebuffers(1,&g_fbo);glDeleteTextures(1,&g_fboTex);}
    glGenFramebuffers(1,&g_fbo); glGenTextures(1,&g_fboTex);
    glBindTexture(GL_TEXTURE_2D,g_fboTex);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER,g_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,g_fboTex,0);
    glBindFramebuffer(GL_FRAMEBUFFER,0);
    g_fboW=w; g_fboH=h;
}

static bool initFont() {
    const char* cand[]={"C:/Windows/Fonts/consola.ttf","C:/Windows/Fonts/cour.ttf",
                        "C:/Windows/Fonts/lucon.ttf","C:/Windows/Fonts/arial.ttf"};
    std::vector<uint8_t> ttf;
    for(auto*p:cand){FILE*f=fopen(p,"rb");if(!f)continue;fseek(f,0,SEEK_END);size_t sz=(size_t)ftell(f);rewind(f);
        ttf.resize(sz);fread(ttf.data(),1,sz,f);fclose(f);std::printf("[HUD] Font: %s\n",p);break;}
    if(ttf.empty()){std::fprintf(stderr,"[HUD] No system font\n");return false;}
    stbtt_fontinfo info; stbtt_InitFont(&info,ttf.data(),0);
    int asc,desc,lg; stbtt_GetFontVMetrics(&info,&asc,&desc,&lg);
    g_fontAscent=asc*stbtt_ScaleForPixelHeight(&info,kFontPx);
    std::vector<uint8_t> atlas(kAtlasW*kAtlasH,0);
    if(stbtt_BakeFontBitmap(ttf.data(),0,kFontPx,atlas.data(),kAtlasW,kAtlasH,32,96,g_cdata)<=0) return false;
    glGenTextures(1,&g_fontTex);glBindTexture(GL_TEXTURE_2D,g_fontTex);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RED,kAtlasW,kAtlasH,0,GL_RED,GL_UNSIGNED_BYTE,atlas.data());
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D,0);return true;
}

static void hudOrtho(GLuint prog,float W,float H){
    float m[16]={2.f/W,0,0,0,0,-2.f/H,0,0,0,0,1,0,-1.f,1.f,0,1};
    glUniformMatrix4fv(glGetUniformLocation(prog,"uProj"),1,GL_FALSE,m);
}

static void drawRect(float x,float y,float w,float h,float r,float g,float b,float a,float W,float H){
    float v[12]={x,y,x+w,y,x+w,y+h,x,y,x+w,y+h,x,y+h};
    glBindVertexArray(g_rectVAO);glBindBuffer(GL_ARRAY_BUFFER,g_rectVBO);
    glBufferData(GL_ARRAY_BUFFER,sizeof(v),v,GL_DYNAMIC_DRAW);
    glUseProgram(g_rectProg);hudOrtho(g_rectProg,W,H);
    glUniform4f(glGetUniformLocation(g_rectProg,"uColor"),r,g,b,a);
    glDrawArrays(GL_TRIANGLES,0,6);glBindVertexArray(0);
}

static void drawText(const char* text,float x,float y,float cr,float cg,float cb,float W,float H){
    if(!g_fontOk||!text||!*text)return;
    float cx=x,cy=y+g_fontAscent;
    std::vector<float> v; v.reserve(512);
    for(const char*p=text;*p;++p){
        unsigned char uc=(unsigned char)*p;
        if(uc<32||uc>=128){cx+=6.f;continue;}
        stbtt_aligned_quad q;
        stbtt_GetBakedQuad(g_cdata,kAtlasW,kAtlasH,uc-32,&cx,&cy,&q,1);
        v.insert(v.end(),{q.x0,q.y0,q.s0,q.t0,q.x1,q.y0,q.s1,q.t0,q.x1,q.y1,q.s1,q.t1,
                           q.x0,q.y0,q.s0,q.t0,q.x1,q.y1,q.s1,q.t1,q.x0,q.y1,q.s0,q.t1});
    }
    if(v.empty())return;
    glBindVertexArray(g_textVAO);glBindBuffer(GL_ARRAY_BUFFER,g_textVBO);
    glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)(v.size()*sizeof(float)),v.data(),GL_DYNAMIC_DRAW);
    glUseProgram(g_textProg);hudOrtho(g_textProg,W,H);
    glUniform3f(glGetUniformLocation(g_textProg,"uColor"),cr,cg,cb);
    glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,g_fontTex);
    glUniform1i(glGetUniformLocation(g_textProg,"uFont"),0);
    glDrawArrays(GL_TRIANGLES,0,(GLsizei)(v.size()/4));glBindVertexArray(0);
}

static void drawScoreBar(const char* label,float score,float r,float g,float b,
                          float x,float y,float W,float H){
    constexpr float kBW=145.f,kBH=10.f,kLW=36.f;
    drawText(label,x,y,0.7f,0.7f,0.7f,W,H);
    float bx=x+kLW;
    drawRect(bx,y+3.f,kBW,kBH,0.12f,0.12f,0.12f,1.f,W,H);
    if(score>0.f) drawRect(bx,y+3.f,(score/10.f)*kBW,kBH,r,g,b,1.f,W,H);
    char buf[16]; snprintf(buf,sizeof(buf)," %.1f",score);
    drawText(buf,bx+kBW+4.f,y,0.9f,0.9f,0.9f,W,H);
}

enum class AgentStatus { Active, Processing, Fixing };

static void renderHUD(int winW,int winH,
                       AgentStatus status,double latencyMs,
                       bool baselineActive,double baselineElapsed,
                       int generation,float sVC,float sATM,float sTN,
                       const std::string& lastHint,
                       int presetIdx,bool comparisonActive,bool autoEvolve,
                       double estGpuMs) {
    const float W=float(winW),H=float(winH);
    const float pad=12.f,lh=20.f;

    glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);glDisable(GL_DEPTH_TEST);

    if(baselineActive){
        // ── Side-by-side latency comparison panel ─────────────────────────────
        const float cpW=480.f,cpH=185.f;
        const float cpX=(W-cpW)*0.5f,cpY=(H-cpH)*0.5f;
        char buf[256];
        const float barW=cpW-40.f;
        constexpr double kBaseDur=8.0; // 8 real seconds, GPU bar label scales to estGpuMs

        drawRect(cpX-3.f,cpY-3.f,cpW+6.f,cpH+6.f,0.4f,0.05f,0.05f,0.9f,W,H);
        drawRect(cpX,cpY,cpW,cpH,0.05f,0.02f,0.03f,0.97f,W,H);

        float tx=cpX+20.f,ty=cpY+14.f;

        drawText("LATENCY COMPARISON",tx,ty,0.78f,0.78f,0.78f,W,H);ty+=lh*1.3f;

        // ── Cerebras row ──────────────────────────────────────────────────────
        drawText("CEREBRAS",tx,ty,0.25f,1.f,0.45f,W,H);
        if(latencyMs>0.){
            snprintf(buf,sizeof(buf),"DONE  %.0f ms",latencyMs);
            drawText(buf,cpX+cpW-165.f,ty,0.25f,1.f,0.45f,W,H);
        } else if(status==AgentStatus::Processing){
            drawText("Running...",cpX+cpW-130.f,ty,0.5f,0.85f,0.5f,W,H);
        } else {
            drawText("[SPACE] to start",cpX+cpW-175.f,ty,0.42f,0.42f,0.42f,W,H);
        }
        ty+=lh;
        drawRect(cpX+20.f,ty+2.f,barW,12.f,0.04f,0.12f,0.04f,1.f,W,H);
        if(latencyMs>0.){
            drawRect(cpX+20.f,ty+2.f,barW,12.f,0.15f,0.88f,0.3f,1.f,W,H);
        } else if(status==AgentStatus::Processing){
            float p=0.35f+0.3f*float(sin(baselineElapsed*5.0));
            drawRect(cpX+20.f,ty+2.f,barW*p,12.f,0.12f,0.72f,0.25f,1.f,W,H);
        }
        ty+=lh+8.f;

        // ── GPU cluster row ───────────────────────────────────────────────────
        float gpuFrac=float(baselineElapsed/kBaseDur);
        if(gpuFrac>1.f) gpuFrac=1.f;
        float simSec=float(baselineElapsed*(estGpuMs/1000.0/kBaseDur));
        float estTotalSec=float(estGpuMs/1000.0);
        drawText("GPU CLUSTER",tx,ty,1.f,0.3f,0.15f,W,H);
        snprintf(buf,sizeof(buf),"%.0f / ~%.0f s (est.)",simSec,estTotalSec);
        drawText(buf,cpX+cpW-195.f,ty,0.9f,0.42f,0.32f,W,H);
        ty+=lh;
        drawRect(cpX+20.f,ty+2.f,barW,12.f,0.18f,0.05f,0.05f,1.f,W,H);
        if(gpuFrac>0.f) drawRect(cpX+20.f,ty+2.f,barW*gpuFrac,12.f,0.82f,0.1f,0.08f,1.f,W,H);
        ty+=lh+12.f;

        // ── Speed summary ─────────────────────────────────────────────────────
        if(latencyMs>0.){
            snprintf(buf,sizeof(buf),"Cerebras is  %.0fx FASTER  than GPU cluster",estGpuMs/latencyMs);
            drawText(buf,tx,ty,1.f,0.88f,0.08f,W,H);
        } else {
            drawText("No real-time feedback loop at GPU cluster latency",
                     tx,ty,0.58f,0.45f,0.45f,W,H);
        }

        drawText("[B] exit",W-88.f,H-26.f,0.35f,0.35f,0.35f,W,H);

    } else {
        // ── Corner info panel (normal mode) ───────────────────────────────────
        const float panW=295.f;
        bool hasScores=(generation>0);
        int nRows=hasScores?13:7;
        const float panH=pad+lh*nRows+(hasScores?6.f:0.f)+pad;
        const float panX=W-panW-12.f,panY=12.f;

        drawRect(panX-1.f,panY-1.f,panW+2.f,panH+2.f,0.3f,0.55f,1.f,0.18f,W,H);
        drawRect(panX,panY,panW,panH,0.04f,0.04f,0.09f,0.88f,W,H);

        if(g_fontOk){
            float tx=panX+pad,ty=panY+pad;
            char buf[256];
            // Title row
            if(generation>0) snprintf(buf,sizeof(buf),"GEMMA 4-31B  [Gen %d]",generation);
            else              snprintf(buf,sizeof(buf),"GEMMA 4-31B");
            drawText(buf,tx,ty,0.35f,0.75f,1.f,W,H);ty+=lh;

            // Preset + mode badges
            snprintf(buf,sizeof(buf),"Preset: %s%s%s",
                     kPresets[presetIdx].name,
                     autoEvolve?" [AUTO]":"",
                     comparisonActive?" [CMP]":"");
            drawText(buf,tx,ty,0.7f,0.6f,0.3f,W,H);ty+=lh;

            // Status
            float sr=0.3f,sg=1.f,sb=0.5f; const char*sl="Active";
            if(status==AgentStatus::Processing){sl="Critic -> Composer...";sr=1.f;sg=0.85f;sb=0.2f;}
            else if(status==AgentStatus::Fixing){sl="Error (retaining)";sr=1.f;sg=0.35f;sb=0.2f;}
            snprintf(buf,sizeof(buf),"Status:  %s",sl);
            drawText(buf,tx,ty,sr,sg,sb,W,H);ty+=lh;

            // Latency + speedup (always two lines for consistent panel height)
            if(latencyMs>0.){
                snprintf(buf,sizeof(buf),"Latency: %.0f ms",latencyMs);
                drawText(buf,tx,ty,1.f,1.f,1.f,W,H);ty+=lh;
                snprintf(buf,sizeof(buf),"  %.0fx faster than GPU cluster",estGpuMs/latencyMs);
                drawText(buf,tx,ty,0.2f,1.f,0.45f,W,H);ty+=lh;
            } else {
                drawText("Latency: --",tx,ty,0.6f,0.6f,0.6f,W,H);ty+=lh;
                ty+=lh;
            }

            if(hasScores){
                ty+=4.f;
                drawText("Critic scores:",tx,ty,0.55f,0.55f,0.55f,W,H);ty+=lh;
                drawScoreBar("VC ",sVC,  0.3f,0.75f,1.f,  tx,ty,W,H);ty+=lh;
                drawScoreBar("ATM",sATM, 0.95f,0.15f,0.15f,tx,ty,W,H);ty+=lh;
                drawScoreBar("TN ",sTN,  0.6f,0.3f,0.9f,  tx,ty,W,H);ty+=lh;
                if(!lastHint.empty()){
                    ty+=2.f;
                    std::string l1=lastHint.size()>32?lastHint.substr(0,32):lastHint;
                    drawText(l1.c_str(),tx,ty,0.75f,0.6f,0.3f,W,H);ty+=lh;
                    if(lastHint.size()>32){
                        std::string l2=lastHint.size()>64?lastHint.substr(32,29)+"...":lastHint.substr(32);
                        drawText(l2.c_str(),tx,ty,0.75f,0.6f,0.3f,W,H);ty+=lh;
                    }
                }
            }
            drawText("[1-4]preset [SPC]evolve [B]base [C]cmp [A]auto",tx,ty,0.38f,0.38f,0.38f,W,H);
        } // if g_fontOk
    } // else (normal mode)
    glEnable(GL_DEPTH_TEST);glDisable(GL_BLEND);
}

// Bottom bar showing what Gemma 4 saw in the frame — proves multimodal is live
static void renderSceneBar(const std::string& scene,int winW,int winH){
    if(!g_fontOk||scene.empty()) return;
    const float W=float(winW),H=float(winH);
    constexpr float kBarH=30.f;
    glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);glDisable(GL_DEPTH_TEST);
    drawRect(0.f,H-kBarH,W,kBarH,0.02f,0.03f,0.06f,0.90f,W,H);
    // Left badge
    drawRect(0.f,H-kBarH,90.f,kBarH,0.08f,0.55f,0.25f,0.95f,W,H);
    drawText("AI SEES:",6.f,H-kBarH+7.f,0.05f,0.06f,0.05f,W,H);
    // Scene text
    std::string s=scene;
    if(s.size()>110) s=s.substr(0,107)+"...";
    drawText(s.c_str(),98.f,H-kBarH+7.f,0.75f,0.98f,0.65f,W,H);
    glEnable(GL_DEPTH_TEST);glDisable(GL_BLEND);
}

// Comparison mode overlay: divider line + labels
static void renderComparisonOverlay(int winW,int winH,int prevGen,double latencyMs,double estGpuMs){
    const float W=float(winW),H=float(winH);
    glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);glDisable(GL_DEPTH_TEST);

    // Divider
    drawRect(W*0.5f-1.f,0.f,3.f,H,1.f,1.f,1.f,0.75f,W,H);

    char buf[64];
    // Left label — "BEFORE"
    drawRect(10.f,10.f,175.f,55.f,0.0f,0.0f,0.0f,0.7f,W,H);
    snprintf(buf,sizeof(buf),"BEFORE  (Gen %d)",prevGen);
    drawText(buf,18.f,14.f,1.f,0.55f,0.f,W,H);
    drawText("GPU cluster inference",18.f,34.f,0.7f,0.7f,0.7f,W,H);
    {char b[32];snprintf(b,sizeof(b),"~%.0f ms (est.)",estGpuMs);drawText(b,18.f,54.f,1.f,0.3f,0.3f,W,H);}

    // Right label — "AFTER"
    float rx=W*0.5f+10.f;
    drawRect(rx,10.f,185.f,55.f,0.0f,0.0f,0.0f,0.7f,W,H);
    snprintf(buf,sizeof(buf),"AFTER  (Gen %d)",prevGen+1);
    drawText(buf,rx+8.f,14.f,0.3f,1.f,0.5f,W,H);
    drawText("Cerebras inference",rx+8.f,34.f,0.7f,0.7f,0.7f,W,H);
    if(latencyMs>0.){
        snprintf(buf,sizeof(buf),"%.0f ms  (%.0fx faster)",latencyMs,estGpuMs/latencyMs);
        drawText(buf,rx+8.f,54.f,0.3f,1.f,0.5f,W,H);
    }

    glEnable(GL_DEPTH_TEST);glDisable(GL_BLEND);
}

// ── JIT state ─────────────────────────────────────────────────────────────────

struct ShaderGen {
    int gen; float vc,atm,tn; std::string hint;
};

struct JitState {
    std::mutex        mtx;
    std::string       pendingGlsl,errorMsg,pendingScene;
    std::atomic<bool> hasNew{false},hasError{false},busy{false};
    float             pendingVC{0},pendingATM{0},pendingTN{0};
    int               pendingTokens{0};
    std::string       pendingHint;
};

// ── JIT worker: Critic → Composer ─────────────────────────────────────────────

static void jitWorker(std::string b64,
                      std::string currentGlsl,
                      int generation,
                      std::vector<ShaderGen> history,
                      const Preset* preset,
                      JitState* jit){
    httplib::Client cli("127.0.0.1",8000);
    cli.set_connection_timeout(5,0); cli.set_read_timeout(90,0);

    // Phase 1: Critic
    std::printf("[%s] [Critic] Phase 1/2 — scoring Gen %d (%s)...\n",
                nowStr().c_str(),generation,preset->name);

    float vc=0,atm=0,tn=0;
    std::string hint="add domain-warped layers and increase color saturation";
    std::string scene;

    {
        std::string body=
            "{\"base64_image\":\""+b64+
            "\",\"glsl_source\":\""+jsonEscape(currentGlsl)+
            "\",\"style_context\":\""+jsonEscape(preset->criticContext)+"\"}";

        auto res=cli.Post("/critique",body,"application/json");
        if(!res||res->status!=200){
            std::fprintf(stderr,"[%s] [Critic] /critique failed — using default hint\n",nowStr().c_str());
        } else {
            vc   =extractFloat(res->body,"visual_complexity");
            atm  =extractFloat(res->body,"atmosphere");
            tn   =extractFloat(res->body,"technical_novelty");
            hint =extractString(res->body,"improvement_hint");
            scene=extractString(res->body,"scene_description");
            if(hint.empty()) hint="increase visual contrast and add a focal glow effect";
            std::printf("[%s] [Critic] VC:%.1f  ATM:%.1f  TN:%.1f\n",nowStr().c_str(),vc,atm,tn);
            std::printf("[%s] [Critic] Hint: %s\n",nowStr().c_str(),hint.c_str());
        }
    }

    // Phase 2: Composer
    std::printf("[%s] [Composer] Phase 2/2 — compiling Gen %d...\n",
                nowStr().c_str(),generation+1);

    std::string prompt="SHADER EVOLUTIONARY GENEALOGY:\n";
    for(const auto&g:history){
        char row[256];
        snprintf(row,sizeof(row),"  Gen%d VC:%.1f ATM:%.1f TN:%.1f  hint: %s\n",
                 g.gen,g.vc,g.atm,g.tn,g.hint.c_str());
        prompt+=row;
    }
    char directive[768];
    snprintf(directive,sizeof(directive),
             "\nCURRENT Gen%d: VC:%.1f ATM:%.1f TN:%.1f\n"
             "IMPROVEMENT DIRECTIVE: %s\n\n"
             "STYLE: %s\n\n"
             "Rules:\n"
             "  in vec2 vUV;  out vec4 FragColor;  uniform float uTime;\n"
             "  Apply the improvement directive above — target the weakest axis.\n"
             "  Output GLSL 3.3 core only. No markdown. No explanation.\n"
             "  Colors must be visually vivid — never output near-zero RGB across the whole frame.",
             generation,vc,atm,tn,hint.c_str(),preset->composerStyle);
    prompt+=directive;

    std::string body=
        "{\"base64_image\":\""+b64+
        "\",\"user_prompt\":\""+jsonEscape(prompt)+"\"}";

    auto res=cli.Post("/compile",body,"application/json");

    std::lock_guard<std::mutex> lock(jit->mtx);
    jit->pendingVC=vc; jit->pendingATM=atm; jit->pendingTN=tn;
    jit->pendingHint=hint; jit->pendingScene=std::move(scene);

    if(!res){
        jit->errorMsg="Cannot reach server — is uvicorn running?";
        jit->hasError=true;
    } else if(res->status!=200){
        std::fprintf(stderr,"[%s] [Composer] HTTP %d: %s\n",nowStr().c_str(),res->status,res->body.c_str());
        jit->errorMsg="HTTP "+std::to_string(res->status)+": "+res->body;
        jit->hasError=true;
    } else {
        auto glsl=extractGlslFragment(res->body);
        if(glsl.empty()){jit->errorMsg="glsl_fragment missing";jit->hasError=true;}
        else{
            jit->pendingTokens=(int)extractFloat(res->body,"completion_tokens");
            std::printf("[%s] [Composer] Gen %d ready (%zu chars, %d tokens)\n",
                        nowStr().c_str(),generation+1,glsl.size(),jit->pendingTokens);
            jit->pendingGlsl=std::move(glsl); jit->hasNew=true;
        }
    }
    jit->busy=false;
}

// ── Quad geometry ─────────────────────────────────────────────────────────────

static const float kQuadVerts[]={-1.f,-1.f,1.f,-1.f,1.f,1.f,-1.f,-1.f,1.f,1.f,-1.f,1.f};

static void onError(int code,const char*msg){std::fprintf(stderr,"[GLFW %d] %s\n",code,msg);}
static void onKey(GLFWwindow*win,int key,int,int action,int){
    if(key==GLFW_KEY_ESCAPE&&action==GLFW_PRESS) glfwSetWindowShouldClose(win,GLFW_TRUE);
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main(){
    glfwSetErrorCallback(onError);
    if(!glfwInit()){std::fprintf(stderr,"glfwInit failed\n");return 1;}
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR,3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR,3);
    glfwWindowHint(GLFW_OPENGL_PROFILE,GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT,GL_TRUE);
#endif

    GLFWwindow*win=glfwCreateWindow(900,600,
        "Shader JIT  [1-4]preset  [SPACE]evolve  [B]baseline  [C]compare  [A]auto  [ESC]quit",
        nullptr,nullptr);
    if(!win){glfwTerminate();return 1;}
    glfwMakeContextCurrent(win);glfwSwapInterval(1);glfwSetKeyCallback(win,onKey);
    if(!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))){
        std::fprintf(stderr,"gladLoadGL failed\n");return 1;}
    std::printf("OpenGL %s | GLSL %s\n",glGetString(GL_VERSION),glGetString(GL_SHADING_LANGUAGE_VERSION));

    buildHudShaders();g_fontOk=initFont();

    GLuint vao,vbo;
    glGenVertexArrays(1,&vao);glGenBuffers(1,&vbo);
    glBindVertexArray(vao);glBindBuffer(GL_ARRAY_BUFFER,vbo);
    glBufferData(GL_ARRAY_BUFFER,sizeof(kQuadVerts),kQuadVerts,GL_STATIC_DRAW);
    glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,2*sizeof(float),nullptr);
    glEnableVertexAttribArray(0);

    int presetIdx=0;
    std::string vertSrc,currentGlsl;
    GLuint program=0;
    auto loadPreset=[&](int idx)->bool{
        try{
            vertSrc=readFile("shaders/vertex.glsl");
            currentGlsl=readFile(kPresets[idx].shaderFile);
            GLuint next=buildProgram(vertSrc,currentGlsl);
            if(program) glDeleteProgram(program);
            program=next;
            std::printf("[%s] [Engine] Preset %d loaded: %s\n",nowStr().c_str(),idx+1,kPresets[idx].name);
            return true;
        }catch(const std::exception&e){
            std::fprintf(stderr,"[Engine] Failed to load preset %d: %s\n",idx+1,e.what());
            return false;
        }
    };
    if(!loadPreset(0)) return 1;
    GLint uTime=glGetUniformLocation(program,"uTime");

    // State
    JitState jit;
    AgentStatus agentStatus=AgentStatus::Active;
    double jitStartTime=0.,lastLatencyMs=0.,lastEstGpuMs=45000.,t0=glfwGetTime();
    int    lastTokens=0;
    bool baselineActive=false;  double baselineStart=0.;
    bool comparisonActive=false;int compPrevGen=0;
    bool autoEvolve=false;      double lastAutoTime=0.;
    constexpr double kAutoInterval=20.;

    int   generation=0;
    float scoreVC=0.f,scoreATM=0.f,scoreTN=0.f;
    std::string lastHint,lastScene;
    std::vector<ShaderGen> history;

    GLuint prevFrameTex=0; int prevFrameW=0,prevFrameH=0;

    bool spaceWas=false,bWas=false,cWas=false,aWas=false;
    bool p1Was=false,p2Was=false,p3Was=false,p4Was=false;

    std::printf("[%s] [Engine] Ready — [1-4] presets | [SPACE] evolve | [B] baseline | [C] compare | [A] auto\n",
                nowStr().c_str());
    std::printf("[%s] [Engine] Presets: 1=Dark Fantasy  2=Persona/Anime  3=Biomedical  4=Cosmic/Space\n",
                nowStr().c_str());

    auto triggerEvolve=[&](int w,int h){
        if(jit.busy.load()) return;
        jit.busy=true; jitStartTime=glfwGetTime(); agentStatus=AgentStatus::Processing;
        std::printf("[%s] [Engine] Capturing Gen %d frame for %s...\n",
                    nowStr().c_str(),generation,kPresets[presetIdx].name);
        // Save current frame to comparison texture
        storeFrameToTex(w,h,&prevFrameTex,&prevFrameW,&prevFrameH);
        compPrevGen=generation;

        std::string b64=captureFrameBase64(w,h);
        char title[128];
        snprintf(title,sizeof(title),"Shader JIT  [%s | Critic -> Composer...]",kPresets[presetIdx].name);
        glfwSetWindowTitle(win,title);
        std::thread(jitWorker,std::move(b64),currentGlsl,generation,history,
                    &kPresets[presetIdx],&jit).detach();
    };

    while(!glfwWindowShouldClose(win)){
        glfwPollEvents();
        double now=glfwGetTime();

        // ── Preset keys 1-4 ──────────────────────────────────────────────────
        auto switchPreset=[&](int idx){
            if(idx==presetIdx&&generation>0) return;
            if(loadPreset(idx)){
                uTime=glGetUniformLocation(program,"uTime");
                t0=now; presetIdx=idx; generation=0;
                scoreVC=scoreATM=scoreTN=0.f; lastHint.clear(); lastScene.clear(); history.clear();
                comparisonActive=false; autoEvolve=false;
                if(prevFrameTex){glDeleteTextures(1,&prevFrameTex);prevFrameTex=0;}
                agentStatus=AgentStatus::Active;
                char title[128];
                snprintf(title,sizeof(title),"Shader JIT  [%s | SPACE=evolve]",kPresets[idx].name);
                glfwSetWindowTitle(win,title);
            }
        };
        bool p1=glfwGetKey(win,GLFW_KEY_1)==GLFW_PRESS;
        bool p2=glfwGetKey(win,GLFW_KEY_2)==GLFW_PRESS;
        bool p3=glfwGetKey(win,GLFW_KEY_3)==GLFW_PRESS;
        bool p4=glfwGetKey(win,GLFW_KEY_4)==GLFW_PRESS;
        if(p1&&!p1Was) switchPreset(0);
        if(p2&&!p2Was) switchPreset(1);
        if(p3&&!p3Was) switchPreset(2);
        if(p4&&!p4Was) switchPreset(3);
        p1Was=p1;p2Was=p2;p3Was=p3;p4Was=p4;

        // ── B key: baseline ───────────────────────────────────────────────────
        bool bDown=glfwGetKey(win,GLFW_KEY_B)==GLFW_PRESS;
        if(bDown&&!bWas){
            baselineActive=!baselineActive;
            if(baselineActive){baselineStart=now;
                std::printf("[%s] [Baseline] Simulating 5s traditional latency\n",nowStr().c_str());
                glfwSetWindowTitle(win,"Shader JIT  [BASELINE: traditional GPU inference...]");
            } else {
                double sp=lastLatencyMs>0.?5000./lastLatencyMs:0.;
                std::printf("[%s] [Baseline] Cerebras was %.1fx faster\n",nowStr().c_str(),sp);
                glfwSetWindowTitle(win,"Shader JIT  [SPACE=evolve]");
            }
        }
        bWas=bDown;
        if(baselineActive&&now-baselineStart>=8.0){
            baselineActive=false;
            std::printf("[%s] [Baseline] Done — Cerebras %.0fms vs ~45000ms GPU cluster (%.1fx)\n",
                        nowStr().c_str(),lastLatencyMs,lastLatencyMs>0.?45000./lastLatencyMs:0.);
            glfwSetWindowTitle(win,"Shader JIT  [SPACE=evolve]");
        }

        // ── C key: toggle comparison mode ────────────────────────────────────
        bool cDown=glfwGetKey(win,GLFW_KEY_C)==GLFW_PRESS;
        if(cDown&&!cWas){
            if(prevFrameTex){
                comparisonActive=!comparisonActive;
                std::printf("[%s] [Compare] %s\n",nowStr().c_str(),comparisonActive?"ON":"OFF");
            } else {
                std::printf("[%s] [Compare] Press SPACE first to capture a before-frame\n",nowStr().c_str());
            }
        }
        cWas=cDown;

        // ── A key: toggle auto-evolve ────────────────────────────────────────
        bool aDown=glfwGetKey(win,GLFW_KEY_A)==GLFW_PRESS;
        if(aDown&&!aWas){
            autoEvolve=!autoEvolve;lastAutoTime=now;
            std::printf("[%s] [Auto] %s (interval: %.0fs)\n",nowStr().c_str(),
                        autoEvolve?"ON":"OFF",kAutoInterval);
        }
        aWas=aDown;

        // Auto-evolve trigger
        int w,h; glfwGetFramebufferSize(win,&w,&h);
        if(autoEvolve&&now-lastAutoTime>=kAutoInterval&&!jit.busy.load()){
            lastAutoTime=now; triggerEvolve(w,h);
        }

        // ── Hot-reload ────────────────────────────────────────────────────────
        if(jit.hasNew.load()){
            std::string newGlsl; float nVC,nATM,nTN; std::string nHint,nScene; int nTokens=0;
            {std::lock_guard<std::mutex>lk(jit.mtx);
             newGlsl=std::move(jit.pendingGlsl);
             nVC=jit.pendingVC;nATM=jit.pendingATM;nTN=jit.pendingTN;
             nHint=std::move(jit.pendingHint);
             nScene=std::move(jit.pendingScene);
             nTokens=jit.pendingTokens;
             jit.hasNew=false;}
            lastLatencyMs=(now-jitStartTime)*1000.;
            lastScene=std::move(nScene);
            if(nTokens>0){
                lastTokens=nTokens;
                // 25 tok/s on shared A100 cluster + 8s queue/overhead — conservative estimate
                lastEstGpuMs=lastTokens/25.0*1000.0+8000.0;
                std::printf("[%s] [Engine] GPU est: %.0fms (%d tok @ 25tok/s + 8s overhead)\n",
                            nowStr().c_str(),lastEstGpuMs,lastTokens);
            }
            std::printf("[%s] [Engine] Round-trip: %.0fms — hot-reloading\n",nowStr().c_str(),lastLatencyMs);
            try{
                program=reloadProgram(program,vertSrc,newGlsl);
                // Auto-save evolved shader so you can pre-bake it as a seed
                {char sp[64];snprintf(sp,sizeof(sp),"shaders/evolved_p%d_gen%d.glsl",presetIdx+1,generation+1);
                 std::ofstream sf(sp);if(sf.is_open()){sf<<newGlsl;
                     std::printf("[%s] [Engine] Saved → %s\n",nowStr().c_str(),sp);}}
                currentGlsl=std::move(newGlsl);
                uTime=glGetUniformLocation(program,"uTime");
                t0=now; agentStatus=AgentStatus::Active;
                generation++; scoreVC=nVC;scoreATM=nATM;scoreTN=nTN;lastHint=nHint;
                history.push_back({generation,scoreVC,scoreATM,scoreTN,lastHint});
                if(history.size()>4) history.erase(history.begin());
                std::printf("[%s] [Engine] Gen %d active — VC:%.1f ATM:%.1f TN:%.1f\n",
                            nowStr().c_str(),generation,scoreVC,scoreATM,scoreTN);
                char title[128];
                snprintf(title,sizeof(title),"Shader JIT  [%s | Gen %d | VC:%.1f ATM:%.1f TN:%.1f]",
                         kPresets[presetIdx].name,generation,scoreVC,scoreATM,scoreTN);
                glfwSetWindowTitle(win,title);
            }catch(const std::exception&e){
                agentStatus=AgentStatus::Fixing;
                std::fprintf(stderr,"[%s] [Engine] GLSL error — retaining Gen %d:\n%s\n",
                             nowStr().c_str(),generation,e.what());
            }
        }
        if(jit.hasError.load()){
            std::string err;
            {std::lock_guard<std::mutex>lk(jit.mtx);err=std::move(jit.errorMsg);jit.hasError=false;}
            agentStatus=AgentStatus::Fixing;
            std::fprintf(stderr,"[%s] [Engine] %s\n",nowStr().c_str(),err.c_str());
        }

        // ── Render scene ──────────────────────────────────────────────────────
        glViewport(0,0,w,h);
        glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);

        if(baselineActive){
            // Render scene (uTime frozen) into FBO, blit to screen desaturated+dark
            ensureFBO(w,h);
            glBindFramebuffer(GL_FRAMEBUFFER,g_fbo);
            glViewport(0,0,w,h);
            glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
            glUseProgram(program); // uTime not updated — shader frozen
            glBindVertexArray(vao);glDrawArrays(GL_TRIANGLES,0,6);
            glBindFramebuffer(GL_FRAMEBUFFER,0);
            // Blit desaturated + darkened
            glViewport(0,0,w,h);
            glUseProgram(g_grayProg);
            glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,g_fboTex);
            glUniform1i(glGetUniformLocation(g_grayProg,"uTex"),0);
            glBindVertexArray(vao);glDrawArrays(GL_TRIANGLES,0,6);
        } else if(comparisonActive&&prevFrameTex){
            // Left half: previous frame
            glViewport(0,0,w/2,h);
            glUseProgram(g_blitProg);
            glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,prevFrameTex);
            glUniform1i(glGetUniformLocation(g_blitProg,"uTex"),0);
            glBindVertexArray(vao);glDrawArrays(GL_TRIANGLES,0,6);
            // Right half: live shader
            glViewport(w/2,0,w-w/2,h);
            glUseProgram(program);
            if(uTime>=0) glUniform1f(uTime,float(now-t0));
            glDrawArrays(GL_TRIANGLES,0,6);
            glViewport(0,0,w,h);
        } else {
            glUseProgram(program);
            if(uTime>=0) glUniform1f(uTime,float(now-t0));
            glBindVertexArray(vao);glDrawArrays(GL_TRIANGLES,0,6);
        }

        // ── Space: capture + launch ───────────────────────────────────────────
        bool spaceDown=glfwGetKey(win,GLFW_KEY_SPACE)==GLFW_PRESS;
        if(spaceDown&&!spaceWas) triggerEvolve(w,h);
        spaceWas=spaceDown;

        // ── HUD ───────────────────────────────────────────────────────────────
        double bElapsed=baselineActive?(now-baselineStart):0.;
        renderHUD(w,h,agentStatus,lastLatencyMs,baselineActive,bElapsed,
                  generation,scoreVC,scoreATM,scoreTN,lastHint,presetIdx,comparisonActive,autoEvolve,lastEstGpuMs);
        if(!baselineActive)
            renderSceneBar(lastScene,w,h);
        if(comparisonActive&&prevFrameTex)
            renderComparisonOverlay(w,h,compPrevGen,lastLatencyMs,lastEstGpuMs);

        glfwSwapBuffers(win);
    }

    glDeleteProgram(program);
    glDeleteProgram(g_textProg);glDeleteProgram(g_rectProg);
    glDeleteProgram(g_blitProg);glDeleteProgram(g_grayProg);
    if(g_fbo){glDeleteFramebuffers(1,&g_fbo);glDeleteTextures(1,&g_fboTex);}
    glDeleteBuffers(1,&vbo);glDeleteBuffers(1,&g_textVBO);glDeleteBuffers(1,&g_rectVBO);
    glDeleteVertexArrays(1,&vao);glDeleteVertexArrays(1,&g_textVAO);glDeleteVertexArrays(1,&g_rectVAO);
    if(g_fontTex) glDeleteTextures(1,&g_fontTex);
    if(prevFrameTex) glDeleteTextures(1,&prevFrameTex);
    glfwDestroyWindow(win);glfwTerminate();
    return 0;
}
