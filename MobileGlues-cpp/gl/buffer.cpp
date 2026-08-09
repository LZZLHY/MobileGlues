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

// ---- How far the storage promotion reaches, and what may be assumed because of it -------------
//
// glBufferStorage promotes stores to MAP_WRITE | PERSISTENT | COHERENT. Four other places in this
// file then *rely* on the COHERENT half having been granted, and until now that reliance was
// unconditional - which is why the two halves could not be separated and why 6.12's experiment had
// to withdraw all three bits at once and could not tell which one mattered.
//
// These two functions split the question. `mg_promote_mode()` is the single reader of the
// environment variable; `mg_coherence_promised()` is the predicate every dependent site now asks
// instead of assuming. When it is true nothing changes - the default path stays bit-identical to
// today, which is what keeps the three vanilla configurations out of the blast radius.
//
// Modes (see MyApplication/entry/src/main/cpp/platform/mg_config.cpp for the device-side plumbing):
//    1  (default)  promote WRITE | COHERENT | PERSISTENT on every eligible store - historical
//    2             promote WRITE | PERSISTENT, withhold COHERENT
//    0             promote only stores the application itself asked to map
//   -1             no promotion at all
//
// Why mode 2 exists: the buffers the GPU reads terrain vertices and indices out of every frame carry
// effective flags 0x1c2, and coherent buffer memory on this class of GPU is typically uncached. The
// application is blocked on its own fence about 6 ms per frame - 29% of a frame - so the binding
// constraint is GPU throughput, and this is the one thing this layer does to those buffers that
// could plausibly cost GPU read bandwidth. PERSISTENT and MAP_WRITE are what let *this* layer map
// them, per EXT_buffer_storage, and mode 2 keeps both; only the memory type changes. That is exactly
// the mechanism 6.12 said this axis must not be re-opened without.
//
// Read once. It must not change between two stores in one session, or a store created early and a
// store created late would disagree about what may be assumed of them.
static int mg_promote_mode() {
    static const int mode = [] {
        const char* v = getenv("MG_PROMOTE_COHERENT");
        const int m = (!v || !*v) ? 1 : atoi(v);
        // LOG_I rather than LOG_D: release builds compile LOG_D out, and which mode a device session
        // actually ran under is the first thing any later reading of that session depends on. It also
        // puts the string in the shipped .so, so the build can be verified without running it.
        LOG_I("[MG-PROMOTE] mode=%d (%s)", m,
              m == 1   ? "WRITE|PERSISTENT|COHERENT, historical"
              : m == 2 ? "WRITE|PERSISTENT, COHERENT withheld"
              : m == 0 ? "only stores the application asked to map"
                       : "no promotion");
        return m;
    }();
    return mode;
}

// True iff a store this layer promoted can be assumed coherent. Mode 2 withholds the bit, and modes
// 0 and -1 may leave a store unpromoted altogether, so none of them may be assumed coherent.
static bool mg_coherence_promised() {
    return mg_promote_mode() == 1;
}

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
// Both defined further down, next to the persistent-mapping cache. remove_buffer sits above it
// because it belongs with the id allocator, so the two are declared here.
static void mg_pmap_forget(GLuint buffer);
#endif

// Drops every binding record that still names a buffer being deleted.
//
// This implements GL's implicit unbind: deleting a buffer detaches it from the binding points it
// occupies, and afterwards those points read as 0. Without it the shadow keeps naming a dead fake
// id, and three separate things then go wrong - all of them reachable, none of them theoretical:
//
//   1. restoreTemporaryBufferBinding (gl/ExtWrappers/DSAWrapper.cpp, 18 call sites) and
//      deferred_restore_copy_write_binding below both put the shadow's value back with
//      glBindBuffer. For a deleted id has_buffer() is false, so glBindBuffer takes its
//      `GLES.glBindBuffer(target, buffer)` branch and hands the *fake* number to the driver as if
//      it were a driver name. Fake ids and driver names are both small sequential integers, so
//      this usually lands on some unrelated live buffer.
//   2. gen_buffer recycles from g_free_buffer_ids with back()/pop_back(), i.e. LIFO - the id just
//      released is the next one handed out. A buffer created right after a delete therefore tends
//      to get the same fake id that the shadow still holds, and temporarilyBindBuffer then sees
//      prev == bufferID and *skips the rebind* on the strength of the shadow alone. The driver
//      stays bound to whatever case 1 left there, and the following write goes into the wrong
//      buffer.
//   3. get_buffer_data_size, the dedup shadow, the deferred queue's range tests and
//      glGetIntegerv(GL_*_BUFFER_BINDING) all resolve through this shadow, so each of them
//      silently answers for a buffer that no longer exists.
//
// Minecraft 26.2 makes this reachable rather than hypothetical. GlStateManager._glDeleteBuffers is
// 26 bytes of assert plus a Tracy counter plus GL33C.glDeleteBuffers - no unbind, and no shadow of
// its own, unlike _glBindFramebuffer in the same class which does track readFbo/writeFbo. Meanwhile
// GlCommandEncoder.drawFromBuffers binds GL_ELEMENT_ARRAY_BUFFER and never binds 0 (the literal
// 34963 appears four times in the whole client, never with 0), and
// VertexArrayCache$Emulated.setupCombinedAttributes does the same for GL_ARRAY_BUFFER. Sodium
// issues no native GL at all - its arena buffers, usage 120 = VERTEX|INDEX|COPY_SRC|COPY_DST, are
// released through GpuBuffer.close() from releaseBufferForReuse, transferSegments and
// deleteSingleOwner as regions come and go. So a vertex or index buffer that is still the resting
// binding gets deleted, repeatedly, at region granularity.
//
// The element-array slot of g_bound_buffers_arr is never read - find_bound_buffer answers
// GL_ELEMENT_ARRAY_BUFFER_BINDING from get_ibo_by_vao instead - so the per-VAO table has to be
// swept as well or the index-buffer half of this stays broken.
//
// Deviation from the specification, stated because it is deliberate: GL detaches a deleted buffer
// from the element-array binding of the *currently bound* vertex array only, leaving the name
// dangling in other vertex arrays. This sweeps every vertex array. That is safe here and strictly
// more useful: the value feeds this layer's own bookkeeping, where a dead fake id is never the
// right answer, and nothing in the application depends on the dangling one - Blaze3D rebinds
// GL_ELEMENT_ARRAY_BUFFER before every indexed draw (drawFromBuffers, executeDraws,
// executeDrawIndirect) and VertexArrayCache's cache-hit path only restores vertex attributes, never
// the element array. Nothing reads the binding back either: the only glGetInteger calls in the
// client are for label lengths, timestamps and capability probes.
static void unbind_deleted_buffer(GLuint key) {
    if (key == 0) return;

    unsigned targets = 0;
    for (GLuint& slot : g_bound_buffers_arr) {
        if (slot == key) {
            slot = 0;
            targets++;
        }
    }

    unsigned vaos = 0;
    for (GLuint& ibo : g_element_array_buffer_per_vao) {
        if (ibo == key) {
            ibo = 0;
            vaos++;
        }
    }

#if defined(MG_PLATFORM_OHOS)
    // Whether this ever fires is the whole question about the region-misplacement artifact, so it
    // is counted rather than assumed. See diagnostics/counters.h unbind_on_delete_calls.
    mg::diagnostics::record_unbind_on_delete(targets, vaos);
#else
    (void)targets;
    (void)vaos;
#endif
}

