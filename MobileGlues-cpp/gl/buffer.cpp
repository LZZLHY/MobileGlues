// MobileGlues - gl/buffer.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#include "buffer.h"
#include "../egl/context.h"
#include <mutex>
#include <memory>
#include <ska/flat_hash_map.hpp>
#include <array>
#include <chrono>
#include "texture.h"
#include "buffer_contract_core.h"
#include "terrain_upload_core.h"
#include "upload_scheduler_core.h"

#define DEBUG 0

static_assert(GL_MAP_READ_BIT == mg::buffer_contract::MapRead);
static_assert(GL_MAP_WRITE_BIT == mg::buffer_contract::MapWrite);
static_assert(GL_MAP_FLUSH_EXPLICIT_BIT == mg::buffer_contract::MapFlushExplicit);
static_assert(GL_MAP_PERSISTENT_BIT == mg::buffer_contract::MapPersistent);
static_assert(GL_MAP_COHERENT_BIT == mg::buffer_contract::MapCoherent);
static_assert(GL_DYNAMIC_STORAGE_BIT == mg::buffer_contract::DynamicStorage);

GLuint bound_array;
static GLint maxBufferId = 0;
static GLint maxArrayId = 0;

// ---------------------------------------------------------------------------
// Per-share-group and per-context storage
//
// GL scopes buffer names to the share group -- two contexts created against each
// other see one set of names -- while vertex array objects and the current
// bindings are container state and belong to the context alone, even inside a
// share group. All of it used to be one process-wide set, so a second context
// inherited the first one's names, sizes and bindings.
//
// The tables stay private to this file and are selected by a thread_local
// pointer that eglMakeCurrent swaps, which is why the ~90 access sites only
// changed shape rather than routing through an accessor on every use.
// The state is held by pointer: the map moves its elements when it grows, and
// these thread_local pointers have to outlive other contexts being added.
// ---------------------------------------------------------------------------

namespace {

struct buffer_group_state_t { // shared across a share group
    std::vector<GLuint> gen_buffers;
    std::vector<char> gen_buffer_exists;
    std::vector<GLuint> free_buffer_ids;
    std::vector<size_t> buffer_datasize;
    std::vector<std::uint64_t> object_generation;
    std::vector<std::uint64_t> storage_generation;
    std::vector<mg::buffer_contract::StorageState> storage_contracts;
    std::vector<mg::buffer_contract::MappingState> mappings;
    std::vector<std::uint64_t> mapping_contract_logged_generation;
    std::uint64_t next_object_generation{1};
    std::uint64_t next_storage_generation{1};
    std::size_t active_contexts{0};
};

struct upload_stats_t {
    std::uint64_t ordinary_calls{0};
    std::uint64_t named_calls{0};
    std::uint64_t input_bytes{0};
    std::uint64_t candidate_32_calls{0};
    std::uint64_t candidate_128_calls{0};
    std::uint64_t queued_calls{0};
    std::uint64_t merged_calls{0};
    std::uint64_t submitted_calls{0};
    std::uint64_t submitted_bytes{0};
    std::uint64_t submitted_source_bytes{0};
    std::uint64_t direct_fallback_calls{0};
    std::uint64_t allocation_failures{0};
    std::uint64_t watermark_drains{0};
    std::uint64_t barrier_drains{0};
    std::uint64_t present_drains{0};
    std::uint64_t driver_ns{0};
    std::uint64_t driver_max_ns{0};
    std::uint64_t max_queue_records{0};
    std::uint64_t max_queue_bytes{0};
};

struct buffer_ctx_state_t { // private to one context
    std::vector<GLuint> gen_arrays;
    std::vector<char> gen_array_exists;
    std::vector<GLuint> free_array_ids;
    std::vector<GLuint> element_array_buffer_per_vao;
    std::array<GLuint, 13> bound_buffers{};
    unsigned long long context_id{0};
    unsigned long long share_group_id{0};
    bool registered{false};
    bool scheduler_faulted{false};
    bool scheduler_mode_logged{false};
    BufferUploadMode scheduler_last_effective{BufferUploadMode::MaxValue};
    int scheduler_last_reason{-1};
    mg::upload::Queue upload_queue{};
    mg::upload::OpportunityModel upload_opportunity{};
    upload_stats_t upload_stats{};
    std::uint64_t upload_last_report_ns{0};
};

std::mutex g_buf_mutex;
// The tables hold their state by pointer. A thread_local pointer into an entry is
// the whole point of the design -- the ~90 access sites read through g_bg/g_bc
// rather than looking anything up -- and the map moves its elements when it
// grows, so the entry itself must not be what moves. The unique_ptr stays put
// while the map rehashes around it.
ska::flat_hash_map<unsigned long long, std::unique_ptr<buffer_group_state_t>> g_buf_groups;
ska::flat_hash_map<unsigned long long, std::unique_ptr<buffer_ctx_state_t>> g_buf_ctxs;

buffer_group_state_t g_buf_group_default;
buffer_ctx_state_t g_buf_ctx_default;

thread_local buffer_group_state_t* g_bg = &g_buf_group_default;
thread_local buffer_ctx_state_t* g_bc = &g_buf_ctx_default;

} // namespace

void mg_buffer_register_context(unsigned long long ctx_id, unsigned long long group_id) {
    if (ctx_id == 0 || group_id == 0) return;
    std::lock_guard<std::mutex> lock(g_buf_mutex);
    std::unique_ptr<buffer_group_state_t>& group = g_buf_groups[group_id];
    if (!group) group = std::make_unique<buffer_group_state_t>();
    std::unique_ptr<buffer_ctx_state_t>& ctx = g_buf_ctxs[ctx_id];
    if (!ctx) ctx = std::make_unique<buffer_ctx_state_t>();
    if (ctx->registered) return;
    ctx->registered = true;
    ctx->context_id = ctx_id;
    ctx->share_group_id = group_id;
    ++group->active_contexts;
}

void mg_buffer_bind_context(unsigned long long ctx_id, unsigned long long group_id) {
    if (ctx_id == 0) {
        g_bg = &g_buf_group_default;
        g_bc = &g_buf_ctx_default;
        return;
    }
    std::lock_guard<std::mutex> lock(g_buf_mutex);
    std::unique_ptr<buffer_group_state_t>& group = g_buf_groups[group_id];
    if (!group) group = std::make_unique<buffer_group_state_t>();
    std::unique_ptr<buffer_ctx_state_t>& ctx = g_buf_ctxs[ctx_id];
    if (!ctx) ctx = std::make_unique<buffer_ctx_state_t>();
    if (!ctx->registered) {
        ctx->registered = true;
        ctx->context_id = ctx_id;
        ctx->share_group_id = group_id;
        ++group->active_contexts;
    }
    g_bg = group.get();
    g_bc = ctx.get();
}

void mg_buffer_forget_context(unsigned long long ctx_id) {
    if (ctx_id == 0) return;
    std::lock_guard<std::mutex> lock(g_buf_mutex);
    const auto it = g_buf_ctxs.find(ctx_id);
    if (it == g_buf_ctxs.end()) return;

    const unsigned long long group_id = it->second->share_group_id;
    if (it->second->registered) {
        const auto group = g_buf_groups.find(group_id);
        if (group != g_buf_groups.end() && group->second && group->second->active_contexts > 0) {
            --group->second->active_contexts;
        }
    }
    if (g_bc == it->second.get()) g_bc = &g_buf_ctx_default;
    g_buf_ctxs.erase(it);

    const auto group = g_buf_groups.find(group_id);
    if (group != g_buf_groups.end() && group->second && group->second->active_contexts == 0) {
        if (g_bg == group->second.get()) g_bg = &g_buf_group_default;
        g_buf_groups.erase(group);
    }
}

#define g_gen_buffers (g_bg->gen_buffers)
#define g_gen_buffer_exists (g_bg->gen_buffer_exists)
#define g_free_buffer_ids (g_bg->free_buffer_ids)
#define g_buffer_datasize (g_bg->buffer_datasize)
#define g_buffer_object_generation (g_bg->object_generation)
#define g_buffer_storage_generation (g_bg->storage_generation)
#define g_buffer_storage_contracts (g_bg->storage_contracts)
#define g_buffer_mappings (g_bg->mappings)
#define g_buffer_mapping_contract_logged_generation (g_bg->mapping_contract_logged_generation)
#define g_gen_arrays (g_bc->gen_arrays)
#define g_gen_array_exists (g_bc->gen_array_exists)
#define g_free_array_ids (g_bc->free_array_ids)
#define g_element_array_buffer_per_vao (g_bc->element_array_buffer_per_vao)

enum BindingIndex : int {
    BI_ARRAY_BUFFER = 0,
    BI_ATOMIC_COUNTER,
    BI_COPY_READ,
    BI_COPY_WRITE,
    BI_DRAW_INDIRECT,
    BI_DISPATCH_INDIRECT,
    BI_ELEMENT_ARRAY,
    BI_PIXEL_PACK,
    BI_PIXEL_UNPACK,
    BI_SHADER_STORAGE,
    BI_TRANSFORM_FEEDBACK,
    BI_UNIFORM_BUFFER,
    BI_PARAMETER_BUFFER,
    BINDING_COUNT
};
#define g_bound_buffers_arr (g_bc->bound_buffers)
static_assert(BINDING_COUNT == 13, "buffer_ctx_state_t::bound_buffers must match BindingIndex");

static inline int ensure_buffer_capacity(GLuint id) {
    if ((int)g_gen_buffers.size() <= (int)id) {
        g_gen_buffers.resize(id + 1, 0);
        g_gen_buffer_exists.resize(id + 1, 0);
        if (g_buffer_datasize.size() <= (size_t)id) g_buffer_datasize.resize(id + 1, 0);
        if (g_buffer_object_generation.size() <= (size_t)id) g_buffer_object_generation.resize(id + 1, 0);
        if (g_buffer_storage_generation.size() <= (size_t)id) g_buffer_storage_generation.resize(id + 1, 0);
        if (g_buffer_storage_contracts.size() <= (size_t)id) g_buffer_storage_contracts.resize(id + 1);
        if (g_buffer_mappings.size() <= (size_t)id) g_buffer_mappings.resize(id + 1);
        if (g_buffer_mapping_contract_logged_generation.size() <= (size_t)id)
            g_buffer_mapping_contract_logged_generation.resize(id + 1, 0);
    }
    return 0;
}

static inline int ensure_array_capacity(GLuint id) {
    if ((int)g_gen_arrays.size() <= (int)id) {
        g_gen_arrays.resize(id + 1, 0);
        g_gen_array_exists.resize(id + 1, 0);
        if (g_element_array_buffer_per_vao.size() <= (size_t)id) g_element_array_buffer_per_vao.resize(id + 1, 0);
    }
    return 0;
}

