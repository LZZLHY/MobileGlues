// MobileGlues - gl/multidraw.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#include "multidraw.h"
#include "../config/settings.h"
#include <cstdint>
#include <limits>
#include <vector>
#if defined(MG_PLATFORM_OHOS)
// For the deferred-upload drain below. Declared rather than including gl/buffer.h, which would pull
// in the whole buffer-shadow surface for one entry point. Defined in gl/buffer.cpp.
extern "C" void mg_deferred_upload_flush_for_draw(void);
extern "C" GLboolean mg_deferred_upload_pending(void);
// REMOVED: the diagnostics include that draw timing needed. Nothing in this file is instrumented any
// more - multi-draw was measured and cleared. See the notes at the two multiindirect backends.
#endif

#define DEBUG 0

typedef void (*glMultiDrawElements_t)(GLenum, const GLsizei*, GLenum, const void* const*, GLsizei);

void glMultiDrawElements(GLenum mode, const GLsizei* count, GLenum type, const void* const* indices,
                         GLsizei primcount) {
    static glMultiDrawElements_t func_ptr = nullptr;

    if (func_ptr == nullptr) {
        switch (global_settings.multidraw_mode) {
        case multidraw_mode_t::PreferIndirect:
            func_ptr = mg_glMultiDrawElements_indirect;
            break;
        case multidraw_mode_t::PreferBaseVertex:
            func_ptr = mg_glMultiDrawElements_basevertex;
            break;
        case multidraw_mode_t::PreferMultidrawIndirect:
            func_ptr = mg_glMultiDrawElements_multiindirect;
            break;
        case multidraw_mode_t::DrawElements:
            func_ptr = mg_glMultiDrawElements_drawelements;
            break;
        case multidraw_mode_t::Compute:
            func_ptr = mg_glMultiDrawElements_compute;
            break;
        default:
            func_ptr = mg_glMultiDrawElements_drawelements;
            break;
        }
    }
#if defined(MG_PLATFORM_OHOS)
    // Land any deferred terrain upload before drawing from it. See gl/buffer.cpp
    // mg_deferred_upload_flush_for_draw for why the multi-draw entry points are the right and only
    // place for this.
    if (mg_deferred_upload_pending()) mg_deferred_upload_flush_for_draw();
#endif
    func_ptr(mode, count, type, indices, primcount);
}

typedef void (*glMultiDrawElementsBaseVertex_t)(GLenum, GLsizei*, GLenum, const void* const*, GLsizei, const GLint*);

void glMultiDrawElementsBaseVertex(GLenum mode, GLsizei* counts, GLenum type, const void* const* indices,
                                   GLsizei primcount, const GLint* basevertex) {
    static glMultiDrawElementsBaseVertex_t func_ptr = nullptr;

    if (func_ptr == nullptr) {
        switch (global_settings.multidraw_mode) {
        case multidraw_mode_t::PreferIndirect:
            func_ptr = mg_glMultiDrawElementsBaseVertex_indirect;
            break;
        case multidraw_mode_t::PreferBaseVertex:
            func_ptr = mg_glMultiDrawElementsBaseVertex_basevertex;
            break;
        case multidraw_mode_t::PreferMultidrawIndirect:
            func_ptr = mg_glMultiDrawElementsBaseVertex_multiindirect;
            break;
        case multidraw_mode_t::DrawElements:
            func_ptr = mg_glMultiDrawElementsBaseVertex_drawelements;
            break;
        case multidraw_mode_t::Compute:
            func_ptr = mg_glMultiDrawElementsBaseVertex_compute;
            break;
        default:
            func_ptr = mg_glMultiDrawElementsBaseVertex_drawelements;
            break;
        }
    }

#if defined(MG_PLATFORM_OHOS)
    // This is the one that matters: Sodium's terrain draws arrive here, and Sodium uploads *before*
    // it draws within a frame (Minecraft.renderFrame offset 440 uploads, offset 526 draws), unlike
    // Minecraft's own renderer which draws at LevelRenderer offset 603 and uploads at 682. Without
    // this drain the deferred bytes would not land until the present and Sodium's draws would read
    // the previous tenant of a reused arena segment.
    if (mg_deferred_upload_pending()) mg_deferred_upload_flush_for_draw();
#endif

    // NOTE: this dispatcher is NOT how the application gets here, and that is a finding in its own
    // right. glx/lookup.cpp handle_multidraw_func_name rewrites "glMultiDrawElementsBaseVertex" into
    // "mg_glMultiDrawElementsBaseVertex_<mode>" at getProcAddress time, so LWJGL resolves a pointer
    // straight to the backend and this function is never called. The device round that timed draws
    // proved it: the dispatcher's own call counter read 0 while the per-part counters inside the
    // multiindirect backend were non-zero.
    //
    // Which means the deferred-upload drain above has never run either. That drain is the safety net
    // the whole deferred design rests on, and deferred_forced_flush reading 0 was taken as "the range
    // test almost never fires" when the truth is that the hook was unreachable. Exposure is currently
    // small - deferred_enqueued measures about 2 per window - but it is a real hole and it is recorded
    // in docs/ohos/RENDER-ADAPTATION.md rather than fixed in the same build as a measurement, so that
    // one does not confound the other.
    func_ptr(mode, counts, type, indices, primcount, basevertex);
}

