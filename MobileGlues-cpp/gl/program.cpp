// Copyright (c) 2025-2026 MobileGL-Dev
// SPDX-License-Identifier: LGPL-2.1-only
// MobileGlues program/link state. LGPL-2.1-only.
#include "program.h"
#include "shader.h"
#include "mg.h"
#include "../gles/loader.h"
#include <algorithm>
#include <cstring>
#include <vector>
#define DEBUG 0

static void collect_deleted_programs(mg_shader_group& group) {
    // Only names explicitly marked for deferred deletion can need work here.
    // Do not scan the live object graph on every program bind.
    for (auto pending = group.pending_program_deletions.begin(); pending != group.pending_program_deletions.end();) {
        if (GLES.glIsProgram(*pending)) { ++pending; continue; }
        const auto it = group.programs.find(*pending);
        if (it != group.programs.end()) {
            for (GLuint shader : it->second.attached) mg_release_shader_attachment(group, shader);
            group.programs.erase(it);
        }
        pending = group.pending_program_deletions.erase(pending);
    }
}

GLuint glCreateProgram() {
    LOG()
    const GLuint program = GLES.glCreateProgram();
    if (program) {
        auto& group = mg_shader_objects();
        std::lock_guard<std::recursive_mutex> lock(group.mutex);
        const auto old = group.programs.find(program);
        if (old != group.programs.end()) {
            for (GLuint shader : old->second.attached) mg_release_shader_attachment(group, shader);
        }
        group.pending_program_deletions.erase(program);
        auto& record = group.programs[program];
        record = {};
        record.generation = group.generation++;
    }
    return program;
}

void glAttachShader(GLuint program, GLuint shader) {
    LOG()
    auto& group = mg_shader_objects();
    std::lock_guard<std::recursive_mutex> lock(group.mutex);
    mg_begin_driver_operation();
    GLES.glAttachShader(program, shader);
    if (!mg_end_driver_operation("glAttachShader")) return;
    auto& attached = group.programs[program].attached;
    if (std::find(attached.begin(), attached.end(), shader) == attached.end()) {
        attached.push_back(shader);
        const auto it = group.shaders.find(shader);
        if (it != group.shaders.end()) ++it->second.attachment_count;
    }
}

void glDetachShader(GLuint program, GLuint shader) {
    LOG()
    auto& group = mg_shader_objects();
    std::lock_guard<std::recursive_mutex> lock(group.mutex);
    mg_begin_driver_operation();
    GLES.glDetachShader(program, shader);
    if (!mg_end_driver_operation("glDetachShader")) return;
    const auto it = group.programs.find(program);
    if (it != group.programs.end()) {
        auto& attached = it->second.attached;
        const auto member = std::find(attached.begin(), attached.end(), shader);
        if (member != attached.end()) {
            attached.erase(member);
            mg_release_shader_attachment(group, shader);
        }
    }
}

void glDeleteProgram(GLuint program) {
    LOG()
    auto& group = mg_shader_objects();
    std::lock_guard<std::recursive_mutex> lock(group.mutex);
    mg_begin_driver_operation();
    GLES.glDeleteProgram(program);
    if (!mg_end_driver_operation("glDeleteProgram")) return;
    const auto it = group.programs.find(program);
    if (it != group.programs.end()) {
        it->second.deleted = true;
        group.pending_program_deletions.insert(program);
    }
    collect_deleted_programs(group);
}

void glBindFragDataLocation(GLuint program, GLuint color, const GLchar* name) {
    LOG()
    if (!name || !GLES.glIsProgram(program)) {
        mg_set_gl_error(GL_INVALID_OPERATION);
        return;
    }
    if (std::strncmp(name, "gl_", 3) == 0) {
        mg_set_gl_error(GL_INVALID_OPERATION);
        return;
    }
    GLint maxBuffers = 0;
    GLES.glGetIntegerv(GL_MAX_DRAW_BUFFERS, &maxBuffers);
    if (color >= static_cast<GLuint>(maxBuffers)) {
        mg_set_gl_error(GL_INVALID_VALUE);
        return;
    }
    auto& group = mg_shader_objects();
    std::lock_guard<std::recursive_mutex> lock(group.mutex);
    group.programs[program].outputs[name] = color;
}

static GLuint compile_link_shader(GLenum type, const std::string& source, std::string& failure) {
    const GLuint shader = GLES.glCreateShader(type);
    if (!shader) {
        failure = "Unable to allocate link shader";
        return 0;
    }
    const GLchar* text = source.data();
    const GLint length = static_cast<GLint>(source.size());
    GLES.glShaderSource(shader, 1, &text, &length);
    GLES.glCompileShader(shader);
    GLint status = 0;
    GLES.glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[4096]{};
        GLES.glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        failure = std::string("Link shader compile: ") + log;
    }
    return shader;
}