GLuint gen_buffer() {
    if (!g_free_buffer_ids.empty()) {
        GLuint id = g_free_buffer_ids.back();
        g_free_buffer_ids.pop_back();
        ensure_buffer_capacity(id);
        g_gen_buffers[id] = 0;
        g_gen_buffer_exists[id] = 1;
        g_buffer_datasize[id] = 0;
        g_buffer_object_generation[id] = g_bg->next_object_generation++;
        g_buffer_storage_generation[id] = 0;
        g_buffer_storage_contracts[id] = {};
        g_buffer_mappings[id] = {};
        g_buffer_mapping_contract_logged_generation[id] = 0;
        if (id > (GLuint)maxBufferId) maxBufferId = id;
        return id;
    }
    maxBufferId++;
    ensure_buffer_capacity((GLuint)maxBufferId);
    g_gen_buffers[maxBufferId] = 0;
    g_gen_buffer_exists[maxBufferId] = 1;
    g_buffer_datasize[maxBufferId] = 0;
    g_buffer_object_generation[maxBufferId] = g_bg->next_object_generation++;
    g_buffer_storage_generation[maxBufferId] = 0;
    g_buffer_storage_contracts[maxBufferId] = {};
    g_buffer_mappings[maxBufferId] = {};
    g_buffer_mapping_contract_logged_generation[maxBufferId] = 0;
    return (GLuint)maxBufferId;
}

GLboolean has_buffer(GLuint key) {
    return key < g_gen_buffer_exists.size() ? (g_gen_buffer_exists[key] != 0) : 0;
}

void modify_buffer(GLuint key, GLuint value) {
    if (key >= g_gen_buffers.size()) ensure_buffer_capacity(key);
    if (!g_gen_buffer_exists[key]) {
        g_buffer_object_generation[key] = g_bg->next_object_generation++;
        g_buffer_storage_generation[key] = 0;
        g_buffer_storage_contracts[key] = {};
        g_buffer_mappings[key] = {};
        g_buffer_mapping_contract_logged_generation[key] = 0;
    }
    g_gen_buffers[key] = value;
    if (key >= g_gen_buffer_exists.size()) g_gen_buffer_exists.resize(key + 1, 0);
    g_gen_buffer_exists[key] = 1;
}

void remove_buffer(GLuint key) {
    if (key < g_gen_buffer_exists.size() && g_gen_buffer_exists[key]) {
        g_gen_buffer_exists[key] = 0;
        g_gen_buffers[key] = 0;
        if (key < g_buffer_datasize.size()) g_buffer_datasize[key] = 0;
        if (key < g_buffer_storage_generation.size()) g_buffer_storage_generation[key] = 0;
        if (key < g_buffer_storage_contracts.size()) g_buffer_storage_contracts[key] = {};
        if (key < g_buffer_mappings.size()) g_buffer_mappings[key] = {};
        if (key < g_buffer_mapping_contract_logged_generation.size())
            g_buffer_mapping_contract_logged_generation[key] = 0;
        g_free_buffer_ids.push_back(key);
    }
}

GLuint find_real_buffer(GLuint key) {
    if (key < g_gen_buffers.size() && g_gen_buffer_exists[key]) return g_gen_buffers[key];
    return 0;
}

GLuint get_ibo_by_vao(GLuint vao) {
    if (vao < g_element_array_buffer_per_vao.size()) return g_element_array_buffer_per_vao[vao];
    return 0;
}

GLuint find_bound_array() {
    return bound_array;
}

void update_vao_ibo_binding(GLuint vao, GLuint ibo) {
    ensure_array_capacity(vao);
    g_element_array_buffer_per_vao[vao] = ibo;
}

void set_buffer_data_size(GLuint buffer, size_t size) {
    ensure_buffer_capacity(buffer);
    g_buffer_datasize[buffer] = size;
}

static void note_buffer_storage(GLuint buffer, GLsizeiptr size,
                                const mg::buffer_contract::StorageState& contract) {
    if (buffer == 0 || !has_buffer(buffer) || size < 0) return;
    ensure_buffer_capacity(buffer);
    g_buffer_datasize[buffer] = static_cast<size_t>(size);
    g_buffer_storage_generation[buffer] = g_bg->next_storage_generation++;
    g_buffer_storage_contracts[buffer] = contract;
    g_buffer_mappings[buffer] = {};
    g_buffer_mapping_contract_logged_generation[buffer] = 0;
}

size_t get_buffer_data_size(GLuint buffer) {
    if (buffer < g_buffer_datasize.size()) return g_buffer_datasize[buffer];
    return 0;
}

static inline int binding_target_to_index(GLenum target) {
    switch (target) {
    case GL_ARRAY_BUFFER:
        return BI_ARRAY_BUFFER;
    case GL_ATOMIC_COUNTER_BUFFER:
        return BI_ATOMIC_COUNTER;
    case GL_COPY_READ_BUFFER:
        return BI_COPY_READ;
    case GL_COPY_WRITE_BUFFER:
        return BI_COPY_WRITE;
    case GL_DRAW_INDIRECT_BUFFER:
        return BI_DRAW_INDIRECT;
    case GL_DISPATCH_INDIRECT_BUFFER:
        return BI_DISPATCH_INDIRECT;
    case GL_ELEMENT_ARRAY_BUFFER:
        return BI_ELEMENT_ARRAY;
    case GL_PIXEL_PACK_BUFFER:
        return BI_PIXEL_PACK;
    case GL_PIXEL_UNPACK_BUFFER:
        return BI_PIXEL_UNPACK;
    case GL_SHADER_STORAGE_BUFFER:
        return BI_SHADER_STORAGE;
    case GL_TRANSFORM_FEEDBACK_BUFFER:
        return BI_TRANSFORM_FEEDBACK;
    case GL_UNIFORM_BUFFER:
        return BI_UNIFORM_BUFFER;
    case GL_PARAMETER_BUFFER:
        return BI_PARAMETER_BUFFER;
    default:
        return -1;
    }
}

void set_bound_buffer_by_target(GLenum target, GLuint buffer) {
    int idx = binding_target_to_index(target);
    if (idx >= 0) g_bound_buffers_arr[idx] = buffer;
}

// find_bound_buffer below answers the *_BINDING query enums, which is what
// glGetIntegerv passes it. Callers holding a bind target need this one instead:
// handing a target to find_bound_buffer falls through to its default and comes
// back 0, which is a valid buffer name and so goes unnoticed.
GLuint find_bound_buffer_by_target(GLenum target) {
    if (target == GL_ELEMENT_ARRAY_BUFFER) return get_ibo_by_vao(find_bound_array());
    const int idx = binding_target_to_index(target);
    return idx >= 0 ? g_bound_buffers_arr[idx] : 0;
}

// The name the *driver* has bound to `target`, i.e. what
// GLES.glGetIntegerv(<target>_BINDING) would answer, worked out from the tracked
// bindings rather than by asking the driver.
//
// Buffer names are renamed across this boundary: the application sees names
// gen_buffer() hands out and the driver sees the ones glGenBuffers gave back, so
// find_bound_buffer_by_target's answer must not be passed to GLES.glBindBuffer
// unmapped. This one may. A name the application bound without ever generating it
// is forwarded verbatim by glBindBuffer -- GLES creates the object on first bind
// -- so it is its own driver name.
//
// Two limits, both shared with this layer's own glGetIntegerv:
//   - It reports the last name bound to the target, and glDeleteBuffers does not
//     clear the binding slots (only GL_PARAMETER_BUFFER, which has no driver-side
//     binding to fall back on). Deleting a still-bound buffer resets the driver's
//     binding to 0 while this keeps reporting the dead name.
//   - It is the tracked state, so it is only the driver's state where the two
//     agree. Every internal path that binds GL_ELEMENT_ARRAY_BUFFER or
//     GL_DRAW_INDIRECT_BUFFER through GLES.* directly (gl/multidraw.cpp,
//     gl/drawing.cpp, gl/restart.cpp) saves and restores around its own work, so
//     they disagree only inside those windows -- ask before the temporary bind,
//     never during it. gl/gl.cpp's depth-clear triangle is the one path that does
//     not: it leaves the driver on vertex array 0 and GL_ARRAY_BUFFER 0 without
//     putting the application's back, which desynchronises the element array
//     binding too, since that is vertex array state.
//
// GL_PARAMETER_BUFFER has no GLES binding at all; the mapped name is returned for
// it anyway, because gl/multidraw.cpp is the only thing that asks and it needs the
// real object to bind somewhere else.
GLuint mg_driver_bound_buffer(GLenum target) {
    const GLuint name = find_bound_buffer_by_target(target);
    const GLuint real = (name == 0 || !has_buffer(name)) ? name : find_real_buffer(name);
#if GLOBAL_DEBUG
    // The divergence this answer is vulnerable to -- driver state mutated
    // behind the frontend's back -- is undetectable at runtime: the tracked
    // state always has an answer and cannot know it is stale. So debug builds
    // pay the round-trip this function exists to avoid, and scream on a
    // mismatch instead of letting a wrong binding surface three calls later as
    // a skipped draw or a corrupted restore. Release builds trust the tracking.
    if (GLES.glGetIntegerv) {
        GLenum pname = 0;
        switch (target) {
        case GL_ARRAY_BUFFER:          pname = GL_ARRAY_BUFFER_BINDING; break;
        case GL_ELEMENT_ARRAY_BUFFER:  pname = GL_ELEMENT_ARRAY_BUFFER_BINDING; break;
        case GL_DRAW_INDIRECT_BUFFER:  pname = GL_DRAW_INDIRECT_BUFFER_BINDING; break;
        case GL_PIXEL_UNPACK_BUFFER:   pname = GL_PIXEL_UNPACK_BUFFER_BINDING; break;
        case GL_PIXEL_PACK_BUFFER:     pname = GL_PIXEL_PACK_BUFFER_BINDING; break;
        case GL_COPY_READ_BUFFER:      pname = GL_COPY_READ_BUFFER_BINDING; break;
        case GL_COPY_WRITE_BUFFER:     pname = GL_COPY_WRITE_BUFFER_BINDING; break;
        default: break;
        }
        if (pname != 0) {
            GLint driver = 0;
            GLES.glGetIntegerv(pname, &driver);
            if (static_cast<GLuint>(driver) != real) {
                LOG_E("mg_driver_bound_buffer(0x%X): tracked %u (real %u) but the driver holds %u -- "
                      "something mutated this binding without going through the frontend",
                      target, name, real, static_cast<GLuint>(driver))
            }
        }
    }
#endif
    return real;
}