// The indirect command buffer, rotated over several GL names rather than one.
//
// Why this is a ring and not a single buffer, established on device 2026-08-08:
//
// Sodium issues one multi-draw per render region per pass, so a frame contains many, and each one
// rewrites this staging area and then immediately issues a draw that reads it. With a single buffer,
// multi-draw N+1's map lands on memory that multi-draw N's glMultiDrawElementsIndirectEXT may still
// be reading. The map below asks for GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT and *not*
// GL_MAP_UNSYNCHRONIZED_BIT, so a conforming driver has two ways to answer: rename the storage, or
// wait for the read to finish. Waiting is a stall on every multi-draw after the first; renaming is
// free. If a driver does neither - and this one appears not to - the commands read are stale or
// half-written, which means a wrong baseVertex/firstIndex, which means geometry drawn from the wrong
// offset in Sodium's arena.
//
// That last sentence is 3.2's artifact word for word - "regions render as void while the geometry
// that belonged there appears elsewhere" - and switching to multidrawMode=2, which removes this
// buffer from the picture entirely, made the artifact disappear on device. It also moved windows at
// 60 fps or better from 11.2% to 51.0%, so the hazard was costing frame rate as well as correctness.
//
// The ring gives each multi-draw its own storage for the next MG_INDIRECT_RING_SIZE calls, so the
// GPU has that many multi-draws' worth of slack before a write can catch up with a read. Buffers are
// tiny - primcount is at most about 1800, so 36 KB each - which is why the ring is cheap enough to
// be the fix rather than a compromise. Sizes are tracked per slot because each grows independently.
//
// This does not remove the glGetIntegerv below, which is a separate per-call cost on a tiled GPU and
// a separate change; one variable at a time.
static constexpr int MG_INDIRECT_RING_SIZE = 8;
static bool g_indirect_cmds_inited = false;
static GLuint g_indirectbuffers[MG_INDIRECT_RING_SIZE] = {0};
static GLsizei g_cmdbufsizes[MG_INDIRECT_RING_SIZE] = {0};
static int g_indirect_slot = 0;
static GLuint prevIndirectBuffer = 0;

