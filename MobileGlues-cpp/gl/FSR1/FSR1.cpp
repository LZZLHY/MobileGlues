// MobileGlues - gl/FSR1/FSR1.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header
#include "FSR1.h"
#include <mutex>
#include <ska/flat_hash_map.hpp>
#include "FSRShaderSource.h"
#include "../../config/settings.h"

#define DEBUG 0

// Which pieces of GL state a body in this file overwrites. Saving the rest is not
// free: everything the guard cannot answer from this layer's own tracking is a
// driver round trip, and the upscale runs once per presented frame.
enum GLStateBits : unsigned int {
    GUARD_PROGRAM = 1u << 0,
    GUARD_VAO = 1u << 1,
    GUARD_ARRAY_BUFFER = 1u << 2,
    // Unit 0's GL_TEXTURE_2D binding and the active unit together, because the
    // guard makes unit 0 current for its whole lifetime.
    GUARD_TEXTURE = 1u << 3,
    GUARD_FRAMEBUFFER = 1u << 4,
    GUARD_RENDERBUFFER = 1u << 5,
    GUARD_RASTER = 1u << 6,
    GUARD_UNPACK_BUFFER = 1u << 7,
};

// Query actual driver bindings: internal passes bypass frontend shadows.
// Each guard saves only the groups its body borrows and restores them on every exit.
struct GLStateGuard {
    unsigned int saved;
    GLint prevProgram = 0;
    GLint prevVAO = 0;
    GLint prevArrayBuffer = 0;
    GLint prevActiveTexture = GL_TEXTURE0;
    GLint prevTexture = 0;
    GLint prevReadFBO = 0;
    GLint prevDrawFBO = 0;
    GLint prevRenderbuffer = 0;
    GLint prevSampler = 0, prevUnpackBuffer = 0, prevViewport[4]{};
    GLboolean prevColorMask[4]{};
    GLfloat prevClearColor[4]{};
    const GLenum rasterCaps[6] = {GL_BLEND,      GL_SCISSOR_TEST, GL_CULL_FACE,
                                  GL_DEPTH_TEST, GL_STENCIL_TEST, GL_RASTERIZER_DISCARD};
    GLboolean rasterEnabled[6]{};