GLuint find_bound_buffer(GLenum key) {
    GLenum target = 0;
    switch (key) {
    case GL_ARRAY_BUFFER_BINDING:
        target = GL_ARRAY_BUFFER;
        break;
    case GL_ATOMIC_COUNTER_BUFFER_BINDING:
        target = GL_ATOMIC_COUNTER_BUFFER;
        break;
    case GL_COPY_READ_BUFFER_BINDING:
        target = GL_COPY_READ_BUFFER;
        break;
    case GL_COPY_WRITE_BUFFER_BINDING:
        target = GL_COPY_WRITE_BUFFER;
        break;
    case GL_DRAW_INDIRECT_BUFFER_BINDING:
        target = GL_DRAW_INDIRECT_BUFFER;
        break;
    case GL_DISPATCH_INDIRECT_BUFFER_BINDING:
        target = GL_DISPATCH_INDIRECT_BUFFER;
        break;
    case GL_ELEMENT_ARRAY_BUFFER_BINDING:
        target = GL_ELEMENT_ARRAY_BUFFER;
        break;
    case GL_PIXEL_PACK_BUFFER_BINDING:
        target = GL_PIXEL_PACK_BUFFER;
        break;
    case GL_PIXEL_UNPACK_BUFFER_BINDING:
        target = GL_PIXEL_UNPACK_BUFFER;
        break;
    case GL_SHADER_STORAGE_BUFFER_BINDING:
        target = GL_SHADER_STORAGE_BUFFER;
        break;
    case GL_TRANSFORM_FEEDBACK_BUFFER_BINDING:
        target = GL_TRANSFORM_FEEDBACK_BUFFER;
        break;
    case GL_UNIFORM_BUFFER_BINDING:
        target = GL_UNIFORM_BUFFER;
        break;
    case GL_PARAMETER_BUFFER_BINDING:
        target = GL_PARAMETER_BUFFER;
        break;
    default:
        target = 0;
        break;
    }
    if (target == GL_ELEMENT_ARRAY_BUFFER) {
        return get_ibo_by_vao(find_bound_array());
    }
    int idx = binding_target_to_index(target);
    if (idx >= 0) return g_bound_buffers_arr[idx];
    return 0;
}

GLuint gen_array() {
    if (!g_free_array_ids.empty()) {
        GLuint id = g_free_array_ids.back();
        g_free_array_ids.pop_back();
        ensure_array_capacity(id);
        g_gen_arrays[id] = 0;
        g_gen_array_exists[id] = 1;
        g_element_array_buffer_per_vao[id] = 0;
        if (id > (GLuint)maxArrayId) maxArrayId = id;
        return id;
    }
    maxArrayId++;
    ensure_array_capacity((GLuint)maxArrayId);
    g_gen_arrays[maxArrayId] = 0;
    g_gen_array_exists[maxArrayId] = 1;
    g_element_array_buffer_per_vao[maxArrayId] = 0;
    return (GLuint)maxArrayId;
}

GLboolean has_array(GLuint key) {
    return key < g_gen_array_exists.size() ? (g_gen_array_exists[key] != 0) : 0;
}

void modify_array(GLuint key, GLuint value) {
    if (key >= g_gen_arrays.size()) ensure_array_capacity(key);
    g_gen_arrays[key] = value;
    if (key >= g_gen_array_exists.size()) g_gen_array_exists.resize(key + 1, 0);
    g_gen_array_exists[key] = 1;
}

void remove_array(GLuint key) {
    if (key < g_gen_array_exists.size() && g_gen_array_exists[key]) {
        g_gen_array_exists[key] = 0;
        g_gen_arrays[key] = 0;
        if (key < g_element_array_buffer_per_vao.size()) g_element_array_buffer_per_vao[key] = 0;
        g_free_array_ids.push_back(key);
    }
}

GLuint find_real_array(GLuint key) {
    if (key < g_gen_arrays.size() && g_gen_array_exists[key]) return g_gen_arrays[key];
    return 0;
}

static GLenum get_binding_query(GLenum target) {
    switch (target) {
    case GL_ARRAY_BUFFER:
        return GL_ARRAY_BUFFER_BINDING;
    case GL_ELEMENT_ARRAY_BUFFER:
        return GL_ELEMENT_ARRAY_BUFFER_BINDING;
    case GL_PIXEL_PACK_BUFFER:
        return GL_PIXEL_PACK_BUFFER_BINDING;
    case GL_PIXEL_UNPACK_BUFFER:
        return GL_PIXEL_UNPACK_BUFFER_BINDING;
    case GL_COPY_WRITE_BUFFER:
        return GL_COPY_WRITE_BUFFER_BINDING;
    case GL_COPY_READ_BUFFER:
        return GL_COPY_READ_BUFFER_BINDING;
    case GL_UNIFORM_BUFFER:
        return GL_UNIFORM_BUFFER_BINDING;
    case GL_SHADER_STORAGE_BUFFER:
        return GL_SHADER_STORAGE_BUFFER_BINDING;
    case GL_TRANSFORM_FEEDBACK_BUFFER:
        return GL_TRANSFORM_FEEDBACK_BUFFER_BINDING;
    case GL_ATOMIC_COUNTER_BUFFER:
        return GL_ATOMIC_COUNTER_BUFFER_BINDING;
    case GL_DRAW_INDIRECT_BUFFER:
        return GL_DRAW_INDIRECT_BUFFER_BINDING;
    case GL_DISPATCH_INDIRECT_BUFFER:
        return GL_DISPATCH_INDIRECT_BUFFER_BINDING;
    default:
        return 0;
    }
}

void InitBufferMap(size_t expectedSize) {
    g_gen_buffers.reserve(expectedSize + 2);
    g_gen_buffer_exists.reserve(expectedSize + 2);
    g_buffer_datasize.reserve(expectedSize + 2);
    g_buffer_object_generation.reserve(expectedSize + 2);
    g_buffer_storage_generation.reserve(expectedSize + 2);
    g_buffer_storage_contracts.reserve(expectedSize + 2);
    g_buffer_mappings.reserve(expectedSize + 2);
    g_gen_buffers.resize(1, 0);
    g_gen_buffer_exists.resize(1, 0);
    g_buffer_datasize.resize(1, 0);
    g_buffer_object_generation.resize(1, 0);
    g_buffer_storage_generation.resize(1, 0);
    g_buffer_storage_contracts.resize(1);
    g_buffer_mappings.resize(1);
}

void InitVertexArrayMap(size_t expectedSize) {
    g_gen_arrays.reserve(expectedSize + 2);
    g_gen_array_exists.reserve(expectedSize + 2);
    g_element_array_buffer_per_vao.reserve(expectedSize + 2);
    g_gen_arrays.resize(1, 0);
    g_gen_array_exists.resize(1, 0);
    g_element_array_buffer_per_vao.resize(1, 0);
}

void glGenBuffers(GLsizei n, GLuint* buffers) {
    LOG()
    LOG_D("glGenBuffers(%i, %p)", n, buffers)
    for (int i = 0; i < n; ++i) {
        buffers[i] = gen_buffer();
    }
}

void glDeleteBuffers(GLsizei n, const GLuint* buffers) {
    LOG()
    LOG_D("glDeleteBuffers(%i, %p)", n, buffers)
    for (int i = 0; i < n; ++i) {
        // GL resets a binding to 0 when the bound buffer is deleted. The
        // parameter buffer slot is the only source of truth gl/multidraw.cpp has
        // for where the draw count lives -- there is no driver-side binding to
        // cross-check it against -- and deleted ids are recycled by gen_buffer(),
        // so a stale slot would silently point at somebody else's buffer.
        if (buffers[i] != 0 && find_bound_buffer(GL_PARAMETER_BUFFER_BINDING) == buffers[i]) {
            set_bound_buffer_by_target(GL_PARAMETER_BUFFER, 0);
        }
        if (find_real_buffer(buffers[i])) {
            GLuint real_buff = find_real_buffer(buffers[i]);
            GLES.glDeleteBuffers(1, &real_buff);
            CHECK_GL_ERROR
        }
        remove_buffer(buffers[i]);
    }
}

GLboolean glIsBuffer(GLuint buffer) {
    LOG()
    LOG_D("glIsBuffer, buffer = %d", buffer)
    return has_buffer(buffer);
}

void glBindBuffer(GLenum target, GLuint buffer) {
    LOG()
    LOG_D("glBindBuffer, target = %s, buffer = %d", glEnumToString(target), buffer)
    set_bound_buffer_by_target(target, buffer);

    if (target == GL_PARAMETER_BUFFER) {
        // GLES has no GL_PARAMETER_BUFFER. The binding is tracked here and read
        // back by gl/multidraw.cpp for glMultiDraw*IndirectCount; forwarding the
        // target to the driver would only raise GL_INVALID_ENUM. The backing
        // object still has to exist, because nothing else will create it.
        if (buffer != 0 && has_buffer(buffer) && !find_real_buffer(buffer)) {
            GLuint real_buffer = 0;
            GLES.glGenBuffers(1, &real_buffer);
            modify_buffer(buffer, real_buffer);
            CHECK_GL_ERROR
        }
        return;
    }

    // save ibo binding to vao
    if (target == GL_ELEMENT_ARRAY_BUFFER) {
        update_vao_ibo_binding(find_bound_array(), buffer);
    }

    if (!has_buffer(buffer) || buffer == 0) {
        GLES.glBindBuffer(target, buffer);
        CHECK_GL_ERROR
        return;
    }
    GLuint real_buffer = find_real_buffer(buffer);
    if (!real_buffer) {
        GLES.glGenBuffers(1, &real_buffer);
        modify_buffer(buffer, real_buffer);
        CHECK_GL_ERROR
    }
    LOG_D("glBindBuffer: %d -> %d", buffer, real_buffer)
    GLES.glBindBuffer(target, real_buffer);
    CHECK_GL_ERROR
}

static std::vector<GLuint> g_buffer_map_ssbo_id;

void glBindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size) {
    LOG()
    LOG_D("glBindBufferRange, target = %s, index = %d, buffer = %d, offset = %p, size = %zi", glEnumToString(target),
          index, buffer, (void*)offset, size)

    if (!has_buffer(buffer) || buffer == 0) {
        GLES.glBindBufferRange(target, index, buffer, offset, size);
        CHECK_GL_ERROR
        return;
    }
    GLuint real_buffer = find_real_buffer(buffer);
    if (!real_buffer) {
        GLES.glGenBuffers(1, &real_buffer);
        modify_buffer(buffer, real_buffer);
        CHECK_GL_ERROR
    }
    GLES.glBindBufferRange(target, index, real_buffer, offset, size);
    CHECK_GL_ERROR
}

void glBindBufferBase(GLenum target, GLuint index, GLuint buffer) {
    LOG()
    LOG_D("glBindBufferBase, target = %s, index = %d, buffer = %d", glEnumToString(target), index, buffer)

    if (!has_buffer(buffer) || buffer == 0) {
        GLES.glBindBufferBase(target, index, buffer);
        CHECK_GL_ERROR
        return;
    }
    GLuint real_buffer = find_real_buffer(buffer);
    if (!real_buffer) {
        GLES.glGenBuffers(1, &real_buffer);
        modify_buffer(buffer, real_buffer);
        CHECK_GL_ERROR
    }
    GLES.glBindBufferBase(target, index, real_buffer);
    if (target == GL_SHADER_STORAGE_BUFFER) {
        if (g_buffer_map_ssbo_id.empty()) {
            g_buffer_map_ssbo_id.resize(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, 0);
        }
        g_buffer_map_ssbo_id[index] = buffer;
    }
    CHECK_GL_ERROR
}

void glBindVertexBuffer(GLuint bindingindex, GLuint buffer, GLintptr offset, GLsizei stride) {
    LOG()
    LOG_D("glBindVertexBuffer, bindingindex = %d, buffer = %d, offset = %p, stride = %i", bindingindex, buffer, offset,
          stride)
    // Todo: should record fake buffer binding here, when glGetVertexArrayIntegeri_v is called, should return fake
    // buffer id
    if (!has_buffer(buffer) || buffer == 0) {
        GLES.glBindVertexBuffer(bindingindex, buffer, offset, stride);
        CHECK_GL_ERROR
        return;
    }
    GLuint real_buffer = find_real_buffer(buffer);
    if (!real_buffer) {
        GLES.glGenBuffers(1, &real_buffer);
        modify_buffer(buffer, real_buffer);
        CHECK_GL_ERROR
    }
    GLES.glBindVertexBuffer(bindingindex, real_buffer, offset, stride);
    CHECK_GL_ERROR
}