void remove_buffer(GLuint key) {
    if (key < g_gen_buffer_exists.size() && g_gen_buffer_exists[key]) {
        g_gen_buffer_exists[key] = 0;
        g_gen_buffers[key] = 0;
        if (key < g_buffer_datasize.size()) g_buffer_datasize[key] = 0;
        // Before the id reaches the free list, because gen_buffer hands it straight back out.
        unbind_deleted_buffer(key);
#if defined(MG_PLATFORM_OHOS)
        // Fake ids are recycled from the free list below, so any per-buffer record has to be cleared
        // here or the next owner of this id inherits it.
        mg_clear_buffer_copy_destination(key);
        // Order matters: account for the eviction while the record still exists, then wipe the slot
        // so the next owner of this recycled id does not inherit the "declined" flag or a stale
        // write count. The mapping itself needs no glUnmapBuffer - glDeleteBuffers has already run
        // by the time remove_buffer is reached, and deleting a buffer releases its mapping.
        mg_pmap_drop(key, /*boundTarget=*/0, MG_PMAP_EVICT_DELETED);
        mg_pmap_forget(key);
        // The store is gone, so the application's mapping of it is gone too - and the fake id is about
        // to be handed to a different buffer, which must not inherit the pointer.
        mg_appmap_forget(key, MG_APPMAP_FORGET_DELETED);
        mg_subdata_invalidate(key);
        mg_record_store(key, 0, 0, /*entry=*/0);
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

// ---- Redundant sub-data elimination ---------------------------------------------------------
//
// See gl/buffer.h for why this exists and what its correctness depends on. One slot per buffer is
// enough for the case it targets: the duplicate writes are to the SAME offset, so remembering the
// most recent small write per buffer catches all of them.
namespace {
    constexpr GLsizeiptr SUBDATA_SHADOW_MAX = 8; // only tiny writes are worth remembering

    struct SubDataShadow {
        GLintptr offset = 0;
        GLsizeiptr size = 0; // 0 means the slot holds nothing
        unsigned char bytes[SUBDATA_SHADOW_MAX] = {};
    };

    std::vector<SubDataShadow> g_subdata_shadow;
} // namespace

GLboolean mg_subdata_is_redundant(GLuint buffer, GLintptr offset, GLsizeiptr size, const void* data) {
    if (!data || size <= 0 || size > SUBDATA_SHADOW_MAX) return GL_FALSE;
    if (buffer >= g_subdata_shadow.size()) return GL_FALSE;
    const SubDataShadow& s = g_subdata_shadow[buffer];
    if (s.size != size || s.offset != offset) return GL_FALSE;
    return memcmp(s.bytes, data, static_cast<size_t>(size)) == 0 ? GL_TRUE : GL_FALSE;
}

void mg_subdata_record(GLuint buffer, GLintptr offset, GLsizeiptr size, const void* data) {
    if (!data || size <= 0 || size > SUBDATA_SHADOW_MAX) {
        // Too large to remember, so the slot no longer describes the buffer.
        mg_subdata_invalidate(buffer);
        return;
    }
    if (buffer == 0) return;
    if (g_subdata_shadow.size() <= (size_t)buffer) g_subdata_shadow.resize((size_t)buffer + 1);
    SubDataShadow& s = g_subdata_shadow[buffer];
    s.offset = offset;
    s.size = size;
    memcpy(s.bytes, data, static_cast<size_t>(size));
}

void mg_subdata_invalidate(GLuint buffer) {
    if (buffer < g_subdata_shadow.size()) g_subdata_shadow[buffer].size = 0;
}

// ---- Store creation record ------------------------------------------------------------------
// See gl/buffer.h. Identification only; nothing branches on it.
namespace {
    struct StoreRecord {
        GLbitfield requested = 0;
        GLbitfield effective = 0;
        int entry = 0; // 0 unknown, 1 glBufferStorage, 2 glBufferData
    };

    std::vector<StoreRecord> g_store_records;
} // namespace

void mg_record_store(GLuint buffer, GLbitfield requested, GLbitfield effective, int entry) {
    if (buffer == 0) return;
    if (g_store_records.size() <= (size_t)buffer) g_store_records.resize((size_t)buffer + 1);
    StoreRecord& s = g_store_records[buffer];
    s.requested = requested;
    s.effective = effective;
    s.entry = entry;
}

GLbitfield mg_store_requested_flags(GLuint buffer) {
    return buffer < g_store_records.size() ? g_store_records[buffer].requested : 0;
}

GLbitfield mg_store_effective_flags(GLuint buffer) {
    return buffer < g_store_records.size() ? g_store_records[buffer].effective : 0;
}

int mg_store_entry(GLuint buffer) {
    return buffer < g_store_records.size() ? g_store_records[buffer].entry : 0;
}

// ---- Layer-owned persistent mapping for hot small writes -------------------------------------
// See gl/buffer.h for what this is for, what it measured, and what it deliberately gives up.
namespace {

    // Upper bound on a store this layer will hold mapped.
    //
    // Chosen to sit clear of both neighbours rather than tuned: the store this exists for is 229,376
    // bytes and Sodium's uniform ring is 262,144, while DIRECT_MAP_MIN_SIZE in
    // gl/ExtWrappers/DSAWrapper.cpp is 16 MiB. 4 MiB is an order of magnitude above the former and
    // two below the latter, so the two policies cannot both claim a buffer and the amount of address
    // space held open stays trivial.
    constexpr size_t PMAP_MAX_BYTES = 4u * 1024u * 1024u;

    // Only writes this small count towards adoption. The writes this targets are 4 bytes, with a
    // 1,024-byte second kind when a region empties (UniformBufferManager.clearRegionTimes), and the
    // measured mean was 8.2 bytes. A larger write is a different access pattern and should not be
    // able to drag a buffer into a mapping.
    constexpr GLsizeiptr PMAP_MAX_WRITE = 1024;

    // How many small writes a store has to take before it earns a mapping.
    //
    // The point is to exclude one-shot buffers, of which the application creates many. 64 is reached
    // by the target store within the first second or two of terrain building - it took 9,301 writes
    // over the measured session - and is never reached by a buffer written once at construction.
    constexpr uint32_t PMAP_ADOPT_AFTER = 64;

    // How many times the application may take the mapping away before this layer stops trying.
    //
    // An application mapping used to bar a buffer permanently, and that was wrong in a way the first
    // device round demonstrated: the store this cache exists for is mapped twice by
    // UniformBufferManager.<init> at construction and then never again (measured, maps = 2), so a
    // single transient mapping at world load was enough to lock the buffer out for the whole session.
    // The result was that the cache adopted the cheap 64-byte uniform buffers - 148 writes/s at
    // microseconds each - and never touched the 229,376-byte store that carries 100% of the cost.
    //
    // So an application mapping now releases and resets rather than bars, and only a buffer that
    // keeps taking the mapping back is given up on. Four strikes rather than one, because two is
    // indistinguishable from construction plus one resize; if this ever needs raising, the
    // pmap_evict_app_map counter will say so instead of the cache silently doing nothing.
    constexpr uint32_t PMAP_APP_MAP_STRIKES = 4;

    struct PersistentMap {
        void* base = nullptr;
        size_t length = 0;
        uint32_t smallWrites = 0;
        uint32_t appMapStrikes = 0;
        bool declined = false;  // barred permanently; something else needs the mapping
        bool needsFlush = false; // the accepted mapping is not coherent, so each write must be flushed
    };

    std::vector<PersistentMap> g_pmap;
    size_t g_pmap_live = 0;

    // "This buffer has been bound as an index buffer at least once."
    //
    // Replaces a call to mg_pmap_drop from glBindBuffer, which was firing about 5,000 times a second -
    // once per indexed draw - and doing a vector grow plus a counter update each time to record a bar
    // that a single flag expresses. The bar itself still has to exist for every index buffer, even one
    // this cache has never considered, or a buffer used for indices early and written hot later would
    // slip through; see the comment in glBindBuffer for why an index buffer must never be held mapped.
    std::vector<char> g_pmap_ever_element;

    // The access masks to try, in the order they are tried, and why this is a ladder rather than one
    // mask.
    //
    // The first version requested WRITE | PERSISTENT | COHERENT and treated a null return as "this
    // driver cannot do it, give up on the buffer". On device the mapping of the one store that matters
    // failed and the store was barred for the whole session, which is why three rounds of this fix
    // changed nothing. The justification in the commit was that "a persistent mapping of it succeeds,
    // access 0x62" - but 0x62 is WRITE | UNSYNCHRONIZED | PERSISTENT, with no COHERENT bit. The
    // application never asked for coherence on the mapping, so nothing had ever established that this
    // driver grants it.
    //
    // So the driver is asked instead of assumed. Entry 4 is the mask the application itself uses on
    // this very buffer and is therefore known to be accepted; the earlier entries are preferred
    // because they need no cache maintenance, and entries 2 and 3 need a flush per write, which is a
    // cost this build measures rather than predicts. The ladder was the right shape for the wrong
    // reason and is kept, because asking beats assuming either way - but note what it actually proved:
    // rung 0 was granted (pmap_accepted_access = 0xc2), so this driver does grant coherent mappings of
    // a promoted store.
    //
    // CORRECTED 2026-08-07; see RENDER-ADAPTATION.md 6.15. This comment used to cite 6.4 and assert
    // that "coherence is a property requested on the mapping, and this layer only ever sets it on
    // storage". Per EXT_buffer_storage that is backwards: coherence is a property of the STORE, the
    // access bit merely has to agree with it, and a mapping made with MapBufferRange of a coherent
    // store gets coherent behaviour. Which also means rungs 0 and 1 are refused only when the store
    // itself lacks the bit - so under promote mode 2 they are skipped rather than tried, and rung 4,
    // which can publish nothing at all, is skipped as a correctness requirement.
    struct PmapAccess {
        GLbitfield mask;
        bool needsFlush;
    };

    constexpr PmapAccess PMAP_LADDER[] = {
        {GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT, false},
        {GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT | GL_MAP_UNSYNCHRONIZED_BIT, false},
        {GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_FLUSH_EXPLICIT_BIT, true},
        {GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_FLUSH_EXPLICIT_BIT | GL_MAP_UNSYNCHRONIZED_BIT, true},
        // Last resort: byte-identical to what GlBuffer$Direct gets after this layer strips
        // GL_MAP_FLUSH_EXPLICIT_BIT, and measured to succeed on this store. It offers no way to publish
        // a write explicitly, so using it means relying on the same unproven premise as 6.4 - which is
        // why it is last and is counted separately.
        {GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_UNSYNCHRONIZED_BIT, false},
    };

    PersistentMap* pmap_find(GLuint buffer) {
        if (buffer == 0 || buffer >= g_pmap.size()) return nullptr;
        return &g_pmap[buffer];
    }

    PersistentMap& pmap_slot(GLuint buffer) {
        if (g_pmap.size() <= (size_t)buffer) g_pmap.resize((size_t)buffer + 1);
        return g_pmap[buffer];
    }

} // namespace

GLboolean mg_pmap_any(void) {
    return g_pmap_live != 0 ? GL_TRUE : GL_FALSE;
}

void mg_pmap_note_element(GLuint buffer) {
    if (buffer == 0) return;
    if (g_pmap_ever_element.size() <= (size_t)buffer) g_pmap_ever_element.resize((size_t)buffer + 1, 0);
    g_pmap_ever_element[buffer] = 1;
}

GLboolean mg_pmap_write(GLenum target, GLuint buffer, GLintptr offset, GLsizeiptr size, const void* data) {
    if (g_pmap_live == 0 || !data || size <= 0 || offset < 0) return GL_FALSE;
    PersistentMap* m = pmap_find(buffer);
    if (!m || !m->base) return GL_FALSE;
    // A write outside the mapping would be memory corruption rather than a wrong pixel, so it falls
    // back to the ordinary path instead of being clamped.
    if ((size_t)offset + (size_t)size > m->length) return GL_FALSE;

    const uint64_t startNs = mg::diagnostics::timestamp();
    memcpy(static_cast<unsigned char*>(m->base) + offset, data, static_cast<size_t>(size));
    mg::diagnostics::record_pmap_write(mg::diagnostics::non_negative_bytes(size),
                                       mg::diagnostics::elapsed_ns(startNs));

    // A coherent mapping publishes the write with no further call. A non-coherent one does not, and
    // the only way to publish it is glFlushMappedBufferRange on a mapping that asked for
    // GL_MAP_FLUSH_EXPLICIT_BIT - which is why the ladder pairs the two. This is cache maintenance,
    // not synchronization, so it should be cheap; "should be" is why it is timed separately rather
    // than folded into the write.
    if (m->needsFlush && GLES.glFlushMappedBufferRange) {
        const uint64_t flushStartNs = mg::diagnostics::timestamp();
        GLES.glFlushMappedBufferRange(target, offset, size);
        mg::diagnostics::record_pmap_flush(mg::diagnostics::elapsed_ns(flushStartNs));
    }
    return GL_TRUE;
}

void mg_pmap_consider(GLenum target, GLuint buffer, GLsizeiptr size, size_t bufferBytes) {
    if (buffer == 0 || size <= 0 || size > PMAP_MAX_WRITE) return;
    if (bufferBytes == 0 || bufferBytes > PMAP_MAX_BYTES) return;
    if (!GLES.glMapBufferRange) return;

    // An index buffer must never be held mapped; see glBindBuffer for what goes wrong. Tested here
    // rather than barred at bind time so the bind path stays a single flag write.
    if (buffer < g_pmap_ever_element.size() && g_pmap_ever_element[buffer]) return;

    PersistentMap& m = pmap_slot(buffer);

    // Report the state of every candidate on every call, before any decision is taken. The first
    // device round could not say why the one buffer that matters was never adopted - map_failures was
    // 0 and every eviction counter was 0, which between them ruled out everything except a silent
    // "declined", and nothing recorded that. Reasoning filled the gap and that is exactly what this
    // file's rules forbid. See diagnostics/counters.h PmapDest.
    mg::diagnostics::record_pmap_state(buffer, bufferBytes, m.smallWrites, m.base != nullptr, m.declined,
                                       m.appMapStrikes, static_cast<uint64_t>(mg_store_effective_flags(buffer)));

    if (m.declined || m.base) return;

    // The mapping is only legal if the store was created able to hold one. This layer promotes such
    // stores itself in glBufferStorage, so in practice this tests whether the promotion happened -
    // and refusing when it did not is what keeps this from turning into a GL error on a store the
    // application created some other way.
    // What the STORAGE has to allow, which is not the same as what the mapping will ask for. A
    // persistent mapping needs the store to have been created persistent and writable; coherence is a
    // separate property that the ladder below negotiates on the mapping. Requiring COHERENT here as
    // well would exclude a store that could still take rungs 2 to 4, and conflating the two is the
    // mistake that made the first version fail.
    const GLbitfield eff = mg_store_effective_flags(buffer);
    const GLbitfield needStorage = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT;
    if ((eff & needStorage) != needStorage) return;

    if (++m.smallWrites < PMAP_ADOPT_AFTER) return;

    // Adopt. The buffer is already bound to `target` by the caller, so no binding is disturbed.
    //
    // Errors are drained first so that the error recorded against a failure is this call's and not an
    // older one still sitting in the queue - the same reason GlBuffer$Direct.map calls
    // GlStateManager.clearGlErrors before mapping.
    if (GLES.glGetError) {
        for (int drain = 0; drain < 8 && GLES.glGetError() != GL_NO_ERROR; ++drain) {
        }
    }

    void* base = nullptr;
    int rung = -1;
    const bool storeIsCoherent = (eff & GL_MAP_COHERENT_BIT) != 0;
    for (int i = 0; i < (int)(sizeof(PMAP_LADDER) / sizeof(PMAP_LADDER[0])); ++i) {
        // Skip rungs this store cannot legally take, rather than letting the driver refuse them.
        //
        // Two different reasons, both keyed on the same bit:
        //   - a rung that asks for COHERENT on a store without it is GL_INVALID_OPERATION by
        //     EXT_buffer_storage, so trying it only pollutes pmap_ladder_failures with refusals that
        //     were predictable from the flags. This is a precision improvement, not a correctness one.
        //   - the last rung has neither COHERENT nor FLUSH_EXPLICIT and is marked needsFlush=false, so
        //     it has no way at all to publish a write. It is only sound on a store that really is
        //     coherent. On one that is not, taking it would put writes into memory the GPU never sees,
        //     and a silently wrong pixel is worse than falling back to glBufferSubData.
        // Keyed on this store's own recorded flags rather than on a global, because a store the
        // application created coherent itself is still eligible for both.
        const bool rungWantsCoherent = (PMAP_LADDER[i].mask & GL_MAP_COHERENT_BIT) != 0;
        const bool rungCanPublish = rungWantsCoherent || (PMAP_LADDER[i].mask & GL_MAP_FLUSH_EXPLICIT_BIT) != 0;
        if (!storeIsCoherent && (rungWantsCoherent || !rungCanPublish)) continue;

        base = GLES.glMapBufferRange(target, 0, (GLsizeiptr)bufferBytes, PMAP_LADDER[i].mask);
        if (base) {
            rung = i;
            break;
        }
        const GLenum err = GLES.glGetError ? GLES.glGetError() : 0;
        mg::diagnostics::record_pmap_ladder_failure(i, static_cast<uint64_t>(PMAP_LADDER[i].mask),
                                                    static_cast<uint64_t>(err));
        LOG_D("[pmap] buffer %u refused access 0x%x for %zu bytes, error 0x%x", buffer, PMAP_LADDER[i].mask,
              bufferBytes, err);
    }

    if (!base) {
        // Every rung refused. Ask the driver what it thinks this buffer is, once, before giving up.
        //
        // The specification narrows GL_INVALID_OPERATION from MapBufferRange to a short list, and
        // after eliminating the mask-dependent entries - all five rungs set WRITE, none sets READ,
        // both FLUSH rungs also set WRITE, and length is not zero - the survivors are: the buffer is
        // already mapped; nothing is bound to the target; or its BUFFER_STORAGE_FLAGS does not
        // actually contain a bit the access asks for. Every one of those is queryable, so it gets
        // queried instead of argued about. Note that all five rungs set GL_MAP_PERSISTENT_BIT, so a
        // storage that is missing that single bit fails all five identically - which is what the
        // device shows, and which glBufferStorage would never have noticed because it reads no error
        // after glBufferStorageEXT.
        if (mg::diagnostics::g_pmap_probe.captured == 0 && GLES.glGetBufferParameteriv && GLES.glGetIntegerv) {
            mg::diagnostics::PmapProbe& p = mg::diagnostics::g_pmap_probe;
            GLint binding = 0, sz = 0, immutable = 0, sflags = 0, mapped = 0, aflags = 0;
            GLES.glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &binding);
            GLES.glGetBufferParameteriv(target, GL_BUFFER_SIZE, &sz);
            GLES.glGetBufferParameteriv(target, GL_BUFFER_IMMUTABLE_STORAGE, &immutable);
            GLES.glGetBufferParameteriv(target, GL_BUFFER_STORAGE_FLAGS, &sflags);
            GLES.glGetBufferParameteriv(target, GL_BUFFER_MAPPED, &mapped);
            GLES.glGetBufferParameteriv(target, GL_BUFFER_ACCESS_FLAGS, &aflags);
            p.query_error = GLES.glGetError ? static_cast<uint64_t>(GLES.glGetError()) : 0;
            p.buffer = buffer;
            p.recorded_bytes = static_cast<uint64_t>(bufferBytes);
            p.driver_binding = static_cast<uint64_t>(binding);
            p.expected_real = static_cast<uint64_t>(find_real_buffer(buffer));
            p.buffer_size = static_cast<uint64_t>(sz);
            p.immutable = static_cast<uint64_t>(immutable);
            p.storage_flags = static_cast<uint64_t>(sflags);
            p.mapped = static_cast<uint64_t>(mapped);
            p.access_flags = static_cast<uint64_t>(aflags);
            p.captured = 1;
        }

        // Bar the buffer rather than retrying every 64 writes, which would each cost five failed maps
        // on a path that runs tens of times a second.
        m.declined = true;
        mg::diagnostics::record_pmap_map_failure();
        return;
    }

    m.base = base;
    m.length = bufferBytes;
    m.needsFlush = PMAP_LADDER[rung].needsFlush;
    g_pmap_live++;
    mg::diagnostics::record_pmap_accepted(rung, static_cast<uint64_t>(PMAP_LADDER[rung].mask));
    // Writes now bypass the sub-data path, so the dedup shadow will no longer describe this buffer.
    mg_subdata_invalidate(buffer);
    mg::diagnostics::record_pmap_adopted();
    LOG_D("[pmap] adopted buffer %u, %zu bytes, after %u small writes", buffer, bufferBytes, m.smallWrites);
}

void mg_pmap_drop(GLuint buffer, GLenum boundTarget, int reason) {
    if (buffer == 0) return;

    // Index buffers are handled by mg_pmap_note_element at bind time, so nothing here needs to create
    // a record: every reason that reaches this point concerns a buffer the cache has already seen.
    const bool isAppMap = (reason == MG_PMAP_EVICT_APP_MAP);
    PersistentMap* m = pmap_find(buffer);
    if (!m) return;

    if (isAppMap) {
        // Release and let the buffer earn the mapping again, rather than barring it. A transient
        // application mapping - construction, a resize - must not cost the buffer its place for the
        // rest of the session; that mistake is what made the first version of this cache adopt three
        // cheap buffers and miss the expensive one. Only a buffer that keeps taking the mapping back
        // is given up on.
        if (++m->appMapStrikes >= PMAP_APP_MAP_STRIKES) m->declined = true;
        m->smallWrites = 0;
    }

    if (!m->base) {
        // Nothing to release. Still worth counting, because "barred before it was ever adopted" was
        // invisible in the first device round and that is precisely the case that went wrong.
        if (isAppMap) mg::diagnostics::record_pmap_barred(reason);
        return;
    }

    // boundTarget == 0 means the store is already gone - glDeleteBuffers - and GL released the
    // mapping with it. Calling glUnmapBuffer then would either be an error or act on whatever is
    // bound now, so the pointer is simply abandoned.
    if (boundTarget != 0 && GLES.glUnmapBuffer) GLES.glUnmapBuffer(boundTarget);

    m->base = nullptr;
    m->length = 0;
    m->needsFlush = false;
    m->smallWrites = 0;
    if (g_pmap_live > 0) g_pmap_live--;
    // The bytes written through the mapping were never recorded, so anything remembered about this
    // buffer's contents is stale from here on.
    mg_subdata_invalidate(buffer);
    mg::diagnostics::record_pmap_evicted(reason);
    LOG_D("[pmap] released buffer %u, reason %d", buffer, reason);
}

// Clears the slot when a fake id is recycled. Declared here, called from remove_buffer.
static void mg_pmap_forget(GLuint buffer) {
    PersistentMap* m = pmap_find(buffer);
    if (!m) return;
    *m = PersistentMap{};
}

// ---- Reusing the persistent mapping the application already holds ----------------------------
// See gl/buffer.h for the evidence this rests on and why it is not a liberty.
namespace {

    struct AppMap {
        void* base = nullptr;
        GLintptr offset = 0;
        GLsizeiptr length = 0;
        GLbitfield access = 0;
        bool flushExplicit = false; // may publish with glFlushMappedBufferRange
    };

    std::vector<AppMap> g_appmap;
    size_t g_appmap_live = 0;

    AppMap* appmap_find(GLuint buffer) {
        if (buffer == 0 || buffer >= g_appmap.size()) return nullptr;
        return &g_appmap[buffer];
    }

} // namespace

GLboolean mg_appmap_any(void) {
    return g_appmap_live != 0 ? GL_TRUE : GL_FALSE;
}

void mg_appmap_record(GLuint buffer, void* base, GLintptr offset, GLsizeiptr length, GLbitfield access) {
    if (buffer == 0 || !base || length <= 0 || offset < 0) return;
    if (g_appmap.size() <= (size_t)buffer) g_appmap.resize((size_t)buffer + 1);
    AppMap& a = g_appmap[buffer];
    if (!a.base) g_appmap_live++;
    a.base = base;
    a.offset = offset;
    a.length = length;
    a.access = access;
    a.flushExplicit = (access & GL_MAP_FLUSH_EXPLICIT_BIT) != 0;
    mg::diagnostics::record_appmap_tracked(buffer, mg::diagnostics::non_negative_bytes(length),
                                           static_cast<uint64_t>(access));
}

void mg_appmap_forget(GLuint buffer, int reason) {
    AppMap* a = appmap_find(buffer);
    if (!a || !a->base) return;
    *a = AppMap{};
    if (g_appmap_live > 0) g_appmap_live--;
    mg::diagnostics::record_appmap_forgot(reason);
}

GLboolean mg_appmap_write(GLenum target, GLuint buffer, GLintptr offset, GLsizeiptr size, const void* data) {
    if (g_appmap_live == 0 || !data || size <= 0 || offset < 0) return GL_FALSE;
    const AppMap* a = appmap_find(buffer);
    if (!a || !a->base) return GL_FALSE;

    // The write has to fall entirely inside the mapped range. GlBuffer$Direct always maps the whole
    // buffer - offsets 182-184 pass lconst_0 and size() - so in practice this always holds, but a
    // partial mapping from some other caller must not be written past.
    if (offset < a->offset || (offset - a->offset) + size > a->length) {
        mg::diagnostics::record_appmap_out_of_range();
        return GL_FALSE;
    }

    const uint64_t startNs = mg::diagnostics::timestamp();
    memcpy(static_cast<unsigned char*>(a->base) + (offset - a->offset), data, static_cast<size_t>(size));
    mg::diagnostics::record_appmap_write(mg::diagnostics::non_negative_bytes(size),
                                         mg::diagnostics::elapsed_ns(startNs));

    // Publish it.
    //
    // The mapping's own access mask has no COHERENT bit - the driver reports 0x62 - and a persistent
    // mapping has no unmap that would publish a write either, so glFlushMappedBufferRange is the
    // explicit mechanism, and it requires the mapping to carry GL_MAP_FLUSH_EXPLICIT_BIT. That bit is
    // the one this layer used to strip, which is why glMapBufferRange keeps it for persistent mappings.
    //
    // CORRECTED 2026-08-07; see RENDER-ADAPTATION.md 6.15. This used to say the write is "not visible
    // to the GPU on its own", reasoning from the mapping's mask. Coherence is a property of the store,
    // not the mapping, so under promote mode 1 - where the store carries COHERENT - the write is in
    // fact visible on its own and this flush is redundant. It is kept because it is measured cheap
    // (1.8 ms across 5,107 writes, worst 14 us) and because under promote mode 2 the store is not
    // coherent and this becomes the only way the bytes ever reach the GPU.
    //
    // GLES.glFlushMappedBufferRange is called directly rather than through this layer's own wrapper,
    // because that wrapper deliberately skips the driver call when the coherent-as-flush policy is on -
    // a policy about the application's flushes, which must not silence this one.
    if (a->flushExplicit && GLES.glFlushMappedBufferRange) {
        const uint64_t flushStartNs = mg::diagnostics::timestamp();
        GLES.glFlushMappedBufferRange(target, offset, size);
        mg::diagnostics::record_appmap_flush(mg::diagnostics::elapsed_ns(flushStartNs));
    } else if (mg_coherence_promised()) {
        // No way to publish explicitly. Counted rather than hidden: it means the bytes are resting on
        // the storage-level coherence promise alone, which is the open question in 6.4.
        mg::diagnostics::record_appmap_unpublished();
    } else {
        // Same situation, but under a promote mode that withholds COHERENT there is no promise left to
        // rest on, so the bytes would simply never reach the GPU. The memcpy above has already run and
        // is harmless - it wrote into a mapping nobody will read - but this write must not be reported
        // as served. Returning GL_FALSE sends the caller back to glBufferSubData, which is slower and
        // correct. Still counted, because it reaching a non-zero value means glMapBufferRange handed
        // out a mapping without GL_MAP_FLUSH_EXPLICIT_BIT and that pairing needs looking at.
        mg::diagnostics::record_appmap_unpublished();
        return GL_FALSE;
    }
    return GL_TRUE;
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
// How this differs from each closed axis in docs/ohos/RENDER-ADAPTATION.md, since the rule is to say:
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

    // Did the last fence wait actually prove that the queued bytes' readers have retired?
    //
    // Only GL_ALREADY_SIGNALED or GL_CONDITION_SATISFIED prove it. A timeout, a failure, or no fence
    // at all does not, and in that case the replay must not be unsynchronized. This exists because
    // the previous code recorded the wait result into the counters and then ignored it.
    bool g_deferred_fence_wait_proved = false;

    // Cap on queued bytes for a single frame. Beyond this the caller is told to write immediately,
    // which is exactly today's behaviour, so overflow costs performance and never correctness.
    // 32 MiB is far above one frame's measured traffic and still small next to a 128 MiB heap.
    constexpr size_t DEFERRED_BYTES_CAP = 32u * 1024u * 1024u;

    // How a queued record is written when it is replayed.
    //
    // Unsynchronized is the fast one - 0.8 us against 3,946-4,604 us for the ordered write into the
    // same store - but it performs no write-after-read synchronization, so it races any draw still
    // reading the destination. It is only legal once something has proved those draws retired, and
    // the only proof available here is the fence taken when the queue opened.
    //
    // Ordered needs no proof at all: the driver orders the write against its own readers. That makes
    // it the only correct choice at a drain which runs inside the frame that queued the bytes, where
    // the queue's fence is unsubmitted and cannot be waited on cheaply. See deferred_wait_for_fence.
    enum class ReplayMode {
        Unsynchronized,
        Ordered
    };

    // Replays one record. The destination is bound to GL_COPY_WRITE_BUFFER rather than
    // GL_ARRAY_BUFFER: at replay time the application's vertex-source binding must not be
    // disturbed, and a transfer destination has no business being a live pipeline binding.
    void deferred_replay_one(const DeferredUpload& rec, ReplayMode mode) {
        GLES.glBindBuffer(GL_COPY_WRITE_BUFFER, rec.realBuffer);

        if (mode == ReplayMode::Ordered) {
            // No fence was waited on and none is needed - this is what "ordered" buys.
            //
            // glBufferSubData is legal on these stores even though they are immutable: every buffer
            // that reaches the deferred path is a copy destination, and GlConst.bufferUsageToGlFlag
            // maps USAGE_COPY_DST to GL_DYNAMIC_STORAGE_BIT, which is exactly the bit
            // glBufferSubData requires. Verified for Minecraft's terrain heaps (usage masks 56 and
            // 88) and Sodium's arenas (mask 120); all three yield flags 0x100.
            //
            // None of them is mapped by the application either, so there is no mapped-buffer
            // conflict: GlBuffer$Direct only takes a persistent mapping when usage has MAP_READ or
            // MAP_WRITE, and none of those three masks does.
            const uint64_t orderedStartNs = mg::diagnostics::timestamp();
            GLES.glBufferSubData(GL_COPY_WRITE_BUFFER, rec.dstOffset, rec.size,
                                 g_deferred_bytes.data() + rec.srcOffset);
            mg::diagnostics::record_deferred_ordered_replay(mg::diagnostics::non_negative_bytes(rec.size),
                                                            mg::diagnostics::elapsed_ns(orderedStartNs));
            return;
        }

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

    // Waits for the fence taken when the queue opened, so an unsynchronized replay cannot race the
    // draws that were in flight when the bytes were queued.
    //
    // FENCE PROVENANCE (RENDER-ADAPTATION.md rule 5.2): the fence is created mid-frame, but this
    // function is only ever reached from the present-time drain in egl/egl.cpp. That present
    // submitted the fence, so the zero-timeout poll can succeed and normally does - measured 89%
    // already-signalled. Cost when it succeeds is nothing.
    //
    // It must NOT be called from a drain that runs inside the frame which opened the queue. There
    // the fence is still unsubmitted, so the poll cannot succeed - a sync object only signals once
    // its fence command has completed, and an unsubmitted command has not - and the fallback below
    // pays GL_SYNC_FLUSH_COMMANDS_BIT, measured at 3.77 ms plus a pipeline drain, mid-frame with the
    // render pass open. That was the Sodium frame collapse. Mid-frame drains use ReplayMode::Ordered
    // and deferred_discard_fence instead.
    void deferred_wait_for_fence() {
        // Cleared first so the flag can never be read stale. No fence means nothing was proved: the
        // queue can hold records with no fence if glFenceSync failed when the queue was opened, and
        // that case must fall back to the ordered write rather than inherit an earlier "proved".
        g_deferred_fence_wait_proved = false;
        if (!g_deferred_fence) {
            // Accounting blind spot, closed 2026-08-08. Returning here leaves proved=false, so the
            // caller downgrades the whole batch to the ordered path - one synchronous glBufferSubData
            // per record, measured at 3,424 us each with a worst single record of 97 ms. And because
            // this return happens *before* record_deferred_fence_wait, none of the fence counters
            // moved: a device round showed 66 ordered replays with deferred_fence_timeout = 0, which
            // read as a contradiction until this path was found. Counted as its own reason now.
            mg::diagnostics::record_deferred_fence_missing();
            return;
        }
        GLenum result = GLES.glClientWaitSync(g_deferred_fence, 0, 0);
        if (result != GL_ALREADY_SIGNALED && result != GL_CONDITION_SATISFIED) {
            result = GLES.glClientWaitSync(g_deferred_fence, GL_SYNC_FLUSH_COMMANDS_BIT, 100000000ULL);
        }
        mg::diagnostics::record_deferred_fence_wait(result);
        GLES.glDeleteSync(g_deferred_fence);
        g_deferred_fence = nullptr;

        if (result != GL_ALREADY_SIGNALED && result != GL_CONDITION_SATISFIED) {
            // The wait did not prove anything, so the unsynchronized write is not authorised. This
            // used to record the result and then replay unsynchronized regardless, which is the
            // second defect described in RENDER-ADAPTATION.md 3.3: on timeout the write proceeded
            // against draws that were still in flight. Reporting failure here makes the caller fall
            // back to the ordered write, which needs no proof.
            g_deferred_fence_wait_proved = false;
            return;
        }
        g_deferred_fence_wait_proved = true;
    }

    // Drops the queue's fence without waiting on it. Used by every mid-frame drain, which replays in
    // ordered mode and therefore needs no proof that anything retired. Deleting rather than leaking
    // it matters: sync objects are not free on this driver.
    void deferred_discard_fence() {
        if (!g_deferred_fence) return;
        GLES.glDeleteSync(g_deferred_fence);
        g_deferred_fence = nullptr;
    }

    void deferred_restore_copy_write_binding() {
        // Put the application's own GL_COPY_WRITE_BUFFER binding back, translating the fake id the
        // shadow holds. Done once per flush rather than once per record.
        glBindBuffer(GL_COPY_WRITE_BUFFER, find_bound_buffer(GL_COPY_WRITE_BUFFER_BINDING));
    }

    // Replays everything queued. The mode decides both how the bytes are written and which fence
    // discipline applies; the two are not independent, which is why this is one function.
    void deferred_flush(ReplayMode mode) {
        if (g_deferred_records.empty()) {
            // The fence is only ever taken alongside a record, but be defensive: leaking sync
            // objects is not free on this driver.
            deferred_discard_fence();
            return;
        }

        ReplayMode effective = mode;
        if (mode == ReplayMode::Unsynchronized) {
            deferred_wait_for_fence();
            // Downgrade rather than write unsynchronized on an unproven fence.
            if (!g_deferred_fence_wait_proved) effective = ReplayMode::Ordered;
        } else {
            deferred_discard_fence();
        }

        for (const DeferredUpload& rec : g_deferred_records)
            deferred_replay_one(rec, effective);
        deferred_restore_copy_write_binding();

        g_deferred_records.clear();
        g_deferred_used = 0;
    }

} // namespace

// Defined below. Declared here because the overflow path in mg_deferred_upload_enqueue drains, and
// an overflow drain happens inside the frame that queued the bytes like every other mid-frame drain.
static void mg_deferred_upload_flush_midframe(void);

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
        // path. Replaying early costs one ordered write per queued record and never costs
        // correctness; it no longer costs a fence wait, because an overflow drain is a mid-frame
        // drain like any other.
        mg::diagnostics::record_deferred_overflow();
        mg_deferred_upload_flush_midframe();
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
        // Opening the queue. Take the fence now, so that whatever was in flight at this moment can
        // later be proved retired - which is what authorises the unsynchronized replay.
        //
        // The fence is only useful to a drain that happens after a present has submitted it, i.e.
        // the present-time drain. On Minecraft's own renderer that is the only drain there is, and
        // the fence covers this frame's terrain draws, which were issued at LevelRenderer offset 603
        // before the uploads at 682. Under Sodium the uploads come first and the drain happens
        // mid-frame instead, where this fence is useless; that path replays in ordered mode and
        // discards it. See mg_deferred_upload_flush_midframe.
        g_deferred_fence = GLES.glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    }

    memcpy(g_deferred_bytes.data() + g_deferred_used, data, need);
    g_deferred_records.push_back(DeferredUpload{realBuffer, offset, size, g_deferred_used});
    g_deferred_used += need;

    mg::diagnostics::record_deferred_enqueue(mg::diagnostics::non_negative_bytes(size));
    return GL_TRUE;
}

