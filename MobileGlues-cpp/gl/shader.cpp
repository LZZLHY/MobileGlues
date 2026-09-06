// Copyright (c) 2025-2026 MobileGL-Dev
// SPDX-License-Identifier: LGPL-2.1-only
// MobileGlues shader objects and submissions. LGPL-2.1-only.
#include "shader.h"
#include "program.h"
#include "../egl/context.h"
#include "../gles/loader.h"
#include "glsl/glsl_for_es.h"
#include "glsl/uniform_initializer_core.h"
#include "mg.h"
#include "FSR1/FSR1.h"
#include "../config/settings.h"
#include <algorithm>
#include <climits>
#include <cstring>
#include <sstream>
#define DEBUG 0

mg_shader_group& mg_shader_objects() {
    if (g_current_ctx && g_current_ctx->share_group && g_current_ctx->share_group->shader_objects)
        return *g_current_ctx->share_group->shader_objects;
    static thread_local mg_shader_group fallback;
    return fallback;
}

void mg_release_shader_attachment(mg_shader_group& group, GLuint shader) {
    const auto it = group.shaders.find(shader);
    if (it == group.shaders.end()) return;
    if (it->second.attachment_count > 0) --it->second.attachment_count;
    if (it->second.deleted && it->second.attachment_count == 0) group.shaders.erase(it);
}

bool mg_shader_translate(mg_shader_record& shader, const mg_frag_bindings* outputs, std::string& source,
                         mg_glsl_metadata& metadata, std::string& error) {
    int rc = -1;
    try {
        size_t start = 0;
        mg::glsl::detail::skipTrivia(shader.original, start);
        std::istringstream header(shader.original.substr(start));
        std::string directive, profile;
        unsigned version = 0;
        header >> directive >> version >> profile;
        const bool direct = directive == "#version" &&
                            (version == 100 || (profile == "es" && version >= 300 && version <= hardware->es_version));
        if (direct && (!outputs || outputs->empty())) {
            source = shader.original;
            metadata = {};
            return true;
        }
        // Program-specific output bindings use the parsed AST, not edits of
        // auto-assigned locations in a different shader's converted source.
        source = GLSLtoGLSLES(shader.original.c_str(), shader.type, hardware->es_version,
                              getGLSLVersion(shader.original.c_str()), rc, outputs);
        if (rc >= 0 && !source.empty()) {
            mg_read_translation_metadata(source, metadata);
            return true;
        }
        error = mg_translation_error();
        if (error.empty()) error = "MobileGlues GLSL translation failed (stage code " + std::to_string(rc) + ")";
    }
    catch (const std::exception& ex) {
        error = std::string("MobileGlues GLSL translation exception: ") + ex.what();
    }
    catch (...) {
        error = "MobileGlues GLSL translation failed with an unknown exception";
    }
    source.clear();
    return false;
}

static void copy_shader_text(const std::string& text, GLsizei size, GLsizei* length, GLchar* out) {
    if (length) *length = 0;
    if (size < 0) {
        mg_set_gl_error(GL_INVALID_VALUE);
        return;
    }
    if (size == 0 || !out) return;
    const size_t n = std::min(text.size(), static_cast<size_t>(size - 1));
    std::memcpy(out, text.data(), n);
    out[n] = 0;
    if (length) *length = static_cast<GLsizei>(n);
}

void glShaderSource(GLuint shader, GLsizei count, const GLchar* const* strings, const GLint* lengths) {
    LOG()
    if (count < 0 || (count > 0 && !strings)) {
        mg_set_gl_error(GL_INVALID_VALUE);
        return;
    }
    if (!GLES.glIsShader(shader)) {
        mg_set_gl_error(GL_INVALID_VALUE);
        return;
    }
    auto& group = mg_shader_objects();
    std::lock_guard<std::recursive_mutex> lock(group.mutex);
    auto& record = group.shaders[shader];
    if (!record.type) {
        GLint type = 0;
        GLES.glGetShaderiv(shader, GL_SHADER_TYPE, &type);
        record.type = type;
    }
    std::string original;
    try {
        for (GLsizei i = 0; i < count; ++i) {
            if (!strings[i]) {
                mg_set_gl_error(GL_INVALID_VALUE);
                return;
            }
            const size_t n = lengths && lengths[i] >= 0 ? static_cast<size_t>(lengths[i]) : std::strlen(strings[i]);
            if (n > static_cast<size_t>(INT_MAX) - original.size()) {
                mg_set_gl_error(GL_OUT_OF_MEMORY);
                return;
            }
            original.append(strings[i], n);
        }
    }
    catch (const std::bad_alloc&) {
        mg_set_gl_error(GL_OUT_OF_MEMORY);
        return;
    }
    record.generation = group.generation++;
    record.original = std::move(original);
    record.converted.clear();
    record.failure.clear();
    record.metadata = {};
    // Empty input is still a source update. It must remove the old driver text.
    if (!record.original.empty())
        mg_shader_translate(record, nullptr, record.converted, record.metadata, record.failure);
    if (record.original.empty()) record.converted.clear();
    std::string submitted = record.converted;
    if (!record.failure.empty()) {
        submitted = "#version 300 es\n#error MobileGlues_translation_failed\n";
        LOG_W_FORCE("[MG-SHADER-FAIL] shader=%u generation=%llu count=%d: %s", shader,
                    static_cast<unsigned long long>(record.generation), count, record.failure.c_str())
    }
    const GLchar* text = submitted.data();
    const GLint length = static_cast<GLint>(submitted.size());
    GLES.glShaderSource(shader, 1, &text, &length);
}

