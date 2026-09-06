// Copyright (c) 2025-2026 MobileGL-Dev
// SPDX-License-Identifier: LGPL-2.1-only
// MobileGlues draw preparation. LGPL-2.1-only.
#include "drawing.h"
#include "shader.h"
#include "index_rebase_core.h"
#include "restart.h"
#include "buffer.h"
#include "framebuffer.h"
#include "mg.h"
#include "texture.h"
#include "../egl/context.h"
#include <algorithm>
#define DEBUG 0

void setupBufferTextureUniforms(GLuint program) {
    auto& group = mg_shader_objects();
    std::lock_guard<std::recursive_mutex> lock(group.mutex);
    const auto it = group.programs.find(program);
    if (it == group.programs.end() || it->second.metadata.buffer_samplers.empty()) return;
    auto& record = it->second;
    if (record.sampler_cache_generation != record.link_generation) {
        record.sampler_locations.clear();
        record.buffer_width_location = GLES.glGetUniformLocation(program, "u_BufferTexWidth");
        record.buffer_height_location = GLES.glGetUniformLocation(program, "u_BufferTexHeight");
        GLint count = 0;
        GLES.glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &count);
        for (GLint i = 0; i < count; ++i) {
            char name[1024]{};
            GLint size = 0;
            GLenum type = 0;
            GLsizei length = 0;
            GLES.glGetActiveUniform(program, i, sizeof(name), &length, &size, &type, name);
            std::string base(name);
            const size_t bracket = base.find('[');
            if (bracket != std::string::npos) base.resize(bracket);
            if (std::find(record.metadata.buffer_samplers.begin(), record.metadata.buffer_samplers.end(), base) ==
                record.metadata.buffer_samplers.end())
                continue;
            for (GLint element = 0; element < size; ++element) {
                const std::string uniform =
                    bracket != std::string::npos ? base + "[" + std::to_string(element) + "]" : base;
                const GLint location = GLES.glGetUniformLocation(program, uniform.c_str());
                if (location >= 0) record.sampler_locations.push_back(location);
            }
        }
        record.sampler_cache_generation = record.link_generation;
    }
    if (record.buffer_width_location < 0 || record.sampler_locations.empty()) return;
    GLuint texture = 0;
    constexpr GLint unit = 15;
    if (!mg_driver_texture_binding_at_unit(unit, GL_TEXTURE_2D, &texture)) {
        const int previous = mg_driver_active_texture_unit();
        GLES.glActiveTexture(GL_TEXTURE0 + unit);
        GLint value = 0;
        GLES.glGetIntegerv(GL_TEXTURE_BINDING_2D, &value);
        texture = value;
        GLES.glActiveTexture(GL_TEXTURE0 + previous);
    }
    const TextureObject* object = mgGetTexObjectByID(texture);
    if (!object) return;
    // Only original buffer samplers are rewritten. Ordinary sampler2D uniforms
    // keep the application's unit even in a program that also uses a buffer.
    for (GLint location : record.sampler_locations)
        GLES.glUniform1i(location, unit);
    GLES.glUniform1i(record.buffer_width_location, object->width);
    GLES.glUniform1i(record.buffer_height_location, object->height);
}
void prepareForDraw() {
    LOG_D("prepareForDraw...")
    if (hardware->emulate_texture_buffer) {
        setupBufferTextureUniforms(gl_state->current_program);
    }
}

void glDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei primcount) {
    LOG()
    LOG_D("glDrawElementsInstanced, mode: %d, count: %d, type: %d, indices: %p, primcount: %d", mode, count, type,
          indices, primcount)
    prepareForDraw();
    if (mg_restart_needs_rewrite(type) && mg_draw_elements_restart(mode, count, type, indices, 0, primcount)) return;
    const bool restart_fixed = mg_restart_needs_driver_fixed(type);
    if (restart_fixed) GLES.glEnable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
    GLES.glDrawElementsInstanced(mode, count, type, indices, primcount);
    if (restart_fixed) GLES.glDisable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
    CHECK_GL_ERROR
}

void glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    LOG()
    LOG_D("glDrawElements, mode: %d, count: %d, type: %d, indices: %p", mode, count, type, indices)
    prepareForDraw();
    if (mg_restart_needs_rewrite(type) && mg_draw_elements_restart(mode, count, type, indices, 0, -1)) return;
    const bool restart_fixed = mg_restart_needs_driver_fixed(type);
    if (restart_fixed) GLES.glEnable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
    GLES.glDrawElements(mode, count, type, indices);
    if (restart_fixed) GLES.glDisable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
    CHECK_GL_ERROR
}

void glBindImageTexture(GLuint unit, GLuint texture, GLint level, GLboolean layered, GLint layer, GLenum access,
                        GLenum format) {
    LOG()
    LOG_D("glBindImageTexture, unit: %d, texture: %d, level: %d, layered: %d, layer: %d, access: %d, format: %d", unit,
          texture, level, layered, layer, access, format)
    GLES.glBindImageTexture(unit, texture, level, layered, layer, access, format);
    CHECK_GL_ERROR
}

void glUniform1i(GLint location, GLint v0) {
    LOG()
    LOG_D("glUniform1i, location: %d, v0: %d", location, v0)
    GLES.glUniform1i(location, v0);
    CHECK_GL_ERROR
}

void glDispatchCompute(GLuint num_groups_x, GLuint num_groups_y, GLuint num_groups_z) {
    LOG()
    LOG_D("glDispatchCompute, num_groups_x: %d, num_groups_y: %d, num_groups_z: %d", num_groups_x, num_groups_y,
          num_groups_z)
    GLES.glDispatchCompute(num_groups_x, num_groups_y, num_groups_z);
    CHECK_GL_ERROR
}

void glMemoryBarrier(GLbitfield barriers) {
    LOG()
    LOG_D("glMemoryBarrier, barriers: %d", barriers)
    GLES.glMemoryBarrier(barriers);
    CHECK_GL_ERROR
}

namespace {

    // Scratch index buffer for the base-vertex emulation below, and the context that
    // owns it. Modeled on gl/restart.cpp's g_restart_ibo, including the invalidation:
    // thread_local because g_current_ctx is, so two threads with different current
    // contexts keep their own name instead of trading one back and forth.
    thread_local GLuint g_basevertex_ibo = 0;
    thread_local unsigned long long g_basevertex_owner_ctx_id = 0;

    // Drop the cached name when the current context is not the one that created it.
    //
    // Deliberately no glDeleteBuffers: if the owning context is gone the buffer went
    // with it, and if it is merely not current then this name refers to a buffer
    // belonging to whichever context *is* current -- the glBufferData below would
    // overwrite that buffer's contents.
    void basevertex_check_context() {
        const unsigned long long cur = g_current_ctx ? g_current_ctx->id : 0;
        if (cur == g_basevertex_owner_ctx_id) return;
        g_basevertex_ibo = 0;
        g_basevertex_owner_ctx_id = cur;
    }

    // Staging for the rebased index stream. Elements are GLuint so the storage is
    // always aligned for the widest index type it has to hold; the length is in whole
    // GLuints, rounded up. Grown and never shrunk, so a steady stream of draws
    // allocates nothing -- this used to be a malloc and a free per call.
    thread_local std::vector<GLuint> g_basevertex_staging;

    void* basevertex_staging(size_t bytes) {
        // Grown only. A plain resize() to the exact length shrinks after a small draw
        // and then value-initialises the difference on the next large one -- a memset
        // of the whole tail that the caller's memcpy overwrites immediately. Only the
        // first `bytes` bytes are ever read, so what is past them is out of range in
        // the same way it is in gl/multidraw.cpp and gl/restart.cpp.
        const size_t need = (bytes + sizeof(GLuint) - 1) / sizeof(GLuint);
        if (g_basevertex_staging.size() < need) g_basevertex_staging.resize(need);
        return g_basevertex_staging.data();
    }

} // namespace

void glDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices, GLint basevertex) {
    LOG()
    LOG_D("glDrawElementsBaseVertex, mode: %d, count: %d, type: %d, indices: %p, basevertex: %d", mode, count, type,
          indices, basevertex);
    prepareForDraw();
    // The rewrite applies the base vertex itself, so it covers both the emulated
    // and the driver-supported branch below.
    if (mg_restart_needs_rewrite(type) && mg_draw_elements_restart(mode, count, type, indices, basevertex, -1)) return;
    const bool restart_fixed = mg_restart_needs_driver_fixed(type);
    if (restart_fixed) GLES.glEnable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
    struct RestartGuard {
        bool on;
        ~RestartGuard() {
            if (on) GLES.glDisable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
        }
    } restart_guard{restart_fixed};
    if (hardware->es_version < 320 && !g_gles_caps.GL_EXT_draw_elements_base_vertex &&
        !g_gles_caps.GL_OES_draw_elements_base_vertex) {
        // TODO: use indirect drawing for GLES 3.1
        LOG_D("Emulating glDrawElementsBaseVertex")
        if (basevertex == 0) {
            GLES.glDrawElements(mode, count, type, indices);
            return;
        }
        if (count <= 0) return;

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

        const size_t bytes = static_cast<size_t>(count) * indexSize;

        // The tracked binding rather than a driver round trip. It is the driver's
        // name, so it goes straight back to GLES.glBindBuffer, and it is asked for
        // before the temporary bind below, which is the only window in which this
        // function makes the two disagree. gl/gl.cpp's depth-clear triangle is the
        // one path in the layer that leaves the driver on a vertex array the
        // tracked state does not follow, and the element array binding is vertex
        // array state; see the note on the accessor in gl/buffer.cpp.
        const GLuint prevElementBuffer = mg_driver_bound_buffer(GL_ELEMENT_ARRAY_BUFFER);

        void* tempIndices = basevertex_staging(bytes);

        if (prevElementBuffer != 0) {
            // Redundant on paper -- that buffer is already bound -- but it is what
            // guarantees the map below reads the buffer this call resolved, the
            // same way gl/restart.cpp and gl/multidraw.cpp do it.
            GLES.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, prevElementBuffer);
            // Read-only, and it has to stay a map: gl/buffer.cpp tracks a buffer's
            // size but never its contents, so there is no shadow copy of the index
            // data to rebase from.
            void* srcData = GLES.glMapBufferRange(GL_ELEMENT_ARRAY_BUFFER,
                                                  static_cast<GLintptr>(reinterpret_cast<uintptr_t>(indices)),
                                                  static_cast<GLsizeiptr>(bytes), GL_MAP_READ_BIT);
            if (!srcData) {
                // An immutable or persistently mapped index buffer cannot be read
                // back, and there is no driver base vertex on this path to fall
                // back to. Drop the draw rather than place the geometry at the
                // wrong vertices.
                return;
            }
            memcpy(tempIndices, srcData, bytes);
            GLES.glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER);
        } else {
            if (!indices) return;
            memcpy(tempIndices, indices, bytes);
        }

        std::vector<GLuint> rebased(static_cast<size_t>(count));
        const bool preserve_restart = restart_fixed || mg_enable_state()->scalar[MGC_PRIMITIVE_RESTART_FIXED_INDEX];
        const GLuint sentinel = type == GL_UNSIGNED_BYTE ? 0xffu : type == GL_UNSIGNED_SHORT ? 0xffffu : 0xffffffffu;
        mg_rebase_indices_to_u32(rebased.data(), tempIndices, count, type, basevertex, preserve_restart, sentinel);
        // One persistent scratch buffer instead of a glGenBuffers/glDeleteBuffers
        // pair per draw call.
        basevertex_check_context();
        if (!g_basevertex_ibo) GLES.glGenBuffers(1, &g_basevertex_ibo);
        GLES.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_basevertex_ibo);
        GLES.glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(rebased.size() * sizeof(GLuint)),
                          rebased.data(), GL_STREAM_DRAW);

        GLES.glDrawElements(mode, count, GL_UNSIGNED_INT, nullptr);

        GLES.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, prevElementBuffer);

        CHECK_GL_ERROR
    } else {
        GLES.glDrawElementsBaseVertex(mode, count, type, indices, basevertex);
    }
    CHECK_GL_ERROR
}

#define DR_WARN_ONCE(...)                                                                                              \
    do {                                                                                                               \
        static bool mg_dr_warned = false;                                                                              \
        if (!mg_dr_warned) {                                                                                           \
            mg_dr_warned = true;                                                                                       \
            LOG_W_FORCE(__VA_ARGS__)                                                                                   \
        }                                                                                                              \
    } while (0)

