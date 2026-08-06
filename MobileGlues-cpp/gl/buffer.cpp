// MobileGlues - gl/buffer.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#include "buffer.h"
#include "ankerl/unordered_dense.h"
#include "texture.h"
#include "../diagnostics/counters.h"

#define DEBUG 0

GLuint bound_array;
static GLint maxBufferId = 0;
static GLint maxArrayId = 0;

static std::vector<GLuint> g_gen_buffers;
static std::vector<char> g_gen_buffer_exists;
static std::vector<GLuint> g_free_buffer_ids;

static std::vector<GLuint> g_gen_arrays;
static std::vector<char> g_gen_array_exists;
static std::vector<GLuint> g_free_array_ids;

static std::vector<size_t> g_buffer_datasize;

static std::vector<GLuint> g_element_array_buffer_per_vao;

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
    BINDING_COUNT
};
static std::array<GLuint, BINDING_COUNT> g_bound_buffers_arr = {0};

static inline int ensure_buffer_capacity(GLuint id) {
    if ((int)g_gen_buffers.size() <= (int)id) {
        g_gen_buffers.resize(id + 1, 0);
        g_gen_buffer_exists.resize(id + 1, 0);
        if (g_buffer_datasize.size() <= (size_t)id) g_buffer_datasize.resize(id + 1, 0);
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
        if (id > (GLuint)maxBufferId) maxBufferId = id;
        return id;
    }
    maxBufferId++;
    ensure_buffer_capacity((GLuint)maxBufferId);
    g_gen_buffers[maxBufferId] = 0;
    g_gen_buffer_exists[maxBufferId] = 1;
    g_buffer_datasize[maxBufferId] = 0;
    return (GLuint)maxBufferId;
}

GLboolean has_buffer(GLuint key) {
    return key < g_gen_buffer_exists.size() ? (g_gen_buffer_exists[key] != 0) : 0;
}

void modify_buffer(GLuint key, GLuint value) {
    if (key >= g_gen_buffers.size()) ensure_buffer_capacity(key);
    g_gen_buffers[key] = value;
    if (key >= g_gen_buffer_exists.size()) g_gen_buffer_exists.resize(key + 1, 0);
    g_gen_buffer_exists[key] = 1;
}

#if defined(MG_PLATFORM_OHOS)
void mg_clear_buffer_copy_destination(GLuint key);
#endif