// The transfer pair GLES accepts for a given sized internalformat.
//
// The texture-buffer emulation used to allocate and upload with a hardcoded
// GL_RED_INTEGER + GL_BYTE whatever the internalformat was. ES validates
// internalformat/format/type as a triple, and that one is legal for exactly one
// format -- GL_R8I. Everything else (GL_R32I, GL_RGBA32F, even GL_R8UI, which
// wants GL_UNSIGNED_BYTE) failed the glTexImage2D with GL_INVALID_OPERATION,
// left level 0 undefined, and the emulated texelFetch read zeros from an
// incomplete texture. Sized here from the same table as the texel size, so the
// two cannot drift apart.
//
// Returns false for a format with no ES-legal pair -- the normalised 16-bit ones
// need EXT_texture_norm16, and depth formats are not texture-buffer formats at
// all. The caller drops the call instead of guessing.
//
// Deliberately wider than GL 4.6 table 8.16, which lists only the 32-bit
// three-component forms among the RGB ones: the extra entries here (GL_RGB8,
// GL_RGB8I/UI, GL_RGB16I/UI/F) are all valid ES triples, so emulating them costs
// nothing, while refusing them would only break an application that already works
// against the permissive desktop drivers. Being stricter than the hardware buys
// no correctness.
bool get_internal_format_transfer(GLenum internalformat, GLenum* format, GLenum* type) {
    switch (internalformat) {
    // clang-format off
    case GL_R8:        *format = GL_RED;           *type = GL_UNSIGNED_BYTE;  return true;
    case GL_R8I:       *format = GL_RED_INTEGER;   *type = GL_BYTE;           return true;
    case GL_R8UI:      *format = GL_RED_INTEGER;   *type = GL_UNSIGNED_BYTE;  return true;
    case GL_R16I:      *format = GL_RED_INTEGER;   *type = GL_SHORT;          return true;
    case GL_R16UI:     *format = GL_RED_INTEGER;   *type = GL_UNSIGNED_SHORT; return true;
    case GL_R16F:      *format = GL_RED;           *type = GL_HALF_FLOAT;     return true;
    case GL_R32I:      *format = GL_RED_INTEGER;   *type = GL_INT;            return true;
    case GL_R32UI:     *format = GL_RED_INTEGER;   *type = GL_UNSIGNED_INT;   return true;
    case GL_R32F:      *format = GL_RED;           *type = GL_FLOAT;          return true;

    case GL_RG8:       *format = GL_RG;            *type = GL_UNSIGNED_BYTE;  return true;
    case GL_RG8I:      *format = GL_RG_INTEGER;    *type = GL_BYTE;           return true;
    case GL_RG8UI:     *format = GL_RG_INTEGER;    *type = GL_UNSIGNED_BYTE;  return true;
    case GL_RG16I:     *format = GL_RG_INTEGER;    *type = GL_SHORT;          return true;
    case GL_RG16UI:    *format = GL_RG_INTEGER;    *type = GL_UNSIGNED_SHORT; return true;
    case GL_RG16F:     *format = GL_RG;            *type = GL_HALF_FLOAT;     return true;
    case GL_RG32I:     *format = GL_RG_INTEGER;    *type = GL_INT;            return true;
    case GL_RG32UI:    *format = GL_RG_INTEGER;    *type = GL_UNSIGNED_INT;   return true;
    case GL_RG32F:     *format = GL_RG;            *type = GL_FLOAT;          return true;

    case GL_RGB8:      *format = GL_RGB;           *type = GL_UNSIGNED_BYTE;  return true;
    case GL_RGB8I:     *format = GL_RGB_INTEGER;   *type = GL_BYTE;           return true;
    case GL_RGB8UI:    *format = GL_RGB_INTEGER;   *type = GL_UNSIGNED_BYTE;  return true;
    case GL_RGB16I:    *format = GL_RGB_INTEGER;   *type = GL_SHORT;          return true;
    case GL_RGB16UI:   *format = GL_RGB_INTEGER;   *type = GL_UNSIGNED_SHORT; return true;
    case GL_RGB16F:    *format = GL_RGB;           *type = GL_HALF_FLOAT;     return true;
    case GL_RGB32I:    *format = GL_RGB_INTEGER;   *type = GL_INT;            return true;
    case GL_RGB32UI:   *format = GL_RGB_INTEGER;   *type = GL_UNSIGNED_INT;   return true;
    case GL_RGB32F:    *format = GL_RGB;           *type = GL_FLOAT;          return true;

    case GL_RGBA8:     *format = GL_RGBA;          *type = GL_UNSIGNED_BYTE;  return true;
    case GL_RGBA8I:    *format = GL_RGBA_INTEGER;  *type = GL_BYTE;           return true;
    case GL_RGBA8UI:   *format = GL_RGBA_INTEGER;  *type = GL_UNSIGNED_BYTE;  return true;
    case GL_RGBA16I:   *format = GL_RGBA_INTEGER;  *type = GL_SHORT;          return true;
    case GL_RGBA16UI:  *format = GL_RGBA_INTEGER;  *type = GL_UNSIGNED_SHORT; return true;
    case GL_RGBA16F:   *format = GL_RGBA;          *type = GL_HALF_FLOAT;     return true;
    case GL_RGBA32I:   *format = GL_RGBA_INTEGER;  *type = GL_INT;            return true;
    case GL_RGBA32UI:  *format = GL_RGBA_INTEGER;  *type = GL_UNSIGNED_INT;   return true;
    case GL_RGBA32F:   *format = GL_RGBA;          *type = GL_FLOAT;          return true;
    // clang-format on
    default:
        return false;
    }
}

size_t get_internal_format_size(GLenum internalformat) {
    switch (internalformat) {
    case GL_R8:
        return 1;
    case GL_R8I:
    case GL_R8UI:
        return 1;
    case GL_R16:
        return 2;
    case GL_R16I:
    case GL_R16UI:
    case GL_R16F:
        return 2;
    case GL_R32I:
    case GL_R32UI:
    case GL_R32F:
        return 4;

    case GL_RG8:
        return 2;
    case GL_RG8I:
    case GL_RG8UI:
        return 2;
    case GL_RG16:
        return 4;
    case GL_RG16I:
    case GL_RG16UI:
    case GL_RG16F:
        return 4;
    case GL_RG32I:
    case GL_RG32UI:
    case GL_RG32F:
        return 8;

    case GL_RGB8:
        return 3;
    case GL_RGB8I:
    case GL_RGB8UI:
        return 3;
    case GL_RGB16:
        return 6;
    case GL_RGB16I:
    case GL_RGB16UI:
    case GL_RGB16F:
        return 6;
    case GL_RGB32I:
    case GL_RGB32UI:
    case GL_RGB32F:
        return 12;

    case GL_RGBA8:
        return 4;
    case GL_RGBA8I:
    case GL_RGBA8UI:
        return 4;
    case GL_RGBA16:
        return 8;
    case GL_RGBA16I:
    case GL_RGBA16UI:
    case GL_RGBA16F:
        return 8;
    case GL_RGBA32I:
    case GL_RGBA32UI:
    case GL_RGBA32F:
        return 16;

    case GL_DEPTH_COMPONENT16:
        return 2;
    case GL_DEPTH_COMPONENT24:
        return 3;
    case GL_DEPTH_COMPONENT32:
        return 4;
    case GL_DEPTH_COMPONENT32F:
        return 4;
    case GL_DEPTH24_STENCIL8:
        return 4;
    case GL_DEPTH32F_STENCIL8:
        return 5;

    case GL_STENCIL_INDEX8:
        return 1;

    case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
    case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
        return 8;
    case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
    case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
        return 16;

    default:
        LOG_E("Unknown internal format size for %s", glEnumToString(internalformat));
        return 0;
    }
}

extern std::string bufSampelerName;

// Report a rejected argument once per site. This layer cannot raise a GL error
// -- glGetError always answers GL_NO_ERROR by design -- so an unusable argument
// means "do nothing" plus one line a user can paste into a bug report. LOG_W and
// LOG_E compile to nothing in release builds, hence LOG_W_FORCE.
#define BU_WARN_ONCE(...)                                                                                              \
    do {                                                                                                               \
        static bool mg_bu_warned = false;                                                                              \
        if (!mg_bu_warned) {                                                                                           \
            mg_bu_warned = true;                                                                                       \
            LOG_W_FORCE(__VA_ARGS__)                                                                                   \
        }                                                                                                              \
    } while (0)

