// MobileGlues - gl/buffer.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#ifndef MOBILEGLUES_BUFFER_H
#define GL_GLEXT_PROTOTYPES
#include "../config/settings.h"
#include "../gles/loader.h"
#include "../includes.h"
#include "glcorearb.h"
#include "log.h"
#include "mg.h"
#include <GL/gl.h>
#include <cstddef>
#include <vector>

#ifdef __cplusplus
extern "C"
{
#endif

    GLuint gen_buffer();

    GLboolean has_buffer(GLuint key);

    void modify_buffer(GLuint key, GLuint value);

    void remove_buffer(GLuint key);

    GLuint find_real_buffer(GLuint key);

    GLuint find_bound_buffer(GLenum key);

#if defined(MG_PLATFORM_OHOS)

    // Size of a buffer's store as this layer recorded it, 0 when unknown. Recorded by
    // glBufferData and glBufferStorage. Declared here so an upload can answer "how big is this
    // buffer" without a driver round trip; both were previously file-local to buffer.cpp.
    void set_buffer_data_size(GLuint buffer, size_t size);
    size_t get_buffer_data_size(GLuint buffer);

    // "This buffer has been the destination of a buffer-to-buffer copy at least once."
    //
    // A probe, not a policy: nothing in the layer changes behaviour on it yet. It exists to test a
    // candidate discriminator for the MC 26.2 + Sodium terrain race using only calls this layer
    // already sees, rather than assumptions about the application's internal ordering.
    //
    // The idea being tested: Sodium moves terrain through its own persistently mapped ring plus
    // glCopyBufferSubData, and reaches glNamedBufferSubData only when an upload does not fit the
    // ring's remaining space - so its arena is a copy destination. Minecraft's own terrain heap is
    // written directly and, as far as is known, never copied into. If that holds on the device, this
    // flag separates the buffer that races from the one that does not.
    //
    // It must be confirmed on vanilla 26.2 before anything depends on it: see the
    // direct_dest_copy_target counter.
    void mark_buffer_copy_destination(GLuint buffer);
    GLboolean is_buffer_copy_destination(GLuint buffer);

    // ---- Redundant sub-data elimination -----------------------------------------------------
    //
    // Skips a small glNamedBufferSubData whose bytes are already known to be at that offset,
    // because writing identical bytes to the same place twice is indistinguishable from writing
    // them once. This is not an optimisation of the write; it is the removal of a call that has no
    // effect.
    //
    // Why it is worth doing: Sodium's UniformBufferManager.writeMeshTimes is called once per
    // PendingSectionMeshUpload, and RenderRegionManager.uploadResults creates one of those per
    // terrain pass that has a mesh - up to DefaultTerrainRenderPasses.ALL.length, which is 3 - all
    // carrying the same relativeBuiltTime for a given section. So one section produces up to three
    // writes of the same 4 bytes to the same offset, and they are adjacent because the pending list
    // is built section by section. Measured 2026-08-07: 6,374 such writes costing 6,846 ms with a
    // worst single call of 116 ms, all into the 224 KiB u_SectionTimeInfo store.
    //
    // Correctness rests entirely on the shadow being invalidated whenever anything could change the
    // range without this layer seeing the new bytes. mg_subdata_invalidate must therefore be called
    // from every such site: respecification, mapping, deletion, a copy into the buffer, and any
    // sub-data this layer does not record. It is deliberately per buffer rather than global, because
    // the application maps other buffers tens of times per second and a global reset would discard
    // the shadow before the duplicate arrives.
    GLboolean mg_subdata_is_redundant(GLuint buffer, GLintptr offset, GLsizeiptr size, const void* data);
    void mg_subdata_record(GLuint buffer, GLintptr offset, GLsizeiptr size, const void* data);
    void mg_subdata_invalidate(GLuint buffer);

    // ---- Store creation record, for identifying a buffer after the fact -----------------------
    //
    // Remembers how each store was created. The point is identification, not policy: a buffer's
    // creation flags name it, because Blaze3D's GlConst.bufferUsageToGlFlag maps its usage mask onto
    // a small fixed set of flag values -
    //   0x142  MAP_WRITE | PERSISTENT | DYNAMIC_STORAGE   (usage 282 / 26 / 74)
    //   0x100  DYNAMIC_STORAGE only                       (usage 120 / 56 / 88)
    //   0x42   MAP_WRITE | PERSISTENT                     (usage 130)
    // so flags plus size plus call count pin down which store is which.
    //
    // This exists because a 229,376-byte store receiving thousands of tiny stalling writes was
    // identified from its size alone matching UniformBufferManager's formula, and that identification
    // turned out to be inconsistent with the rest of the evidence. Guessing from one coincidence cost
    // a round; recording the flags costs nothing.
    //
    // "requested" is what the application asked for, "effective" is what was passed to the driver
    // after this layer's promotion in glBufferStorage. entry is 1 for glBufferStorage and 2 for
    // glBufferData, so the two creation routes stay distinguishable.
    void mg_record_store(GLuint buffer, GLbitfield requested, GLbitfield effective, int entry);
    GLbitfield mg_store_requested_flags(GLuint buffer);
    GLbitfield mg_store_effective_flags(GLuint buffer);
    int mg_store_entry(GLuint buffer);

    // ---- Layer-owned persistent mapping for hot small writes --------------------------------
    //
    // Holds a persistent coherent mapping of a small store inside this layer and satisfies its
    // writes with a memcpy, instead of letting each one become a glBufferSubData that stalls until
    // the GPU stops reading the store.
    //
    // What this is for, measured rather than supposed (device session 2026-08-07, 287 s, MC 26.2 +
    // Sodium): the 229,376-byte u_SectionTimeInfo store took 9,301 sub-data writes averaging about
    // 8 bytes, costing 11,673 ms. Split by window frame rate it accounted for 99.4% of all
    // sub-threshold upload cost below 30 fps, 97.9% between 30 and 59, and 28.8% at 80 and above.
    // The worst single second spent 556 ms on 152 four-byte writes. The cost is a write-after-read
    // stall - Sodium writes the store during extract while the previous frame's terrain draws are
    // still sampling it - and it is unrelated to the number of bytes.
    //
    // Why a mapping is legal here, also measured: this layer promotes the store in glBufferStorage,
    // so its effective flags are 0x1c2 = MAP_WRITE | PERSISTENT | COHERENT | DYNAMIC_STORAGE, and a
    // persistent mapping of it succeeds on this driver (map_persistent_failures = 0). Coherent means
    // the write is visible without a flush, which is the same premise glFlushMappedBufferRange
    // already relies on.
    //
    // What is deliberately given up: a write through the mapping performs no write-after-read
    // synchronization, so it can land while a draw is still reading. The consequence is bounded and
    // was checked in Sodium's source rather than assumed - the store is a per-section fade timestamp
    // (R32_SINT texel buffer bound in ShaderChunkRenderer, consumed against
    // Options.chunkSectionFadeInTime), so the worst case is one section showing the wrong point of
    // its fade for one frame. It cannot move geometry. This is also exactly what upstream does to
    // this same buffer when DeviceFeatures.persistentMapping() is true.
    //
    // Adoption is deliberately reluctant: a store has to be small, created with the right flags, and
    // already have taken a threshold number of small writes. One-shot buffers therefore never get a
    // mapping, and the mapping that is taken is taken once per store per session.
    // `target` must currently be bound to `buffer`: a non-coherent mapping needs
    // glFlushMappedBufferRange after the write, and that call is target-based.
    GLboolean mg_pmap_write(GLenum target, GLuint buffer, GLintptr offset, GLsizeiptr size, const void* data);

    // "This buffer has been used as an index buffer." Called from glBindBuffer; bars the buffer from
    // ever being adopted, because the base-vertex emulation maps index buffers itself and its failure
    // path drops the draw silently.
    void mg_pmap_note_element(GLuint buffer);

    // Counts one small synchronous write against `buffer` and adopts it once it has proved hot.
    // `target` must currently be bound to `buffer`, because the mapping is taken through it.
    // `bufferBytes` is the size of the store, 0 when unknown - unknown is never adopted.
    void mg_pmap_consider(GLenum target, GLuint buffer, GLsizeiptr size, size_t bufferBytes);

    // Reasons a mapping is given up. Counted separately, because "the store was respecified" and
    // "the application wants to map it itself" would need different responses if either turned out
    // to be frequent.
    enum {
        MG_PMAP_EVICT_RESPECIFIED = 1, // glBufferData / glBufferStorage replaced the store
        MG_PMAP_EVICT_DELETED = 2,     // glDeleteBuffers; the driver has already dropped the mapping
        MG_PMAP_EVICT_APP_MAP = 3,     // the application mapped or unmapped it; never adopt again
        MG_PMAP_EVICT_ELEMENT = 4,     // bound as an index buffer; never adopt again
        MG_PMAP_EVICT_COPY = 5         // a GPU copy wrote into it
    };

    // Gives up the mapping. `boundTarget` must be a target currently bound to the buffer so the
    // mapping can be released, or 0 when the store is already gone and there is nothing to release.
    // MG_PMAP_EVICT_ELEMENT bars the buffer permanently, because an index buffer must never be held
    // mapped. MG_PMAP_EVICT_APP_MAP does not: it releases and makes the buffer earn the mapping
    // again, and only bars after several attempts. Barring on the first application mapping was the
    // defect in the first version - the store this exists for is mapped twice at construction and
    // never again, so one transient mapping locked it out for the session while the cache went and
    // adopted three cheap buffers instead.
    void mg_pmap_drop(GLuint buffer, GLenum boundTarget, int reason);

    // GL_TRUE while any mapping is held. Lets the invalidation sites skip their lookup entirely.
    GLboolean mg_pmap_any(void);

    // ---- Reusing the persistent mapping the APPLICATION already holds ------------------------
    //
    // This is the fix for the chunk-boundary stutter, and it exists because the previous three
    // attempts were all built on a premise that was false from the first line: that this layer could
    // take a mapping of the store in question. It cannot, and it never could.
    //
    // What the device and the bytecode jointly establish:
    //
    //   * `GlBuffer$Direct.<init>` (offsets 82-121) maps the whole buffer in its own constructor
    //     whenever `canPersistentMap && (usage & 3) != 0`, and then **discards the MappedView with a
    //     bare `pop` at 121**. Nothing ever closes it, so `mappingRefCount` stays at or above one for
    //     the object's whole life. `BufferStorage$Immutable.createBuffer` passes `iconst_1` for
    //     `canPersistentMap` (offset 23), so this happens to every immutable buffer whose usage has
    //     MAP_READ or MAP_WRITE.
    //   * The access mask it uses is computed at offsets 41-64: `usage & 2` contributes 0x32 and
    //     `canPersistentMap` contributes 0x40, giving 0x72. This layer strips
    //     GL_MAP_FLUSH_EXPLICIT_BIT, leaving 0x62 - which is bit for bit what the driver reports as
    //     BUFFER_ACCESS_FLAGS for the store, alongside BUFFER_MAPPED = 1.
    //   * A buffer may hold one mapping. So every `glMapBufferRange` this layer attempted on that
    //     store returned null with GL_INVALID_OPERATION - "the buffer is already in a mapped state",
    //     ES 3.2 6.3 - on all five access masks tried, while an identical request succeeded elsewhere.
    //
    // The cross-check that turns this from plausible into settled: `usage & 3 != 0` is equivalent to
    // the requested GL flags containing 0x1 or 0x2, so the bytecode *predicts* which buffers this
    // layer can map. The store that failed was requested with 0x142, which contains 0x2. The three
    // buffers that were successfully adopted were requested with 0x100, which contains neither. The
    // prediction and the measurement agree exactly.
    //
    // And the way out is that the pointer already exists and this layer is the one that produced it:
    // `DirectStateAccess$Core.mapBufferRange` calls `glMapNamedBufferRange`, which reaches
    // `glMapBufferRange` here, which forwards to the driver and hands the result back. So instead of
    // asking for a second mapping - which can never be granted - this records the first one and writes
    // through it.
    //
    // Why writing through it is not a liberty: `UniformBufferManager` took the branch that leaves
    // `sectionTimeInfoMap` null (its `<init>` offset 117 `ifeq 147`), which is exactly why
    // `writeMeshTimes` emits `writeToBuffer` at all - so the application is not using this pointer for
    // anything. And on a device where it does use it, what it does is `memPutInt` into the same
    // address. This performs the application's own fast path on its behalf.
    void mg_appmap_record(GLuint buffer, void* base, GLintptr offset, GLsizeiptr length, GLbitfield access);

    // Reasons a recorded mapping is dropped, counted separately so a surprise shows up as itself.
    enum {
        MG_APPMAP_FORGET_UNMAP = 1,       // the application unmapped it
        MG_APPMAP_FORGET_DELETED = 2,     // glDeleteBuffers
        MG_APPMAP_FORGET_RESPECIFIED = 3, // glBufferData / glBufferStorage replaced the store
        MG_APPMAP_FORGET_REMAPPED = 4     // mapped again; the record is being replaced
    };

    void mg_appmap_forget(GLuint buffer, int reason);

    // Satisfies a write from the application's own mapping. `target` must currently be bound to
    // `buffer`, because publishing the write uses glFlushMappedBufferRange, which is target-based.
    // Returns GL_TRUE when the bytes are in place and the caller must do nothing else.
    GLboolean mg_appmap_write(GLenum target, GLuint buffer, GLintptr offset, GLsizeiptr size, const void* data);

    // GL_TRUE while any application mapping is on record. Lets the invalidation sites skip their work.
    GLboolean mg_appmap_any(void);

    // Zero-timeout poll of the per-present frame fence; defined in egl/egl.cpp. Returns a
    // glClientWaitSync result, or 0 when there is no fence to poll. Cannot block.
    GLenum mg_frame_fence_poll(void);

    // ---- Deferred terrain upload ------------------------------------------------------------
    //
    // Queues the bytes of a large glNamedBufferSubData instead of writing them immediately, and
    // replays them at the frame boundary. See gl/buffer.cpp for the full reasoning; the short
    // version is that this changes *when* the write happens, which is the only axis left.
    //
    // Returns GL_TRUE when the upload was queued and the caller must do nothing else.
    GLboolean mg_deferred_upload_enqueue(GLuint realBuffer, GLintptr offset, GLsizeiptr size, const void* data);

    // Replays everything queued, waiting first for the fence taken when the queue was opened.
    // Called once per present from egl/egl.cpp. Safe to call with an empty queue.
    void mg_deferred_upload_flush_all(void);

    // Replays only the records targeting one buffer. The safety net: any operation that could
    // observe a queued buffer's contents, or destroy it, has to call this first.
    void mg_deferred_upload_flush_buffer(GLuint realBuffer);

    // Replays only if a queued record actually overlaps [offset, offset+size) of realBuffer.
    //
    // This is the discriminating form, and it is what the copy and sub-data paths must use. Sodium
    // writes its arena through both routes - a mapped ring plus glCopyBufferSubData for most
    // uploads, and glNamedBufferSubData when the ring is full - so a per-buffer test fires on
    // essentially every copy, which reinstates a fence wait per upload. Every pending upload owns a
    // separately allocated segment, so a range test almost never fires.
    void mg_deferred_upload_flush_if_range(GLuint realBuffer, GLintptr offset, GLsizeiptr size);

    // Driver buffer name currently bound to a target, or 0. Resolves target -> binding query ->
    // shadow -> real name in one call, from the shadow state only, with no driver round trip.
    //
    // Exists because get_binding_query is declared static in this header and so is not linkable
    // from another translation unit, while the safety-net sites in diagnostics/instrumented_gl.cpp
    // need exactly this lookup.
    GLuint mg_real_buffer_for_target(GLenum target);

    // Application-visible (fake) buffer name currently bound to a target, or 0. Same reason for
    // existing as mg_real_buffer_for_target: get_binding_query is static to buffer.cpp, so another
    // translation unit cannot go from a target to a binding query on its own. The per-buffer tables
    // in this header are keyed by the fake name, so callers that need to address those tables need
    // this rather than the real one.
    GLuint mg_fake_buffer_for_target(GLenum target);

    // Replays before a draw that could read a queued buffer. Called from the multi-draw entry
    // points, which is where Sodium's terrain draws arrive; see gl/buffer.cpp for why that is both
    // necessary and sufficient.
    void mg_deferred_upload_flush_for_draw(void);

    // GL_TRUE while anything is queued. Lets hot paths skip the per-buffer check entirely.
    GLboolean mg_deferred_upload_pending(void);

#endif

    GLuint gen_array();

    GLboolean has_array(GLuint key);

    void modify_array(GLuint key, GLuint value);

    void remove_array(GLuint key);

    GLuint find_real_array(GLuint key);

    GLuint find_bound_array();

    static GLenum get_binding_query(GLenum target);

    void InitBufferMap(size_t expectedSize);

    void InitVertexArrayMap(size_t expectedSize);

    GLAPI GLAPIENTRY void glGenBuffers(GLsizei n, GLuint* buffers);

    GLAPI GLAPIENTRY void glDeleteBuffers(GLsizei n, const GLuint* buffers);

    GLAPI GLAPIENTRY GLboolean glIsBuffer(GLuint buffer);

    GLAPI GLAPIENTRY void glBindBuffer(GLenum target, GLuint buffer);

    GLAPI GLAPIENTRY void glBindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr offset,
                                            GLsizeiptr size);

    GLAPI GLAPIENTRY void glBindBufferBase(GLenum target, GLuint index, GLuint buffer);

    GLAPI GLAPIENTRY void glBindVertexBuffer(GLuint bindingindex, GLuint buffer, GLintptr offset, GLsizei stride);

    GLAPI GLAPIENTRY void glTexBuffer(GLenum target, GLenum internalformat, GLuint buffer);

    GLAPI GLAPIENTRY void glTexBufferRange(GLenum target, GLenum internalformat, GLuint buffer, GLintptr offset,
                                           GLsizeiptr size);

    GLAPI GLAPIENTRY GLboolean glUnmapBuffer(GLenum target);

    GLAPI GLAPIENTRY void* glMapBuffer(GLenum target, GLenum access);

    GLAPI GLAPIENTRY void* glMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access);

    GLAPI GLAPIENTRY void glBufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage);

    GLAPI GLAPIENTRY void glBufferStorage(GLenum target, GLsizeiptr size, const void* data, GLbitfield flags);

    GLAPI GLAPIENTRY void glFlushMappedBufferRange(GLenum target, GLintptr offset, GLsizeiptr length);

    GLAPI GLAPIENTRY void glGenVertexArrays(GLsizei n, GLuint* arrays);

    GLAPI GLAPIENTRY void glDeleteVertexArrays(GLsizei n, const GLuint* arrays);

    GLAPI GLAPIENTRY GLboolean glIsVertexArray(GLuint array);

    GLAPI GLAPIENTRY void glBindVertexArray(GLuint array);

#ifdef __cplusplus
}
#endif

#define MOBILEGLUES_BUFFER_H

#endif // MOBILEGLUES_BUFFER_H