void prepare_indirect_buffer(const GLsizei* counts, GLenum type, const void* const* indices, GLsizei primcount,
                             const GLint* basevertex) {
    GLES.glGetIntegerv(GL_DRAW_INDIRECT_BUFFER_BINDING, (GLint*)&prevIndirectBuffer);
    if (!g_indirect_cmds_inited) {
        GLES.glGenBuffers(MG_INDIRECT_RING_SIZE, g_indirectbuffers);
        for (int i = 0; i < MG_INDIRECT_RING_SIZE; ++i) {
            GLES.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, g_indirectbuffers[i]);
            g_cmdbufsizes[i] = 1;
            GLES.glBufferData(GL_DRAW_INDIRECT_BUFFER, g_cmdbufsizes[i] * sizeof(draw_elements_indirect_command_t),
                              NULL, GL_DYNAMIC_DRAW);
        }
        g_indirect_cmds_inited = true;
    }

    // Advance first, so the slot used by the previous multi-draw is left alone.
    g_indirect_slot = (g_indirect_slot + 1) % MG_INDIRECT_RING_SIZE;
    const int slot = g_indirect_slot;
    GLES.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, g_indirectbuffers[slot]);

    if (g_cmdbufsizes[slot] < primcount) {
        size_t sz = g_cmdbufsizes[slot];

        LOG_D("Before resize: %d", sz)

        // 2-exponential to reduce reallocation
        while (sz < primcount)
            sz *= 2;

        GLES.glBufferData(GL_DRAW_INDIRECT_BUFFER, sz * sizeof(draw_elements_indirect_command_t), NULL,
                          GL_DYNAMIC_DRAW);
        g_cmdbufsizes[slot] = sz;
    }

    LOG_D("After resize: %d (slot %d)", g_cmdbufsizes[slot], slot)

    auto* pcmds = (draw_elements_indirect_command_t*)GLES.glMapBufferRange(
        GL_DRAW_INDIRECT_BUFFER, 0, primcount * sizeof(draw_elements_indirect_command_t),
        GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);

    GLsizei elementSize;
    switch (type) {
    case GL_UNSIGNED_BYTE:
        elementSize = 1;
        break;
    case GL_UNSIGNED_SHORT:
        elementSize = 2;
        break;
    case GL_UNSIGNED_INT:
        elementSize = 4;
        break;
    default:
        elementSize = 4;
    }

    for (GLsizei i = 0; i < primcount; ++i) {
        auto byteOffset = reinterpret_cast<uintptr_t>(indices[i]);
        pcmds[i].firstIndex = static_cast<GLuint>(byteOffset / elementSize);
        pcmds[i].count = counts[i];
        pcmds[i].instanceCount = 1;
        pcmds[i].baseVertex = basevertex ? basevertex[i] : 0;
        pcmds[i].reservedMustBeZero = 0;
    }

    GLES.glUnmapBuffer(GL_DRAW_INDIRECT_BUFFER);
}

void mg_glMultiDrawElementsBaseVertex_drawelements(GLenum mode, GLsizei* counts, GLenum type,
                                                   const void* const* indices, GLsizei primcount,
                                                   const GLint* basevertex) {
    LOG()
    void prepareForDraw();
    prepareForDraw();
    GLint prevElementBuffer;
    GLES.glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &prevElementBuffer);

    for (GLsizei i = 0; i < primcount; ++i) {
        if (counts[i] <= 0) continue;

        GLsizei currentCount = counts[i];
        const GLvoid* currentIndices = indices[i];
        GLint currentBaseVertex = basevertex[i];

        size_t indexSize;
        switch (type) {
        case GL_UNSIGNED_INT:
            indexSize = sizeof(GLuint);
            break;
        case GL_UNSIGNED_SHORT:
            indexSize = sizeof(GLushort);
            break;
        case GL_UNSIGNED_BYTE:
            indexSize = sizeof(GLubyte);
            break;
        default:
            return;
        }

        GLuint tempBuffer;
        GLES.glGenBuffers(1, &tempBuffer);
        GLES.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, tempBuffer);

        void* srcData = nullptr;
        void* tempIndices = malloc(currentCount * indexSize);
        if (!tempIndices) {
            GLES.glDeleteBuffers(1, &tempBuffer);
            continue;
        }

        if (prevElementBuffer != 0) {
            GLES.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, prevElementBuffer);
            srcData = GLES.glMapBufferRange(GL_ELEMENT_ARRAY_BUFFER, (GLintptr)currentIndices, currentCount * indexSize,
                                            GL_MAP_READ_BIT);

            if (!srcData) {
                free(tempIndices);
                GLES.glDeleteBuffers(1, &tempBuffer);
                continue;
            }
        } else {
            srcData = (void*)currentIndices;
        }

        switch (type) {
        case GL_UNSIGNED_INT:
            for (int j = 0; j < currentCount; ++j) {
                ((GLuint*)tempIndices)[j] = ((GLuint*)srcData)[j] + currentBaseVertex;
            }
            break;
        case GL_UNSIGNED_SHORT:
            for (int j = 0; j < currentCount; ++j) {
                ((GLushort*)tempIndices)[j] = ((GLushort*)srcData)[j] + currentBaseVertex;
            }
            break;
        case GL_UNSIGNED_BYTE:
            for (int j = 0; j < currentCount; ++j) {
                ((GLubyte*)tempIndices)[j] = ((GLubyte*)srcData)[j] + currentBaseVertex;
            }
            break;
        }

        if (prevElementBuffer != 0) {
            GLES.glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER);
        }

        GLES.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, tempBuffer);
        GLES.glBufferData(GL_ELEMENT_ARRAY_BUFFER, currentCount * indexSize, tempIndices, GL_STREAM_DRAW);
        free(tempIndices);
        GLES.glDrawElements(mode, currentCount, type, 0);

        GLES.glDeleteBuffers(1, &tempBuffer);
    }

    GLES.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, prevElementBuffer);

    CHECK_GL_ERROR
}