// Todo: any glGet* related to this function?
void glTexBuffer(GLenum target, GLenum internalformat, GLuint buffer) {
    LOG()
    LOG_D("glTexBuffer, target = %s, internalformat = %s, buffer = %d", glEnumToString(target),
          glEnumToString(internalformat), buffer)
    if (target != GL_TEXTURE_BUFFER) return;

    if (!has_buffer(buffer) || buffer == 0) {
        GLES.glTexBuffer(target, internalformat, buffer);
        CHECK_GL_ERROR
        return;
    }
    GLuint real_buffer = find_real_buffer(buffer);
    if (!real_buffer) {
        GLES.glGenBuffers(1, &real_buffer);
        modify_buffer(buffer, real_buffer);
        CHECK_GL_ERROR
    }

    if (hardware->emulate_texture_buffer) {
        LOG_D("Emulating glTexBuffer");

        // internalformat arrives unvalidated -- a format outside GL 4.6 table 8.16
        // is GL_INVALID_ENUM in real GL and this layer raises nothing -- so
        // get_internal_format_size answers 0 for it, as its own default case says.
        // That 0 used to reach "bufferSize / pixelSize" below: undefined, and on
        // arm64 it divides to zero, giving a 0 x 1 texture that the emulated
        // texelFetch then indexes modulo zero. Size the texel first and drop the
        // call if we cannot, before any binding is disturbed.
        GLuint pixelSize = get_internal_format_size(internalformat);
        if (pixelSize == 0) {
            BU_WARN_ONCE("glTexBuffer: no texel size known for internalformat %s, texture buffer left untouched",
                         glEnumToString(internalformat));
            mg_set_gl_error(GL_INVALID_ENUM);
            return;
        }

        // The transfer pair this internalformat actually accepts. Hardcoding one
        // pair here is what made every format but GL_R8I fail to allocate.
        GLenum tb_format = GL_RED_INTEGER, tb_type = GL_BYTE;
        if (!get_internal_format_transfer(internalformat, &tb_format, &tb_type)) {
            BU_WARN_ONCE("glTexBuffer: no GLES transfer pair for internalformat %s, texture buffer left untouched",
                         glEnumToString(internalformat));
            mg_set_gl_error(GL_INVALID_ENUM);
            return;
        }

        GLint boundTexture = 0;
        GLint prev_pixel_buffer_binding = 0;

        GLES.glActiveTexture(GL_TEXTURE0 + 15);

        GLES.glGetIntegerv(GL_TEXTURE_BINDING_2D, &boundTexture);
        LOG_D("Current GL_TEXTURE_BINDING_BUFFER = %d", boundTexture);
        GLES.glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &prev_pixel_buffer_binding);
        LOG_D("Previous GL_PIXEL_UNPACK_BUFFER_BINDING = %d", prev_pixel_buffer_binding);

        if (!boundTexture) {
            LOG_D("No texture bound to GL_TEXTURE_BUFFER, skipping emulation.");
            // Unit 15 is only ever borrowed for the emulated buffer texture; every
            // other borrower hands it back. Returning from here without doing so
            // left the app's next glBindTexture landing on unit 15.
            GLES.glActiveTexture(GL_TEXTURE0 + gl_state->current_tex_unit);
            return;
        }

        GLES.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, real_buffer);
        LOG_D("Bound GL_PIXEL_UNPACK_BUFFER to buffer %u", real_buffer);

        GLint bufferSize;
        GLES.glGetBufferParameteriv(GL_PIXEL_UNPACK_BUFFER, GL_BUFFER_SIZE, &bufferSize);
        LOG_D("Buffer size = %d bytes", bufferSize);

        GLES.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

        GLES.glBindTexture(GL_TEXTURE_2D, boundTexture);
        LOG_D("Binding texture %u to GL_TEXTURE_2D", boundTexture);

        const GLuint MAX_WIDTH = 8192;
        GLuint numElements = bufferSize / pixelSize;
        if (numElements == 0) {
            // A buffer too small to hold one texel. The pixelSize == 0 guard above
            // exists because a zero-sized texture makes the emulated texelFetch
            // index modulo zero; this reaches the same place by the other road,
            // through a 0 x 1 glTexImage2D and a u_BufferTexWidth of 0.
            BU_WARN_ONCE("glTexBuffer: buffer of %d bytes holds no %u-byte texel, texture buffer left untouched",
                         bufferSize, pixelSize);
            mg_set_gl_error(GL_INVALID_VALUE);
            GLES.glActiveTexture(GL_TEXTURE0 + gl_state->current_tex_unit);
            return;
        }

        GLuint width = numElements;
        GLuint height = 1;

        if (width > MAX_WIDTH) {
            width = MAX_WIDTH;
            height = (numElements + MAX_WIDTH - 1) / MAX_WIDTH;
        }

        GLint prev_alignment, prev_row_length, prev_skip_pixels, prev_skip_rows;
        GLES.glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_alignment);
        GLES.glGetIntegerv(GL_UNPACK_ROW_LENGTH, &prev_row_length);
        GLES.glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &prev_skip_pixels);
        GLES.glGetIntegerv(GL_UNPACK_SKIP_ROWS, &prev_skip_rows);

        // why do these 2 params not work
        // GLES.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        // GLES.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0)
        GLES.glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        GLES.glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);

        // TODO: Optimize the glTexImage2D call
        GLES.glTexImage2D(GL_TEXTURE_2D, 0, internalformat, width, height, 0, tb_format, tb_type, nullptr);

        GLES.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, real_buffer);

        for (GLuint row = 0; row < height; ++row) {
            // The last row is short whenever the element count is not a multiple
            // of the row width. Asking for a full row anyway made the driver read
            // past the end of the unpack buffer, which GLES answers with
            // GL_INVALID_OPERATION and a no-op -- so the tail of the buffer was
            // never uploaded and texelFetch read it back as whatever the
            // allocation left there.
            const GLuint row_texels = (row + 1 == height) ? (numElements - row * width) : width;
            if (row_texels == 0) break;
            void* offset = (void*)(static_cast<size_t>(row) * width * pixelSize);
            GLES.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, row, row_texels, 1, tb_format, tb_type, offset);
        }

        GLES.glPixelStorei(GL_UNPACK_ALIGNMENT, prev_alignment);
        GLES.glPixelStorei(GL_UNPACK_ROW_LENGTH, prev_row_length);
        GLES.glPixelStorei(GL_UNPACK_SKIP_PIXELS, prev_skip_pixels);
        GLES.glPixelStorei(GL_UNPACK_SKIP_ROWS, prev_skip_rows);

        auto tex = mgGetTexObjectByTarget(target);
        tex->target = ConvertGLEnumToTextureTarget(target);
        tex->internal_format = internalformat;
        tex->width = width;
        tex->height = height;
        tex->depth = 1;
        tex->swizzle_param[0] = GL_RED;
        tex->swizzle_param[1] = GL_GREEN;
        tex->swizzle_param[2] = GL_BLUE;
        tex->swizzle_param[3] = GL_ALPHA;

        LOG_D("Called glTexImage2D with internalformat = 0x%X", internalformat);

        GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        LOG_D("Set texture parameters: MIN_FILTER=NEAREST, MAG_FILTER=NEAREST, WRAP_S/T=CLAMP_TO_EDGE");

        GLES.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, prev_pixel_buffer_binding);

        GLES.glActiveTexture(GL_TEXTURE0 + gl_state->current_tex_unit);

        LOG_D("Restored bindings: GL_PIXEL_UNPACK_BUFFER=%d", prev_pixel_buffer_binding);

        CHECK_GL_ERROR;
        return;
    }

    GLES.glTexBuffer(target, internalformat, real_buffer);
    CHECK_GL_ERROR
}

void glTexBufferRange(GLenum target, GLenum internalformat, GLuint buffer, GLintptr offset, GLsizeiptr size) {
    LOG()
    LOG_D("glTexBufferRange, target = %s, internalformat = %s, buffer = %d, offset = %p, size = %zi",
          glEnumToString(target), glEnumToString(internalformat), buffer, (void*)offset, size)
    if (!has_buffer(buffer) || buffer == 0) {
        GLES.glTexBufferRange(target, internalformat, buffer, offset, size);
        CHECK_GL_ERROR
        return;
    }
    GLuint real_buffer = find_real_buffer(buffer);
    if (!real_buffer) {
        GLES.glGenBuffers(1, &real_buffer);
        modify_buffer(buffer, real_buffer);
        CHECK_GL_ERROR
    }
    GLES.glTexBufferRange(target, internalformat, real_buffer, offset, size);
    CHECK_GL_ERROR
}

// GLES has no GL_PARAMETER_BUFFER, so glBindBuffer above tracks the binding
// without ever handing that target to the driver. GL 4.6 still lets an
// application fill and query the buffer through it, which used to reach GLES
// verbatim and come back GL_INVALID_ENUM -- the buffer stayed empty, and
// glMultiDraw*IndirectCount then found a zero-byte parameter buffer and drew
// nothing.
//
// GL_COPY_WRITE_BUFFER is borrowed for the duration of one call and put back
// afterwards. GLES defines it as a generic target with no meaning of its own, so
// the swap is invisible: nothing observes it, and no draw depends on it.
namespace {
struct borrowed_target_t {
    GLenum target;
    GLint saved = 0;
    bool borrowed = false;

    explicit borrowed_target_t(GLenum requested) : target(requested) {
        if (requested != GL_PARAMETER_BUFFER) return;
        const GLuint real = find_real_buffer(find_bound_buffer(GL_PARAMETER_BUFFER_BINDING));
        GLES.glGetIntegerv(GL_COPY_WRITE_BUFFER_BINDING, &saved);
        GLES.glBindBuffer(GL_COPY_WRITE_BUFFER, real);
        target = GL_COPY_WRITE_BUFFER;
        borrowed = true;
    }
    ~borrowed_target_t() {
        if (borrowed) GLES.glBindBuffer(GL_COPY_WRITE_BUFFER, static_cast<GLuint>(saved));
    }

    borrowed_target_t(const borrowed_target_t&) = delete;
    borrowed_target_t& operator=(const borrowed_target_t&) = delete;
};
} // namespace

namespace {

#ifndef AMCL_MG_BUFFER_UPLOAD_MODE_OVERRIDE
#define AMCL_MG_BUFFER_UPLOAD_MODE_OVERRIDE -1
#endif
static_assert(AMCL_MG_BUFFER_UPLOAD_MODE_OVERRIDE >= -1 &&
                  AMCL_MG_BUFFER_UPLOAD_MODE_OVERRIDE <= 2,
              "AMCL_MG_BUFFER_UPLOAD_MODE_OVERRIDE must be -1, 0, 1 or 2");

constexpr std::size_t kCandidate32MiB = 32U * 1024U * 1024U;
constexpr std::size_t kCandidate128MiB = 128U * 1024U * 1024U;
constexpr std::uint64_t kUploadReportIntervalNs = 1'000'000'000ULL;

enum class upload_mode_reason_t : int {
    RequestedDisabled = 0,
    NoTrackedContext,
    MultiContextShareGroup,
    AngleBackend,
    MissingBackendEntry,
    RuntimeFault,
    Accepted,
};

struct upload_mode_decision_t {
    BufferUploadMode effective{BufferUploadMode::Disabled};
    upload_mode_reason_t reason{upload_mode_reason_t::RequestedDisabled};
};

const char* upload_mode_name(BufferUploadMode mode) {
    switch (mode) {
    case BufferUploadMode::Observe: return "observe";
    case BufferUploadMode::OrderedCoalesce: return "ordered-coalesce";
    case BufferUploadMode::Disabled:
    default: return "disabled";
    }
}

const char* upload_reason_name(upload_mode_reason_t reason) {
    switch (reason) {
    case upload_mode_reason_t::RequestedDisabled: return "requested-disabled";
    case upload_mode_reason_t::NoTrackedContext: return "no-tracked-context";
    case upload_mode_reason_t::MultiContextShareGroup: return "multi-context-share-group";
    case upload_mode_reason_t::AngleBackend: return "angle-backend";
    case upload_mode_reason_t::MissingBackendEntry: return "missing-backend-entry";
    case upload_mode_reason_t::RuntimeFault: return "runtime-fault";
    case upload_mode_reason_t::Accepted: return "accepted";
    }
    return "unknown";
}

std::uint64_t upload_now_ns() noexcept {
    using clock = std::chrono::steady_clock;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now().time_since_epoch()).count());
}

upload_mode_decision_t upload_mode_decision() {
    const BufferUploadMode requested = AMCL_MG_BUFFER_UPLOAD_MODE_OVERRIDE >= 0
                                           ? static_cast<BufferUploadMode>(AMCL_MG_BUFFER_UPLOAD_MODE_OVERRIDE)
                                           : global_settings.buffer_upload_mode;
    if (requested == BufferUploadMode::Disabled) return {};
    if (g_bc == &g_buf_ctx_default || g_bg == &g_buf_group_default || !g_bc->registered) {
        return {BufferUploadMode::Disabled, upload_mode_reason_t::NoTrackedContext};
    }
    if (g_bc->scheduler_faulted) {
        return {BufferUploadMode::Disabled, upload_mode_reason_t::RuntimeFault};
    }
    // Cross-context observers are the hardest ordering hole to close. The first
    // candidate therefore fails closed for every share group with more than one
    // live context, even if only one happens to be current at this instant.
    if (g_bg->active_contexts != 1) {
        return {BufferUploadMode::Disabled, upload_mode_reason_t::MultiContextShareGroup};
    }
    // ANGLE is a different provider and storage policy. It gets its own A/B; the
    // native Maleoon workaround must not silently change that backend.
    if (global_settings.angle == AngleMode::Enabled) {
        return {BufferUploadMode::Disabled, upload_mode_reason_t::AngleBackend};
    }
    if (GLES.glBindBuffer == nullptr || GLES.glBufferSubData == nullptr) {
        return {BufferUploadMode::Disabled, upload_mode_reason_t::MissingBackendEntry};
    }
    return {requested, upload_mode_reason_t::Accepted};
}