    explicit GLStateGuard(unsigned int bits) : saved(bits) {
        if (saved & GUARD_PROGRAM) GLES.glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
        if (saved & GUARD_VAO) GLES.glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVAO);
        if (saved & GUARD_ARRAY_BUFFER) GLES.glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArrayBuffer);
        if (saved & GUARD_TEXTURE) {
            GLES.glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActiveTexture);
            GLES.glActiveTexture(GL_TEXTURE0);
            GLES.glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTexture);
            GLES.glGetIntegerv(GL_SAMPLER_BINDING, &prevSampler);
            GLES.glBindSampler(0, 0);
        }
        if (saved & GUARD_FRAMEBUFFER) {
            GLES.glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFBO);
            GLES.glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDrawFBO);
        }
        if (saved & GUARD_RENDERBUFFER) GLES.glGetIntegerv(GL_RENDERBUFFER_BINDING, &prevRenderbuffer);
        if (saved & GUARD_UNPACK_BUFFER) {
            GLES.glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &prevUnpackBuffer);
            GLES.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        }
        if (saved & GUARD_RASTER) {
            GLES.glGetIntegerv(GL_VIEWPORT, prevViewport);
            GLES.glGetFloatv(GL_COLOR_CLEAR_VALUE, prevClearColor);
            GLES.glGetBooleanv(GL_COLOR_WRITEMASK, prevColorMask);
            for (int i = 0; i < 6; ++i) {
                rasterEnabled[i] = GLES.glIsEnabled(rasterCaps[i]);
                GLES.glDisable(rasterCaps[i]);
            }
            GLES.glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        }
    }

    // Follow a framebuffer this guard saved through a delete-and-recreate.
    //
    // A saved name that the body then deletes cannot be restored: GL rejects it and
    // leaves the binding wherever the body happened to put it. RecreateFSRFBO is the
    // only body here that deletes framebuffers, and the render FBO is the name
    // gl/framebuffer.cpp redirects a bind of framebuffer 0 to -- which is the case
    // this whole path exists for -- so the guard is told where the replacement went
    // instead of being left to restore a dead name.
    void framebuffer_recreated(GLuint from, GLuint to) {
        if (!(saved & GUARD_FRAMEBUFFER) || from == 0 || from == to) return;
        if (prevReadFBO == static_cast<GLint>(from)) prevReadFBO = static_cast<GLint>(to);
        if (prevDrawFBO == static_cast<GLint>(from)) prevDrawFBO = static_cast<GLint>(to);
    }

    ~GLStateGuard() {
        if (saved & GUARD_RASTER) {
            for (int i = 0; i < 6; ++i)
                if (rasterEnabled[i])
                    GLES.glEnable(rasterCaps[i]);
                else
                    GLES.glDisable(rasterCaps[i]);
            GLES.glColorMask(prevColorMask[0], prevColorMask[1], prevColorMask[2], prevColorMask[3]);
            GLES.glClearColor(prevClearColor[0], prevClearColor[1], prevClearColor[2], prevClearColor[3]);
            GLES.glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
        }
        if (saved & GUARD_UNPACK_BUFFER) GLES.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, prevUnpackBuffer);
        if (saved & GUARD_PROGRAM) GLES.glUseProgram(prevProgram);
        if (saved & GUARD_VAO) GLES.glBindVertexArray(prevVAO);
        if (saved & GUARD_ARRAY_BUFFER) GLES.glBindBuffer(GL_ARRAY_BUFFER, prevArrayBuffer);
        if (saved & GUARD_TEXTURE) {
            // Unit 0 is current for the guard's lifetime, but say so anyway: a body
            // is free to move the active unit as long as this line puts it back.
            GLES.glActiveTexture(GL_TEXTURE0);
            GLES.glBindTexture(GL_TEXTURE_2D, prevTexture);
            GLES.glBindSampler(0, prevSampler);
            GLES.glActiveTexture(prevActiveTexture);
        }
        if (saved & GUARD_RENDERBUFFER) GLES.glBindRenderbuffer(GL_RENDERBUFFER, prevRenderbuffer);
        if (saved & GUARD_FRAMEBUFFER) {
            GLES.glBindFramebuffer(GL_READ_FRAMEBUFFER, prevReadFBO);
            GLES.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prevDrawFBO);
        }
    }
};

namespace FSR1_Context {
    thread_local GLuint g_renderFBO = 0;
    thread_local GLuint g_renderTexture = 0;
    thread_local GLuint g_depthStencilRBO = 0;
    thread_local GLuint g_quadVAO = 0;
    thread_local GLuint g_quadVBO = 0;
    thread_local GLuint g_fsrProgram = 0;

    // Resolved once, when g_fsrProgram is linked. A uniform location is fixed for
    // the life of a program object and this one is never relinked, so asking for it
    // again is a driver-side name lookup per presented frame for an answer that
    // cannot have changed. -1 is what glGetUniformLocation returns for a name the
    // linker dropped, and glUniform* ignores it, so an unresolved location needs no
    // separate "not found" state.
    thread_local GLint g_inputTexLoc = -1;
    thread_local GLint g_const0Loc = -1;
    thread_local GLint g_viewportSizeLoc = -1;

    thread_local GLuint g_targetFBO = 0;
    thread_local GLuint g_targetTexture = 0;

    thread_local GLuint g_currentDrawFBO = 0;
    thread_local GLint g_viewport[4] = {0};
    thread_local GLsizei g_targetWidth = 2400;
    thread_local GLsizei g_targetHeight = 1080;
    thread_local GLsizei g_renderWidth = 1200;
    thread_local GLsizei g_renderHeight = 540;
    thread_local bool g_dirty = false;

    thread_local bool g_resolutionChanged = false;
    thread_local GLsizei g_pendingWidth = 0;
    thread_local GLsizei g_pendingHeight = 0;
} // namespace FSR1_Context