static void restore_source_bindings(GLuint program, const mg_glsl_metadata& metadata) {
    GLint previous = 0;
    GLES.glGetIntegerv(GL_CURRENT_PROGRAM, &previous);
    bool borrowed = false;
    for (const auto& binding : metadata.bindings) {
        if (binding.kind == "sampler" || binding.kind == "image") {
            const GLint location = GLES.glGetUniformLocation(program, binding.name.c_str());
            if (location < 0) continue;
            std::vector<GLint> units(binding.count);
            for (unsigned i = 0; i < binding.count; ++i)
                units[i] = binding.binding + i;
            if (GLES.glProgramUniform1iv)
                GLES.glProgramUniform1iv(program, location, units.size(), units.data());
            else {
                if (!borrowed) {
                    GLES.glUseProgram(program);
                    borrowed = true;
                }
                GLES.glUniform1iv(location, units.size(), units.data());
            }
        } else if (binding.kind == "ubo") {
            for (unsigned i = 0; i < binding.count; ++i) {
                const std::string name =
                    binding.count > 1 ? binding.name + "[" + std::to_string(i) + "]" : binding.name;
                const GLuint index = GLES.glGetUniformBlockIndex(program, name.c_str());
                if (index != GL_INVALID_INDEX) GLES.glUniformBlockBinding(program, index, binding.binding + i);
            }
        }
        // SSBO layout(binding) stays in ESSL. GLES has no
        // glShaderStorageBlockBinding API; removing that qualifier loses it.
    }
    if (borrowed) GLES.glUseProgram(static_cast<GLuint>(previous));
}

void glLinkProgram(GLuint program) {
    LOG()
    if (!GLES.glIsProgram(program)) {
        mg_set_gl_error(GL_INVALID_VALUE);
        return;
    }
    auto& group = mg_shader_objects();
    std::lock_guard<std::recursive_mutex> lock(group.mutex);
    auto& record = group.programs[program];
    record.failure.clear();
    mg_glsl_metadata linked;
    std::vector<std::pair<GLuint, GLuint>> replacements;
    GLuint keeper = 0;
    bool vertex = false, fragment = false;
    // Program-specific variants are temporary shader objects. Original shader
    // source, compile status and other programs' attachments remain untouched.
    for (const GLuint id : record.attached) {
        auto it = group.shaders.find(id);
        if (it == group.shaders.end()) continue;
        auto& shader = it->second;
        vertex |= shader.type == GL_VERTEX_SHADER;
        fragment |= shader.type == GL_FRAGMENT_SHADER;
        mg_glsl_metadata metadata = shader.compiled_metadata;
        if (!shader.failure.empty()) record.failure = shader.failure;
        if (shader.type == GL_FRAGMENT_SHADER && !record.outputs.empty() && shader.failure.empty() && shader.compiled) {
            std::string source, error;
            mg_shader_record compiled_source = shader;
            compiled_source.original = shader.compiled_original;
            if (!mg_shader_translate(compiled_source, &record.outputs, source, metadata, error)) {
                record.failure = error;
                source = "#version 300 es\n#error MobileGlues_link_translation_failed\n";
            }
            const GLuint clone = compile_link_shader(GL_FRAGMENT_SHADER, source, record.failure);
            if (clone) {
                // A shader may already be delete-pending. Keep a driver
                // reference while replacing its attachment for this link.
                if (!keeper) keeper = GLES.glCreateProgram();
                if (!keeper) {
                    GLES.glDeleteShader(clone);
                    record.failure = "Unable to retain link shader";
                    break;
                }
                GLES.glAttachShader(keeper, id);
                GLES.glDetachShader(program, id);
                GLES.glAttachShader(program, clone);
                replacements.emplace_back(id, clone);
            }
        }
        linked.bindings.insert(linked.bindings.end(), metadata.bindings.begin(), metadata.bindings.end());
        linked.buffer_samplers.insert(linked.buffer_samplers.end(), metadata.buffer_samplers.begin(),
                                      metadata.buffer_samplers.end());
    }
    GLuint defaultFS = 0;
    if (vertex && !fragment) {
        const std::string source =
            "#version 300 es\nprecision mediump float;\nout vec4 fragColor;\nvoid main(){fragColor=vec4(1.0);}\n";
        defaultFS = compile_link_shader(GL_FRAGMENT_SHADER, source, record.failure);
        if (defaultFS) GLES.glAttachShader(program, defaultFS);
    }
    GLint status = 0;
    if (record.failure.empty()) {
        GLES.glLinkProgram(program);
        GLES.glGetProgramiv(program, GL_LINK_STATUS, &status);
    }
    for (const auto& replacement : replacements) {
        GLES.glDetachShader(program, replacement.second);
        GLES.glAttachShader(program, replacement.first);
        GLES.glDetachShader(keeper, replacement.first);
        GLES.glDeleteShader(replacement.second);
    }
    if (keeper) GLES.glDeleteProgram(keeper);
    if (defaultFS) {
        GLES.glDetachShader(program, defaultFS);
        GLES.glDeleteShader(defaultFS);
    }
    if (status && record.failure.empty()) {
        // Failed relinks leave the old executable installed while current.
        // Keep its reflection until a successful replacement is available.
        ++record.link_generation;
        record.metadata = std::move(linked);
        restore_source_bindings(program, record.metadata);
    } else {
        if (record.failure.empty()) {
            char log[4096]{};
            GLES.glGetProgramInfoLog(program, sizeof(log), nullptr, log);
            record.failure = log;
            if (record.failure.empty()) record.failure = "GLES program link failed without an information log";
        }
        LOG_W_FORCE("[MG-PROGRAM-LINK] program=%u generation=%llu failed: %s", program,
                    static_cast<unsigned long long>(record.link_generation), record.failure.c_str())
    }
}