// The present-time drain, called once per eglSwapBuffers. This is the only site allowed to replay
// unsynchronized, because it is the only one where the queue's fence has been submitted - by the
// present that just happened. See deferred_wait_for_fence for the provenance argument, and
// mg_deferred_upload_flush_midframe for everywhere else.
void mg_deferred_upload_flush_all(void) {
    deferred_flush(ReplayMode::Unsynchronized);
}

// Every drain that happens inside the frame which queued the bytes.
//
// It replays with ordered writes and waits on nothing. That is the fix for the Sodium frame
// collapse: the queue's fence is unsubmitted at this point, so waiting on it could only succeed by
// paying GL_SYNC_FLUSH_COMMANDS_BIT - 3.77 ms plus a pipeline drain, with the render pass open, once
// per frame that queued anything, which is once per frame while the camera is moving. An ordered
// write needs no fence at all, so the flush disappears rather than being made cheaper.
//
// This costs 3,946-4,604 us per record against 0.8 us for the unsynchronized write, so it is only
// used where the unsynchronized write cannot be authorised. Vanilla 26.2, 26.1.2 and 1.21.11 never
// reach it: they upload after they draw, so their queue is empty at every draw and every guard site,
// and they are served entirely by the present-time drain above. deferred_midframe_flush reads 0 on
// those versions and is the check that this remains true.
static void mg_deferred_upload_flush_midframe(void) {
    mg::diagnostics::record_deferred_midframe_flush();
    deferred_flush(ReplayMode::Ordered);
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
    mg_deferred_upload_flush_midframe();
}