// ---------------------------------------------------------------------------
// The rest of the indexed draw family
//
// These were pass-throughs in gl/gl_native.cpp, so GL_PRIMITIVE_RESTART with a
// custom index went straight to a driver that has no such feature and every
// restart in the batch was drawn as ordinary geometry -- strips joined end to
// end. They are indexed draws like the three above and owe the same treatment:
// rewrite the stream when the chosen value is not the fixed one, and otherwise
// switch the driver's fixed-index restart on for the duration.
// ---------------------------------------------------------------------------

// Brackets a draw with GLES' fixed-index restart. Scoped so an early return
// cannot leave it enabled behind the application's back.
namespace {
    struct restart_guard_t {
        bool on;
        explicit restart_guard_t(GLenum type) : on(mg_restart_needs_driver_fixed(type)) {
            if (on) GLES.glEnable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
        }
        ~restart_guard_t() {
            if (on) GLES.glDisable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
        }
        restart_guard_t(const restart_guard_t&) = delete;
        restart_guard_t& operator=(const restart_guard_t&) = delete;
    };
} // namespace

void glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void* indices) {
    LOG()
    LOG_D("glDrawRangeElements, mode: %d, start: %u, end: %u, count: %d, type: %d", mode, start, end, count, type)
    prepareForDraw();
    // The rewritten stream is 32-bit with 0xFFFFFFFF sentinels, so start/end no
    // longer describe it. They are only a promise about the index range, and
    // dropping the promise is allowed; drawing the wrong primitives is not.
    if (mg_restart_needs_rewrite(type) && mg_draw_elements_restart(mode, count, type, indices, 0, -1)) return;
    restart_guard_t guard(type);
    GLES.glDrawRangeElements(mode, start, end, count, type, indices);
    CHECK_GL_ERROR
}

void glDrawRangeElementsBaseVertex(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                                   const void* indices, GLint basevertex) {
    LOG()
    LOG_D("glDrawRangeElementsBaseVertex, mode: %d, count: %d, type: %d, basevertex: %d", mode, count, type, basevertex)
    prepareForDraw();
    if (mg_restart_needs_rewrite(type) && mg_draw_elements_restart(mode, count, type, indices, basevertex, -1)) return;
    restart_guard_t guard(type);
    if (GLES.glDrawRangeElementsBaseVertex) {
        GLES.glDrawRangeElementsBaseVertex(mode, start, end, count, type, indices, basevertex);
    } else {
        // glDrawElementsBaseVertex above already emulates the base vertex when
        // the driver cannot; the range is the only thing lost.
        glDrawElementsBaseVertex(mode, count, type, indices, basevertex);
    }
    CHECK_GL_ERROR
}

void glDrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                       GLsizei instancecount, GLint basevertex) {
    LOG()
    LOG_D("glDrawElementsInstancedBaseVertex, mode: %d, count: %d, type: %d, instancecount: %d, basevertex: %d", mode,
          count, type, instancecount, basevertex)
    prepareForDraw();
    if (mg_restart_needs_rewrite(type) &&
        mg_draw_elements_restart(mode, count, type, indices, basevertex, instancecount))
        return;
    restart_guard_t guard(type);
    if (GLES.glDrawElementsInstancedBaseVertex) {
        GLES.glDrawElementsInstancedBaseVertex(mode, count, type, indices, instancecount, basevertex);
    } else if (basevertex == 0) {
        GLES.glDrawElementsInstanced(mode, count, type, indices, instancecount);
    } else {
        DR_WARN_ONCE("glDrawElementsInstancedBaseVertex: no base vertex support on this context, drawing without it");
        GLES.glDrawElementsInstanced(mode, count, type, indices, instancecount);
    }
    CHECK_GL_ERROR
}

