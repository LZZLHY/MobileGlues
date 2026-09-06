// MobileGlues - gl/FSR1/FSR1.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header
#pragma once

#include <cstdlib>
#include <cstring>
#include <vector>

#ifndef __APPLE__
#include <malloc.h>
#endif

#ifdef __ANDROID__
#include <android/log.h>
#endif

#include "../../gles/gles.h"
#include "../../gles/loader.h"
#include "../../includes.h"
#include "../framebuffer.h"
#include "../glsl/glsl_for_es.h"
#include "../log.h"
#include "../mg.h"
#include <GL/gl.h>

namespace FSR1_Context {
    extern thread_local GLuint g_renderFBO;
    extern thread_local GLuint g_renderTexture;
    extern thread_local GLuint g_depthStencilRBO;
    extern thread_local GLuint g_quadVAO;
    extern thread_local GLuint g_quadVBO;
    extern thread_local GLuint g_fsrProgram;
    // Uniform locations of g_fsrProgram, resolved when it is linked and valid for
    // as long as it lives. -1 for a name the linker dropped, which glUniform*
    // ignores.
    extern thread_local GLint g_inputTexLoc;
    extern thread_local GLint g_const0Loc;
    extern thread_local GLint g_viewportSizeLoc;

    extern thread_local GLuint g_targetFBO;
    extern thread_local GLuint g_targetTexture;

    extern thread_local GLuint g_currentDrawFBO;
    extern thread_local GLint g_viewport[4];
    extern thread_local GLsizei g_targetWidth;
    extern thread_local GLsizei g_targetHeight;
    extern thread_local GLsizei g_renderWidth;
    extern thread_local GLsizei g_renderHeight;
    extern thread_local bool g_dirty;

    extern thread_local bool g_resolutionChanged;
    extern thread_local GLsizei g_pendingWidth;
    extern thread_local GLsizei g_pendingHeight;
} // namespace FSR1_Context

extern thread_local bool fsrInitialized;

// Swap the FSR1 objects when the current context changes.
//
// Every name above is a GL object owned by the context that created it, and
// gl/framebuffer.cpp redirects framebuffer 0 to g_renderFBO -- in a second
// context that name refers to nothing, or to somebody else's object. Each thread
// has its own working values; switching contexts saves and loads the complete
// resource/dimension state under the context table's mutex.
void mg_fsr1_bind_context(unsigned long long ctx_id);
void ApplyFSR();
void InitFSRResources();
void CheckResolutionChange(EGLDisplay display, EGLSurface surface);
void OnResize(int width, int height);

extern "C"
{
    GLAPI void glViewport(GLint x, GLint y, GLsizei w, GLsizei h);
}