void CalculateTargetResolution(FSR1_Quality_Preset preset, int renderWidth, int renderHeight, int* targetWidth,
                               int* targetHeight) {
    float scale;
    switch (preset) {
    case FSR1_Quality_Preset::UltraQuality:
        scale = 1.3f;
        break;
    case FSR1_Quality_Preset::Quality:
        scale = 1.5f;
        break;
    case FSR1_Quality_Preset::Balanced:
        scale = 1.7f;
        break;
    case FSR1_Quality_Preset::Performance:
        scale = 2.0f;
        break;
    default:
        scale = 1.5f;
        break;
    }

    *targetWidth = static_cast<int>(renderWidth * scale);
    *targetHeight = static_cast<int>(renderHeight * scale);

    *targetWidth = (*targetWidth + 1) & ~1;
    *targetHeight = (*targetHeight + 1) & ~1;
    LOG_D("Render resolution: %dx%d", renderWidth, renderHeight);
    LOG_D("Target resolution: %dx%d", *targetWidth, *targetHeight);
}

void CalculateRenderResolution(FSR1_Quality_Preset preset, int targetWidth, int targetHeight, int* renderWidth,
                               int* renderHeight) {
    float scale;
    switch (preset) {
    case FSR1_Quality_Preset::UltraQuality:
        scale = 1.3f;
        break;
    case FSR1_Quality_Preset::Quality:
        scale = 1.5f;
        break;
    case FSR1_Quality_Preset::Balanced:
        scale = 1.7f;
        break;
    case FSR1_Quality_Preset::Performance:
        scale = 2.0f;
        break;
    default:
        scale = 1.5f;
    }

    *renderWidth = (int)(targetWidth / scale);
    *renderHeight = (int)(targetHeight / scale);

    *renderWidth = (*renderWidth + 1) & ~1;
    *renderHeight = (*renderHeight + 1) & ~1;
}

GLuint CompileFSRShader() {
    const GLuint program = GLES.glCreateProgram();
    if (!program) return 0;
    GLuint shaders[2]{};
    const GLenum types[] = {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER};
    const char* sources[] = {FSR_VSSource, FSR_FSSource};
    bool success = true;
    for (int i = 0; i < 2; ++i) {
        // FSR's bundled source is desktop GLSL 4.50. Use the translator without
        // entering glCreateShader (which itself initializes FSR resources).
        int translated = -1;
        const std::string essl = GLSLtoGLSLES(sources[i], types[i], hardware->es_version,
                                              getGLSLVersion(sources[i]), translated);
        if (translated < 0 || essl.empty()) {
            LOG_W_FORCE("[MG-FSR] shader translation failed: %s", mg_translation_error().c_str())
            success = false;
            break;
        }
        shaders[i] = GLES.glCreateShader(types[i]);
        if (!shaders[i]) {
            success = false;
            break;
        }
        const GLchar* source = essl.data();
        const GLint sourceLength = static_cast<GLint>(essl.size());
        GLES.glShaderSource(shaders[i], 1, &source, &sourceLength);
        GLES.glCompileShader(shaders[i]);
        GLint status = 0;
        GLES.glGetShaderiv(shaders[i], GL_COMPILE_STATUS, &status);
        if (!status) {
            char log[2048]{};
            GLES.glGetShaderInfoLog(shaders[i], sizeof(log), nullptr, log);
            LOG_W_FORCE("[MG-FSR] shader compilation failed: %s", log)
            success = false;
            break;
        }
        GLES.glAttachShader(program, shaders[i]);
    }
    if (success) {
        GLES.glLinkProgram(program);
        GLint status = 0;
        GLES.glGetProgramiv(program, GL_LINK_STATUS, &status);
        success = status == GL_TRUE;
        if (!success) {
            char log[2048]{};
            GLES.glGetProgramInfoLog(program, sizeof(log), nullptr, log);
            LOG_W_FORCE("[MG-FSR] program link failed: %s", log)
        }
    }
    for (GLuint shader : shaders)
        if (shader) GLES.glDeleteShader(shader);
    if (!success) {
        GLES.glDeleteProgram(program);
        return 0;
    }
    return program;
}
void InitFullscreenQuad() {
    GLStateGuard state(GUARD_VAO | GUARD_ARRAY_BUFFER);
    const float quadVertices[] = {-1.0f, 1.0f, 0.0f, 1.0f, -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, -1.0f, 1.0f, 0.0f,

                                  -1.0f, 1.0f, 0.0f, 1.0f, 1.0f,  -1.0f, 1.0f, 0.0f, 1.0f, 1.0f,  1.0f, 1.0f};

    GLES.glGenVertexArrays(1, &FSR1_Context::g_quadVAO);
    GLES.glGenBuffers(1, &FSR1_Context::g_quadVBO);

    GLES.glBindVertexArray(FSR1_Context::g_quadVAO);
    GLES.glBindBuffer(GL_ARRAY_BUFFER, FSR1_Context::g_quadVBO);

    GLES.glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);

    GLES.glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    GLES.glEnableVertexAttribArray(0);

    GLES.glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    GLES.glEnableVertexAttribArray(1);

    GLES.glBindBuffer(GL_ARRAY_BUFFER, 0);
    GLES.glBindVertexArray(0);
}

