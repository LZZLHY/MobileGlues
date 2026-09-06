// Real buffer/texture/pixel wrappers; allocation and binding failures are injected.
#include "gl/buffer.h"
#include "gl/texture.h"
#include "gl/mg.h"
#include "egl/context.h"
#include "config/settings.h"
#include <map>
#include <cassert>
#include <cstdio>
size_t get_buffer_data_size(GLuint buffer);

gles_func_t g_gles_func{};
gles_caps_t g_gles_caps{};
global_settings_t global_settings{};
hardware_s hw{320, false};
hardware_t hardware = &hw;
gl_state_s g_default_gl_state{};
thread_local gl_state_t gl_state = &g_default_gl_state;
thread_local MGContext* g_current_ctx = nullptr;
bool g_angle_in_use = false;
namespace FSR1_Context {
    thread_local GLuint g_renderFBO = 0;
    thread_local bool g_dirty = false;
} // namespace FSR1_Context
thread_local bool fsrInitialized = false;
extern "C" void write_log(const char*, ...) {}
int __android_log_print(int, const char*, const char*, ...) {
    return 0;
}
namespace {
    GLuint nextBuffer = 100, nextArray = 200, currentArray = 0, boundTexture = 0;
    std::map<GLenum, GLuint> bound;
    std::map<GLuint, GLint> sizes;
    GLenum error = 0, pending = 0;
    bool failAllocation = false;
    int allocations = 0;
    GLenum getError() {
        GLenum e = error;
        error = 0;
        return e;
    }
    void genBuffers(GLsizei n, GLuint* ids) {
        while (n--)
            *ids++ = nextBuffer++;
    }
    void bindBuffer(GLenum target, GLuint id) {
        bound[target] = id;
    }
    void deleteBuffers(GLsizei n, const GLuint* ids) {
        for (int i = 0; i < n; ++i) {
            for (auto& b : bound)
                if (b.second == ids[i]) b.second = 0;
            sizes.erase(ids[i]);
        }
    }
    void data(GLenum t, GLsizeiptr n, const void*, GLenum) {
        ++allocations;
        if (failAllocation) {
            error = GL_OUT_OF_MEMORY;
            return;
        }
        sizes[bound[t]] = n;
    }
    void storage(GLenum t, GLsizeiptr n, const void* ptr, GLbitfield) {
        data(t, n, ptr, 0);
    }
    void getBuffer(GLenum t, GLenum pname, GLint* value) {
        *value = pname == GL_BUFFER_SIZE ? sizes[bound[t]] : 0;
    }
    void genArrays(GLsizei n, GLuint* ids) {
        while (n--)
            *ids++ = nextArray++;
    }
    void bindArray(GLuint array) {
        currentArray = array;
    }
    void deleteArrays(GLsizei n, const GLuint* arrays) {
        for (int i = 0; i < n; ++i)
            if (currentArray == arrays[i]) currentArray = 0;
    }
    void getInteger(GLenum pname, GLint* v) {
        *v = pname == GL_TEXTURE_BINDING_2D ? boundTexture : pname == GL_ACTIVE_TEXTURE ? GL_TEXTURE0 : 0;
    }
    void bindTexture(GLenum, GLuint texture) {
        boundTexture = texture;
    }
    void deleteTextures(GLsizei, const GLuint*) {
        boundTexture = 0;
    }
    void activeTexture(GLenum) {}
    void texStorage(GLenum, GLsizei, GLenum, GLsizei, GLsizei) {
        if (failAllocation) error = GL_OUT_OF_MEMORY;
    }
    void texStorage3(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLsizei) {
        if (failAllocation) error = GL_OUT_OF_MEMORY;
    }
} // namespace
void mg_set_gl_error(GLenum e) {
    if (!pending) pending = e;
}
void mg_begin_driver_operation() {
    GLenum e = getError();
    if (e) mg_set_gl_error(e);
}
bool mg_end_driver_operation(const char*) {
    GLenum e = getError();
    if (e) mg_set_gl_error(e);
    return !e;
}
extern "C" void glGetIntegerv(GLenum pname, GLint* value) {
    getInteger(pname, value);
}

int main() {
    GLES.glGetError = getError;
    GLES.glGenBuffers = genBuffers;
    GLES.glBindBuffer = bindBuffer;
    GLES.glDeleteBuffers = deleteBuffers;
    GLES.glBufferData = data;
    GLES.glBufferStorageEXT = storage;
    GLES.glGetBufferParameteriv = getBuffer;
    GLES.glGenVertexArrays = genArrays;
    GLES.glBindVertexArray = bindArray;
    GLES.glDeleteVertexArrays = deleteArrays;
    GLES.glGetIntegerv = getInteger;
    GLES.glBindTexture = bindTexture;
    GLES.glActiveTexture = activeTexture;
    GLES.glDeleteTextures = deleteTextures;
    GLES.glTexStorage2D = texStorage;
    GLES.glTexStorage3D = texStorage3;
    global_settings.fsr1_setting = FSR1_Quality_Preset::Disabled;
    GLuint buffer = 0;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, 8, nullptr, GL_STATIC_DRAW);
    assert(get_buffer_data_size(buffer) == 8);
    failAllocation = true;
    glBufferStorage(GL_ARRAY_BUFFER, 1024, nullptr, 0);
    assert(get_buffer_data_size(buffer) == 8 && pending == GL_OUT_OF_MEMORY);
    failAllocation = false;
    pending = 0;
    int before = allocations;
    glBufferStorage(GL_ARRAY_BUFFER, 16, nullptr, 0);
    assert(allocations == before + 1 && get_buffer_data_size(buffer) == 16);
    GLint immutable = 0;
    glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_IMMUTABLE_STORAGE, &immutable);
    assert(immutable == GL_TRUE);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buffer);
    glDeleteBuffers(1, &buffer);
    assert(bound[GL_PIXEL_UNPACK_BUFFER] == 0 && find_bound_buffer_by_target(GL_PIXEL_UNPACK_BUFFER) == 0 &&
           mg_driver_bound_buffer(GL_PIXEL_UNPACK_BUFFER) == 0);
    GLuint next = 0;
    glGenBuffers(1, &next);
    assert(mg_driver_bound_buffer(GL_PIXEL_UNPACK_BUFFER) == 0);
    GLuint vao = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glDeleteVertexArrays(1, &vao);
    assert(currentArray == 0 && find_bound_array() == 0);
    glBindTexture(GL_TEXTURE_2D, 55);
    auto* texture = mgGetTexObjectByID(55);
    assert(texture);
    texture->width = 8;
    texture->height = 8;
    failAllocation = true;
    pending = 0;
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 1024, 1024);
    assert(texture->width == 8 && pending == GL_OUT_OF_MEMORY);
    failAllocation = false;
    pending = 0;
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 16, 16);
    assert(texture->width == 16 && pending == 0);
    failAllocation = true;
    glTexStorage3D(GL_TEXTURE_2D, 1, GL_RGBA8, 32, 32, 4);
    assert(texture->width == 16);
    const GLuint textureName = 55;
    glDeleteTextures(1, &textureName);
    puts("buffer/VAO/texture failure and deletion contracts passed");
}