void glCompileShader(GLuint shader) {
    LOG()
    auto& group = mg_shader_objects();
    std::lock_guard<std::recursive_mutex> lock(group.mutex);
    GLES.glCompileShader(shader);
    GLint status = GL_FALSE;
    GLES.glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    const auto it = group.shaders.find(shader);
    if (it != group.shaders.end()) {
        it->second.compiled = status == GL_TRUE && it->second.failure.empty();
        if (it->second.compiled) {
            it->second.compiled_original = it->second.original;
            it->second.compiled_metadata = it->second.metadata;
        }
    }
    if (!status) {
        char log[4096]{};
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        LOG_W_FORCE("[MG-SHADER-COMPILE] shader=%u failed: %s", shader, log)
    }
}

void glGetShaderiv(GLuint shader, GLenum pname, GLint* params) {
    LOG()
    if (!params) {
        mg_set_gl_error(GL_INVALID_VALUE);
        return;
    }
    auto& group = mg_shader_objects();
    std::lock_guard<std::recursive_mutex> lock(group.mutex);
    const auto it = group.shaders.find(shader);
    if (it != group.shaders.end()) {
        if (pname == GL_SHADER_SOURCE_LENGTH) {
            *params = it->second.original.empty() ? 0 : static_cast<GLint>(it->second.original.size() + 1);
            return;
        }
        if (!it->second.failure.empty()) {
            if (pname == GL_COMPILE_STATUS) {
                *params = GL_FALSE;
                return;
            }
            if (pname == GL_INFO_LOG_LENGTH) {
                *params = static_cast<GLint>(it->second.failure.size() + 1);
                return;
            }
        }
    }
    GLES.glGetShaderiv(shader, pname, params);
}

void glGetShaderInfoLog(GLuint shader, GLsizei size, GLsizei* length, GLchar* log) {
    auto& group = mg_shader_objects();
    std::lock_guard<std::recursive_mutex> lock(group.mutex);
    const auto it = group.shaders.find(shader);
    if (it != group.shaders.end() && !it->second.failure.empty()) {
        copy_shader_text(it->second.failure, size, length, log);
        return;
    }
    GLES.glGetShaderInfoLog(shader, size, length, log);
}

void glGetShaderSource(GLuint shader, GLsizei size, GLsizei* length, GLchar* source) {
    auto& group = mg_shader_objects();
    std::lock_guard<std::recursive_mutex> lock(group.mutex);
    const auto it = group.shaders.find(shader);
    if (it != group.shaders.end()) {
        copy_shader_text(it->second.original, size, length, source);
        return;
    }
    GLES.glGetShaderSource(shader, size, length, source);
}

GLuint glCreateShader(GLenum type) {
    LOG()
    if (global_settings.fsr1_setting != FSR1_Quality_Preset::Disabled && !fsrInitialized) InitFSRResources();
    const GLuint shader = GLES.glCreateShader(type);
    if (shader) {
        auto& group = mg_shader_objects();
        std::lock_guard<std::recursive_mutex> lock(group.mutex);
        auto& record = group.shaders[shader];
        record = {};
        record.type = type;
        record.generation = group.generation++;
    }
    return shader;
}

void glDeleteShader(GLuint shader) {
    LOG()
    auto& group = mg_shader_objects();
    std::lock_guard<std::recursive_mutex> lock(group.mutex);
    mg_begin_driver_operation();
    GLES.glDeleteShader(shader);
    if (!mg_end_driver_operation("glDeleteShader")) return;
    auto it = group.shaders.find(shader);
    if (it != group.shaders.end()) {
        it->second.deleted = true;
        if (it->second.attachment_count == 0) group.shaders.erase(it);
    }
}

extern "C"
{
    GLAPI GLAPIENTRY void glCompileShaderARB(GLuint) __attribute__((alias("glCompileShader")));
    GLAPI GLAPIENTRY void glDeleteShaderARB(GLuint) __attribute__((alias("glDeleteShader")));
    GLAPI GLAPIENTRY void glGetShaderInfoLogARB(GLuint, GLsizei, GLsizei*, GLchar*)
        __attribute__((alias("glGetShaderInfoLog")));
    GLAPI GLAPIENTRY void glGetShaderSourceARB(GLuint, GLsizei, GLsizei*, GLchar*)
        __attribute__((alias("glGetShaderSource")));
}