void log_upload_mode_if_changed(const upload_mode_decision_t& decision) {
    const int reason = static_cast<int>(decision.reason);
    if (g_bc->scheduler_mode_logged && g_bc->scheduler_last_effective == decision.effective &&
        g_bc->scheduler_last_reason == reason) {
        return;
    }
    g_bc->scheduler_mode_logged = true;
    g_bc->scheduler_last_effective = decision.effective;
    g_bc->scheduler_last_reason = reason;
    const BufferUploadMode requested = AMCL_MG_BUFFER_UPLOAD_MODE_OVERRIDE >= 0
                                           ? static_cast<BufferUploadMode>(AMCL_MG_BUFFER_UPLOAD_MODE_OVERRIDE)
                                           : global_settings.buffer_upload_mode;
    const char* source = AMCL_MG_BUFFER_UPLOAD_MODE_OVERRIDE >= 0
                             ? "compile-override"
                             : (global_settings.buffer_upload_mode_source == BufferUploadModeSource::Config
                                    ? "config"
                                    : "default");
    LOG_I("[MG-UPLOAD] schema=1 requested=%s source=%s effective=%s reason=%s context=%llu share_group=%llu "
          "share_contexts=%zu provider=%s wrapper_glBufferSubData=%p backend_glBufferSubData=%p",
          upload_mode_name(requested), source,
          upload_mode_name(decision.effective), upload_reason_name(decision.reason), g_bc->context_id,
          g_bc->share_group_id, g_bg->active_contexts,
          global_settings.angle == AngleMode::Enabled ? "angle" : "native",
          reinterpret_cast<void*>(&glBufferSubData), reinterpret_cast<void*>(GLES.glBufferSubData))
}

bool upload_identity(GLuint frontend, GLintptr offset, GLsizeiptr size, mg::upload::BufferIdentity* identity,
                     std::size_t* store_size) {
    if (frontend == 0 || !has_buffer(frontend) || offset < 0 || size <= 0 || frontend >= g_gen_buffers.size() ||
        frontend >= g_buffer_datasize.size() || frontend >= g_buffer_object_generation.size() ||
        frontend >= g_buffer_storage_generation.size() || frontend >= g_buffer_mappings.size()) {
        return false;
    }
    const GLuint backend = find_real_buffer(frontend);
    const std::size_t bytes = g_buffer_datasize[frontend];
    if (frontend >= g_buffer_storage_contracts.size()) return false;
    const auto& storage = g_buffer_storage_contracts[frontend];
    if (backend == 0 || !storage.immutable ||
        storage.requested_flags != mg::buffer_contract::DynamicStorage ||
        !mg::terrain_upload::IsRenderPearlTerrainStore(bytes) ||
        g_buffer_mappings[frontend].active ||
        g_buffer_object_generation[frontend] == 0 || g_buffer_storage_generation[frontend] == 0) {
        return false;
    }
    const std::uint64_t begin = static_cast<std::uint64_t>(offset);
    const std::uint64_t count = static_cast<std::uint64_t>(size);
    if (begin > bytes || count > static_cast<std::uint64_t>(bytes) - begin) return false;
    *identity = {g_bc->share_group_id, g_buffer_object_generation[frontend], g_buffer_storage_generation[frontend],
                 frontend, backend};
    *store_size = bytes;
    return true;
}

bool upload_identity_still_current(const mg::upload::BufferIdentity& identity) {
    const GLuint frontend = identity.frontend_name;
    return identity.share_group == g_bc->share_group_id && frontend < g_gen_buffers.size() && has_buffer(frontend) &&
           find_real_buffer(frontend) == identity.backend_name &&
           frontend < g_buffer_object_generation.size() &&
           g_buffer_object_generation[frontend] == identity.object_generation &&
           frontend < g_buffer_storage_generation.size() &&
           g_buffer_storage_generation[frontend] == identity.storage_generation;
}

enum class upload_drain_reason_t : int {
    Barrier,
    Present,
    Watermark,
    ContextRelease,
    ObjectMutation,
};

void drain_upload_queue(upload_drain_reason_t reason) {
    mg::upload::Queue& queue = g_bc->upload_queue;
    if (queue.empty()) {
        g_bc->upload_opportunity.barrier();
        return;
    }

    switch (reason) {
    case upload_drain_reason_t::Present: ++g_bc->upload_stats.present_drains; break;
    case upload_drain_reason_t::Watermark: ++g_bc->upload_stats.watermark_drains; break;
    default: ++g_bc->upload_stats.barrier_drains; break;
    }

    const GLuint saved_copy_write = mg_driver_bound_buffer(GL_COPY_WRITE_BUFFER);
    GLuint bound_backend = saved_copy_write;
    for (const mg::upload::Record& record : queue.records()) {
        if (!upload_identity_still_current(record.identity)) {
            // Every mutation route drains first. Reaching this branch therefore
            // means hook coverage or lifecycle ownership is broken; replaying to
            // the stale backend name would be worse than failing closed.
            g_bc->scheduler_faulted = true;
            LOG_W_FORCE("[MG-UPLOAD] identity mismatch; disabling scheduler context=%llu frontend=%u backend=%u "
                        "object_generation=%llu storage_generation=%llu",
                        g_bc->context_id, record.identity.frontend_name, record.identity.backend_name,
                        static_cast<unsigned long long>(record.identity.object_generation),
                        static_cast<unsigned long long>(record.identity.storage_generation))
            break;
        }
        if (bound_backend != record.identity.backend_name) {
            GLES.glBindBuffer(GL_COPY_WRITE_BUFFER, record.identity.backend_name);
            bound_backend = record.identity.backend_name;
        }
        const std::uint64_t start_ns = upload_now_ns();
        GLES.glBufferSubData(GL_COPY_WRITE_BUFFER, static_cast<GLintptr>(record.offset),
                             static_cast<GLsizeiptr>(record.size), queue.payload(record));
        const std::uint64_t elapsed_ns = upload_now_ns() - start_ns;
        ++g_bc->upload_stats.submitted_calls;
        g_bc->upload_stats.submitted_bytes += record.size;
        g_bc->upload_stats.submitted_source_bytes += record.source_bytes;
        g_bc->upload_stats.driver_ns += elapsed_ns;
        g_bc->upload_stats.driver_max_ns = std::max(g_bc->upload_stats.driver_max_ns, elapsed_ns);
    }
    if (bound_backend != saved_copy_write) GLES.glBindBuffer(GL_COPY_WRITE_BUFFER, saved_copy_write);
    queue.clear();
    g_bc->upload_opportunity.barrier();
}

bool is_upload_entry(const char* function_name) {
    return function_name != nullptr &&
           (std::strcmp(function_name, "glBufferSubData") == 0 ||
            std::strcmp(function_name, "glNamedBufferSubData") == 0 ||
            std::strcmp(function_name, "glNamedBufferSubDataEXT") == 0);
}

void report_upload_stats_if_due() {
    const std::uint64_t now_ns = upload_now_ns();
    if (g_bc->upload_last_report_ns == 0) {
        g_bc->upload_last_report_ns = now_ns;
        return;
    }
    const std::uint64_t window_ns = now_ns - g_bc->upload_last_report_ns;
    if (window_ns < kUploadReportIntervalNs) return;

    const mg::upload::OpportunityTotals opportunity = g_bc->upload_opportunity.totals();
    const upload_stats_t& stats = g_bc->upload_stats;
    LOG_I("[MG-UPLOAD-STATS] schema=1 mode=%s window_ms=%.1f ordinary_calls=%llu named_calls=%llu "
          "input_mib=%.3f candidate32_calls=%llu candidate128_calls=%llu eligible_calls=%llu eligible_mib=%.3f "
          "would_submit_calls=%llu mergeable_calls=%llu max_run_calls=%llu max_run_kib=%.1f queued_calls=%llu "
          "merged_calls=%llu submitted_calls=%llu submitted_mib=%.3f source_mib=%.3f driver_ms=%.3f "
          "driver_max_ms=%.3f direct_fallback_calls=%llu allocation_failures=%llu watermark_drains=%llu "
          "barrier_drains=%llu present_drains=%llu max_queue_records=%llu max_queue_kib=%.1f",
          upload_mode_name(upload_mode_decision().effective), static_cast<double>(window_ns) / 1.0e6,
          static_cast<unsigned long long>(stats.ordinary_calls),
          static_cast<unsigned long long>(stats.named_calls), static_cast<double>(stats.input_bytes) / 1048576.0,
          static_cast<unsigned long long>(stats.candidate_32_calls),
          static_cast<unsigned long long>(stats.candidate_128_calls),
          static_cast<unsigned long long>(opportunity.eligible_calls),
          static_cast<double>(opportunity.eligible_bytes) / 1048576.0,
          static_cast<unsigned long long>(opportunity.would_submit_calls),
          static_cast<unsigned long long>(opportunity.mergeable_calls),
          static_cast<unsigned long long>(opportunity.max_run_calls),
          static_cast<double>(opportunity.max_run_bytes) / 1024.0,
          static_cast<unsigned long long>(stats.queued_calls), static_cast<unsigned long long>(stats.merged_calls),
          static_cast<unsigned long long>(stats.submitted_calls),
          static_cast<double>(stats.submitted_bytes) / 1048576.0,
          static_cast<double>(stats.submitted_source_bytes) / 1048576.0,
          static_cast<double>(stats.driver_ns) / 1.0e6, static_cast<double>(stats.driver_max_ns) / 1.0e6,
          static_cast<unsigned long long>(stats.direct_fallback_calls),
          static_cast<unsigned long long>(stats.allocation_failures),
          static_cast<unsigned long long>(stats.watermark_drains),
          static_cast<unsigned long long>(stats.barrier_drains),
          static_cast<unsigned long long>(stats.present_drains),
          static_cast<unsigned long long>(stats.max_queue_records),
          static_cast<double>(stats.max_queue_bytes) / 1024.0)
    g_bc->upload_opportunity.resetTotals();
    g_bc->upload_stats = {};
    g_bc->upload_last_report_ns = now_ns;
}