void remove_buffer(GLuint key) {
    if (key < g_gen_buffer_exists.size() && g_gen_buffer_exists[key]) {
        g_gen_buffer_exists[key] = 0;
        g_gen_buffers[key] = 0;
        if (key < g_buffer_datasize.size()) g_buffer_datasize[key] = 0;
#if defined(MG_PLATFORM_OHOS)
        // Fake ids are recycled from the free list below, so any per-buffer record has to be cleared
        // here or the next owner of this id inherits it.
        mg_clear_buffer_copy_destination(key);
#endif
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

size_t get_buffer_data_size(GLuint buffer) {
    if (buffer < g_buffer_datasize.size()) return g_buffer_datasize[buffer];
    return 0;
}

#if defined(MG_PLATFORM_OHOS)
// See gl/buffer.h for why this exists. Measurement only: nothing branches on it yet.
//
// Kept in its own vector rather than added to an existing per-buffer table so that removing the
// probe later is a clean deletion. Cleared in remove_buffer, because fake ids are recycled from a
// free list - a stale flag on a recycled id is exactly the class of bug that made an earlier
// per-buffer table unreliable.
static std::vector<char> g_buffer_is_copy_destination;

void mark_buffer_copy_destination(GLuint buffer) {
    if (buffer == 0) return;
    if (g_buffer_is_copy_destination.size() <= (size_t)buffer) {
        g_buffer_is_copy_destination.resize((size_t)buffer + 1, 0);
    }
    g_buffer_is_copy_destination[buffer] = 1;
}

GLboolean is_buffer_copy_destination(GLuint buffer) {
    if (buffer < g_buffer_is_copy_destination.size()) return g_buffer_is_copy_destination[buffer] != 0;
    return GL_FALSE;
}

void mg_clear_buffer_copy_destination(GLuint key) {
    if (key < g_buffer_is_copy_destination.size()) g_buffer_is_copy_destination[key] = 0;
}

// =============================================================================================
// Deferred terrain upload
//
// This changes *when* a large upload happens. Nothing else. The bytes take the same route, into
// the same storage, with the same access bits.
//
// Why that is the only axis left. Minecraft 26.x declares three 128 MiB terrain vertex heaps and
// one 32 MiB translucent index heap with GL_DYNAMIC_STORAGE_BIT only, never maps them, and writes
// them with glNamedBufferSubData. Every route into such a store was measured on a Maleoon 920:
//
//   mapped write, UNSYNCHRONIZED ....................      0.8 us   fast, and incorrect
//   the same write with UNSYNCHRONIZED dropped ......  24,934 us   worst 104 ms
//   glBufferSubData .................................   3,946 us   to 4,604 us
//   glBufferSubData, promotion withheld .............  55,000 us   peak 188 ms
//   staging buffer plus glCopyBufferSubData ..........          single-digit frame rates
//
// There are 48.2 such writes per second at about 45 presents per second, so the cheapest correct
// route costs roughly 188 ms per second. Nothing fits the frame budget, and per-buffer or
// per-frame gating cannot help either: LevelRenderer.render issues every terrain draw at bytecode
// offset 603 and only then uploads at offset 682, so *every* terrain upload happens after *every*
// terrain draw of its frame. There is no safe subset to keep on the fast path.
//
// That same ordering is what makes deferral free. The bytes written at offset 682 are not read
// until offset 603 of the *next* frame, so moving the write later within this frame cannot make
// any draw see stale data. Moving it past the present is therefore invisible to the application,
// while giving the frame's draws the whole rest of the frame plus the present to retire.
//
// Correctness comes from a fence, not from hope. The queue takes a fence when it opens - at the
// first deferred write of a frame, by which point all of that frame's terrain draws are already
// submitted - and the replay waits on it. So at replay time every draw that could read these
// ranges has provably completed, and the write may then be unsynchronized without racing anything.
//
// How this differs from each rejected row in docs/ohos/PERF-MALEOON.md, since the rule is to say:
//   * "Unsynchronized CPU copy from the staging mapping into the arena": same timing as today,
//     different source. This keeps today's source and changes only the timing.
//   * "Same, with one synchronizing fence per staging batch": that *added* waits, 261-454 ms/s
//     across 41-66 waits/s. This adds one wait per present, at a point where a wait is already
//     being paid and measured at 114 us/s with 89% already-signalled.
//   * "Staging ring plus glCopyBufferSubData": a GPU copy into a different storage class. This
//     issues byte-identical GL calls to today, just later.
//   * "Vertex-source gate v1/v2": routed some uploads to synchronous glBufferSubData. This never
//     uses the synchronous path except on queue overflow.
//   * "Withhold the promotion" / "non-coherent plus explicit flush": change the storage class.
//     This leaves storage untouched.
//
// The residual assumption, stated so it can be checked rather than discovered: nothing between the
// upload and the present reads these heaps. Verified from bytecode for Minecraft 26.2 - the window
// contains only GUI and post-processing, which draw from 512 KiB transient blocks (below the
// threshold, never queued) and from textures. The safety net below covers everything the layer can
// see regardless, and its counter says whether it ever fires.
// =============================================================================================

namespace {

    struct DeferredUpload {
        GLuint realBuffer;
        GLintptr dstOffset;
        GLsizeiptr size;
        size_t srcOffset; // into g_deferred_bytes
    };

    // Host-side staging for the queued bytes. Grown on demand and then reused, never shrunk, so the
    // steady state performs no allocation. Measured need is 48.2 writes/s of at most 371 KB each,
    // one frame's worth at a time.
    std::vector<unsigned char> g_deferred_bytes;
    std::vector<DeferredUpload> g_deferred_records;
    size_t g_deferred_used = 0;
    GLsync g_deferred_fence = nullptr;

    // Cap on queued bytes for a single frame. Beyond this the caller is told to write immediately,
    // which is exactly today's behaviour, so overflow costs performance and never correctness.
    // 32 MiB is far above one frame's measured traffic and still small next to a 128 MiB heap.
    constexpr size_t DEFERRED_BYTES_CAP = 32u * 1024u * 1024u;

    // Replays one record. The destination is bound to GL_COPY_WRITE_BUFFER rather than
    // GL_ARRAY_BUFFER: at replay time the application's vertex-source binding must not be
    // disturbed, and a transfer destination has no business being a live pipeline binding.
    void deferred_replay_one(const DeferredUpload& rec) {
        GLES.glBindBuffer(GL_COPY_WRITE_BUFFER, rec.realBuffer);
        void* p = GLES.glMapBufferRange(GL_COPY_WRITE_BUFFER, rec.dstOffset, rec.size,
                                        GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
        if (p) {
            memcpy(p, g_deferred_bytes.data() + rec.srcOffset, static_cast<size_t>(rec.size));
            GLES.glUnmapBuffer(GL_COPY_WRITE_BUFFER);
            mg::diagnostics::record_deferred_replay(mg::diagnostics::non_negative_bytes(rec.size));
        } else {
            // The driver refused the mapping. Fall back to the ordered call, which is slow but
            // always available; losing the bytes is not an option.
            GLES.glBufferSubData(GL_COPY_WRITE_BUFFER, rec.dstOffset, rec.size,
                                 g_deferred_bytes.data() + rec.srcOffset);
            mg::diagnostics::record_deferred_replay_fallback(mg::diagnostics::non_negative_bytes(rec.size));
        }
    }

    // Waits for the fence taken when the queue opened, so the replay cannot race the draws that
    // were in flight when the bytes were queued.
    //
    // A zero-timeout poll first, because it is measured at 89% already-signalled and costs nothing
    // when it succeeds. Only if that fails is a bounded blocking wait paid - once per present, not
    // once per upload, which is what made every earlier fence experiment unaffordable. The bound is
    // deliberately generous: a late replay costs frame time, an unsynchronized replay costs
    // correctness, and this whole exercise is about not trading the second for the first.
    void deferred_wait_for_fence() {
        if (!g_deferred_fence) return;
        GLenum result = GLES.glClientWaitSync(g_deferred_fence, 0, 0);
        if (result != GL_ALREADY_SIGNALED && result != GL_CONDITION_SATISFIED) {
            result = GLES.glClientWaitSync(g_deferred_fence, GL_SYNC_FLUSH_COMMANDS_BIT, 100000000ULL);
        }
        mg::diagnostics::record_deferred_fence_wait(result);
        GLES.glDeleteSync(g_deferred_fence);
        g_deferred_fence = nullptr;
    }

    void deferred_restore_copy_write_binding() {
        // Put the application's own GL_COPY_WRITE_BUFFER binding back, translating the fake id the
        // shadow holds. Done once per flush rather than once per record.
        glBindBuffer(GL_COPY_WRITE_BUFFER, find_bound_buffer(GL_COPY_WRITE_BUFFER_BINDING));
    }

} // namespace

GLboolean mg_deferred_upload_enqueue(GLuint realBuffer, GLintptr offset, GLsizeiptr size, const void* data) {
    if (realBuffer == 0 || size <= 0 || offset < 0 || !data) return GL_FALSE;
    if (!GLES.glFenceSync || !GLES.glClientWaitSync || !GLES.glDeleteSync || !GLES.glMapBufferRange) return GL_FALSE;

    const size_t need = static_cast<size_t>(size);
    if (need > DEFERRED_BYTES_CAP) {
        // A single upload larger than the whole queue can never be staged.
        mg::diagnostics::record_deferred_overflow();
        return GL_FALSE;
    }
    if (g_deferred_used + need > DEFERRED_BYTES_CAP) {
        // Drain and carry on rather than handing the caller back to the unsynchronized immediate
        // write. Overflow used to fall through to that path, which reinstated the exact race this
        // queue exists to remove - so overflow was silently a correctness hole, not just a slow
        // path. Replaying early costs one fence wait; it never costs correctness.
        mg::diagnostics::record_deferred_overflow();
        mg_deferred_upload_flush_all();
    }

    if (g_deferred_bytes.size() < g_deferred_used + need) {
        // Grow geometrically so a busy frame does not reallocate repeatedly.
        size_t want = g_deferred_bytes.empty() ? (1u << 20) : g_deferred_bytes.size();
        while (want < g_deferred_used + need)
            want *= 2;
        if (want > DEFERRED_BYTES_CAP) want = DEFERRED_BYTES_CAP;
        g_deferred_bytes.resize(want);
    }

    if (g_deferred_records.empty()) {
        // Opening the queue. Take the fence now: every terrain draw of this frame has already been
        // submitted, because Minecraft uploads only after it draws. This fence is what makes the
        // later unsynchronized replay safe.
        g_deferred_fence = GLES.glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    }

    memcpy(g_deferred_bytes.data() + g_deferred_used, data, need);
    g_deferred_records.push_back(DeferredUpload{realBuffer, offset, size, g_deferred_used});
    g_deferred_used += need;

    mg::diagnostics::record_deferred_enqueue(mg::diagnostics::non_negative_bytes(size));
    return GL_TRUE;
}

void mg_deferred_upload_flush_all(void) {
    if (g_deferred_records.empty()) {
        // The fence is only ever taken alongside a record, but be defensive: leaking sync objects
        // is not free on this driver.
        if (g_deferred_fence) {
            GLES.glDeleteSync(g_deferred_fence);
            g_deferred_fence = nullptr;
        }
        return;
    }

    deferred_wait_for_fence();
    for (const DeferredUpload& rec : g_deferred_records)
        deferred_replay_one(rec);
    deferred_restore_copy_write_binding();

    g_deferred_records.clear();
    g_deferred_used = 0;
}

void mg_deferred_upload_flush_buffer(GLuint realBuffer) {
    if (g_deferred_records.empty()) return;

    bool any = false;
    for (const DeferredUpload& rec : g_deferred_records) {
        if (rec.realBuffer == realBuffer) {
            any = true;
            break;
        }
    }
    if (!any) return;

    // Order matters more than selectivity here: records for one buffer may overlap records for
    // another only through the application's own aliasing, but replaying out of order would break
    // the sequence the application issued. So flush everything, which is also what keeps this
    // simple enough to be obviously correct.
    mg::diagnostics::record_deferred_forced_flush();
    mg_deferred_upload_flush_all();
}

GLuint mg_real_buffer_for_target(GLenum target) {
    const GLenum bindingQuery = get_binding_query(target);
    if (!bindingQuery) return 0;
    return find_real_buffer(find_bound_buffer(bindingQuery));
}

void mg_deferred_upload_flush_if_range(GLuint realBuffer, GLintptr offset, GLsizeiptr size) {
    if (g_deferred_records.empty() || realBuffer == 0) return;

    // A negative or absent size means "the whole buffer" - a respecification, or a query whose
    // extent this layer does not know. Treat it as overlapping everything on that buffer.
    const bool wholeBuffer = (size <= 0);
    const GLintptr end = wholeBuffer ? 0 : offset + size;

    for (const DeferredUpload& rec : g_deferred_records) {
        if (rec.realBuffer != realBuffer) continue;
        if (wholeBuffer) {
            mg::diagnostics::record_deferred_forced_flush();
            mg_deferred_upload_flush_all();
            return;
        }
        const GLintptr recEnd = rec.dstOffset + rec.size;
        if (rec.dstOffset < end && offset < recEnd) {
            mg::diagnostics::record_deferred_forced_flush();
            mg_deferred_upload_flush_all();
            return;
        }
    }
}

void mg_deferred_upload_flush_for_draw(void) {
    if (g_deferred_records.empty()) return;

    // Why the multi-draw entry points, and why that is both necessary and sufficient.
    //
    // The queue is only safe if the bytes land before any draw that reads them. For Minecraft's own
    // renderer that is free: LevelRenderer.render draws at bytecode offset 603 and uploads at 682,
    // so at draw time the queue is empty and this never fires.
    //
    // Sodium is the reverse, and this was missed for a round. Its terrain work runs from
    // Minecraft.renderFrame offset 440 - LevelExtractorMixin.cullTerrain -> setupTerrain ->
    // updateChunks (439) and processChunkBuilds (461) - and finalizeRenderLists at 502 builds THIS
    // frame's draw lists afterwards. The draws then arrive at renderFrame offset 526. So Sodium
    // uploads before it draws, in the same frame, and replaying only at the present would hand its
    // draws the previous tenant of a freed-and-reused arena segment. That is the residual
    // intermittent corruption: intermittent because Sodium's own glCopyBufferSubData traffic was
    // accidentally draining the queue most of the time, and only batches consisting purely of
    // ring-overflow writeToBuffer calls - where MappedStagingBuffer.flush() early-returns on an
    // empty pendingCopies - slipped through.
    //
    // Sufficient: Sodium's terrain draws go through glMultiDrawElementsBaseVertex. Necessary: a
    // hook on *every* draw would fire on Minecraft's first GUI draw right after offset 682, where
    // the queue's fence was taken microseconds earlier and would therefore block mid-frame - which
    // is the stall this whole design exists to avoid. So the hook has to be narrow, and multi-draw
    // is the narrowest place that still covers the case that needs covering.
    //
    // Cheap for Sodium too: the fence was taken during updateChunks, early in the frame, so it
    // covers the *previous* frame's draws, which have had a whole frame plus a present to retire.
    // The poll should therefore succeed and the replay costs one memcpy per record.
    mg::diagnostics::record_deferred_forced_flush();
    mg_deferred_upload_flush_all();
}

GLboolean mg_deferred_upload_pending(void) {
    return g_deferred_records.empty() ? GL_FALSE : GL_TRUE;
}
#endif

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
    default:
        return -1;
    }
}

void set_bound_buffer_by_target(GLenum target, GLuint buffer) {
    int idx = binding_target_to_index(target);
    if (idx >= 0) g_bound_buffers_arr[idx] = buffer;
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
    g_gen_buffers.resize(1, 0);
    g_gen_buffer_exists.resize(1, 0);
    g_buffer_datasize.resize(1, 0);
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

#if defined(MG_PLATFORM_OHOS)
GLboolean mg_deferred_upload_pending(void);
void mg_deferred_upload_flush_buffer(GLuint realBuffer);
void mg_deferred_upload_flush_all(void);
#endif

void glDeleteBuffers(GLsizei n, const GLuint* buffers) {
    LOG()
    LOG_D("glDeleteBuffers(%i, %p)", n, buffers)
    for (int i = 0; i < n; ++i) {
        if (find_real_buffer(buffers[i])) {
            GLuint real_buff = find_real_buffer(buffers[i]);
#if defined(MG_PLATFORM_OHOS)
            // A queued upload holds this driver name. Replaying it after the delete would write
            // into a dead or already-recycled buffer, so the queue has to drain first. This is the
            // one safety-net site where skipping it is not a visual artifact but memory corruption.
            if (mg_deferred_upload_pending()) mg_deferred_upload_flush_buffer(real_buff);
#endif
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

struct atomic_buffer {
    GLuint id;
    GLsizeiptr size;
    GLintptr offset;
};

static std::vector<atomic_buffer> g_buffer_map_atomic_buffer_info;
static std::vector<GLuint> g_buffer_map_ssbo_id; // shall we use this in the future?

void bindAllAtomicCounterAsSSBO() {
    const size_t count = g_buffer_map_atomic_buffer_info.size();
    for (size_t i = 0; i < count; ++i) {
        atomic_buffer buf = g_buffer_map_atomic_buffer_info[i];
        if (buf.id != 0) {
            GLuint realID = find_real_buffer(buf.id);
            GLES.glBindBufferRange(GL_SHADER_STORAGE_BUFFER, i, realID, buf.offset, buf.size);
            LOG_D("Bound atomic counter buffer %u(real: %u) as SSBO at index %zu", buf, realID, i);
        }
    }
}

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
    if (target == GL_ATOMIC_COUNTER_BUFFER) {
        if (g_buffer_map_atomic_buffer_info.empty()) {
            g_buffer_map_atomic_buffer_info.resize(GL_MAX_ATOMIC_COUNTER_BUFFER_BINDINGS, {});
        }
        g_buffer_map_atomic_buffer_info[index] = {buffer, size, offset};
    }
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

        GLint boundTexture = 0;
        GLint prev_pixel_buffer_binding = 0;

        GLES.glActiveTexture(GL_TEXTURE0 + 15);

        GLES.glGetIntegerv(GL_TEXTURE_BINDING_2D, &boundTexture);
        LOG_D("Current GL_TEXTURE_BINDING_BUFFER = %d", boundTexture);
        GLES.glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &prev_pixel_buffer_binding);
        LOG_D("Previous GL_PIXEL_UNPACK_BUFFER_BINDING = %d", prev_pixel_buffer_binding);

        if (!boundTexture) {
            LOG_D("No texture bound to GL_TEXTURE_BUFFER, skipping emulation.");
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
        GLuint pixelSize = get_internal_format_size(internalformat);
        GLuint numElements = bufferSize / pixelSize;

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
        GLES.glTexImage2D(GL_TEXTURE_2D, 0, internalformat, width, height, 0, GL_RED_INTEGER, GL_BYTE, nullptr);

        GLES.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, real_buffer);

        for (GLuint row = 0; row < height; ++row) {
            void* offset = (void*)(row * width * pixelSize);
            GLES.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, row, width, 1, GL_RED_INTEGER, GL_BYTE, offset);
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

void glBufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage) {
    LOG()
    LOG_D("glBufferData, target = %s, size = %d, data = 0x%x, usage = %s", glEnumToString(target), size, data,
          glEnumToString(usage))
#if defined(MG_PLATFORM_OHOS)
    // Respecifying a store destroys it, so a queued write aimed at the old allocation must land
    // first - or it would be applied to a fresh allocation at a stale offset. Whole-buffer scope,
    // which is what passing size <= 0 to the range form means.
    if (mg_deferred_upload_pending()) {
        mg_deferred_upload_flush_if_range(find_real_buffer(find_bound_buffer(get_binding_query(target))), 0, 0);
    }
#endif
    GLES.glBufferData(target, size, data, usage);
    set_buffer_data_size(find_bound_buffer(target), size);
    CHECK_GL_ERROR
}

void* glMapBuffer(GLenum target, GLenum access) {
    LOG()
    LOG_D("glMapBuffer, target = %s, access = %s", glEnumToString(target), glEnumToString(access))
    if (g_gles_caps.GL_OES_mapbuffer) {
        return GLES.glMapBufferOES(target, access);
    }
    GLint buffer_size;
    GLES.glGetBufferParameteriv(target, GL_BUFFER_SIZE, &buffer_size);
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
#if defined(MG_PLATFORM_OHOS)
    // The application is about to look at buffer contents itself. Anything queued that overlaps the
    // range it asked for has to be in place first, or it would read bytes this layer has accepted
    // but not yet written.
    if (mg_deferred_upload_pending()) {
        mg_deferred_upload_flush_if_range(find_real_buffer(find_bound_buffer(get_binding_query(target))), offset,
                                          length);
    }
#endif
    if (global_settings.buffer_coherent_as_flush) access &= ~GL_MAP_FLUSH_EXPLICIT_BIT;
    //    access |= GL_MAP_UNSYNCHRONIZED_BIT;
    const uint64_t startNs = mg::diagnostics::timestamp();
    void* result = GLES.glMapBufferRange(target, offset, length, access);
    // Mapping can block: without GL_MAP_UNSYNCHRONIZED_BIT the driver waits for readers of the
    // range, so this is one of the places a stall hides behind a call that looks cheap.
    mg::diagnostics::record_map(mg::diagnostics::non_negative_bytes(length), mg::diagnostics::elapsed_ns(startNs));
    return result;
}

GLboolean glUnmapBuffer(GLenum target) {
    LOG()
    LOG_D("%s(%s)", __func__, glEnumToString(target));
    if (g_gles_caps.GL_OES_mapbuffer) return GLES.glUnmapBuffer(target);

    GLboolean result = GLES.glUnmapBuffer(target);
    CHECK_GL_ERROR
    return result;
}

void glBufferStorage(GLenum target, GLsizeiptr size, const void* data, GLbitfield flags) {
    LOG()
#if defined(MG_PLATFORM_OHOS)
    // Record the immutable size so an upload never has to ask the driver for GL_BUFFER_SIZE.
    // glBufferData already does this; an immutable store had no equivalent, which left
    // glNamedBufferSubData querying the driver on every call. See DSAWrapper.cpp.
    {
        const GLenum bindingQuery = get_binding_query(target);
        if (bindingQuery && size > 0) set_buffer_data_size(find_bound_buffer(bindingQuery), static_cast<size_t>(size));
    }
#endif
    if (GLES.glBufferStorageEXT) {
        if (global_settings.buffer_coherent_as_flush &&
            ((flags & GL_MAP_PERSISTENT_BIT) != 0 || (flags & GL_DYNAMIC_STORAGE_BIT) != 0))
            flags |= (GL_MAP_WRITE_BIT | GL_MAP_COHERENT_BIT | GL_MAP_PERSISTENT_BIT);
        GLES.glBufferStorageEXT(target, size, data, flags);
    }
    CHECK_GL_ERROR
}

void glFlushMappedBufferRange(GLenum target, GLintptr offset, GLsizeiptr length) {
    LOG()
    // A coherent mapping publishes writes without cache maintenance, so the driver call is
    // skipped. Counting the skips separately makes that visible instead of looking like a flush
    // that silently did nothing.
    const bool callDriver = !global_settings.buffer_coherent_as_flush;
    const uint64_t startNs = mg::diagnostics::timestamp();
    if (callDriver) GLES.glFlushMappedBufferRange(target, offset, length);
    mg::diagnostics::record_flush(mg::diagnostics::non_negative_bytes(length), callDriver,
                                  mg::diagnostics::elapsed_ns(startNs));
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
