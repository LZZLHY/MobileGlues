// Real FSR/ANGLE-clear implementations, with a stateful GLES/EGL double.
#include "gl/FSR1/FSR1.h"
#include "config/settings.h"
#include "egl/context.h"
#include <cassert>
#include <map>
#include <thread>
#include <cstdio>
gles_func_t g_gles_func{};
gles_caps_t g_gles_caps{};
global_settings_t global_settings{};
hardware_s hw{320, false};
hardware_t hardware = &hw;
char* glsl_cache_file_path = nullptr;
gl_state_s g_default_gl_state{};
thread_local gl_state_t gl_state = &g_default_gl_state;
thread_local MGContext* g_current_ctx = nullptr;
void *gles = nullptr, *egl = reinterpret_cast<void*>(1);
void (*mg_texture_barrier_backend)() = nullptr;
extern "C" void write_log(const char*, ...) {}
int __android_log_print(int, const char*, const char*, ...) {
    return 0;
}
void DrawDepthClearTri();
extern "C" void glTextureBarrier();
namespace {
    GLuint next = 100;
    GLenum pending = 0, driverError = 0;
    int surfaceWidth = 640, surfaceHeight = 480, allocations = 0, draws = 0;
    bool validSurface = true, incomplete = false;
    int barriers = 0;
    void textureBarrier() {
        ++barriers;
    }
    GLint program = 7, vao = 9, array = 11, active = GL_TEXTURE3, read = 0, draw = 0, renderbuffer = 0, unpack = 27;
    GLint viewport[4] = {2, 3, 300, 200};
    GLuint textures[32]{}, samplers[32]{};
    GLfloat clearColor[4] = {.25f, .5f, .75f, 1};
    GLboolean colorMask[4] = {0, 0, 0, 0}, depthMask = GL_TRUE;
    GLint depthFunc = GL_LESS;
    std::map<GLenum, GLboolean> enabled;
    void getInt(GLenum p, GLint* v) {
        switch (p) {
        case GL_CURRENT_PROGRAM:
            *v = program;
            break;
        case GL_VERTEX_ARRAY_BINDING:
            *v = vao;
            break;
        case GL_ARRAY_BUFFER_BINDING:
            *v = array;
            break;
        case GL_ACTIVE_TEXTURE:
            *v = active;
            break;
        case GL_TEXTURE_BINDING_2D:
            *v = textures[active - GL_TEXTURE0];
            break;
        case GL_SAMPLER_BINDING:
            *v = samplers[active - GL_TEXTURE0];
            break;
        case GL_READ_FRAMEBUFFER_BINDING:
            *v = read;
            break;
        case GL_DRAW_FRAMEBUFFER_BINDING:
            *v = draw;
            break;
        case GL_RENDERBUFFER_BINDING:
            *v = renderbuffer;
            break;
        case GL_PIXEL_UNPACK_BUFFER_BINDING:
            *v = unpack;
            break;
        case GL_VIEWPORT:
            for (int i = 0; i < 4; ++i)
                v[i] = viewport[i];
            break;
        case GL_DEPTH_FUNC:
            *v = depthFunc;
            break;
        default:
            *v = 0;
        }
    }
    void getBool(GLenum p, GLboolean* v) {
        if (p == GL_COLOR_WRITEMASK)
            for (int i = 0; i < 4; ++i)
                v[i] = colorMask[i];
        else
            *v = depthMask;
    }
    void getFloat(GLenum, GLfloat* v) {
        for (int i = 0; i < 4; ++i)
            v[i] = clearColor[i];
    }
    GLboolean isEnabled(GLenum p) {
        return enabled[p];
    }
    void enable(GLenum p) {
        enabled[p] = true;
    }
    void disable(GLenum p) {
        enabled[p] = false;
    }
    void use(GLuint p) {
        program = p;
    }
    void bindVAO(GLuint p) {
        vao = p;
    }
    void bindBuffer(GLenum t, GLuint p) {
        if (t == GL_ARRAY_BUFFER)
            array = p;
        else if (t == GL_PIXEL_UNPACK_BUFFER)
            unpack = p;
    }
    void activeTexture(GLenum p) {
        active = p;
    }
    void bindTexture(GLenum, GLuint p) {
        textures[active - GL_TEXTURE0] = p;
    }
    void bindSampler(GLuint unit, GLuint sampler) {
        samplers[unit] = sampler;
    }
    void bindFramebuffer(GLenum target, GLuint p) {
        if (target != GL_DRAW_FRAMEBUFFER) read = p;
        if (target != GL_READ_FRAMEBUFFER) draw = p;
    }
    void bindRenderbuffer(GLenum, GLuint p) {
        renderbuffer = p;
    }
    void setViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
        viewport[0] = x;
        viewport[1] = y;
        viewport[2] = w;
        viewport[3] = h;
    }
    void setColor(GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
        colorMask[0] = r;
        colorMask[1] = g;
        colorMask[2] = b;
        colorMask[3] = a;
    }
    void setClear(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
        clearColor[0] = r;
        clearColor[1] = g;
        clearColor[2] = b;
        clearColor[3] = a;
    }
    void setDepthMask(GLboolean v) {
        depthMask = v;
    }
    void setDepthFunc(GLenum v) {
        depthFunc = v;
    }
    void gen(GLsizei n, GLuint* ids) {
        while (n--)
            *ids++ = next++;
    }
    void del(GLsizei, const GLuint*) {}
    void delObject(GLuint) {}
    GLuint createShader(GLenum) {
        return next++;
    }
    GLuint createProgram() {
        return next++;
    }
    void source(GLuint, GLsizei count, const GLchar* const* text, const GLint*) {
        assert(count == 1);
        assert(std::string(text[0]).find("#version 450") == std::string::npos);
    }
    void compile(GLuint) {}
    void attach(GLuint, GLuint) {}
    void link(GLuint) {}
    void getStatus(GLuint, GLenum, GLint* status) {
        *status = GL_TRUE;
    }
    void infoLog(GLuint, GLsizei, GLsizei*, GLchar*) {}
    GLint location(GLuint, const GLchar*) {
        return 1;
    }
    void uniform(GLint, GLint) {}
    void uniform4(GLint, GLsizei, const GLfloat*) {}
    void uniform2(GLint, GLsizei, const GLfloat*) {}
    void data(GLenum, GLsizeiptr, const void*, GLenum) {}
    void attrib(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*) {}
    void enableAttrib(GLuint) {}
    void texImage(GLenum, GLint, GLint format, GLsizei w, GLsizei h, GLint, GLenum, GLenum, const void*) {
        assert(w > 0 && h > 0 && format == GL_RGBA8 && unpack == 0);
        ++allocations;
    }
    void texParam(GLenum, GLenum, GLint) {}
    void renderStorage(GLenum, GLenum, GLsizei w, GLsizei h) {
        assert(w > 0 && h > 0);
        ++allocations;
    }
    void frameTexture(GLenum, GLenum, GLenum, GLuint, GLint) {}
    void frameRender(GLenum, GLenum, GLenum, GLuint) {}
    GLenum frameStatus(GLenum) {
        return incomplete ? GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT : GL_FRAMEBUFFER_COMPLETE;
    }
    GLenum getError() {
        GLenum e = driverError;
        driverError = 0;
        return e;
    }
    void clear(GLbitfield) {}
    void drawArrays(GLenum, GLint, GLsizei count) {
        if (count == 6) {
            ++draws;
            assert(colorMask[0] && colorMask[1] && !enabled[GL_SCISSOR_TEST] && !enabled[GL_BLEND] &&
                   !enabled[GL_RASTERIZER_DISCARD] && samplers[0] == 0);
        }
    }
    void blit(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum) {
        assert(!enabled[GL_SCISSOR_TEST]);
    }
    EGLBoolean query(EGLDisplay, EGLSurface, EGLint p, EGLint* v) {
        if (!validSurface) return EGL_FALSE;
        *v = p == EGL_WIDTH ? surfaceWidth : surfaceHeight;
        return EGL_TRUE;
    }
} // namespace
void mg_set_gl_error(GLenum e) {
    pending = e;
}
void mg_begin_driver_operation() {
    if (GLenum e = getError()) pending = e;
}
bool mg_end_driver_operation(const char*) {
    GLenum e = getError();
    if (e) pending = e;
    return !e;
}
bool mg_draw_framebuffer_all_none() {
    return true;
}
extern "C" void* proc_address(void*, const char* name) {
    return std::string(name) == "eglQuerySurface" ? reinterpret_cast<void*>(&query) : nullptr;
}
extern "C" EGLDisplay eglGetCurrentDisplay() {
    return reinterpret_cast<EGLDisplay>(1);
}
extern "C" EGLSurface eglGetCurrentSurface(EGLint) {
    return reinterpret_cast<EGLSurface>(1);
}
extern "C" void glBindFramebuffer(GLenum target, GLuint value) {
    if (value == 0 && target != GL_READ_FRAMEBUFFER) value = FSR1_Context::g_renderFBO;
    bindFramebuffer(target, value);
    if (target != GL_READ_FRAMEBUFFER) gl_state->current_draw_fbo = value;
}
void set_gl_state_current_draw_fbo(GLuint value) {
    gl_state->current_draw_fbo = value;
}