void mg_glMultiDrawElementsBaseVertex_indirect(GLenum mode, GLsizei* counts, GLenum type, const void* const* indices,
                                               GLsizei primcount, const GLint* basevertex) {
    LOG()
    void prepareForDraw();
    prepareForDraw();

    prepare_indirect_buffer(counts, type, indices, primcount, basevertex);

    // Draw indirect!
    for (GLsizei i = 0; i < primcount; ++i) {
        const GLvoid* offset = reinterpret_cast<GLvoid*>(i * sizeof(draw_elements_indirect_command_t));
        GLES.glDrawElementsIndirect(mode, type, offset);
    }

    GLES.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, prevIndirectBuffer);

    CHECK_GL_ERROR
}

void mg_glMultiDrawElementsBaseVertex_multiindirect(GLenum mode, GLsizei* counts, GLenum type,
                                                    const void* const* indices, GLsizei primcount,
                                                    const GLint* basevertex) {
    LOG()
    void prepareForDraw();

    // REMOVED: per-part draw timing (prepareForDraw / indirect-command-buffer build / driver call).
    // It answered its question and the answer was negative, so the cost of three clock reads per
    // multi-draw is no longer worth paying. Measured per *frame* rather than per second, so the
    // number does not simply track the frame count: windows under 30 fps spent 0.33 ms per frame in
    // multi-draw against 0.30 ms per frame in windows at 80 fps or better. Flat. Multi-draw is not
    // where the chunk-boundary stutter lives. See docs/ohos/RENDER-ADAPTATION.md.
    prepareForDraw();
    prepare_indirect_buffer(counts, type, indices, primcount, basevertex);
    // Multi-draw indirect!
    GLES.glMultiDrawElementsIndirectEXT(mode, type, 0, primcount, 0);

    GLES.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, prevIndirectBuffer);

    CHECK_GL_ERROR
}

void mg_glMultiDrawElementsBaseVertex_basevertex(GLenum mode, GLsizei* counts, GLenum type, const void* const* indices,
                                                 GLsizei primcount, const GLint* basevertex) {
    LOG()
    void prepareForDraw();
    prepareForDraw();

    for (GLsizei i = 0; i < primcount; ++i) {
        const GLsizei count = counts[i];
        if (count > 0) {
            LOG_D("GLES.glDrawElementsBaseVertex, mode = %s, count = %d, type = %s, indices[i] = 0x%x, basevertex[i] = "
                  "%d",
                  glEnumToString(mode), count, glEnumToString(type), indices[i], basevertex[i])
            GLES.glDrawElementsBaseVertex(mode, count, type, indices[i], basevertex[i]);
        }
    }
    CHECK_GL_ERROR
}

void mg_glMultiDrawElements_indirect(GLenum mode, const GLsizei* count, GLenum type, const void* const* indices,
                                     GLsizei primcount) {
    LOG()
    void prepareForDraw();
    prepareForDraw();

    prepare_indirect_buffer(count, type, indices, primcount, 0);
    // Draw indirect!
    for (GLsizei i = 0; i < primcount; ++i) {
        const GLvoid* offset = reinterpret_cast<GLvoid*>(i * sizeof(draw_elements_indirect_command_t));
        GLES.glDrawElementsIndirect(mode, type, offset);
    }

    GLES.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, prevIndirectBuffer);
    CHECK_GL_ERROR
}

void mg_glMultiDrawElements_drawelements(GLenum mode, const GLsizei* count, GLenum type, const void* const* indices,
                                         GLsizei primcount) {
    LOG()
    void prepareForDraw();
    prepareForDraw();

    for (GLsizei i = 0; i < primcount; ++i) {
        const GLsizei c = count[i];
        if (c > 0) {
            GLES.glDrawElements(mode, c, type, indices[i]);
        }
    }

    CHECK_GL_ERROR
}