thread_local bool fsrInitialized = false;
namespace {
    thread_local bool fsrInitializing = false;
    struct FSRTargets {
        GLuint renderFBO = 0, renderTexture = 0, depth = 0, targetFBO = 0, targetTexture = 0;
    };
    void delete_targets(const FSRTargets& targets) {
        if (targets.renderFBO) GLES.glDeleteFramebuffers(1, &targets.renderFBO);
        if (targets.targetFBO) GLES.glDeleteFramebuffers(1, &targets.targetFBO);
        if (targets.renderTexture) GLES.glDeleteTextures(1, &targets.renderTexture);
        if (targets.targetTexture) GLES.glDeleteTextures(1, &targets.targetTexture);
        if (targets.depth) GLES.glDeleteRenderbuffers(1, &targets.depth);
    }
    bool replace_targets(GLsizei rw, GLsizei rh, GLsizei tw, GLsizei th) {
        if (rw <= 0 || rh <= 0 || tw <= 0 || th <= 0) return false;
        GLStateGuard state(GUARD_TEXTURE | GUARD_FRAMEBUFFER | GUARD_RENDERBUFFER | GUARD_UNPACK_BUFFER);
        FSRTargets next;
        mg_begin_driver_operation();
        auto make_texture = [](GLuint* texture, GLsizei width, GLsizei height) {
            GLES.glGenTextures(1, texture);
            GLES.glBindTexture(GL_TEXTURE_2D, *texture);
            GLES.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        };
        make_texture(&next.renderTexture, rw, rh);
        GLES.glGenRenderbuffers(1, &next.depth);
        GLES.glBindRenderbuffer(GL_RENDERBUFFER, next.depth);
        GLES.glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, rw, rh);
        GLES.glGenFramebuffers(1, &next.renderFBO);
        GLES.glBindFramebuffer(GL_FRAMEBUFFER, next.renderFBO);
        GLES.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, next.renderTexture, 0);
        GLES.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, next.depth);
        bool complete = GLES.glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        make_texture(&next.targetTexture, tw, th);
        GLES.glGenFramebuffers(1, &next.targetFBO);
        GLES.glBindFramebuffer(GL_FRAMEBUFFER, next.targetFBO);
        GLES.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, next.targetTexture, 0);
        complete &= GLES.glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        const bool accepted = mg_end_driver_operation("FSR/allocate-targets");
        if (!complete || !accepted || !next.renderFBO || !next.targetFBO || !next.renderTexture ||
            !next.targetTexture || !next.depth) {
            delete_targets(next);
            LOG_W_FORCE("[MG-FSR] target allocation failed; previous resources retained")
            return false;
        }
        const FSRTargets old{FSR1_Context::g_renderFBO, FSR1_Context::g_renderTexture, FSR1_Context::g_depthStencilRBO,
                             FSR1_Context::g_targetFBO, FSR1_Context::g_targetTexture};
        FSR1_Context::g_renderFBO = next.renderFBO;
        FSR1_Context::g_renderTexture = next.renderTexture;
        FSR1_Context::g_depthStencilRBO = next.depth;
        FSR1_Context::g_targetFBO = next.targetFBO;
        FSR1_Context::g_targetTexture = next.targetTexture;
        FSR1_Context::g_renderWidth = rw;
        FSR1_Context::g_renderHeight = rh;
        FSR1_Context::g_targetWidth = tw;
        FSR1_Context::g_targetHeight = th;
        state.framebuffer_recreated(old.renderFBO, next.renderFBO);
        state.framebuffer_recreated(old.targetFBO, next.targetFBO);
        if (gl_state->current_draw_fbo == old.renderFBO) {
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
            state.prevDrawFBO = next.renderFBO;
        }
        delete_targets(old);
        return true;
    }
    bool surface_size(EGLDisplay display, EGLSurface surface, GLsizei& width, GLsizei& height) {
        LOAD_EGL(eglQuerySurface);
        if (display == EGL_NO_DISPLAY || surface == EGL_NO_SURFACE || !egl_eglQuerySurface) return false;
        return egl_eglQuerySurface(display, surface, EGL_WIDTH, &width) == EGL_TRUE &&
               egl_eglQuerySurface(display, surface, EGL_HEIGHT, &height) == EGL_TRUE && width > 0 && height > 0;
    }
} // namespace

