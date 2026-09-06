// Exercise the production cache/command/upload paths across live contexts.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <map>
#include <thread>
static thread_local bool count_heap = false;
static thread_local unsigned heap_allocations = 0;
void* operator new(std::size_t n) {
    if (count_heap) ++heap_allocations;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

// Include the real implementations to reach command-buffer preparation without
// introducing a public testing API. Unused driver paths are linker-collected.
#include "gl/multidraw.cpp"
#include "gl/restart.cpp"
#include "gl/drawing.cpp"

gles_func_t g_gles_func{};
gles_caps_t g_gles_caps{};
global_settings_t global_settings{};
hardware_s hw{310, false};
hardware_t hardware = &hw;
gl_state_s g_default_gl_state{};
thread_local gl_state_t gl_state = &g_default_gl_state;
thread_local MGContext* g_current_ctx = nullptr;
void* gles = nullptr;
int __android_log_print(int, const char*, const char*, ...) { return 0; }
extern "C" void write_log(const char*, ...) {}
static mg_shader_group shader_group;
mg_shader_group& mg_shader_objects() { return shader_group; }
TextureObject* mgGetTexObjectByID(GLuint) { return nullptr; }
bool mg_driver_texture_binding_at_unit(GLint, GLenum, GLuint*) { return false; }
int mg_driver_active_texture_unit() { return 0; }
mg_enable_state_t* mg_enable_state() { return &g_current_ctx->enable; }
GLboolean mg_enable_get(GLenum, GLuint) { return GL_FALSE; }
bool mg_primitive_restart_enabled() { return false; }
GLuint mg_primitive_restart_index_for(GLenum) { return g_current_ctx->enable.primitive_restart_index; }
md_backend_t md_next_backend(md_entry_t, md_backend_t) {
    assert(false && "unexpected fallback in the resource-cache fixture");
    return md_backend_t{};
}

namespace {
GLuint next_buffer = 1000;
unsigned generated = 0;
thread_local GLuint indirect_binding = 0, element_binding = 0;
thread_local bool application_has_ibo = true;
thread_local bool array_commands = false;
std::map<GLuint, GLsizeiptr> sizes;
std::map<GLuint, unsigned long long> owners;
GLsizeiptr last_upload = 0;
void gen(GLsizei n, GLuint* ids) {
    for (GLsizei i = 0; i < n; ++i) {
        ids[i] = next_buffer++;
        owners[ids[i]] = g_current_ctx->id;
        ++generated;
    }
}
void bind(GLenum target, GLuint id) {
    if (id >= 1000) assert(owners.at(id) == g_current_ctx->id);
    (target == GL_DRAW_INDIRECT_BUFFER ? indirect_binding : element_binding) = id;
}
void data(GLenum target, GLsizeiptr bytes, const void*, GLenum) {
    sizes[target == GL_DRAW_INDIRECT_BUFFER ? indirect_binding : element_binding] = bytes;
    last_upload = bytes;
}
void subdata(GLenum, GLintptr, GLsizeiptr bytes, const void* p) {
    if (array_commands) {
        assert(bytes > 0 && bytes % sizeof(draw_arrays_indirect_command_t) == 0);
        const auto* cmds = static_cast<const draw_arrays_indirect_command_t*>(p);
        for (size_t i = 0; i < bytes / sizeof(*cmds); ++i)
            assert(cmds[i].count == 3 && cmds[i].first == 0 && cmds[i].baseInstanceOrReserved == 0);
    } else {
        assert(bytes > 0 && bytes % sizeof(draw_elements_indirect_command_t) == 0);
        const auto* cmds = static_cast<const draw_elements_indirect_command_t*>(p);
        for (size_t i = 0; i < bytes / sizeof(*cmds); ++i)
            assert(cmds[i].count == 3 && cmds[i].firstIndex == 0 && cmds[i].baseVertex == 0);
    }
}
void getsize(GLenum target, GLenum, GLint* result) {
    if (target == GL_ELEMENT_ARRAY_BUFFER && application_has_ibo) { *result = 12; return; }
    *result = static_cast<GLint>(sizes.at(target == GL_DRAW_INDIRECT_BUFFER ? indirect_binding : element_binding));
}
void draw(GLenum, GLsizei count, GLenum type, const void*) {
    assert(count > 0 && type == GL_UNSIGNED_INT && last_upload == count * sizeof(GLuint));
}
void drawInstanced(GLenum mode, GLsizei count, GLenum type, const void* p, GLsizei) { draw(mode,count,type,p); }
void cap(GLenum) {}
GLenum error() { return GL_NO_ERROR; }
int failures = 0;
void expect_zero(const char* label, unsigned n) {
    std::printf("%s: extra=%u\n", label, n);
    if (n) ++failures;
}
void indirect(GLsizei n = 1) {
    const GLsizei count[16] = {3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3};
    const void* indices[16]{};
    const GLint first[16]{};
    GLuint previous = 0;
    std::size_t slot = 0;
    multidraw_check_context();
    if (array_commands)
        assert(prepare_arrays_indirect_buffer(first, count, n, &previous, &slot));
    else
        assert(prepare_indirect_buffer(count, GL_UNSIGNED_INT, indices, n, nullptr, &previous, &slot));
    bind(GL_DRAW_INDIRECT_BUFFER, previous);
}
} // namespace
GLuint mg_driver_bound_buffer(GLenum target) {
    return target == GL_ELEMENT_ARRAY_BUFFER ? (application_has_ibo ? 1 : 0) : indirect_binding;
}

int main() {
    GLES.glGenBuffers = gen;
    GLES.glBindBuffer = bind;
    GLES.glBufferData = data;
    GLES.glBufferSubData = subdata;
    GLES.glGetBufferParameteriv = getsize;
    GLES.glGetError = error;
    GLES.glEnable = cap;
    GLES.glDisable = cap;
    GLES.glDrawElements = draw;
    GLES.glDrawElementsInstanced = drawInstanced;
    MGContext contexts[2]{};
    contexts[0].id = 101;
    contexts[1].id = 202;
    for (unsigned i = 0; i < 16; ++i) { g_current_ctx = &contexts[i % 2]; indirect(); }
    unsigned before = generated;
    for (unsigned i = 0; i < 128; ++i) { g_current_ctx = &contexts[i % 2]; indirect(); }
    expect_zero("multidraw context switching", generated - before);

    application_has_ibo = false;
    const GLuint indices[] = {0, 1, 2};
    auto restart = [&] { assert(mg_draw_elements_restart(GL_TRIANGLES, 3, GL_UNSIGNED_INT, indices, 0, -1)); };
    for (unsigned i = 0; i < 2; ++i) { g_current_ctx = &contexts[i]; restart(); }
    before = generated;
    for (unsigned i = 0; i < 128; ++i) { g_current_ctx = &contexts[i % 2]; restart(); }
    expect_zero("restart context switching", generated - before);

    auto basevertex = [&] { glDrawElementsBaseVertex(GL_TRIANGLES, 3, GL_UNSIGNED_INT, indices, 1); };
    for (unsigned i = 0; i < 2; ++i) { g_current_ctx = &contexts[i]; basevertex(); }
    before = generated;
    for (unsigned i = 0; i < 128; ++i) { g_current_ctx = &contexts[i % 2]; basevertex(); }
    expect_zero("basevertex context switching", generated - before);
    const GLuint larger_indices[12]{};
    glDrawElementsBaseVertex(GL_TRIANGLES, 12, GL_UNSIGNED_INT, larger_indices, 1);
    count_heap = true;
    for (unsigned i = 0; i < 128; ++i) basevertex();
    count_heap = false;
    expect_zero("basevertex steady heap allocations", heap_allocations);
    assert(last_upload == 3 * sizeof(GLuint));

    // An EGL context can migrate to a different thread while retaining objects.
    before = generated;
    std::thread migrated([&] { g_current_ctx = &contexts[0]; indirect(); });
    migrated.join();
    expect_zero("context migration", generated - before);
    application_has_ibo = true;
    for (bool arrays : {false, true}) {
        array_commands = arrays;
        for (unsigned i = 0; i < 16; ++i) { g_current_ctx = &contexts[i % 2]; indirect(16); }
        before = generated;
        heap_allocations = 0;
        count_heap = true;
        for (unsigned i = 0; i < 128; ++i) { g_current_ctx = &contexts[i % 2]; indirect(i % 2 ? 16 : 3); }
        count_heap = false;
        expect_zero(arrays ? "arrays command-buffer reuse" : "elements command-buffer reuse", generated - before);
        expect_zero(arrays ? "arrays command staging" : "elements command staging", heap_allocations);
    }
    array_commands = false;
    application_has_ibo = true;
    g_current_ctx = &contexts[0];
    multidraw_check_context();
    g_compute_inited = true;
    g_max_compute_groups_x = 65535;
    GLsizei empty_counts[16]{};
    const void* offsets[16]{};
    mg_glMultiDrawElementsBaseVertex_compute(GL_TRIANGLES, empty_counts, GL_UNSIGNED_INT, offsets, 16, nullptr);
    heap_allocations = 0;
    count_heap = true;
    for (unsigned i = 0; i < 128; ++i)
        mg_glMultiDrawElementsBaseVertex_compute(GL_TRIANGLES, empty_counts, GL_UNSIGNED_INT, offsets, i % 2 ? 16 : 3, nullptr);
    count_heap = false;
    expect_zero("compute preparation steady heap allocations", heap_allocations);
    return failures != 0;
}