bool try_schedule_upload(GLuint frontend, GLintptr offset, GLsizeiptr size, const void* data, bool named) {
    upload_stats_t& stats = g_bc->upload_stats;
    if (named) ++stats.named_calls;
    else ++stats.ordinary_calls;
    if (size > 0) stats.input_bytes += static_cast<std::uint64_t>(size);

    const upload_mode_decision_t decision = upload_mode_decision();
    log_upload_mode_if_changed(decision);
    if (decision.effective == BufferUploadMode::Disabled) return false;

    mg::upload::BufferIdentity identity{};
    std::size_t store_size = 0;
    if (!upload_identity(frontend, offset, size, &identity, &store_size) || data == nullptr) {
        drain_upload_queue(upload_drain_reason_t::Barrier);
        return false;
    }
    if (store_size == kCandidate32MiB) ++stats.candidate_32_calls;
    else ++stats.candidate_128_calls;
    g_bc->upload_opportunity.upload(identity, static_cast<std::uint64_t>(offset), static_cast<std::size_t>(size));

    if (decision.effective == BufferUploadMode::Observe) return false;

    mg::upload::EnqueueResult result =
        g_bc->upload_queue.enqueue(identity, static_cast<std::uint64_t>(offset), static_cast<std::size_t>(size), data);
    if (result == mg::upload::EnqueueResult::Queued || result == mg::upload::EnqueueResult::Merged) {
        ++stats.queued_calls;
        if (result == mg::upload::EnqueueResult::Merged) ++stats.merged_calls;
        stats.max_queue_records = std::max<std::uint64_t>(stats.max_queue_records,
                                                          g_bc->upload_queue.recordCount());
        stats.max_queue_bytes = std::max<std::uint64_t>(stats.max_queue_bytes,
                                                        g_bc->upload_queue.payloadBytes());
        return true;
    }

    if (result == mg::upload::EnqueueResult::AllocationFailure) ++stats.allocation_failures;
    // Preserve call order: earlier queued writes must reach the driver before
    // this write takes the synchronous fallback.
    drain_upload_queue(upload_drain_reason_t::Watermark);
    ++stats.direct_fallback_calls;
    return false;
}

bool is_terrain_upload(GLuint frontend, GLintptr offset, GLsizeiptr size, const void* data) {
    if (frontend == 0 || data == nullptr || offset < 0 || size <= 0 || !has_buffer(frontend) ||
        frontend >= g_buffer_datasize.size() || frontend >= g_buffer_storage_contracts.size() ||
        frontend >= g_buffer_mappings.size()) {
        return false;
    }
    const std::size_t store_size = g_buffer_datasize[frontend];
    const auto& storage = g_buffer_storage_contracts[frontend];
    if (!storage.immutable || storage.requested_flags != mg::buffer_contract::DynamicStorage ||
        !mg::terrain_upload::IsRenderPearlTerrainStore(store_size) || g_buffer_mappings[frontend].active) {
        return false;
    }
    const std::uint64_t begin = static_cast<std::uint64_t>(offset);
    const std::uint64_t count = static_cast<std::uint64_t>(size);
    return begin <= store_size && count <= static_cast<std::uint64_t>(store_size) - begin;
}

} // namespace

extern "C" void mg_buffer_upload_gl_entry(const char* function_name) noexcept {
    const upload_mode_decision_t decision = upload_mode_decision();
    if (decision.effective == BufferUploadMode::Disabled) {
        // A mode can become ineligible after a second shared context appears.
        // If that happens with work pending, drain while the owner is still
        // current instead of leaving a queue that can no longer make progress.
        if (!g_bc->upload_queue.empty()) drain_upload_queue(upload_drain_reason_t::Barrier);
        return;
    }
    log_upload_mode_if_changed(decision);
    if (!is_upload_entry(function_name)) drain_upload_queue(upload_drain_reason_t::Barrier);
}

extern "C" void mg_buffer_upload_present(void) noexcept {
    if (g_bc == &g_buf_ctx_default) return;
    const upload_mode_decision_t decision = upload_mode_decision();
    log_upload_mode_if_changed(decision);
    drain_upload_queue(upload_drain_reason_t::Present);
    if (decision.effective != BufferUploadMode::Disabled) report_upload_stats_if_due();
}

extern "C" void mg_buffer_upload_context_release(void) noexcept {
    if (g_bc == &g_buf_ctx_default) return;
    drain_upload_queue(upload_drain_reason_t::ContextRelease);
}

void glBufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage) {
    LOG()
    // Always-on rather than causal-only: on a pre-1.20 vanilla client this is the
    // only per-frame buffer write path, and none of the four armCapture() triggers
    // can fire there, so a causal-only scope reported observed=0% and left slow
    // frames unattributable. See isAlwaysSelected() in gl/frame_stats_core.h for
    // the full precondition. Nested DSA forwarding is depth-deduplicated.
    MG_FRAME_STATS_SELECTED_GL_SCOPE()
    LOG_D("glBufferData, target = %s, size = %d, data = 0x%x, usage = %s", glEnumToString(target), size, data,
          glEnumToString(usage))
    const GLuint frontend = find_bound_buffer_by_target(target);
    if (frontend != 0 && frontend < g_buffer_storage_contracts.size() &&
        g_buffer_storage_contracts[frontend].immutable) {
        // A mutable provider allocation may back an immutable frontend store.
        // Preserve the desktop contract instead of letting a later BufferData
        // silently replace it.
        mg_set_gl_error(GL_INVALID_OPERATION);
        return;
    }
    borrowed_target_t t(target);
    GLES.glBufferData(t.target, size, data, usage);
    note_buffer_storage(frontend, size, mg::buffer_contract::MutableStorage());
    CHECK_GL_ERROR
}

// Both of these were plain pass-throughs in gl/gl_native.cpp. They live here now
// so that GL_PARAMETER_BUFFER reaches the driver as a target it understands.
void glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data) {
    LOG()
    // Selective telemetry times this public upload even when exhaustive GL
    // accounting is disabled. Nested DSA forwarding is depth-deduplicated.
    MG_FRAME_STATS_SELECTED_GL_SCOPE()
    // Statistics are attached to the outer public wrapper. A core DSA upload
    // reaches here recursively, but frame_stats accepts only the first byte
    // annotation so one transfer cannot be counted twice.
    if (size > 0) mg::frame_stats::recordBufferBytes(static_cast<std::uint64_t>(size));
    LOG_D("glBufferSubData, target = %s, offset = %p, size = %zi", glEnumToString(target), (void*)offset, size)
    const GLuint frontend = find_bound_buffer_by_target(target);
    const bool terrain_upload = is_terrain_upload(frontend, offset, size, data);
    if (terrain_upload) mg::frame_stats::recordTerrainUpload(static_cast<std::uint64_t>(size));
#if AMCL_MG_UPLOAD_SCHEDULER_LAB
    if (try_schedule_upload(frontend, offset, size, data, /*named=*/false)) return;
#endif
    borrowed_target_t t(target);
    GLES.glBufferSubData(t.target, offset, size, data);
    CHECK_GL_ERROR
}

GLboolean mg_buffer_sub_data_named(GLuint buffer, GLintptr offset, GLsizeiptr size, const void* data) {
    const bool terrain_upload = is_terrain_upload(buffer, offset, size, data);
    if (terrain_upload) mg::frame_stats::recordTerrainUpload(static_cast<std::uint64_t>(size));
#if AMCL_MG_UPLOAD_SCHEDULER_LAB
    if (try_schedule_upload(buffer, offset, size, data, /*named=*/true)) return GL_TRUE;
#endif
    if (buffer == 0 || !has_buffer(buffer)) return GL_FALSE;
    const GLuint backend = find_real_buffer(buffer);
    if (backend == 0) return GL_FALSE;
    const GLuint saved = mg_driver_bound_buffer(GL_COPY_WRITE_BUFFER);
    if (saved != backend) GLES.glBindBuffer(GL_COPY_WRITE_BUFFER, backend);
    GLES.glBufferSubData(GL_COPY_WRITE_BUFFER, offset, size, data);
    if (saved != backend) GLES.glBindBuffer(GL_COPY_WRITE_BUFFER, saved);
    CHECK_GL_ERROR
    return GL_TRUE;
}

void glGetBufferParameteriv(GLenum target, GLenum pname, GLint* params) {
    LOG()
    LOG_D("glGetBufferParameteriv, target = %s, pname = %s", glEnumToString(target), glEnumToString(pname))
    const GLuint frontend = find_bound_buffer_by_target(target);
    // BC-01: the application owns the frontend storage contract. A backend may
    // provide a stronger store internally, but exposing those extra flags would
    // let the application select a mapping path it never requested.
    if (params && frontend != 0 && has_buffer(frontend) && frontend < g_buffer_storage_contracts.size() &&
        g_buffer_storage_contracts[frontend].immutable) {
        if (pname == GL_BUFFER_STORAGE_FLAGS) {
            *params = static_cast<GLint>(g_buffer_storage_contracts[frontend].requested_flags);
            return;
        }
        if (pname == GL_BUFFER_IMMUTABLE_STORAGE) {
            *params = GL_TRUE;
            return;
        }
    }
    borrowed_target_t t(target);
    GLES.glGetBufferParameteriv(t.target, pname, params);
    CHECK_GL_ERROR
}

void glGetBufferParameteri64v(GLenum target, GLenum pname, GLint64* params) {
    LOG()
    LOG_D("glGetBufferParameteri64v, target = %s, pname = %s", glEnumToString(target), glEnumToString(pname))
    const GLuint frontend = find_bound_buffer_by_target(target);
    if (params && frontend != 0 && has_buffer(frontend) && frontend < g_buffer_storage_contracts.size() &&
        g_buffer_storage_contracts[frontend].immutable) {
        if (pname == GL_BUFFER_STORAGE_FLAGS) {
            *params = static_cast<GLint64>(g_buffer_storage_contracts[frontend].requested_flags);
            return;
        }
        if (pname == GL_BUFFER_IMMUTABLE_STORAGE) {
            *params = GL_TRUE;
            return;
        }
    }
    borrowed_target_t t(target);
    GLES.glGetBufferParameteri64v(t.target, pname, params);
    CHECK_GL_ERROR
}

void* glMapBuffer(GLenum target, GLenum access) {
    LOG()
    LOG_D("glMapBuffer, target = %s, access = %s", glEnumToString(target), glEnumToString(access))
    GLbitfield required_access = 0;
    switch (access) {
    case GL_READ_ONLY: required_access = GL_MAP_READ_BIT; break;
    case GL_WRITE_ONLY: required_access = GL_MAP_WRITE_BIT; break;
    case GL_READ_WRITE: required_access = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT; break;
    default: break;
    }
    const GLuint frontend = find_bound_buffer_by_target(target);
    const mg::buffer_contract::StorageState storage =
        frontend < g_buffer_storage_contracts.size() ? g_buffer_storage_contracts[frontend]
                                                     : mg::buffer_contract::MutableStorage();
    if (required_access != 0 && !mg::buffer_contract::ApplicationMappingAllowed(
                                    storage, static_cast<std::uint32_t>(required_access))) {
        mg_set_gl_error(GL_INVALID_OPERATION);
        return nullptr;
    }
    if (g_gles_caps.GL_OES_mapbuffer) {
        borrowed_target_t t(target);
        void* result = GLES.glMapBufferOES(t.target, access);
        if (result != nullptr && frontend < g_buffer_mappings.size()) {
            const mg::buffer_contract::MappingDecision legacy{
                static_cast<std::uint32_t>(required_access),
                static_cast<std::uint32_t>(required_access), false};
            g_buffer_mappings[frontend] = mg::buffer_contract::SuccessfulMapping(0, 0, legacy);
        }
        return result;
    }
    GLint buffer_size;
    glGetBufferParameteriv(target, GL_BUFFER_SIZE, &buffer_size);
    if (buffer_size <= 0 || glGetError() != GL_NO_ERROR) {
        return nullptr;
    }
    GLbitfield flags = 0;
    switch (access) {
    case GL_READ_ONLY:
        flags = GL_MAP_READ_BIT;
        break;
    case GL_WRITE_ONLY:
        flags = GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT;
        break;
    case GL_READ_WRITE:
        flags = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT;
        break;
    default:
        return nullptr;
    }
    void* ptr = glMapBufferRange(target, 0, buffer_size, flags);
    return ptr;
}