void InitFSRResources() {
    if (fsrInitialized || fsrInitializing) return;
    GLsizei width = 0, height = 0;
    if (!surface_size(eglGetCurrentDisplay(), eglGetCurrentSurface(EGL_DRAW), width, height)) return;
    fsrInitializing = true;
    struct InitializingGuard {
        ~InitializingGuard() { fsrInitializing = false; }
    } initializingGuard;
    GLint targetWidth = 0, targetHeight = 0;
    CalculateTargetResolution(global_settings.fsr1_setting, width, height, &targetWidth, &targetHeight);
    const GLuint program = CompileFSRShader();
    if (!program) return;
    {
        GLStateGuard state(GUARD_PROGRAM);
        FSR1_Context::g_fsrProgram = program;
        FSR1_Context::g_inputTexLoc = GLES.glGetUniformLocation(program, "uInputTex");
        FSR1_Context::g_const0Loc = GLES.glGetUniformLocation(program, "uConst0");
        FSR1_Context::g_viewportSizeLoc = GLES.glGetUniformLocation(program, "uViewportSize");
        GLES.glUseProgram(program);
        GLES.glUniform1i(FSR1_Context::g_inputTexLoc, 0);
    }
    mg_begin_driver_operation();
    InitFullscreenQuad();
    const bool quadOK = mg_end_driver_operation("FSR/fullscreen-quad");
    if (!quadOK || !FSR1_Context::g_quadVAO || !FSR1_Context::g_quadVBO ||
        !replace_targets(width, height, targetWidth, targetHeight)) {
        if (FSR1_Context::g_quadVAO) GLES.glDeleteVertexArrays(1, &FSR1_Context::g_quadVAO);
        if (FSR1_Context::g_quadVBO) GLES.glDeleteBuffers(1, &FSR1_Context::g_quadVBO);
        GLES.glDeleteProgram(program);
        FSR1_Context::g_fsrProgram = 0;
        FSR1_Context::g_quadVAO = 0;
        FSR1_Context::g_quadVBO = 0;
        return;
    }
    fsrInitialized = true;
    FSR1_Context::g_resolutionChanged = false;
}

void RecreateFSRFBO() {
    if (!fsrInitialized) {
        InitFSRResources();
        return;
    }
    const GLsizei width = FSR1_Context::g_pendingWidth, height = FSR1_Context::g_pendingHeight;
    GLint tw = 0, th = 0;
    CalculateTargetResolution(global_settings.fsr1_setting, width, height, &tw, &th);
    if (replace_targets(width, height, tw, th)) FSR1_Context::g_resolutionChanged = false;
}
thread_local std::vector<std::pair<GLsizei, GLsizei>> g_viewportStack;