void glGetProgramiv(GLuint program, GLenum pname, GLint* params) {
    LOG()
    if (!params) {
        mg_set_gl_error(GL_INVALID_VALUE);
        return;
    }
    auto& group = mg_shader_objects();
    std::lock_guard<std::recursive_mutex> lock(group.mutex);
    const auto it = group.programs.find(program);
    if (it != group.programs.end() && !it->second.failure.empty()) {
        if (pname == GL_LINK_STATUS) {
            *params = GL_FALSE;
            return;
        }
        if (pname == GL_INFO_LOG_LENGTH) {
            *params = it->second.failure.size() + 1;
            return;
        }
    }
    GLES.glGetProgramiv(program, pname, params);
}

void glGetProgramInfoLog(GLuint program, GLsizei size, GLsizei* length, GLchar* log) {
    if (size < 0) {
        mg_set_gl_error(GL_INVALID_VALUE);
        return;
    }
    auto& group = mg_shader_objects();
    std::lock_guard<std::recursive_mutex> lock(group.mutex);
    const auto it = group.programs.find(program);
    if (it == group.programs.end() || it->second.failure.empty()) {
        GLES.glGetProgramInfoLog(program, size, length, log);
        return;
    }
    if (length) *length = 0;
    if (size > 0 && log) {
        const size_t n = std::min(it->second.failure.size(), static_cast<size_t>(size - 1));
        std::memcpy(log, it->second.failure.data(), n);
        log[n] = 0;
        if (length) *length = n;
    }
}

void glUseProgram(GLuint program) {
    LOG()
    auto& group = mg_shader_objects();
    std::lock_guard<std::recursive_mutex> lock(group.mutex);
    const auto it = group.programs.find(program);
    if (it != group.programs.end() && !it->second.failure.empty()) {
        mg_set_gl_error(GL_INVALID_OPERATION);
        return;
    }
    mg_begin_driver_operation();
    GLES.glUseProgram(program);
    if (mg_end_driver_operation("glUseProgram")) {
        gl_state->current_program = program;
        if (!group.pending_program_deletions.empty()) collect_deleted_programs(group);
    }
}

extern "C"
{
    GLAPI GLAPIENTRY void glDeleteProgramARB(GLuint) __attribute__((alias("glDeleteProgram")));
    GLAPI GLAPIENTRY void glDetachShaderARB(GLuint, GLuint) __attribute__((alias("glDetachShader")));
    GLAPI GLAPIENTRY void glGetProgramInfoLogARB(GLuint, GLsizei, GLsizei*, GLchar*)
        __attribute__((alias("glGetProgramInfoLog")));
}
void glGetActiveUniformName(GLuint program, GLuint uniformIndex, GLsizei bufSize, GLsizei* length,
                            GLchar* uniformName) {
    LOG()
    LOG_D("glGetActiveUniformName(program: %u, index: %u, bufSize: %d)", program, uniformIndex, bufSize)

    if (length) *length = 0;
    if (bufSize <= 0 || uniformName == nullptr) {
        // Nothing to write. Still forwarded when bufSize is negative so the driver
        // raises the GL_INVALID_VALUE the caller is owed.
        if (bufSize < 0) GLES.glGetActiveUniform(program, uniformIndex, bufSize, nullptr, nullptr, nullptr, nullptr);
        CHECK_GL_ERROR
        return;
    }

    // Same buffer contract in both calls: at most bufSize-1 characters plus the
    // terminator, and a length that excludes it. The size and type this also
    // returns are what glGetActiveUniformsiv is for; they are discarded here.
    GLint size = 0;
    GLenum type = 0;
    GLsizei written = 0;
    uniformName[0] = '\0';
    GLES.glGetActiveUniform(program, uniformIndex, bufSize, &written, &size, &type, uniformName);
    if (length) *length = written;

    LOG_D("  -> \"%s\"", uniformName)
    CHECK_GL_ERROR
}