int main() {
    GLES.glGetIntegerv = getInt;
    GLES.glGetBooleanv = getBool;
    GLES.glGetFloatv = getFloat;
    GLES.glIsEnabled = isEnabled;
    GLES.glEnable = enable;
    GLES.glDisable = disable;
    GLES.glUseProgram = use;
    GLES.glBindVertexArray = bindVAO;
    GLES.glBindBuffer = bindBuffer;
    GLES.glActiveTexture = activeTexture;
    GLES.glBindTexture = bindTexture;
    GLES.glBindSampler = bindSampler;
    GLES.glBindFramebuffer = bindFramebuffer;
    GLES.glBindRenderbuffer = bindRenderbuffer;
    GLES.glViewport = setViewport;
    GLES.glColorMask = setColor;
    GLES.glClearColor = setClear;
    GLES.glDepthMask = setDepthMask;
    GLES.glDepthFunc = setDepthFunc;
    GLES.glGenTextures = gen;
    GLES.glGenBuffers = gen;
    GLES.glGenFramebuffers = gen;
    GLES.glGenRenderbuffers = gen;
    GLES.glGenVertexArrays = gen;
    GLES.glDeleteTextures = del;
    GLES.glDeleteBuffers = del;
    GLES.glDeleteFramebuffers = del;
    GLES.glDeleteRenderbuffers = del;
    GLES.glDeleteVertexArrays = del;
    GLES.glCreateShader = createShader;
    GLES.glCreateProgram = createProgram;
    GLES.glShaderSource = source;
    GLES.glCompileShader = compile;
    GLES.glAttachShader = attach;
    GLES.glLinkProgram = link;
    GLES.glDeleteShader = delObject;
    GLES.glDeleteProgram = delObject;
    GLES.glGetShaderiv = getStatus;
    GLES.glGetProgramiv = getStatus;
    GLES.glGetShaderInfoLog = infoLog;
    GLES.glGetProgramInfoLog = infoLog;
    GLES.glGetUniformLocation = location;
    GLES.glUniform1i = uniform;
    GLES.glUniform4fv = uniform4;
    GLES.glUniform2fv = uniform2;
    GLES.glBufferData = data;
    GLES.glVertexAttribPointer = attrib;
    GLES.glEnableVertexAttribArray = enableAttrib;
    GLES.glTexImage2D = texImage;
    GLES.glTexParameteri = texParam;
    GLES.glRenderbufferStorage = renderStorage;
    GLES.glFramebufferTexture2D = frameTexture;
    GLES.glFramebufferRenderbuffer = frameRender;
    GLES.glCheckFramebufferStatus = frameStatus;
    GLES.glGetError = getError;
    GLES.glClear = clear;
    GLES.glDrawArrays = drawArrays;
    GLES.glBlitFramebuffer = blit;
    global_settings.fsr1_setting = FSR1_Quality_Preset::Quality;
    gl_state->current_program = 7;
    textures[0] = 11;
    samplers[0] = 12;
    enabled[GL_SCISSOR_TEST] = true;
    enabled[GL_BLEND] = true;
    enabled[GL_RASTERIZER_DISCARD] = true;
    mg_fsr1_bind_context(101);
    validSurface = false;
    InitFSRResources();
    assert(!fsrInitialized && allocations == 0);
    validSurface = true;
    InitFSRResources();
    std::fprintf(stderr, "FSR init: ready=%d allocations=%d width=%d unpack=%d\n", fsrInitialized, allocations,
                 FSR1_Context::g_renderWidth, unpack);
    assert(fsrInitialized && allocations == 3 && FSR1_Context::g_renderWidth == 640 && unpack == 27);
    GLuint old = FSR1_Context::g_renderFBO;
    ApplyFSR();
    assert(draws == 1 && program == 7 && vao == 9 && array == 11 && active == GL_TEXTURE3 && textures[0] == 11 &&
           samplers[0] == 12 && colorMask[0] == 0 && clearColor[0] == .25f && viewport[0] == 2 &&
           enabled[GL_RASTERIZER_DISCARD]);
    incomplete = true;
    surfaceWidth = 800;
    CheckResolutionChange(eglGetCurrentDisplay(), eglGetCurrentSurface(EGL_DRAW));
    assert(FSR1_Context::g_renderFBO == old && FSR1_Context::g_renderWidth == 640);
    incomplete = false;
    CheckResolutionChange(eglGetCurrentDisplay(), eglGetCurrentSurface(EGL_DRAW));
    assert(FSR1_Context::g_renderFBO != old && FSR1_Context::g_renderWidth == 800);
    old = FSR1_Context::g_renderFBO;
    std::thread other([] {
        mg_fsr1_bind_context(202);
        FSR1_Context::g_renderFBO = 999;
    });
    other.join();
    mg_fsr1_bind_context(101);
    assert(FSR1_Context::g_renderFBO == old);
    DrawDepthClearTri();
    assert(program == 7 && vao == 9 && array == 11);
    pending = 0;
    glTextureBarrier();
    assert(pending == GL_INVALID_OPERATION);
    pending = 0;
    mg_texture_barrier_backend = textureBarrier;
    glTextureBarrier();
    assert(barriers == 1 && pending == 0);
    puts("FSR initialization/resize/state/context and ANGLE clear contracts passed");
}