void ApplyFSR() {
    // No GUARD_ARRAY_BUFFER or GUARD_RENDERBUFFER: nothing below binds either.
    // GL_ARRAY_BUFFER_BINDING is context state and not vertex array object state, so
    // the glBindVertexArray below cannot disturb it.
    if (!fsrInitialized || FSR1_Context::g_renderWidth <= 0 || FSR1_Context::g_renderHeight <= 0 ||
        FSR1_Context::g_targetWidth <= 0 || FSR1_Context::g_targetHeight <= 0)
        return;
    GLStateGuard state(GUARD_PROGRAM | GUARD_VAO | GUARD_TEXTURE | GUARD_FRAMEBUFFER | GUARD_RASTER);

    GLES.glBindFramebuffer(GL_FRAMEBUFFER, FSR1_Context::g_targetFBO);
    GLES.glViewport(0, 0, FSR1_Context::g_targetWidth, FSR1_Context::g_targetHeight);
    GLES.glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    GLES.glClear(GL_COLOR_BUFFER_BIT);

    GLES.glUseProgram(FSR1_Context::g_fsrProgram);

    // Unit 0 is already current -- the guard made it so, and it is the unit
    // uInputTex was pointed at when the program was linked.
    GLES.glBindTexture(GL_TEXTURE_2D, FSR1_Context::g_renderTexture);

    // Plain arrays rather than a vector type from a maths library: these two are
    // handed straight to glUniform*fv, and nothing is ever computed with them.
    const GLfloat const0[4] = {float(FSR1_Context::g_renderWidth) / FSR1_Context::g_targetWidth,
                               float(FSR1_Context::g_renderHeight) / FSR1_Context::g_targetHeight,
                               1.0f / FSR1_Context::g_targetWidth, 1.0f / FSR1_Context::g_targetHeight};

    GLES.glUniform4fv(FSR1_Context::g_const0Loc, 1, const0);

    const GLfloat viewportSize[2] = {(float)FSR1_Context::g_renderWidth, (float)FSR1_Context::g_renderHeight};
    GLES.glUniform2fv(FSR1_Context::g_viewportSizeLoc, 1, viewportSize);

    GLES.glBindVertexArray(FSR1_Context::g_quadVAO);
    GLES.glDrawArrays(GL_TRIANGLES, 0, 6);

    GLES.glBindFramebuffer(GL_READ_FRAMEBUFFER, FSR1_Context::g_targetFBO);
    GLES.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    GLES.glBlitFramebuffer(0, 0, FSR1_Context::g_targetWidth, FSR1_Context::g_targetHeight, 0, 0,
                           FSR1_Context::g_targetWidth, FSR1_Context::g_targetHeight, GL_COLOR_BUFFER_BIT, GL_LINEAR);

    // GLStateGuard restores the caller's viewport, bindings and raster state.
}

void CheckResolutionChange(EGLDisplay display, EGLSurface surface) {
    GLsizei width = 0, height = 0;
    if (!surface_size(display, surface, width, height)) return;
    if (!fsrInitialized) {
        InitFSRResources();
        return;
    }
    OnResize(width, height);
    if (FSR1_Context::g_resolutionChanged) RecreateFSRFBO();
}
void OnResize(int width, int height) {
    if (FSR1_Context::g_renderWidth == width && FSR1_Context::g_renderHeight == height) return;

    FSR1_Context::g_pendingWidth = width;
    FSR1_Context::g_pendingHeight = height;
    FSR1_Context::g_resolutionChanged = true;
}

void glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    LOG()
    LOG_D("glViewport: x=%d, y=%d, w=%d, h=%d", x, y, w, h);

    if (w > FSR1_Context::g_pendingWidth || h > FSR1_Context::g_pendingHeight) {
        FSR1_Context::g_pendingWidth = w;
        FSR1_Context::g_pendingHeight = h;
        FSR1_Context::g_resolutionChanged = true;
    }

    GLES.glViewport(x, y, w, h);
}

// ---------------------------------------------------------------------------

namespace {

    struct fsr1_ctx_state_t {
        GLuint renderFBO = 0, renderTexture = 0, depthStencilRBO = 0;
        GLuint quadVAO = 0, quadVBO = 0, fsrProgram = 0;
        // Locations belong to fsrProgram, so they travel with it rather than being
        // re-resolved after a context switch.
        GLint inputTexLoc = -1, const0Loc = -1, viewportSizeLoc = -1;
        GLuint targetFBO = 0, targetTexture = 0, currentDrawFBO = 0;
        GLsizei targetWidth = 0, targetHeight = 0, renderWidth = 0, renderHeight = 0;
        bool initialised = false;
        bool resolutionChanged = false, dirty = false;
        GLsizei pendingWidth = 0, pendingHeight = 0;
    };