void mg_glMultiDrawElements_compute(GLenum mode, const GLsizei* count, GLenum type, const void* const* indices,
                                    GLsizei primcount) {
    LOG()
    void prepareForDraw();
    prepareForDraw();

    for (GLsizei i = 0; i < primcount; ++i) {
        const GLsizei c = count[i];
        if (c > 0) {
            GLES.glDrawElements(mode, c, type, indices[i]);
        }
    }

    CHECK_GL_ERROR
}

void mg_glMultiDrawElements_multiindirect(GLenum mode, const GLsizei* count, GLenum type, const void* const* indices,
                                          GLsizei primcount) {
    LOG()
    void prepareForDraw();

    // REMOVED: per-part draw timing, same as the base-vertex variant above and for the same reason.
    prepareForDraw();
    prepare_indirect_buffer(count, type, indices, primcount, 0);
    // Multi-draw indirect!
    GLES.glMultiDrawElementsIndirectEXT(mode, type, 0, primcount, 0);

    GLES.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, prevIndirectBuffer);

    CHECK_GL_ERROR
}

void mg_glMultiDrawElements_basevertex(GLenum mode, const GLsizei* count, GLenum type, const void* const* indices,
                                       GLsizei primcount) {
    LOG()
    void prepareForDraw();
    prepareForDraw();

    for (GLsizei i = 0; i < primcount; ++i) {
        const GLsizei c = count[i];
        if (c > 0) {
            GLES.glDrawElements(mode, c, type, indices[i]);
        }
    }

    CHECK_GL_ERROR
}

static bool is_strip_like_mode(GLenum mode) {
    switch (mode) {
    case GL_LINE_STRIP:
    case GL_LINE_LOOP:
    case GL_TRIANGLE_STRIP:
    case GL_TRIANGLE_FAN:
    case GL_LINE_STRIP_ADJACENCY:
    case GL_TRIANGLE_STRIP_ADJACENCY:
        return true;
    default:
        return false;
    }
}

const std::string multidraw_comp_shader =
    R"(#version 310 es

layout(local_size_x = 64) in;

layout(location = 0) uniform uint uElementSize;

layout(std430, binding = 0) readonly buffer Input { uint in_indices[]; };
layout(std430, binding = 1) readonly buffer FirstIndex { uint firstIndex[]; };
layout(std430, binding = 2) readonly buffer BaseVertex { int baseVertex[]; };
layout(std430, binding = 3) readonly buffer Prefix { uint prefixSums[]; };
layout(std430, binding = 4) writeonly buffer Output { uint out_indices[]; };

uint read_index(uint elementIndex) {
    if (uElementSize == 4u) {
        return in_indices[elementIndex];
    }
    if (uElementSize == 2u) {
        uint word = in_indices[elementIndex >> 1u];
        uint shift = (elementIndex & 1u) * 16u;
        return (word >> shift) & 0xFFFFu;
    }
    uint word = in_indices[elementIndex >> 2u];
    uint shift = (elementIndex & 3u) * 8u;
    return (word >> shift) & 0xFFu;
}

void main() {
    uint outIdx = gl_GlobalInvocationID.x;
    uint drawCount = uint(prefixSums.length());
    if (drawCount == 0u) {
        return;
    }
    uint total = prefixSums[drawCount - 1u];
    if (outIdx >= total) {
        return;
    }

    int low = 0;
    int high = int(drawCount) - 1;
    while (low < high) {
        int mid = low + (high - low) / 2;
        if (prefixSums[mid] > outIdx) {
            high = mid; // next [low, mid)
        } else {
            low = mid + 1; // next [mid + 1, high)
        }
    }

    uint localIdx = outIdx - ((low == 0) ? 0u : (prefixSums[low - 1]));
    uint inIndex = localIdx + firstIndex[low];

    int idx = int(read_index(inIndex));
    out_indices[outIdx] = uint(idx + baseVertex[low]);
}

)";

static bool g_compute_inited = false;
std::vector<GLuint> g_prefix_sum(1);
GLuint g_prefixsumbuffer = 0;
GLuint g_firstidx_ssbo = 0;
GLuint g_basevtx_ssbo = 0;
GLuint g_outputibo = 0;
GLuint g_compute_program = 0;
GLint g_element_size_loc = -1;
char g_compile_info[1024];

