// Production shader/program/translator/cache with explicit GLES state doubles.
#include "gl/shader.h"
#include "gl/program.h"
#include "gl/drawing.h"
#include "gl/texture.h"
#include "gl/glsl/cache.h"
#include "egl/context.h"
#include "config/settings.h"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <thread>
#include <fstream>
#include <set>

gles_func_t g_gles_func{};
gles_caps_t g_gles_caps{};
global_settings_t global_settings{};
gl_state_s g_default_gl_state{};
thread_local gl_state_t gl_state = &g_default_gl_state;
thread_local MGContext* g_current_ctx = nullptr;
hardware_s hw{320, false};
hardware_t hardware = &hw;
char* glsl_cache_file_path = nullptr;
thread_local bool fsrInitialized = false;
void InitFSRResources() {}
extern "C" void write_log(const char*, ...) {}
int __android_log_print(int, const char*, const char*, ...) {
    return 0;
}
int mg_driver_active_texture_unit() {
    return 0;
}
bool mg_driver_texture_binding_at_unit(int, GLenum, GLuint* value) {
    *value = 99;
    return true;
}
TextureObject texture;
TextureObject* mgGetTexObjectByID(unsigned) {
    return &texture;
}

namespace {
    struct Shader {
        GLenum type;
        std::string source;
        bool compiled = false;
        bool deleted = false;
    };
    struct Program {
        std::set<GLuint> attached;
        bool linked = false, deleted = false;
        unsigned link = 0;
        std::map<std::string, std::string> linkedSources;
    };
    std::map<GLuint, Shader> shaders;
    std::map<GLuint, Program> programs;
    GLuint nextName = 1, current = 0;
    int submissions = 0, lastCount = 0, useCalls = 0, queries = 0;
    GLenum driverError = 0, frontendError = 0;
    bool rejectUse = false;
    std::map<int, int> uniforms;
    GLuint createShader(GLenum type) {
        GLuint id = nextName++;
        shaders.emplace(id, Shader{type, {}});
        return id;
    }
    GLuint createProgram() {
        GLuint id = nextName++;
        programs[id] = {};
        return id;
    }
    GLboolean isShader(GLuint id) {
        return shaders.count(id);
    }
    GLboolean isProgram(GLuint id) {
        return programs.count(id);
    }
    void source(GLuint id, GLsizei n, const GLchar* const* src, const GLint* len) {
        ++submissions;
        lastCount = n;
        auto& s = shaders[id];
        s.source.clear();
        for (int i = 0; i < n; ++i)
            s.source.append(src[i], len && len[i] >= 0 ? len[i] : std::strlen(src[i]));
    }
    void compile(GLuint id) {
        shaders[id].compiled = !shaders[id].source.empty() && shaders[id].source.find("#error") == std::string::npos;
    }
    void shaderiv(GLuint id, GLenum p, GLint* v) {
        auto& s = shaders[id];
        *v = p == GL_SHADER_TYPE ? s.type : p == GL_COMPILE_STATUS ? s.compiled : p == GL_INFO_LOG_LENGTH ? 5 : 0;
    }
    void shaderlog(GLuint, GLsizei n, GLsizei*, GLchar* out) {
        if (n > 0) out[0] = 0;
    }
    void shadersource(GLuint id, GLsizei n, GLsizei*, GLchar* out) {
        if (n > 0) std::snprintf(out, n, "%s", shaders[id].source.c_str());
    }
    void attach(GLuint p, GLuint s) {
        if (!programs[p].attached.insert(s).second) driverError = GL_INVALID_OPERATION;
    }
    void detach(GLuint p, GLuint s) {
        if (!programs[p].attached.erase(s)) driverError = GL_INVALID_OPERATION;
        bool referenced = false;
        for (const auto& entry : programs)
            referenced |= entry.second.attached.count(s) != 0;
        if (!referenced && shaders.count(s) && shaders[s].deleted) shaders.erase(s);
    }
    void deleteShader(GLuint id) {
        bool attached = false;
        for (auto& p : programs)
            attached |= p.second.attached.count(id) != 0;
        if (attached)
            shaders[id].deleted = true;
        else
            shaders.erase(id);
    }
    void deleteProgram(GLuint id) {
        if (id == current)
            programs[id].deleted = true;
        else
            programs.erase(id);
    }
    void link(GLuint id) {
        auto& p = programs[id];
        ++p.link;
        p.linked = true;
        p.linkedSources.clear();
        for (GLuint s : p.attached) {
            p.linked &= shaders[s].compiled;
            p.linkedSources[std::to_string(shaders[s].type)] = shaders[s].source;
        }
    }
    void programiv(GLuint id, GLenum pname, GLint* v) {
        *v = pname == GL_ACTIVE_UNIFORMS ? 2 : pname == GL_INFO_LOG_LENGTH ? 5 : programs[id].linked;
    }
    void programlog(GLuint, GLsizei n, GLsizei*, GLchar* out) {
        if (n > 0) out[0] = 0;
    }
    void use(GLuint id) {
        ++useCalls;
        if (rejectUse || (id && !programs[id].linked)) {
            driverError = GL_INVALID_OPERATION;
            return;
        }
        GLuint old = current;
        current = id;
        if (old != id && programs.count(old) && programs[old].deleted) programs.erase(old);
    }
    void getInt(GLenum p, GLint* v) {
        *v = p == GL_MAX_DRAW_BUFFERS ? 8 : p == GL_CURRENT_PROGRAM ? current : 0;
    }
    GLenum getError() {
        auto e = driverError;
        driverError = 0;
        return e;
    }
    GLint location(GLuint p, const GLchar* name) {
        ++queries;
        int base = !strcmp(name, "atlas")               ? 3
                   : !strcmp(name, "bufferData")        ? 4
                   : !strcmp(name, "u_BufferTexWidth")  ? 1
                   : !strcmp(name, "u_BufferTexHeight") ? 2
                                                        : 5;
        return base + programs[p].link * 10;
    }
    void uniform(GLint loc, GLint value) {
        uniforms[loc] = value;
    }
    void uniforms1(GLint loc, GLsizei n, const GLint* values) {
        for (int i = 0; i < n; ++i)
            uniforms[loc + i] = values[i];
    }
    void programUniform(GLuint, GLint loc, GLsizei n, const GLint* values) {
        uniforms1(loc, n, values);
    }
    GLuint blockIndex(GLuint, const char*) {
        return GL_INVALID_INDEX;
    }
    void blockBinding(GLuint, GLuint, GLuint) {}
    void activeUniform(GLuint, GLuint i, GLsizei n, GLsizei*, GLint* size, GLenum* type, GLchar* name) {
        *size = 1;
        *type = i ? GL_INT_SAMPLER_2D : GL_SAMPLER_2D;
        std::snprintf(name, n, "%s", i ? "bufferData" : "atlas");
    }
    void activeTexture(GLenum) {}
    void install() {
        GLES.glCreateShader = createShader;
        GLES.glCreateProgram = createProgram;
        GLES.glIsShader = isShader;
        GLES.glIsProgram = isProgram;
        GLES.glShaderSource = source;
        GLES.glCompileShader = compile;
        GLES.glGetShaderiv = shaderiv;
        GLES.glGetShaderInfoLog = shaderlog;
        GLES.glGetShaderSource = shadersource;
        GLES.glAttachShader = attach;
        GLES.glDetachShader = detach;
        GLES.glDeleteShader = deleteShader;
        GLES.glDeleteProgram = deleteProgram;
        GLES.glLinkProgram = link;
        GLES.glGetProgramiv = programiv;
        GLES.glGetProgramInfoLog = programlog;
        GLES.glUseProgram = use;
        GLES.glGetIntegerv = getInt;
        GLES.glGetError = getError;
        GLES.glGetUniformLocation = location;
        GLES.glUniform1i = uniform;
        GLES.glUniform1iv = uniforms1;
        GLES.glProgramUniform1iv = programUniform;
        GLES.glGetUniformBlockIndex = blockIndex;
        GLES.glUniformBlockBinding = blockBinding;
        GLES.glGetActiveUniform = activeUniform;
        GLES.glActiveTexture = activeTexture;
    }
    GLuint makeShader(GLenum type, const char* code) {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &code, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        assert(ok);
        return s;
    }
} // namespace
void mg_set_gl_error(GLenum e) {
    if (!frontendError) frontendError = e;
}
void mg_begin_driver_operation() {
    GLenum e = getError();
    if (e) mg_set_gl_error(e);
}
bool mg_end_driver_operation(const char*) {
    GLenum e = getError();
    if (e) mg_set_gl_error(e);
    return e == 0;
}