GLuint mg_real_buffer_for_target(GLenum target) {
    const GLenum bindingQuery = get_binding_query(target);
    if (!bindingQuery) return 0;
    return find_real_buffer(find_bound_buffer(bindingQuery));
}

GLuint mg_fake_buffer_for_target(GLenum target) {
    const GLenum bindingQuery = get_binding_query(target);
    if (!bindingQuery) return 0;
    return find_bound_buffer(bindingQuery);
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
            mg_deferred_upload_flush_midframe();
            return;
        }
        const GLintptr recEnd = rec.dstOffset + rec.size;
        if (rec.dstOffset < end && offset < recEnd) {
            mg::diagnostics::record_deferred_forced_flush();
            mg_deferred_upload_flush_midframe();
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
    // Sufficient: Sodium's terrain draws go through glMultiDrawElementsBaseVertex - confirmed from
    // Blaze3D, where GlRenderPass.multiDrawIndexed forwards to GlCommandEncoder.executeDraws and
    // emits nglMultiDrawElementsBaseVertex with no other branch. Necessary: a hook on *every* draw
    // would fire on Minecraft's first GUI draw right after offset 682, draining a queue that nothing
    // was about to read. So the hook stays narrow.
    //
    // WITHDRAWN, and this is the frame collapse. The paragraph here used to claim the drain was cheap
    // for Sodium because "the fence was taken during updateChunks, early in the frame, so it covers
    // the previous frame's draws". Both halves are wrong:
    //
    //   1. The queue cannot survive a present. egl/egl.cpp drains it unconditionally on every
    //      eglSwapBuffers, so a fence found here was necessarily created in THIS frame, not a
    //      previous one.
    //   2. Nothing submits between the enqueue and this drain. Minecraft.renderFrame calls
    //      GameRenderer.extract at offset 440 and GameRenderer.render at 526, with only
    //      RenderSystem.executePendingTasks and NeoForge hooks in between, and
    //      CommandEncoder.submit has exactly one call site in the entire client - offset 702, after
    //      all drawing.
    //
    // So the zero-timeout poll could never succeed here, and every one of these drains paid the
    // GL_SYNC_FLUSH_COMMANDS_BIT fallback: 3.77 ms plus a pipeline drain, mid-frame, with the render
    // pass open, once per frame that queued anything - which is once per frame while moving.
    //
    // The drain itself is still required, for ordering: the bytes must land before this draw reads
    // them. What is removed is the wait. Replaying with ordered writes needs no fence at all, so
    // there is nothing left to flush for. See mg_deferred_upload_flush_midframe.
    mg::diagnostics::record_deferred_forced_flush();
    mg_deferred_upload_flush_midframe();
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
#if defined(MG_PLATFORM_OHOS)
    // An index buffer must never be held mapped by this layer, and the reason is a silent failure
    // rather than a loud one. The base-vertex emulation in gl/multidraw.cpp and gl/drawing.cpp maps
    // the application's element-array buffer with GL_MAP_READ_BIT to rewrite indices on the CPU, and
    // a buffer already carrying a mapping refuses that - after which both call sites take a failure
    // branch that simply `continue`s or returns, i.e. the draw is dropped with no error anywhere.
    // Missing geometry is far worse than the stall this cache exists to remove.
    //
    // Not gated on anything: the flag has to be set even before the cache has adopted anything, or a
    // buffer used for indices early and written hot later would slip through. It is one flag write,
    // which matters because this fires once per indexed draw - measured at about 5,000 times a second
    // under Sodium. The earlier version called mg_pmap_drop here, which grew a vector and updated a
    // counter each time to record what this flag says.
    if (target == GL_ELEMENT_ARRAY_BUFFER) mg_pmap_note_element(buffer);
#endif
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
    // A mapping of this layer's own must be released before the store it points into is replaced.
    // The buffer is bound to `target`, so the release can happen properly rather than by abandoning
    // the pointer.
    if (mg_pmap_any()) mg_pmap_drop(find_bound_buffer(get_binding_query(target)), target, MG_PMAP_EVICT_RESPECIFIED);
    // Respecification destroys the store the recorded pointer points into.
    if (mg_appmap_any()) mg_appmap_forget(find_bound_buffer(get_binding_query(target)), MG_APPMAP_FORGET_RESPECIFIED);
#endif
    GLES.glBufferData(target, size, data, usage);
    // find_bound_buffer takes a *_BINDING query, not a target: its switch maps GL_ARRAY_BUFFER_BINDING
    // (0x8894) to GL_ARRAY_BUFFER, and a target such as GL_ARRAY_BUFFER (0x8892) matches nothing, so it
    // fell to `default: target = 0`, then binding_target_to_index(0) returned -1, and the size of every
    // buffer created here was recorded against id 0 instead of the buffer.
    //
    // Consequence was mild but real: get_buffer_data_size then reported 0 for these buffers, so
    // glNamedBufferSubData fell back to asking the driver via glGetBufferParameteri64v on every call -
    // exactly the round trip the record exists to avoid, and on a tiled GPU a glGet* can force a
    // pipeline flush. glBufferStorage a few lines below always did this correctly, via
    // get_binding_query; this now matches it.
    set_buffer_data_size(find_bound_buffer(get_binding_query(target)), size);
#if defined(MG_PLATFORM_OHOS)
    // Respecification replaces the whole store, so nothing remembered about it still holds.
    {
        const GLuint fake = find_bound_buffer(get_binding_query(target));
        mg_subdata_invalidate(fake);
        // entry=2 marks the glBufferData route; usage is recorded in place of flags, since this route
        // has no flag word. Reaching here at all means BufferStorage$Mutable ran.
        mg_record_store(fake, static_cast<GLbitfield>(usage), 0, /*entry=*/2);
    }
    // Reaching here for a store the application created means it chose BufferStorage$Mutable, i.e.
    // LWJGL's GLCapabilities.GL_ARB_buffer_storage was false. See counters.h storage_calls.
    mg::diagnostics::record_buffer_data();
#endif
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
    // A buffer gets one mapping. If this layer is holding it and the application now wants to map the
    // same buffer, ours has to go first or the driver refuses - and the application's failure path is
    // an IllegalStateException("Failed to map buffer") in GlBuffer$Direct.map, i.e. a crash rather
    // than a wrong pixel. It is barred from adoption afterwards, because a buffer the application
    // maps is one it intends to write itself.
    if (mg_pmap_any()) mg_pmap_drop(find_bound_buffer(get_binding_query(target)), target, MG_PMAP_EVICT_APP_MAP);
    // A mapping that is about to be replaced must not stay on record under the old pointer.
    if (mg_appmap_any()) mg_appmap_forget(find_bound_buffer(get_binding_query(target)), MG_APPMAP_FORGET_REMAPPED);
#endif

    // Keep GL_MAP_FLUSH_EXPLICIT_BIT on a persistent mapping.
    //
    // The coherent-as-flush policy strips this bit and then skips the driver's flush, on the grounds
    // that the store was promoted to coherent. For a mapping that is unmapped later that is at worst
    // wasteful, because the unmap publishes everything. For a PERSISTENT mapping there is no unmap, so
    // stripping the bit removes the only mechanism that can publish a write explicitly.
    //
    // CORRECTED 2026-08-07 against the EXT_buffer_storage text; see RENDER-ADAPTATION.md 6.15. This
    // comment used to add "and the mapping itself is not coherent, because coherence is a property
    // requested on the mapping and this layer only ever sets it on storage". That is wrong. Coherence
    // is a property of the STORE: section 6.2 places exactly one requirement on the mapping - that it
    // be made with MapBufferRange - and then states that with the storage bit set, a client write is
    // visible to subsequent GL commands with no further action. The access bit only has to agree with
    // the storage bit and adds nothing.
    //
    // So under promote mode 1 keeping this bit is belt-and-braces rather than load-bearing: the write
    // would be published by the store's coherence anyway, and the explicit flush mg_appmap_write then
    // issues costs 1.8 ms across 5,107 writes, which is cheap enough not to bother reversing. Under a
    // mode that withholds COHERENT it becomes the only mechanism there is, which is what the extra
    // condition below is for.
    //
    // Narrow on purpose: only mappings that carry GL_MAP_PERSISTENT_BIT are affected, and the
    // application's own glFlushMappedBufferRange calls are still skipped exactly as before - see
    // glFlushMappedBufferRange, which is unchanged. The only new driver flush is the one issued for
    // this layer's own writes in mg_appmap_write.
    //
    // Second narrowing, added with promote mode 2: the strip is also conditional on coherence
    // actually being promised. When it is withheld the bit is the application's only way to publish a
    // write through any mapping, persistent or not, and glFlushMappedBufferRange forwards the call
    // instead of dropping it. Pair with that site - keeping the bit without forwarding the flush
    // changes nothing, and forwarding the flush without keeping the bit is GL_INVALID_OPERATION.
    const bool persistentMapping = (access & GL_MAP_PERSISTENT_BIT) != 0;
    if (global_settings.buffer_coherent_as_flush && !persistentMapping && mg_coherence_promised()) {
        access &= ~GL_MAP_FLUSH_EXPLICIT_BIT;
    }
    //    access |= GL_MAP_UNSYNCHRONIZED_BIT;
    const uint64_t startNs = mg::diagnostics::timestamp();
    void* result = GLES.glMapBufferRange(target, offset, length, access);
#if defined(MG_PLATFORM_OHOS)
    // Both routes reach here: the bound-target form the application may call directly, and
    // glMapNamedBufferRange in gl/ExtWrappers/DSAWrapper.cpp, which binds and delegates. So one probe
    // covers Blaze3D's DSA Core path as well. See counters.h map_attempts.
    //
    // The error is read only when the mapping failed, so the success path adds no glGetError - that
    // call can force a pipeline flush on this driver and must not be put on a hot path just to
    // measure it.
    {
        const GLuint mapped = find_bound_buffer(get_binding_query(target));
        // The application can now write the range itself, so this layer no longer knows its contents.
        // This is one of the invalidation sites the dedup shadow's correctness depends on.
        if (result) mg_subdata_invalidate(mapped);
        // Remember a persistent mapping so a later sub-data into the same store can be satisfied by
        // writing through it. Only persistent ones: a mapping that gets unmapped again is not a
        // pointer worth keeping, and this layer sees the unmap anyway.
        if (result && persistentMapping) {
            mg_appmap_record(mapped, result, offset, length, access);
        }
        if (mg::diagnostics::enabled()) {
            const bool ok = result != nullptr;
            mg::diagnostics::record_map_result(mapped, mg::diagnostics::non_negative_bytes(length),
                                               static_cast<uint64_t>(access), ok, ok ? 0u : GLES.glGetError());
        }
    }
#endif
    // Mapping can block: without GL_MAP_UNSYNCHRONIZED_BIT the driver waits for readers of the
    // range, so this is one of the places a stall hides behind a call that looks cheap.
    mg::diagnostics::record_map(mg::diagnostics::non_negative_bytes(length), mg::diagnostics::elapsed_ns(startNs));
    return result;
}