// ---------------------------------------------------------------------------
// The base instance family (GL 4.2 / ARB_base_instance)
//
// GLES has no base instance in core, so these three were stubs in
// gl/gl_stub.cpp: called, they drew nothing at all. That is the worst of the
// available options -- a mesh that silently never appears is harder to diagnose
// than one in the wrong place, and baseinstance is 0 in the overwhelming
// majority of calls, where these commands are exactly the ones GLES already
// implements.
//
// A nonzero baseinstance is forwarded to the GL_EXT_base_instance entry point
// when the loader resolved one. gles/loader.cpp advertises GL_ARB_base_instance
// on exactly that condition, so an application that saw the string never
// reaches the fallback; without the EXT the old compromise stands -- forward
// without the base instance, report once, stay visible. The zero case keeps
// taking the core-GLES wrappers, whose restart rewrite is more capable than
// the EXT calls below.
// ---------------------------------------------------------------------------

void glDrawArraysInstancedBaseInstance(GLenum mode, GLint first, GLsizei count, GLsizei instancecount,
                                       GLuint baseinstance) {
    LOG()
    LOG_D("glDrawArraysInstancedBaseInstance, mode: %d, first: %d, count: %d, instancecount: %d, baseinstance: %u",
          mode, first, count, instancecount, baseinstance)
    if (baseinstance != 0 && GLES.glDrawArraysInstancedBaseInstanceEXT) {
        prepareForDraw();
        GLES.glDrawArraysInstancedBaseInstanceEXT(mode, first, count, instancecount, baseinstance);
        CHECK_GL_ERROR
        return;
    }
    if (baseinstance != 0) {
        DR_WARN_ONCE("glDrawArraysInstancedBaseInstance: baseinstance %u ignored, GLES has no base instance",
                     baseinstance);
    }
    prepareForDraw();
    GLES.glDrawArraysInstanced(mode, first, count, instancecount);
    CHECK_GL_ERROR
}

void glDrawElementsInstancedBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                         GLsizei instancecount, GLuint baseinstance) {
    LOG()
    LOG_D("glDrawElementsInstancedBaseInstance, mode: %d, count: %d, type: %d, instancecount: %d, baseinstance: %u",
          mode, count, type, instancecount, baseinstance)
    if (baseinstance != 0 && GLES.glDrawElementsInstancedBaseInstanceEXT) {
        prepareForDraw();
        // Same policy as glMultiDrawElementsIndirect: the rewrite path cannot
        // carry a base instance, so only the fixed-index restart is honoured.
        if (mg_restart_needs_rewrite(type)) {
            DR_WARN_ONCE("glDrawElementsInstancedBaseInstance: GL_PRIMITIVE_RESTART with a custom index cannot "
                         "be rewritten with a base instance; restarts will be ignored");
        }
        restart_guard_t guard(type);
        GLES.glDrawElementsInstancedBaseInstanceEXT(mode, count, type, indices, instancecount, baseinstance);
        CHECK_GL_ERROR
        return;
    }
    if (baseinstance != 0) {
        DR_WARN_ONCE("glDrawElementsInstancedBaseInstance: baseinstance %u ignored, GLES has no base instance",
                     baseinstance);
    }
    glDrawElementsInstanced(mode, count, type, indices, instancecount);
}

void glDrawElementsInstancedBaseVertexBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                                   GLsizei instancecount, GLint basevertex, GLuint baseinstance) {
    LOG()
    LOG_D("glDrawElementsInstancedBaseVertexBaseInstance, mode: %d, count: %d, basevertex: %d, baseinstance: %u", mode,
          count, basevertex, baseinstance)
    if (baseinstance != 0 && GLES.glDrawElementsInstancedBaseVertexBaseInstanceEXT) {
        prepareForDraw();
        if (mg_restart_needs_rewrite(type)) {
            DR_WARN_ONCE("glDrawElementsInstancedBaseVertexBaseInstance: GL_PRIMITIVE_RESTART with a custom index "
                         "cannot be rewritten with a base instance; restarts will be ignored");
        }
        restart_guard_t guard(type);
        GLES.glDrawElementsInstancedBaseVertexBaseInstanceEXT(mode, count, type, indices, instancecount, basevertex,
                                                              baseinstance);
        CHECK_GL_ERROR
        return;
    }
    if (baseinstance != 0) {
        DR_WARN_ONCE(
            "glDrawElementsInstancedBaseVertexBaseInstance: baseinstance %u ignored, GLES has no base instance",
            baseinstance);
    }
    glDrawElementsInstancedBaseVertex(mode, count, type, indices, instancecount, basevertex);
}