int main() {
    install();
    global_settings.max_glsl_cache_size = 1024 * 1024;
    global_settings.fsr1_setting = FSR1_Quality_Preset::Disabled;
    g_gles_caps.GL_EXT_texture_query_lod = 1;
    texture.width = 64;
    texture.height = 32;
    const char* vs = "#version 330\nvoid main(){gl_Position=vec4(0.0);}\n";
    const char* fs = "#version 330\nout vec4 color;void main(){color=vec4(1.0);}\n";
    GLuint s = glCreateShader(GL_VERTEX_SHADER);
    const char* parts[] = {"#version 330\n", "void main(){gl_Position=vec4(0.0);}\n"};
    GLint lens[] = {-1, static_cast<GLint>(std::strlen(parts[1]))};
    glShaderSource(s, 2, parts, lens);
    assert(lastCount == 1 && shaders[s].source.find("#version 320 es") != std::string::npos);
    glCompileShader(s);
    const char* bad = "#version 460\nthis is not valid GLSL";
    glShaderSource(s, 1, &bad, nullptr);
    glCompileShader(s);
    GLint ok = 1;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    assert(!ok);
    char log[4096]{};
    glGetShaderInfoLog(s, sizeof(log), nullptr, log);
    assert(std::strlen(log) > 0);
    assert(shaders[s].source.find("#version 460") == std::string::npos);
    const char* empty = "";
    glShaderSource(s, 1, &empty, nullptr);
    assert(shaders[s].source.empty());
    const int before = submissions;
    glShaderSource(s, -1, nullptr, nullptr);
    assert(submissions == before);
    GLuint v = makeShader(GL_VERTEX_SHADER, vs), f = makeShader(GL_FRAGMENT_SHADER, fs),
           other = makeShader(GL_FRAGMENT_SHADER, fs);
    GLuint p = glCreateProgram(), p2 = glCreateProgram();
    glBindFragDataLocation(p, 3, "color");
    glAttachShader(p, v);
    glAttachShader(p, f);
    glAttachShader(p2, other);
    const std::string original = shaders[f].source;
    glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    assert(ok);
    assert(shaders[f].source == original && programs[p].attached.count(f) && !programs[p].attached.count(other));
    assert(programs[p].linkedSources[std::to_string(GL_FRAGMENT_SHADER)].find("location = 3") != std::string::npos);
    const char* explicitFS = "#version 430\n#define OUTPUT_SLOT 0\nlayout(location=OUTPUT_SLOT) out vec4 color;void "
                             "main(){color=vec4(1.0);}";
    glShaderSource(f, 1, &explicitFS, nullptr);
    glCompileShader(f);
    glLinkProgram(p);
    assert(programs[p].linkedSources[std::to_string(GL_FRAGMENT_SHADER)].find("location = 0") != std::string::npos);
    glDeleteShader(f);
    glLinkProgram(p);
    assert(shaders.count(f) && programs[p].attached.count(f));
    const char* directES =
        "#version 300 es\nprecision highp float;layout(location=0) out vec4 color;void main(){color=vec4(1.0);}";
    glShaderSource(f, 1, &directES, nullptr);
    glCompileShader(f);
    glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    assert(ok);
    rejectUse = true;
    int uses = useCalls;
    glUseProgram(p);
    assert(current == 0 && gl_state->current_program == 0);
    rejectUse = false;
    glUseProgram(p);
    assert(current == p && useCalls == uses + 2);
    global_settings.ignore_error = IgnoreErrorLevel::Full;
    programs[p2].linked = false;
    glGetProgramiv(p2, GL_LINK_STATUS, &ok);
    assert(!ok);
    int rc = 0;
    auto bound = GLSLtoGLSLES("#version 430\nlayout(binding=4) uniform sampler2D tex;layout(binding=3,std140) uniform "
                              "Block{vec4 tint;};out vec4 color;void main(){color=texture(tex,vec2(0))*tint;}",
                              GL_FRAGMENT_SHADER, 320, 430, rc);
    assert(rc == 0 && !bound.empty());
    mg_glsl_metadata metadata;
    mg_read_translation_metadata(bound, metadata);
    bool sampler = false, ubo = false;
    for (auto& b : metadata.bindings) {
        sampler |= b.kind == "sampler" && b.binding == 4;
        ubo |= b.kind == "ubo" && b.binding == 3;
    }
    assert(sampler && ubo && bound.find("binding = 4") != std::string::npos);
    hw.emulate_texture_buffer = true;
    auto bufferShader =
        GLSLtoGLSLES("#version 330\nuniform isamplerBuffer bufferData;uniform sampler2D atlas;out vec4 color;void "
                     "main(){color=texture(atlas,vec2(0.0))+vec4(texelFetch(bufferData,0));}",
                     GL_FRAGMENT_SHADER, 310, 330, rc);
    if (rc < 0) std::fprintf(stderr, "buffer shader: %s\n", mg_translation_error().c_str());
    assert(rc == 0);
    mg_read_translation_metadata(bufferShader, metadata);
    assert(metadata.buffer_samplers.size() == 1 && metadata.buffer_samplers[0] == "bufferData");
    hw.emulate_texture_buffer = false;
    auto vert = GLSLtoGLSLES(vs, GL_VERTEX_SHADER, 320, 330, rc);
    assert(rc == 0 && !vert.empty());
    auto frag = GLSLtoGLSLES(vs, GL_FRAGMENT_SHADER, 320, 330, rc);
    assert(rc < 0 && frag.empty());
    // Populate the program's translated interface exactly as a real buffer-sampler
    // translation does, while fake driver reflection also exposes an ordinary atlas.
    auto& record = mg_shader_objects().programs[p];
    record.metadata.buffer_samplers = {"bufferData"};
    ++record.link_generation;
    uniforms[location(p, "atlas")] = 2;
    setupBufferTextureUniforms(p);
    assert(uniforms[location(p, "atlas")] == 2 && uniforms[location(p, "bufferData")] == 15);
    glLinkProgram(p);
    record.metadata.buffer_samplers = {"bufferData"};
    int q = queries;
    setupBufferTextureUniforms(p);
    assert(queries > q);
    // Two non-shared contexts may use exactly the same GLuint names.
    MGContext a{}, b{};
    a.share_group = std::make_shared<MGShareGroup>();
    b.share_group = std::make_shared<MGShareGroup>();
    a.share_group->shader_objects = std::make_shared<mg_shader_group>();
    b.share_group->shader_objects = std::make_shared<mg_shader_group>();
    g_current_ctx = &a;
    mg_shader_objects().shaders[7].original = "A";
    g_current_ctx = &b;
    assert(mg_shader_objects().shaders.count(7) == 0);
    g_current_ctx = &a;
    assert(mg_shader_objects().shaders[7].original == "A");
    g_current_ctx = nullptr;
    Cache cache;
    auto work = [&](int thread) {
        for (int i = 0; i < 1000; ++i) {
            auto key = std::to_string(thread) + ":" + std::to_string(i);
            cache.put(key.c_str(), "essl");
            std::string value;
            assert(cache.get(key.c_str(), value) && value == "essl");
        }
    };
    std::thread one(work, 1), two(work, 2);
    one.join();
    two.join();
    std::string copy;
    cache.put("stable", "retained");
    assert(cache.get("stable", copy));
    cache.put("stable", "replacement");
    assert(copy == "retained");
    char path[] = "/tmp/mg-render-invalid-cache";
    glsl_cache_file_path = path;
    {
        std::ofstream file(path, std::ios::binary);
        size_t count = 1, size = 1024;
        char hash[32]{};
        file.write((char*)&count, sizeof(count));
        file.write(hash, 32);
        file.write((char*)&size, sizeof(size));
    }
    assert(!cache.load());
    assert(cache.get("stable", copy) && copy == "replacement");
    glsl_cache_file_path = nullptr;
    puts("shader/program/real-translator/cache contracts passed");
}