GLboolean glUnmapBuffer(GLenum target) {
    LOG()
    LOG_D("%s(%s)", __func__, glEnumToString(target));
#if defined(MG_PLATFORM_OHOS)
    // Counted before the call, because the binding is what identifies the buffer and the call does
    // not change it. See diagnostics/counters.h Counters::map_dest: maps without a matching unmap is
    // how a held persistent mapping is told apart from a map/memset/close at construction.
    if (mg::diagnostics::enabled()) {
        mg::diagnostics::record_unmap(find_bound_buffer(get_binding_query(target)));
    }
    // Defensive. Reaching here for a buffer this layer holds mapped should be impossible, because
    // glMapBufferRange above releases and bars any such buffer before the application can map it -
    // so the application has no mapping to unmap. If it happens anyway, the call below would destroy
    // our mapping without us knowing, and a later memcpy would write through a dangling pointer.
    // Forgetting the mapping first makes that outcome impossible rather than unlikely; the driver
    // call that follows performs the actual release.
    if (mg_pmap_any()) mg_pmap_drop(find_bound_buffer(get_binding_query(target)), /*boundTarget=*/0,
                                    MG_PMAP_EVICT_APP_MAP);
    // The pointer dies here, so the record must go with it. This is the invalidation site the reuse
    // path's correctness rests on: writing through a stale mapping is memory corruption, not a wrong
    // pixel. Reaching this wrapper at all means the driver is being asked to unmap - GlBuffer$Direct
    // .unmap only calls through once its refcount hits zero - so there is no double counting.
    if (mg_appmap_any()) mg_appmap_forget(find_bound_buffer(get_binding_query(target)), MG_APPMAP_FORGET_UNMAP);
#endif
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
        // A fresh store, so any remembered bytes describe an allocation that no longer exists.
        if (bindingQuery) mg_subdata_invalidate(find_bound_buffer(bindingQuery));
        // Same as glBufferData: release before the store underneath the mapping is replaced.
        if (bindingQuery && mg_pmap_any()) {
            mg_pmap_drop(find_bound_buffer(bindingQuery), target, MG_PMAP_EVICT_RESPECIFIED);
        }
        if (bindingQuery && mg_appmap_any()) {
            mg_appmap_forget(find_bound_buffer(bindingQuery), MG_APPMAP_FORGET_RESPECIFIED);
        }
    }
