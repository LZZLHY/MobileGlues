// MobileGlues - gl/shader.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header
#ifndef MOBILEGLUES_SHADER_H
#define MOBILEGLUES_SHADER_H

#include <GL/gl.h>
#include <string>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include "glsl/translation_state.h"

struct mg_shader_record {
    GLenum type = 0;
    std::uint64_t generation = 0;
    std::string original, converted, failure;
    mg_glsl_metadata metadata;
    std::string compiled_original;
    mg_glsl_metadata compiled_metadata;
    bool compiled = false;
    bool deleted = false;
};
struct mg_program_record {
    std::uint64_t generation = 0, link_generation = 0;
    std::vector<GLuint> attached;
    mg_frag_bindings outputs;
    mg_glsl_metadata metadata;
    std::string failure;
    std::uint64_t sampler_cache_generation = ~std::uint64_t{0};
    GLint buffer_width_location = -1, buffer_height_location = -1;
    std::vector<GLint> sampler_locations;
    bool deleted = false;
};
struct mg_shader_group {
    std::recursive_mutex mutex;
    std::uint64_t generation = 1;
    std::map<GLuint, mg_shader_record> shaders;
    std::map<GLuint, mg_program_record> programs;
};
mg_shader_group& mg_shader_objects();
bool mg_shader_translate(mg_shader_record& shader, const mg_frag_bindings* outputs, std::string& source,
                         mg_glsl_metadata& metadata, std::string& error);
void mg_collect_deleted_shaders(mg_shader_group& group);

#ifdef __cplusplus
extern "C"
{
#endif

    GLAPI GLAPIENTRY void glShaderSource(GLuint shader, GLsizei count, const GLchar* const* string,
                                         const GLint* length);

    GLAPI GLAPIENTRY void glGetShaderiv(GLuint shader, GLenum pname, GLint* params);
    GLAPI GLAPIENTRY void glCompileShader(GLuint shader);
    GLAPI GLAPIENTRY void glGetShaderInfoLog(GLuint shader, GLsizei size, GLsizei* length, GLchar* log);
    GLAPI GLAPIENTRY void glGetShaderSource(GLuint shader, GLsizei size, GLsizei* length, GLchar* source);
    GLAPI GLAPIENTRY void glDeleteShader(GLuint shader);

#ifdef __cplusplus
}
#endif

#endif // FOLD_CRAFT_LAUNCHER_GL_LOADER_H
