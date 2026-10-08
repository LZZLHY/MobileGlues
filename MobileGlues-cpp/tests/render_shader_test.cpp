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
#include <algorithm>

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
    std::vector<GLenum> recordedErrors;
    bool rejectUse = false, rejectLink = false;
    bool feedbackActive = false, feedbackPaused = false;
    int pauses = 0, resumes = 0;
    int driverContextVersion = 320;
    std::map<int, int> uniforms;
    std::map<int, std::string> initializerNames;
    std::map<std::string, int> initializerLocations;
    std::map<std::pair<GLuint, std::string>, std::vector<double>> initializerValues;
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
        if (feedbackActive && id == current) { driverError = GL_INVALID_OPERATION; return; }
        if (rejectLink) { p.linked = false; return; }
        ++p.link;
        // 真实成功link会把普通uniform重新初始化；拒绝link不碰旧executable的值。
        for (auto it = initializerValues.begin(); it != initializerValues.end();) {
            if (it->first.first == id) it = initializerValues.erase(it); else ++it;
        }
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
        if (rejectUse || (feedbackActive && !feedbackPaused && id != current) || (id && !programs[id].linked)) {
            driverError = GL_INVALID_OPERATION;
            return;
        }
        GLuint old = current;
        current = id;
        if (old != id && programs.count(old) && programs[old].deleted) programs.erase(old);
    }
    void getInt(GLenum p, GLint* v) {
        *v = p == GL_MAX_DRAW_BUFFERS ? 8 : p == GL_CURRENT_PROGRAM ? current :
             p == GL_TRANSFORM_FEEDBACK_ACTIVE ? feedbackActive : p == GL_TRANSFORM_FEEDBACK_PAUSED ? feedbackPaused : 0;
    }
    GLenum getError() {
        auto e = driverError;
        driverError = 0;
        return e;
    }
    const GLubyte* driverString(GLenum name) {
        assert(name == GL_VERSION);
        return reinterpret_cast<const GLubyte*>(driverContextVersion == 300 ? "OpenGL ES 3.0" : "OpenGL ES 3.2");
    }
    GLint location(GLuint p, const GLchar* name) {
        ++queries;
        if (!strcmp(name, "initial_inactive")) return -1;
        if (!std::strncmp(name, "initial_", 8)) {
            auto [entry, inserted] = initializerLocations.emplace(name, 1000 + initializerLocations.size() * 100);
            (void)inserted;
            const int result = entry->second + programs[p].link;
            initializerNames[result] = name;
            return result;
        }
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
        if (initializerNames.count(loc)) initializerValues[{current, initializerNames[loc]}] = {double(values[0])};
    }
    template<class T> void initializerSetter(GLint loc, GLsizei n, const T* values, unsigned components) {
        assert(n == 1 && initializerNames.count(loc));
        auto& result = initializerValues[{current, initializerNames[loc]}];
        result.assign(values, values + components);
    }
    void floats1(GLint l, GLsizei n, const GLfloat* v) { initializerSetter(l,n,v,1); }
    void floats2(GLint l, GLsizei n, const GLfloat* v) { initializerSetter(l,n,v,2); }
    void floats3(GLint l, GLsizei n, const GLfloat* v) { initializerSetter(l,n,v,3); }
    void floats4(GLint l, GLsizei n, const GLfloat* v) { initializerSetter(l,n,v,4); }
    void ints2(GLint l, GLsizei n, const GLint* v) { initializerSetter(l,n,v,2); }
    void ints3(GLint l, GLsizei n, const GLint* v) { initializerSetter(l,n,v,3); }
    void ints4(GLint l, GLsizei n, const GLint* v) { initializerSetter(l,n,v,4); }
    void uints1(GLint l, GLsizei n, const GLuint* v) { initializerSetter(l,n,v,1); }
    void uints2(GLint l, GLsizei n, const GLuint* v) { initializerSetter(l,n,v,2); }
    void uints3(GLint l, GLsizei n, const GLuint* v) { initializerSetter(l,n,v,3); }
    void uints4(GLint l, GLsizei n, const GLuint* v) { initializerSetter(l,n,v,4); }
    void matrix2(GLint l, GLsizei n, GLboolean transpose, const GLfloat* v) { assert(!transpose); initializerSetter(l,n,v,4); }
    void programUniform(GLuint program, GLint loc, GLsizei n, const GLint* values) {
        for (int i = 0; i < n; ++i) uniforms[loc + i] = values[i];
        if (initializerNames.count(loc)) initializerValues[{program, initializerNames[loc]}] = {double(values[0])};
    }
    void programFloats4(GLuint program, GLint loc, GLsizei n, const GLfloat* values) {
        assert(n == 1 && initializerNames.count(loc));
        initializerValues[{program, initializerNames[loc]}].assign(values, values + 4);
    }
    void pauseFeedback() { assert(feedbackActive && !feedbackPaused); feedbackPaused = true; ++pauses; }
    void resumeFeedback() { assert(feedbackActive && feedbackPaused); feedbackPaused = false; ++resumes; }
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
        GLES.glGetString = driverString;
        GLES.glGetUniformLocation = location;
        GLES.glUniform1i = uniform;
        GLES.glUniform1iv = uniforms1;
        GLES.glUniform1fv = floats1; GLES.glUniform2fv = floats2; GLES.glUniform3fv = floats3; GLES.glUniform4fv = floats4;
        GLES.glUniform2iv = ints2; GLES.glUniform3iv = ints3; GLES.glUniform4iv = ints4;
        GLES.glUniform1uiv = uints1; GLES.glUniform2uiv = uints2; GLES.glUniform3uiv = uints3; GLES.glUniform4uiv = uints4;
        GLES.glUniformMatrix2fv = matrix2;
        GLES.glProgramUniform1iv = programUniform;
        GLES.glProgramUniform4fv = programFloats4;
        GLES.glPauseTransformFeedback = pauseFeedback; GLES.glResumeTransformFeedback = resumeFeedback;
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
    if (std::find(recordedErrors.begin(), recordedErrors.end(), e) == recordedErrors.end()) recordedErrors.push_back(e);
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
    // Object lifetime work follows attachment/deletion events, not every draw.
    GLuint sharedShader = makeShader(GL_FRAGMENT_SHADER, fs);
    GLuint ownerA = glCreateProgram(), ownerB = glCreateProgram();
    glAttachShader(ownerA, sharedShader);
    glAttachShader(ownerB, sharedShader);
    assert(mg_shader_objects().shaders.at(sharedShader).attachment_count == 2);
    glDeleteShader(sharedShader);
    glDeleteProgram(ownerA);
    assert(mg_shader_objects().shaders.at(sharedShader).attachment_count == 1);
    glDeleteProgram(ownerB);
    assert(!mg_shader_objects().shaders.count(sharedShader));
    GLuint deferred = glCreateProgram();
    glAttachShader(deferred, v);
    glAttachShader(deferred, other);
    glLinkProgram(deferred);
    glUseProgram(deferred);
    glDeleteProgram(deferred);
    assert(mg_shader_objects().pending_program_deletions.count(deferred));
    glUseProgram(p);
    assert(!mg_shader_objects().pending_program_deletions.count(deferred));
    assert(!mg_shader_objects().programs.count(deferred));
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
    const auto linkedGeneration = record.link_generation;
    q = queries;
    rejectLink = true;
    glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    assert(!ok && record.link_generation == linkedGeneration);
    setupBufferTextureUniforms(p);
    assert(queries == q);
    rejectLink = false;
    // 固定声明的初值走真实parse/translate/cache/link TU；driver边界只记录typed setter的实际值。
    const char* initializedFS = R"(#version 430