#endif
    const GLbitfield requestedFlags = flags;
    bool promoted = false;
    if (GLES.glBufferStorageEXT) {
        // How far the promotion reaches. Settable from the device without a rebuild so the
        // behaviours can be compared rather than argued about. The mode values and the reasoning are
        // at mg_promote_mode() near the top of this file; the device-side plumbing is
        // MG_PROMOTE_COHERENT in MyApplication/entry/src/main/cpp/platform/mg_config.cpp.
        //
        // 6.12 measured mode 0 and it was a net loss - but that experiment withdrew MAP_WRITE and
        // PERSISTENT along with COHERENT, and the failure it produced (the deferred queue could no
        // longer map its target, so 6,973 replays fell back to ordered glBufferSubData) is a
        // *mapping* failure. Per EXT_buffer_storage, mapping needs WRITE and PERSISTENT in the
        // storage flags; it does not need COHERENT. Mode 2 keeps the two bits that carry the mapping
        // capability and withholds only the one that changes the memory type.
        const int promoteMode = mg_promote_mode();

        const bool appWantsMapping = (requestedFlags & (GL_MAP_READ_BIT | GL_MAP_WRITE_BIT)) != 0;
        const bool eligible = (flags & GL_MAP_PERSISTENT_BIT) != 0 || (flags & GL_DYNAMIC_STORAGE_BIT) != 0;
        bool allow = false;
        if (promoteMode > 0) allow = true;
        else if (promoteMode == 0) allow = appWantsMapping;

        if (global_settings.buffer_coherent_as_flush && eligible && allow) {
            // MAP_WRITE and PERSISTENT are the mapping capability and are granted in every promoting
            // mode. COHERENT is the memory type and is the only bit mode 2 withholds. Everything that
            // relied on it asks mg_coherence_promised() rather than assuming.
            flags |= (GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT);
            if (mg_coherence_promised()) flags |= GL_MAP_COHERENT_BIT;
            promoted = true;
        }
#if defined(MG_PLATFORM_OHOS)
        // Does the promotion actually take?
        //
        // Nothing here has ever checked. mg_record_store below stores the flags this layer *passed*,
        // not the ones the driver accepted, and that distinction cost a device round: a store whose
        // recorded flags read 0x1c2 refused every persistent mapping with GL_INVALID_OPERATION, which
        // per EXT_buffer_storage is exactly what happens when the buffer's real BUFFER_STORAGE_FLAGS
        // is missing a bit the mapping asks for. If glBufferStorageEXT is failing - out of memory for
        // a coherent allocation, say - the store silently stays whatever it was and every later
        // decision built on the recorded flags is wrong.
        //
        // Errors are drained first so the reading belongs to this call. Once per store creation, of
        // which a session has tens, so the pipeline flush a glGetError may cause is not on a hot path.
        if (mg::diagnostics::enabled() && GLES.glGetError) {
            for (int drain = 0; drain < 8 && GLES.glGetError() != GL_NO_ERROR; ++drain) {
            }
            GLES.glBufferStorageEXT(target, size, data, flags);
            mg::diagnostics::record_storage_error(mg::diagnostics::non_negative_bytes(size),
                                                  static_cast<uint64_t>(requestedFlags),
                                                  static_cast<uint64_t>(flags),
                                                  static_cast<uint64_t>(GLES.glGetError()));
        } else {
            GLES.glBufferStorageEXT(target, size, data, flags);
        }
#else
        GLES.glBufferStorageEXT(target, size, data, flags);
#endif
    }