#if GLOBAL_DEBUG || DEBUG
#include <fstream>
#define BIN_FILE_PREFIX "/sdcard/MG/buf/"
#endif

#if !defined(__APPLE__)
extern "C"
{
    GLAPI GLAPIENTRY void* glMapBufferARB(GLenum target, GLenum access) __attribute__((alias("glMapBuffer")));
    GLAPI GLAPIENTRY void glBufferDataARB(GLenum target, GLsizeiptr size, const void* data, GLenum usage)
        __attribute__((alias("glBufferData")));
    GLAPI GLAPIENTRY GLboolean glUnmapBufferARB(GLenum target) __attribute__((alias("glUnmapBuffer")));
    GLAPI GLAPIENTRY void glBufferStorageARB(GLenum target, GLsizeiptr size, const void* data, GLbitfield flags)
        __attribute__((alias("glBufferStorage")));
    GLAPI GLAPIENTRY void glBindBufferARB(GLenum target, GLuint buffer) __attribute__((alias("glBindBuffer")));
}
#endif

void* glMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access) {
    LOG()
    MG_FRAME_STATS_SELECTED_GL_SCOPE()
    if (length > 0) mg::frame_stats::recordBufferBytes(static_cast<std::uint64_t>(length));
    const GLuint frontend = find_bound_buffer_by_target(target);
    const mg::buffer_contract::StorageState storage =
        frontend < g_buffer_storage_contracts.size() ? g_buffer_storage_contracts[frontend]
                                                     : mg::buffer_contract::MutableStorage();
    if (!mg::buffer_contract::ApplicationMappingAllowed(storage, static_cast<std::uint32_t>(access))) {
        mg_set_gl_error(GL_INVALID_OPERATION);
        return nullptr;
    }
    const auto decision = mg::buffer_contract::DecideMapping(storage, static_cast<std::uint32_t>(access));
    borrowed_target_t t(target);
    void* result = GLES.glMapBufferRange(t.target, offset, length, decision.effective_access);
    auto successful_decision = decision;

    // BC-06: coherent substitution is an optimization, never a new reason for
    // an application mapping to fail. Some GLES providers advertise coherent
    // storage but reject a coherent access combination; consume that internal
    // failure and retry the caller's original access exactly once.
    if (result == nullptr && decision.coherent_substitution) {
        (void)GLES.glGetError();
        result = GLES.glMapBufferRange(t.target, offset, length, access);
        successful_decision = {static_cast<std::uint32_t>(access), static_cast<std::uint32_t>(access), false};
        static bool warned = false;
        if (!warned) {
            warned = true;
            LOG_W_FORCE("buffer coherent mapping substitution failed; falling back to explicit flush")
        }
    }
    if (result != nullptr && frontend < g_buffer_mappings.size()) {
        g_buffer_mappings[frontend] = mg::buffer_contract::SuccessfulMapping(
            static_cast<mg::buffer_contract::Offset>(offset),
            static_cast<mg::buffer_contract::Offset>(length), successful_decision);
        if ((access & GL_MAP_PERSISTENT_BIT) != 0) {
            mg::frame_stats::recordPersistentMap();
#if AMCL_MG_FRAME_STATS_EXHAUSTIVE
            // The selective shipping observer records the persistent-map event
            // above but never logs once per storage generation. RenderPearl
            // recreates many transient stores, so those detailed contract lines
            // remain exhaustive/lab-only to avoid perturbing the render thread.
            const std::uint64_t generation = frontend < g_buffer_storage_generation.size()
                                                 ? g_buffer_storage_generation[frontend]
                                                 : 0;
            if (frontend < g_buffer_mapping_contract_logged_generation.size() &&
                g_buffer_mapping_contract_logged_generation[frontend] != generation) {
                g_buffer_mapping_contract_logged_generation[frontend] = generation;
                LOG_I("[MG-MAP-CONTRACT] schema=1 frontend=%u backend=%u store_kib=%.1f offset=%lld length_kib=%.1f "
                      "storage_requested=0x%x storage_effective=0x%x access_requested=0x%x access_effective=0x%x "
                      "coherent_substitution=%d",
                      frontend, find_real_buffer(frontend),
                      frontend < g_buffer_datasize.size()
                          ? static_cast<double>(g_buffer_datasize[frontend]) / 1024.0
                          : 0.0,
                      static_cast<long long>(offset), static_cast<double>(length) / 1024.0,
                      storage.requested_flags, storage.effective_flags, access,
                      successful_decision.effective_access,
                       successful_decision.coherent_substitution ? 1 : 0)
            }
#endif
        }
    }
    return result;
}

GLboolean glUnmapBuffer(GLenum target) {
    LOG()
    MG_FRAME_STATS_SELECTED_GL_SCOPE()
    LOG_D("%s(%s)", __func__, glEnumToString(target));
    borrowed_target_t t(target);
    if (g_gles_caps.GL_OES_mapbuffer) {
        const GLboolean result = GLES.glUnmapBuffer(t.target);
        const GLuint frontend = find_bound_buffer_by_target(target);
        if (frontend < g_buffer_mappings.size()) g_buffer_mappings[frontend] = {};
        return result;
    }

    GLboolean result = GLES.glUnmapBuffer(t.target);
    const GLuint frontend = find_bound_buffer_by_target(target);
    if (frontend < g_buffer_mappings.size()) g_buffer_mappings[frontend] = {};
    CHECK_GL_ERROR
    return result;
}

void glBufferStorage(GLenum target, GLsizeiptr size, const void* data, GLbitfield flags) {
    LOG()
    // Same policy as glBufferData: the whole BufferAllocation category is
    // always-on so allocation cost stays visible without a causal window.
    MG_FRAME_STATS_SELECTED_GL_SCOPE()
    const GLuint frontend = find_bound_buffer_by_target(target);
    if (frontend != 0 && frontend < g_buffer_storage_contracts.size() &&
        g_buffer_storage_contracts[frontend].immutable) {
        mg_set_gl_error(GL_INVALID_OPERATION);
        return;
    }

    const GLbitfield requested_flags = flags;
    const auto contract = mg::buffer_contract::ImmutableStorage(
        static_cast<std::uint32_t>(requested_flags), global_settings.buffer_coherent_as_flush);
    borrowed_target_t t(target);
    if (GLES.glBufferStorageEXT) {
        flags = static_cast<GLbitfield>(contract.effective_flags);
        GLES.glBufferStorageEXT(t.target, size, data, flags);
    } else {
        mg_set_gl_error(GL_INVALID_OPERATION);
        return;
    }
    note_buffer_storage(frontend, size, contract);

    if (size == static_cast<GLsizeiptr>(kCandidate32MiB) ||
        size == static_cast<GLsizeiptr>(kCandidate128MiB)) {
        LOG_I("[MG-STORAGE] schema=3 size_mib=%llu target=0x%x frontend=%u backend=%u "
              "backend_store=immutable requested_flags=0x%x effective_flags=0x%x initial_data=%d "
              "coherent_as_flush=%d source=%s",
              static_cast<unsigned long long>(size / (1024 * 1024)), target, frontend,
              find_real_buffer(frontend), requested_flags, contract.effective_flags, data != nullptr ? 1 : 0,
              global_settings.buffer_coherent_as_flush ? 1 : 0,
              global_settings.buffer_coherent_as_flush_source == BufferCoherentAsFlushSource::Config
                  ? "config"
                  : "provider-default")
    }
    CHECK_GL_ERROR
}

void glFlushMappedBufferRange(GLenum target, GLintptr offset, GLsizeiptr length) {
    LOG()
    MG_FRAME_STATS_SELECTED_GL_SCOPE()
    if (length > 0) mg::frame_stats::recordBufferBytes(static_cast<std::uint64_t>(length));
    const GLuint frontend = find_bound_buffer_by_target(target);
    const mg::buffer_contract::MappingState mapping =
        frontend < g_buffer_mappings.size() ? g_buffer_mappings[frontend]
                                            : mg::buffer_contract::MappingState{};
    const auto disposition = mg::buffer_contract::DecideFlush(
        mapping, static_cast<mg::buffer_contract::Offset>(offset),
        static_cast<mg::buffer_contract::Offset>(length));
    if (disposition == mg::buffer_contract::FlushDisposition::InvalidValue) {
        mg_set_gl_error(GL_INVALID_VALUE);
        return;
    }
    const bool requested_explicit = mapping.active &&
                                    (mapping.requested_access & GL_MAP_PERSISTENT_BIT) != 0 &&
                                    (mapping.requested_access & GL_MAP_FLUSH_EXPLICIT_BIT) != 0;
    if (requested_explicit) {
        mg::frame_stats::recordExplicitFlush(
            disposition == mg::buffer_contract::FlushDisposition::Suppress);
    }
    if (disposition == mg::buffer_contract::FlushDisposition::Suppress) return;
    borrowed_target_t t(target);
    GLES.glFlushMappedBufferRange(t.target, offset, length);
}

void glGenVertexArrays(GLsizei n, GLuint* arrays) {
    LOG()
    LOG_D("glGenVertexArrays(%i, %p)", n, arrays)
    for (int i = 0; i < n; ++i) {
        arrays[i] = gen_array();
    }
}

void glDeleteVertexArrays(GLsizei n, const GLuint* arrays) {
    LOG()
    LOG_D("glDeleteVertexArrays(%i, %p)", n, arrays)
    for (int i = 0; i < n; ++i) {
        if (find_real_array(arrays[i])) {
            GLuint real_array = find_real_array(arrays[i]);
            GLES.glDeleteVertexArrays(1, &real_array);
            CHECK_GL_ERROR
        }
        remove_array(arrays[i]);
    }
}

GLboolean glIsVertexArray(GLuint array) {
    LOG()
    LOG_D("glIsVertexArray(%d)", array)
    return has_array(array);
}

void glBindVertexArray(GLuint array) {
    LOG()
    LOG_D("glBindVertexArray(%d)", array)
    bound_array = array;

    // update bound ibo
    set_bound_buffer_by_target(GL_ELEMENT_ARRAY_BUFFER, get_ibo_by_vao(array));

    if (!has_array(array) || array == 0) {
        LOG_D("Does not have va=%d found!", array)
        GLES.glBindVertexArray(array);
        CHECK_GL_ERROR
        return;
    }

    GLuint real_array = find_real_array(array);
    if (!real_array) {
        LOG_D("va=%d not initialized, initializing...", array)
        GLES.glGenVertexArrays(1, &real_array);
        modify_array(array, real_array);
        CHECK_GL_ERROR
    }
    LOG_D("glBindVertexArray: %d -> %d", array, real_array)
    GLES.glBindVertexArray(real_array);
    CHECK_GL_ERROR
}