struct Values { vec2 pair; int count; };
uniform vec4 initial_tint=vec4(1,0,0,1);
uniform mat2 initial_matrix=mat2(1,2,3,4);
uniform uint initial_unsigned[2]=uint[2](0xffffffffu,17u);
uniform bvec2 initial_boolean=bvec2(true,false);
uniform Values initial_struct=Values(vec2(0.25,0.5),-7);
uniform vec2 initial_inactive=vec2(9);
out vec4 color;
void main(){color=initial_tint;}
)";
    const GLuint initializedShader = makeShader(GL_FRAGMENT_SHADER, initializedFS);
    const GLuint initializedProgram = glCreateProgram();
    glAttachShader(initializedProgram, v); glAttachShader(initializedProgram, initializedShader);
    glLinkProgram(p);
    glUseProgram(p);
    const GLuint priorProgram = current;
    frontendError = GL_INVALID_VALUE;
    glLinkProgram(initializedProgram);
    glGetProgramiv(initializedProgram, GL_LINK_STATUS, &ok);
    assert(ok && current == priorProgram && gl_state->current_program == priorProgram && frontendError == GL_INVALID_VALUE);
    const auto actual = [&](const char* name) -> const std::vector<double>& { return initializerValues[{initializedProgram,name}]; };
    assert(actual("initial_tint") == (std::vector<double>{1,0,0,1}));
    assert(actual("initial_matrix") == (std::vector<double>{1,2,3,4}));
    assert(actual("initial_unsigned[0]") == (std::vector<double>{4294967295.0}));
    assert(actual("initial_unsigned[1]") == (std::vector<double>{17}));
    assert(actual("initial_boolean") == (std::vector<double>{1,0}));
    assert(actual("initial_struct.pair") == (std::vector<double>{0.25,0.5}));
    assert(actual("initial_struct.count") == (std::vector<double>{-7}));
    assert(!initializerValues.count({initializedProgram,"initial_inactive"}));
    // 新serialized schema包含零项证明；单行/整块丢失、截断和重复不能退回无初值成功。
    mg_glsl_metadata parsed;
    mg_read_translation_metadata("//MG_INIT_BEGIN 0\n//MG_INIT_END 0\n", parsed);
    assert(parsed.initializer_failure.empty() && parsed.initializers.empty());
    const std::string oneInitializer = "//MG_INIT f 1 4 tint 4 1065353216 0 0 1065353216\n";
    mg_read_translation_metadata("//MG_INIT_BEGIN 1\n" + oneInitializer + "//MG_INIT_END 1\n", parsed);
    assert(parsed.initializer_failure.empty() && parsed.initializers.size() == 1);
    for (const std::string& corrupt : std::vector<std::string>{
             "//MG_INIT_BEGIN 1\n//MG_INIT_END 1\n", "#version 320 es\nvoid main(){}\n",
             "//MG_INIT_BEGIN 1\n" + oneInitializer,
             "//MG_INIT_BEGIN 2\n" + oneInitializer + oneInitializer + "//MG_INIT_END 2\n",
             "//MG_INIT_BEGIN 0\n//MG_INIT_END 0\n//MG_INIT_END 0\n",
             "//MG_INIT_BEGIN 1\n//MG_INIT i 1 1 tint 1 -1\n//MG_INIT_END 1\n",
             "//MG_INIT_BEGIN 1\n//MG_INIT i 1 1 tint 1 4294967296\n//MG_INIT_END 1\n"}) {
        mg_read_translation_metadata(corrupt, parsed); assert(!parsed.initializer_failure.empty());
    }
    // 成功relink重置声明初值；失败relink不重置当前旧executable或其代次/值。
    initializerValues[{initializedProgram,"initial_tint"}] = {0,1,0,1};
    frontendError = 0; driverError = GL_INVALID_ENUM;
    glLinkProgram(initializedProgram);
    assert(frontendError == GL_INVALID_ENUM && driverError == 0 && actual("initial_tint") == (std::vector<double>{1,0,0,1}));
    glUseProgram(initializedProgram);
    initializerValues[{initializedProgram,"initial_tint"}] = {0,0,1,1};
    const auto initializerGeneration = mg_shader_objects().programs[initializedProgram].link_generation;
    rejectLink = true; glLinkProgram(initializedProgram); rejectLink = false;
    glGetProgramiv(initializedProgram, GL_LINK_STATUS, &ok);
    assert(!ok && current == initializedProgram && actual("initial_tint") == (std::vector<double>{0,0,1,1}));
    assert(mg_shader_objects().programs[initializedProgram].link_generation == initializerGeneration);
    // 同名跨stage冲突在driverlink前拒绝，避免已经丢初值的ES源码误成功。
    const char* conflictVS = "#version 430\nuniform vec4 initial_tint=vec4(0,1,0,1);void main(){gl_Position=initial_tint;}";
    const GLuint conflictVertex = makeShader(GL_VERTEX_SHADER, conflictVS);
    glDetachShader(initializedProgram, v); glAttachShader(initializedProgram, conflictVertex);
    const auto beforeConflict = programs[initializedProgram].link;
    glLinkProgram(initializedProgram); glGetProgramiv(initializedProgram, GL_LINK_STATUS, &ok);
    assert(!ok && programs[initializedProgram].link == beforeConflict && actual("initial_tint") == (std::vector<double>{0,0,1,1}));
    glUseProgram(p);
    glDeleteProgram(initializedProgram); glDeleteShader(initializedShader); glDeleteShader(conflictVertex);
    frontendError = 0;
    // 真实driver反例已证明活动TF不允许临时UseProgram。3.1+用ProgramUniform；3.0回退只暂停本次捕获。
    const char* feedbackFS = "#version 330\nuniform vec4 initial_tint=vec4(1,0,0,1);out vec4 color;void main(){color=initial_tint;}";
    const GLuint feedbackShader = makeShader(GL_FRAGMENT_SHADER, feedbackFS), feedbackProgram = glCreateProgram();
    glAttachShader(feedbackProgram, v); glAttachShader(feedbackProgram, feedbackShader);
    feedbackActive = true; feedbackPaused = false;
    int previousUses = useCalls;
    glLinkProgram(feedbackProgram); glGetProgramiv(feedbackProgram, GL_LINK_STATUS, &ok);
    assert(ok && frontendError == 0 && current == p && useCalls == previousUses && !feedbackPaused);
    assert((initializerValues[{feedbackProgram,"initial_tint"}] == std::vector<double>{1,0,0,1}));
    driverContextVersion = 300; // bootstrap仍为320，当前API才决定DSA资格。
    glLinkProgram(feedbackProgram); glGetProgramiv(feedbackProgram, GL_LINK_STATUS, &ok);
    assert(ok && frontendError == 0 && current == p && feedbackActive && !feedbackPaused && pauses == 1 && resumes == 1);
    assert((initializerValues[{feedbackProgram,"initial_tint"}] == std::vector<double>{1,0,0,1}));
    feedbackPaused = true;
    glLinkProgram(feedbackProgram);
    assert(current == p && feedbackPaused && pauses == 1 && resumes == 1);
    feedbackActive = false; feedbackPaused = false; driverContextVersion = 320;
    glUseProgram(feedbackProgram);
    initializerValues[{feedbackProgram,"initial_tint"}] = {0,0,1,1};
    const auto feedbackGeneration = mg_shader_objects().programs[feedbackProgram].link_generation;
    const auto feedbackDriverGeneration = programs[feedbackProgram].link;
    feedbackActive = true;
    for (bool paused : {false,true}) {
        feedbackPaused = paused; frontendError = 0; recordedErrors.clear();
        glLinkProgram(feedbackProgram); glGetProgramiv(feedbackProgram, GL_LINK_STATUS, &ok);
        assert(ok && frontendError == GL_INVALID_OPERATION && current == feedbackProgram);
        assert(mg_shader_objects().programs[feedbackProgram].link_generation == feedbackGeneration && programs[feedbackProgram].link == feedbackDriverGeneration);
        assert((initializerValues[{feedbackProgram,"initial_tint"}] == std::vector<double>{0,0,1,1}));
        frontendError = 0; recordedErrors.clear(); driverError = GL_INVALID_ENUM;
        glLinkProgram(feedbackProgram);
        assert((recordedErrors == std::vector<GLenum>{GL_INVALID_ENUM,GL_INVALID_OPERATION}));
        assert(mg_shader_objects().programs[feedbackProgram].link_generation == feedbackGeneration);
    }
    feedbackActive = false; feedbackPaused = false; frontendError = 0; glUseProgram(p);
    glDeleteProgram(feedbackProgram); glDeleteShader(feedbackShader);
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