GLuint compile_compute_program(const std::string& src) {
    INIT_CHECK_GL_ERROR
    auto program = GLES.glCreateProgram();
    CHECK_GL_ERROR_NO_INIT
    GLuint shader = GLES.glCreateShader(GL_COMPUTE_SHADER);
    CHECK_GL_ERROR_NO_INIT
    const char* s[] = {src.c_str()};
    const GLint length[] = {static_cast<GLint>(src.length())};
    GLES.glShaderSource(shader, 1, s, length);
    CHECK_GL_ERROR_NO_INIT
    GLES.glCompileShader(shader);
    CHECK_GL_ERROR_NO_INIT
    int success = 0;
    GLES.glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    CHECK_GL_ERROR_NO_INIT
    if (!success) {
        GLES.glGetShaderInfoLog(shader, 1024, NULL, g_compile_info);
        CHECK_GL_ERROR_NO_INIT
        LOG_E("%s: %s shader compile error: %s\nsrc:\n%s", __func__, "compute", g_compile_info, src.c_str());
#if DEBUG || GLOBAL_DEBUG
        abort();
#endif
        GLES.glDeleteShader(shader);
        GLES.glDeleteProgram(program);
        return 0;
    }

    GLES.glAttachShader(program, shader);
    CHECK_GL_ERROR_NO_INIT
    GLES.glLinkProgram(program);
    CHECK_GL_ERROR_NO_INIT

    GLES.glGetProgramiv(program, GL_LINK_STATUS, &success);
    CHECK_GL_ERROR_NO_INIT
    if (!success) {
        GLES.glGetProgramInfoLog(program, 1024, NULL, g_compile_info);
        CHECK_GL_ERROR_NO_INIT
        LOG_E("program link error: %s", g_compile_info);
#if DEBUG || GLOBAL_DEBUG
        abort();
#endif
        GLES.glDeleteShader(shader);
        GLES.glDeleteProgram(program);
        return 0;
    }

    GLES.glDeleteShader(shader);
    CHECK_GL_ERROR_NO_INIT
    g_element_size_loc = GLES.glGetUniformLocation(program, "uElementSize");
    CHECK_GL_ERROR_NO_INIT

    return program;
}