    std::mutex g_fsr_mutex;
    // Plain value, not a unique_ptr like the other per-context tables: nothing here
    // keeps the address of an entry. Both operator[] calls in mg_fsr1_bind_context
    // are separate statements, so the first reference is dead before the second one
    // can rehash the map.
    ska::flat_hash_map<unsigned long long, fsr1_ctx_state_t> g_fsr_states;
    thread_local fsr1_ctx_state_t g_fsr_default;
    thread_local unsigned long long g_fsr_current_id = 0;

    void store_into(fsr1_ctx_state_t& d) {
        d.renderFBO = FSR1_Context::g_renderFBO;
        d.renderTexture = FSR1_Context::g_renderTexture;
        d.depthStencilRBO = FSR1_Context::g_depthStencilRBO;
        d.quadVAO = FSR1_Context::g_quadVAO;
        d.quadVBO = FSR1_Context::g_quadVBO;
        d.fsrProgram = FSR1_Context::g_fsrProgram;
        d.inputTexLoc = FSR1_Context::g_inputTexLoc;
        d.const0Loc = FSR1_Context::g_const0Loc;
        d.viewportSizeLoc = FSR1_Context::g_viewportSizeLoc;
        d.targetFBO = FSR1_Context::g_targetFBO;
        d.targetTexture = FSR1_Context::g_targetTexture;
        d.currentDrawFBO = FSR1_Context::g_currentDrawFBO;
        d.targetWidth = FSR1_Context::g_targetWidth;
        d.targetHeight = FSR1_Context::g_targetHeight;
        d.renderWidth = FSR1_Context::g_renderWidth;
        d.renderHeight = FSR1_Context::g_renderHeight;
        d.initialised = fsrInitialized;
        d.resolutionChanged = FSR1_Context::g_resolutionChanged;
        d.dirty = FSR1_Context::g_dirty;
        d.pendingWidth = FSR1_Context::g_pendingWidth;
        d.pendingHeight = FSR1_Context::g_pendingHeight;
    }

    void load_from(const fsr1_ctx_state_t& s) {
        FSR1_Context::g_renderFBO = s.renderFBO;
        FSR1_Context::g_renderTexture = s.renderTexture;
        FSR1_Context::g_depthStencilRBO = s.depthStencilRBO;
        FSR1_Context::g_quadVAO = s.quadVAO;
        FSR1_Context::g_quadVBO = s.quadVBO;
        FSR1_Context::g_fsrProgram = s.fsrProgram;
        FSR1_Context::g_inputTexLoc = s.inputTexLoc;
        FSR1_Context::g_const0Loc = s.const0Loc;
        FSR1_Context::g_viewportSizeLoc = s.viewportSizeLoc;
        FSR1_Context::g_targetFBO = s.targetFBO;
        FSR1_Context::g_targetTexture = s.targetTexture;
        FSR1_Context::g_currentDrawFBO = s.currentDrawFBO;
        FSR1_Context::g_targetWidth = s.targetWidth;
        FSR1_Context::g_targetHeight = s.targetHeight;
        FSR1_Context::g_renderWidth = s.renderWidth;
        FSR1_Context::g_renderHeight = s.renderHeight;
        fsrInitialized = s.initialised;
        FSR1_Context::g_resolutionChanged = s.resolutionChanged;
        FSR1_Context::g_dirty = s.dirty;
        FSR1_Context::g_pendingWidth = s.pendingWidth;
        FSR1_Context::g_pendingHeight = s.pendingHeight;
    }

} // namespace

void mg_fsr1_bind_context(unsigned long long ctx_id) {
    if (ctx_id == g_fsr_current_id) return;
    std::lock_guard<std::mutex> lock(g_fsr_mutex);
    store_into(g_fsr_current_id == 0 ? g_fsr_default : g_fsr_states[g_fsr_current_id]);
    load_from(ctx_id == 0 ? g_fsr_default : g_fsr_states[ctx_id]);
    g_fsr_current_id = ctx_id;
}

void mg_fsr1_forget_context(unsigned long long ctx_id) {
    if (ctx_id == 0) return;
    std::lock_guard<std::mutex> lock(g_fsr_mutex);
    // If this is still the loaded set, the live globals describe a context that is
    // gone. Drop back to the default set rather than storing them into the entry
    // about to be erased.
    if (g_fsr_current_id == ctx_id) {
        load_from(g_fsr_default);
        g_fsr_current_id = 0;
    }
    g_fsr_states.erase(ctx_id);
}