#if defined(MG_PLATFORM_OHOS)
    // Remember how this store was made, so a buffer seen later on the upload path can be named.
    {
        const GLenum bq = get_binding_query(target);
        if (bq) mg_record_store(find_bound_buffer(bq), requestedFlags, flags, /*entry=*/1);
    }
    // Reaching here at all means the application chose BufferStorage$Immutable, which means LWJGL's
    // GLCapabilities.GL_ARB_buffer_storage was true. See counters.h storage_calls.
    mg::diagnostics::record_storage(promoted, GLES.glBufferStorageEXT == nullptr);
#endif
    CHECK_GL_ERROR
}

void glFlushMappedBufferRange(GLenum target, GLintptr offset, GLsizeiptr length) {
    LOG()
    // A coherent mapping publishes writes without cache maintenance, so the driver call is
    // skipped. Counting the skips separately makes that visible instead of looking like a flush
    // that silently did nothing.
    //
    // The skip is only sound while the promotion actually grants COHERENT. Under a mode that
    // withholds it there is no coherence to stand on and dropping the application's flush would
    // silently lose its writes, so the call is forwarded. This is one of a pair with the
    // GL_MAP_FLUSH_EXPLICIT_BIT strip in glMapBufferRange: forwarding a flush for a mapping created
    // without that bit is itself GL_INVALID_OPERATION, so the two must move together or not at all.
    // Expect flush_driver to go from 0 to roughly 689 per second when coherence is withheld - that is
    // also the proof the change took effect.
    const bool callDriver = !global_settings.buffer_coherent_as_flush || !mg_coherence_promised();
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