GLAPI GLAPIENTRY void mg_glMultiDrawElementsBaseVertex_compute(GLenum mode, GLsizei* counts, GLenum type,
                                                               const void* const* indices, GLsizei primcount,
                                                               const GLint* basevertex) {
    LOG()
    void prepareForDraw();
    prepareForDraw();

    INIT_CHECK_GL_ERROR

    if (primcount <= 0) return;
    if (!counts || !indices) {
        LOG_E("mg_glMultiDrawElementsBaseVertex_compute: counts or indices is null")
        return;
    }
    if (is_strip_like_mode(mode)) {
        LOG_D("mg_glMultiDrawElementsBaseVertex_compute: strip/loop mode, fallback")
        mg_glMultiDrawElementsBaseVertex_drawelements(mode, counts, type, indices, primcount, basevertex);
        return;
    }

    GLuint elementSize = 0;
    switch (type) {
    case GL_UNSIGNED_BYTE:
        elementSize = 1;
        break;
    case GL_UNSIGNED_SHORT:
        elementSize = 2;
        break;
    case GL_UNSIGNED_INT:
        elementSize = 4;
        break;
    default:
        LOG_E("mg_glMultiDrawElementsBaseVertex_compute: unsupported index type %s", glEnumToString(type))
        mg_glMultiDrawElementsBaseVertex_drawelements(mode, counts, type, indices, primcount, basevertex);
        return;
    }

    // Align compute shader input format with standard OpenGL indirect-draw format
    //    prepare_indirect_buffer(counts, type, indices, primcount, basevertex);
    //    prepare_compute_drawcmd_ssbo(counts, type, indices, primcount, basevertex);

    // Init compute buffers
    if (!g_compute_inited) {
        LOG_D("Initializing multidraw compute pipeline...")
        GLES.glGenBuffers(1, &g_prefixsumbuffer);
        GLES.glGenBuffers(1, &g_firstidx_ssbo);
        GLES.glGenBuffers(1, &g_basevtx_ssbo);
        GLES.glGenBuffers(1, &g_outputibo);

        g_compute_program = compile_compute_program(multidraw_comp_shader);
        if (g_compute_program == 0) {
            LOG_E("mg_glMultiDrawElementsBaseVertex_compute: compute program init failed, fallback")
            GLES.glDeleteBuffers(1, &g_prefixsumbuffer);
            GLES.glDeleteBuffers(1, &g_firstidx_ssbo);
            GLES.glDeleteBuffers(1, &g_basevtx_ssbo);
            GLES.glDeleteBuffers(1, &g_outputibo);
            g_prefixsumbuffer = 0;
            g_firstidx_ssbo = 0;
            g_basevtx_ssbo = 0;
            g_outputibo = 0;
            mg_glMultiDrawElementsBaseVertex_drawelements(mode, counts, type, indices, primcount, basevertex);
            return;
        }

        g_compute_inited = true;
    }

    GLint ibo = 0;
    GLES.glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &ibo);
    CHECK_GL_ERROR_NO_INIT
    if (ibo == 0) {
        LOG_D("mg_glMultiDrawElementsBaseVertex_compute: no element array buffer bound, fallback")
        mg_glMultiDrawElementsBaseVertex_drawelements(mode, counts, type, indices, primcount, basevertex);
        return;
    }
    GLint ibo_size = 0;
    GLES.glGetBufferParameteriv(GL_ELEMENT_ARRAY_BUFFER, GL_BUFFER_SIZE, &ibo_size);
    CHECK_GL_ERROR_NO_INIT
    if (ibo_size <= 0) {
        LOG_E("mg_glMultiDrawElementsBaseVertex_compute: invalid index buffer size, fallback")
        mg_glMultiDrawElementsBaseVertex_drawelements(mode, counts, type, indices, primcount, basevertex);
        return;
    }
    if (elementSize < 4 && (ibo_size % 4) != 0) {
        LOG_E("mg_glMultiDrawElementsBaseVertex_compute: index buffer size not 4-byte aligned, fallback")
        mg_glMultiDrawElementsBaseVertex_drawelements(mode, counts, type, indices, primcount, basevertex);
        return;
    }

    g_prefix_sum.resize(primcount);
    std::vector<GLuint> first_index(primcount, 0);
    std::vector<GLint> base_vtx(primcount, 0);

    uint64_t running = 0;
    for (GLsizei i = 0; i < primcount; ++i) {
        GLsizei c = counts[i];
        if (c < 0) {
            LOG_E("mg_glMultiDrawElementsBaseVertex_compute: negative count at %d", i)
            c = 0;
        }
        running += static_cast<uint64_t>(c);
        if (running > static_cast<uint64_t>(std::numeric_limits<GLuint>::max())) {
            LOG_E("mg_glMultiDrawElementsBaseVertex_compute: total index count overflow, fallback")
            mg_glMultiDrawElementsBaseVertex_drawelements(mode, counts, type, indices, primcount, basevertex);
            return;
        }
        g_prefix_sum[i] = static_cast<GLuint>(running);

        if (c > 0) {
            if (!indices[i]) {
                LOG_E("mg_glMultiDrawElementsBaseVertex_compute: indices[%d] is null", i)
                mg_glMultiDrawElementsBaseVertex_drawelements(mode, counts, type, indices, primcount, basevertex);
                return;
            }
            uintptr_t byteOffset = reinterpret_cast<uintptr_t>(indices[i]);
            uint64_t byteOffset64 = static_cast<uint64_t>(byteOffset);
            if ((byteOffset % elementSize) != 0) {
                LOG_E("mg_glMultiDrawElementsBaseVertex_compute: misaligned index offset at %d", i)
            }
            if (byteOffset64 > static_cast<uint64_t>(ibo_size)) {
                LOG_E("mg_glMultiDrawElementsBaseVertex_compute: index offset out of range at %d", i)
                mg_glMultiDrawElementsBaseVertex_drawelements(mode, counts, type, indices, primcount, basevertex);
                return;
            }
            uint64_t byteEnd = byteOffset64 + static_cast<uint64_t>(c) * elementSize;
            if (byteEnd > static_cast<uint64_t>(ibo_size)) {
                LOG_E("mg_glMultiDrawElementsBaseVertex_compute: index range out of bounds at %d", i)
                mg_glMultiDrawElementsBaseVertex_drawelements(mode, counts, type, indices, primcount, basevertex);
                return;
            }
            uint64_t elementOffset = byteOffset64 / elementSize;
            if (elementOffset > static_cast<uint64_t>(std::numeric_limits<GLuint>::max())) {
                LOG_E("mg_glMultiDrawElementsBaseVertex_compute: index offset overflow at %d", i)
                mg_glMultiDrawElementsBaseVertex_drawelements(mode, counts, type, indices, primcount, basevertex);
                return;
            }
            first_index[i] = static_cast<GLuint>(elementOffset);
        }

        if (basevertex) {
            base_vtx[i] = basevertex[i];
        }
    }

    auto total_indices = g_prefix_sum[primcount - 1];
    if (total_indices == 0) {
        return;
    }

    GLint prev_ssbo_binding = 0;
    GLES.glGetIntegerv(GL_SHADER_STORAGE_BUFFER_BINDING, &prev_ssbo_binding);
    CHECK_GL_ERROR_NO_INIT

    // Fill in the data
    GLES.glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_firstidx_ssbo);
    CHECK_GL_ERROR_NO_INIT
    GLES.glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(GLuint) * primcount, first_index.data(), GL_DYNAMIC_DRAW);
    CHECK_GL_ERROR_NO_INIT

    GLES.glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_basevtx_ssbo);
    CHECK_GL_ERROR_NO_INIT
    GLES.glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(GLint) * primcount, base_vtx.data(), GL_DYNAMIC_DRAW);
    CHECK_GL_ERROR_NO_INIT

    GLES.glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_prefixsumbuffer);
    CHECK_GL_ERROR_NO_INIT
    GLES.glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(GLuint) * primcount, g_prefix_sum.data(), GL_DYNAMIC_DRAW);
    CHECK_GL_ERROR_NO_INIT

    // Allocate output buffer
    GLES.glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_outputibo);
    CHECK_GL_ERROR_NO_INIT
    GLES.glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(GLuint) * total_indices, nullptr, GL_DYNAMIC_DRAW);
    CHECK_GL_ERROR_NO_INIT

    GLES.glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    CHECK_GL_ERROR_NO_INIT

    // Bind buffers
    GLint prev_ssbo_base[5] = {};
    for (int i = 0; i < 5; ++i) {
        GLES.glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, i, &prev_ssbo_base[i]);
        CHECK_GL_ERROR_NO_INIT
    }

    GLES.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ibo);
    CHECK_GL_ERROR_NO_INIT
    GLES.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, g_firstidx_ssbo);
    CHECK_GL_ERROR_NO_INIT
    GLES.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, g_basevtx_ssbo);
    CHECK_GL_ERROR_NO_INIT
    GLES.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, g_prefixsumbuffer);
    CHECK_GL_ERROR_NO_INIT
    GLES.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, g_outputibo);
    CHECK_GL_ERROR_NO_INIT

    // Save states
    GLint prev_program = 0;
    GLES.glGetIntegerv(GL_CURRENT_PROGRAM, &prev_program);
    CHECK_GL_ERROR_NO_INIT
    GLint prev_vb = 0;
    GLES.glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_vb);
    CHECK_GL_ERROR_NO_INIT

    // Dispatch compute
    LOG_D("Using compute program = %d", g_compute_program)
    GLES.glUseProgram(g_compute_program);
    CHECK_GL_ERROR_NO_INIT
    if (g_element_size_loc >= 0) {
        GLES.glUniform1ui(g_element_size_loc, elementSize);
        CHECK_GL_ERROR_NO_INIT
    }
    LOG_D("Dispatch compute")
    GLES.glDispatchCompute((total_indices + 63) / 64, 1, 1);
    CHECK_GL_ERROR_NO_INIT

    // Wait for compute to complete
    LOG_D("memory barrier")
    GLES.glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_ELEMENT_ARRAY_BARRIER_BIT);
    CHECK_GL_ERROR_NO_INIT

    // Bind index buffer and do draw
    LOG_D("draw")
    GLES.glUseProgram(prev_program);
    CHECK_GL_ERROR_NO_INIT
    GLES.glBindBuffer(GL_ARRAY_BUFFER, prev_vb);
    CHECK_GL_ERROR_NO_INIT
    GLES.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_outputibo);
    CHECK_GL_ERROR_NO_INIT
    GLES.glDrawElements(mode, total_indices, GL_UNSIGNED_INT, 0);

    // Restore states
    for (int i = 0; i < 5; ++i) {
        GLES.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, i, prev_ssbo_base[i]);
        CHECK_GL_ERROR_NO_INIT
    }
    GLES.glBindBuffer(GL_SHADER_STORAGE_BUFFER, prev_ssbo_binding);
    CHECK_GL_ERROR_NO_INIT
    GLES.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    CHECK_GL_ERROR_NO_INIT
}
