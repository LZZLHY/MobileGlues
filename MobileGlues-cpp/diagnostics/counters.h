// MobileGlues - diagnostics/counters.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#ifndef MOBILEGLUES_DIAGNOSTICS_COUNTERS_H
#define MOBILEGLUES_DIAGNOSTICS_COUNTERS_H

// Low-overhead counters for the buffer, upload and synchronization paths.
//
// Why this exists: on a native mobile GLES driver, the expensive part of a frame is usually not
// the translation work but what the driver does with memory and synchronization. Those costs are
// invisible in a normal profile of this library, because the time is spent inside the driver on
// behalf of a call that looks cheap. Per-frame logging cannot measure it either: the stalls are
// bursty, and logging each one changes the timing being measured.
//
// So the counters accumulate silently and are flushed as one aggregate record per second, per GL
// thread. That is enough to answer the questions that matter — how much of each second went into
// waiting, how many uploads happened and how large they were, whether a wait actually blocked or
// returned already-signalled — while staying cheap enough to leave in place.
//
// Design rules:
//   * No allocation and no locks on the hot path. Counters are thread_local, because OpenGL calls
//     for one context are serialized on the thread that owns it; a second context on another
//     thread gets its own set instead of contending for one.
//   * Measurement only. Nothing here may influence rendering behaviour, so a build or a device
//     with diagnostics disabled behaves exactly like one without this component. Hazard tracking
//     and anything else correctness relevant belongs in the GL paths themselves, never here.
//   * Stable field names. The records are compared across builds and devices, so renaming a field
//     invalidates earlier measurements and should be treated as a breaking change.
//
// Enabled by the diagnostics setting; see config/settings.h. Disabled by default: the aggregation
// is cheap but not free, and a released build should not pay for it.

#include "../includes.h"

#include <chrono>
#include <cstdint>

namespace mg::diagnostics {

    // Set once by configure(). Read on the hot path, so it is a plain bool rather than an atomic:
    // it is written before rendering starts and never changes afterwards.
    extern bool g_enabled;

    inline bool enabled() {
        return g_enabled;
    }

    // Applies the diagnostics setting. Call once from settings initialization.
    void configure(bool enable);

#if defined(MG_PLATFORM_OHOS)

    // Per-destination accounting for the sub-threshold upload path.
    //
    // Why identity and not just totals: the 2026-08-06 device round measured 17,133 calls of 41.8
    // bytes each costing 6,857 ms, worst single call 116,707 us, all of them glNamedBufferSubData
    // into a buffer below DIRECT_MAP_MIN_SIZE and therefore taking the ordered path. Totals cannot
    // say which buffers those are, and the comment justifying the threshold assumed they were
    // harmless because their byte volume is negligible - which is true and irrelevant, because the
    // cost is a per-call stall rather than a transfer. Naming the destinations is what turns this
    // from a guess into a decision.
    //
    // copy_dest carries the discriminator from gl/buffer.h: a buffer that has been a
    // glCopyBufferSubData destination is Sodium's, one that has not is Minecraft's own. That flag
    // measured 6,474 hits in the same session, so it is live rather than theoretical.
    struct FallbackDest {
        uint64_t buffer = 0;       // fake id as the application sees it, 0 when the slot is unused
        uint64_t dest_bytes = 0;   // total size of the destination store
        uint64_t calls = 0;
        uint64_t ns = 0;
        uint64_t max_ns = 0;
        uint64_t upload_bytes = 0; // summed, so upload_bytes/calls is the mean write size
        uint64_t copy_dest = 0;    // non-zero once seen as a GPU-copy destination
        // How the store was created; this is what names the buffer. See gl/buffer.h mg_record_store.
        uint64_t req_flags = 0;
        uint64_t eff_flags = 0;
        uint64_t entry = 0; // 1 glBufferStorage, 2 glBufferData, 0 never seen
    };

    // Eight is chosen to be larger than the number of distinct destinations a frame is expected to
    // touch while still being a linear scan of trivial cost. Anything beyond it lands in the
    // overflow pair, and a large fallback_overflow_ns is the signal that the table needs widening
    // rather than a licence to ignore the remainder.
    constexpr int FALLBACK_DEST_SLOTS = 8;

    // Destination-size buckets, independent of identity so they cannot overflow. The point is to
    // learn the size class of the stalling stores: a uniform-sized store and a multi-megabyte one
    // imply very different fixes.
    constexpr int FALLBACK_SIZE_BUCKETS = 5; // <64K, <1M, <4M, <16M, >=16M

    // Per-buffer mapping accounting for large stores. See Counters::map_dest.
    struct MapDest {
        uint64_t buffer = 0;
        uint64_t length = 0;
        uint64_t maps = 0;
        uint64_t unmaps = 0;
        uint64_t failures = 0;
        uint64_t access_or = 0;
    };

    // Per-buffer state of the persistent-mapping cache, for every buffer that reached
    // mg_pmap_consider.
    //
    // This exists because the first device round could not explain its own result. The cache adopted
    // three buffers, reported zero map failures and zero evictions, and yet the one store that
    // carries the entire cost was never adopted - and no counter could say which of the several early
    // returns in mg_pmap_consider it took. The gap got filled by reasoning, which is the one thing
    // this work is not allowed to do. These fields make each early return visible instead:
    //
    //   adopted = 1                     it worked; look at pmap_writes for what it bought.
    //   declined = 1                    barred. reason says by what - 4 is an index-buffer bar,
    //                                   3 means the application kept taking the mapping back.
    //   small_writes < 64, not adopted  still earning it, or being reset by something.
    //   small_writes = 0 across windows something resets it every time; that is an application
    //                                   mapping, and strikes will be climbing.
    //   eff_flags without 0x42          the store cannot hold a persistent mapping, so the flag test
    //                                   refused it. Expected 0x1c2 under promote mode 1, 0x142 under
    //                                   mode 2 - which withholds COHERENT and is still adoptable,
    //                                   because mapping needs WRITE|PERSISTENT and nothing more.
    //                                   CORRECTED: this line used to say 0xc2, implying a COHERENT
    //                                   gate. There has never been one - gl/buffer.cpp
    //                                   mg_pmap_consider tests 0x42 and says in so many words that
    //                                   requiring COHERENT here is the mistake that made the first
    //                                   version of the fix fail.
    struct PmapDest {
        uint64_t buffer = 0;
        uint64_t bytes = 0;
        uint64_t small_writes = 0;
        uint64_t adopted = 0;
        uint64_t declined = 0;
        uint64_t strikes = 0;
        uint64_t eff_flags = 0;
        uint64_t considers = 0;
    };

    constexpr int PMAP_DEST_SLOTS = 8;

    constexpr int MAP_DEST_SLOTS = 8;
    // Only stores at or above this size are tracked, to keep the table meaningful. The store under
    // investigation is 229,376 bytes and the uniform ring is 262,144.
    constexpr uint64_t MAP_DEST_MIN_BYTES = 64u * 1024u;

#endif // MG_PLATFORM_OHOS

    struct Counters {
        // Frame boundaries, used as the denominator for everything else.
        uint64_t color_clears = 0;

        // Waits performed by this library at the frame boundary.
        uint64_t frame_wait_calls = 0;
        uint64_t frame_wait_ns = 0;
        uint64_t frame_wait_max_ns = 0;
        uint64_t frame_wait_already = 0;
        uint64_t frame_wait_satisfied = 0;
        uint64_t frame_wait_timeout = 0;
        uint64_t frame_wait_failed = 0;
        uint64_t frame_wait_other = 0;

        // Waits requested by the application through glClientWaitSync. Counted separately: a
        // zero-timeout poll and a blocking wait mean very different things.
        uint64_t sync_wait_calls = 0;
        uint64_t sync_wait_zero_timeout = 0;
        uint64_t sync_wait_positive_timeout = 0;
        uint64_t sync_wait_ns = 0;
        uint64_t sync_wait_max_ns = 0;
        uint64_t sync_wait_already = 0;
        uint64_t sync_wait_satisfied = 0;
        uint64_t sync_wait_timeout = 0;
        uint64_t sync_wait_failed = 0;
        uint64_t sync_wait_other = 0;

        // Direct-state-access uploads, the hot path for terrain in recent Minecraft versions.
        uint64_t named_calls = 0;
        uint64_t named_bytes = 0;
        uint64_t named_ns = 0;
        uint64_t named_max_ns = 0;
        uint64_t named_max_buffer_bytes = 0;
        uint64_t named_max_upload_bytes = 0;

        // Uploads served by writing through a mapping instead of a driver copy.
        uint64_t direct_attempts = 0;
        uint64_t direct_hits = 0;
        uint64_t direct_bytes = 0;
        uint64_t direct_map_ns = 0;
        uint64_t direct_memcpy_ns = 0;
        uint64_t direct_unmap_ns = 0;

#if defined(MG_PLATFORM_OHOS)
        // ----- Probes for the 26.2+Sodium terrain race -------------------------------------
        //
        // These exist to turn four guesses into numbers. Three rounds of work on this problem each
        // started from a mechanism instead of a measurement and each was wrong, while the path in
        // question is reached only about 41 times per second - so measuring it is far cheaper than
        // reasoning about it.
        //
        // Everything here is platform-guarded so the generic translation units stay byte-identical.

        // Was the previous frame's fence already signalled at the moment of a direct write?
        //
        // This decides whether a cheap provably-correct fix exists. If the fence is already
        // signalled, nothing submitted before the last present is still reading, and the
        // unsynchronized write is safe with no buffer, range or binding tracking at all. If it is
        // usually unsignalled, that whole family of fixes is dead and we stop considering it.
        //
        // Note what the fence does NOT prove: eglSwapBuffers waits on the fence created at the
        // *previous* present, so a signalled fence says the GPU finished the previous frame, not
        // the current one. That is why this is polled at the write rather than assumed.
        uint64_t direct_poll_already = 0;
        uint64_t direct_poll_timeout = 0;
        uint64_t direct_poll_other = 0;

        // Is the destination of a direct write also a glCopyBufferSubData destination?
        //
        // Sodium pushes terrain through its own persistently mapped ring plus a GPU copy, reaching
        // glNamedBufferSubData only when an upload does not fit the ring's remaining space.
        // Minecraft's own uploader writes its terrain heap directly and (as far as we know) never
        // copies into it. If that holds, "is a copy destination" separates Sodium's arena from
        // vanilla's heap using only calls this layer already sees - no guesses about the
        // application's internal upload/draw ordering.
        //
        // This counter exists to check that claim on vanilla 26.2 BEFORE relying on it. Expect
        // roughly all direct writes flagged under Sodium and none on vanilla; a non-zero count on
        // vanilla kills the idea and saves a shipped regression.
        uint64_t direct_dest_copy_target = 0;

        // Sampled cost of the same mapped write WITHOUT GL_MAP_UNSYNCHRONIZED_BIT.
        //
        // This is the only fallback that could make any gate affordable. Rejecting all 41 direct
        // writes per second into glBufferSubData costs roughly 177 ms/s on this driver, which is
        // worse than the staging-ring build that was already rejected. Keeping the mapping and
        // letting the driver do the write-after-read wait itself might cost anything between the
        // 158 us an unsynchronized map costs and the 4.6 ms glBufferSubData costs. Nobody has
        // measured it, and the answer decides the design.
        //
        // Sampled rather than always-on, and only while diagnostics are enabled, so the cost of
        // measuring stays bounded and release builds are untouched.
        uint64_t direct_sync_map_calls = 0;
        uint64_t direct_sync_map_ns = 0;
        uint64_t direct_sync_map_max_ns = 0;
        uint64_t direct_unsync_map_calls = 0;
        uint64_t direct_unsync_map_ns = 0;
        uint64_t direct_unsync_map_max_ns = 0;

        // ----- Deferred terrain upload ------------------------------------------------------
        //
        // deferred_forced_flush is the field that matters most: it counts replays triggered by
        // something other than the frame boundary, i.e. the safety net firing. The design rests on
        // nothing reading these heaps between the upload and the present, which was verified from
        // Minecraft 26.2 bytecode; a non-zero count here means that verification does not hold on
        // some path and the assumption needs revisiting rather than trusting.
        //
        // deferred_fence_timeout is the other one to watch. The replay polls the queue's fence and
        // only blocks if the poll fails. Timeouts mean the blocking wait is being paid, which is
        // the one way this design could cost frame time.
        uint64_t deferred_enqueued = 0;
        uint64_t deferred_enqueued_bytes = 0;
        uint64_t deferred_replayed = 0;
        uint64_t deferred_replayed_bytes = 0;
        uint64_t deferred_replay_fallback = 0;
        uint64_t deferred_overflow = 0;
        uint64_t deferred_forced_flush = 0;
        uint64_t deferred_fence_already = 0;
        uint64_t deferred_fence_satisfied = 0;
        uint64_t deferred_fence_timeout = 0;
        uint64_t deferred_fence_other = 0;
        // Drains that found no fence at all. Added 2026-08-08 to close a blind spot: that path
        // returns before any of the four counters above is touched, yet it still forces the whole
        // batch onto the ordered glBufferSubData replay. A device round read 66 ordered replays with
        // deferred_fence_timeout = 0, which looked like a contradiction until this was the answer.
        // Non-zero here means glFenceSync failed when the queue opened, or a drain ran against
        // records that were enqueued without one.
        uint64_t deferred_fence_missing = 0;

        // Mid-frame drains, and the ordered writes they now perform instead of waiting on a fence.
        //
        // A drain that runs inside the frame which queued the bytes cannot wait on the queue's
        // fence cheaply. The fence was created in that same frame, and nothing between Sodium's
        // upload phase and its terrain draw submits: Minecraft.renderFrame calls GameRenderer
        // .extract at offset 440 and GameRenderer.render at 526, while CommandEncoder.submit has
        // exactly one call site in the client, at offset 702, after all drawing. So the
        // zero-timeout poll necessarily fails and the fallback pays GL_SYNC_FLUSH_COMMANDS_BIT -
        // 3.77 ms plus a pipeline drain, with the render pass open. These counters measure the
        // ordered replay that replaces that wait.
        //
        // What to read: deferred_midframe_flush divided by frame_wait_calls is how many mid-frame
        // drains happen per present, and deferred_ordered_ns is what they cost per second. On
        // vanilla 26.2, 26.1.2 and 1.21.11 all four should read 0, because those versions upload
        // after they draw and so are served entirely by the present-time drain. A non-zero count
        // there means a draw is reaching the queue mid-frame on a configuration where it should
        // not, and the frame-order premise in RENDER-ADAPTATION.md 2.1 needs re-deriving.
        uint64_t deferred_midframe_flush = 0;
        uint64_t deferred_ordered_replay = 0;
        uint64_t deferred_ordered_bytes = 0;
        uint64_t deferred_ordered_ns = 0;
        uint64_t deferred_ordered_max_ns = 0;

        // ----- Sub-threshold upload path, per destination ------------------------------------
        //
        // See FallbackDest. These describe the calls counted by fallback_calls, which the
        // 2026-08-06 round showed to be the dominant per-second cost while crossing chunk
        // boundaries - 124 ms/s in low-frame windows against 45 ms/s for Sodium's arena copies,
        // with the frame fence measuring zero timeouts and so ruled out.
        FallbackDest fallback_dest[FALLBACK_DEST_SLOTS] = {};
        uint64_t fallback_overflow_calls = 0;
        uint64_t fallback_overflow_ns = 0;

        uint64_t fallback_bucket_calls[FALLBACK_SIZE_BUCKETS] = {};
        uint64_t fallback_bucket_ns[FALLBACK_SIZE_BUCKETS] = {};
        uint64_t fallback_bucket_max_ns[FALLBACK_SIZE_BUCKETS] = {};

        // Is the cost spread across every call or concentrated in a few?
        //
        // The mean was 400 us and the worst 116,707 us, so the distribution is heavily skewed and
        // the mean describes nothing. If the total lands in a few hundred slow calls, only those
        // need handling and the cheap majority can be left alone; if it is spread evenly, the
        // threshold itself is what has to change. These two answer that, and the answer decides
        // which fix is even applicable.
        uint64_t fallback_slow_calls = 0; // > 1 ms
        uint64_t fallback_slow_ns = 0;

        // Sub-data calls dropped as redundant. fallback_calls + fallback_deduped is what the call
        // count would have been without the elision, which is the comparison that says whether it
        // worked.
        uint64_t fallback_deduped = 0;
        uint64_t fallback_deduped_bytes = 0;

        // Parameters of the single worst call in the window, to pin the offender exactly.
        uint64_t fallback_worst_ns = 0;
        uint64_t fallback_worst_buffer = 0;
        uint64_t fallback_worst_dest_bytes = 0;
        uint64_t fallback_worst_upload_bytes = 0;
        uint64_t fallback_worst_offset = 0;

        // ----- Which BufferStorage implementation did Blaze3D choose? ------------------------
        //
        // This settles a contradiction. MobileGlues reports GL_ARB_buffer_storage (device log
        // 2026-08-07: "glBufferStorageEXT=resolved, GL_ARB_buffer_storage reported to the
        // application"), yet Sodium behaves as though DeviceFeatures.persistentMapping() were false -
        // UniformBufferManager.writeMeshTimes reached the MemoryStack.malloc(4) + writeToBuffer
        // branch, which it only takes when sectionTimeInfoMap is null, which only happens when
        // persistentMapping() is false.
        //
        // The real gate is not the extension string this layer publishes but LWJGL's
        // GLCapabilities.GL_ARB_buffer_storage, which is what BufferStorage.create tests before
        // doing set.add("GL_ARB_buffer_storage"), and that set is what createDeviceInfo reads. If
        // LWJGL never saw the string - wrong ordering, or a function it could not resolve - the flag
        // is false however loudly this layer advertises it.
        //
        // Blaze3D's choice is directly observable from which entry point creates buffers:
        //   BufferStorage$Immutable -> glNamedBufferStorage -> this layer's glBufferStorage
        //   BufferStorage$Mutable   -> glNamedBufferData    -> this layer's glBufferData
        // So storage_calls > 0 means LWJGL did see it and the contradiction lies further in;
        // storage_calls == 0 with bufferdata_calls > 0 means it did not, and the fix is in this
        // layer's extension reporting or its ordering.
        //
        // storage_promoted counts the stores this layer upgraded to COHERENT | PERSISTENT, which is
        // what makes its own glBufferSubData path as cheap as it currently is.
        uint64_t storage_calls = 0;
        uint64_t storage_promoted = 0;
        uint64_t storage_no_ext_fn = 0; // glBufferStorageEXT was null, so nothing was allocated
        uint64_t bufferdata_calls = 0;

        // ----- Does the persistent mapping actually succeed? ---------------------------------
        //
        // This is the last unknown in the chunk-boundary stutter, and the previous round narrowed it
        // to exactly one question.
        //
        // Established on device 2026-08-07, all in one session: MobileGlues reports
        // GL_ARB_buffer_storage; Blaze3D took BufferStorage$Immutable throughout
        // (storage_calls=11,516, bufferdata_calls=0), which proves LWJGL's
        // GLCapabilities.GL_ARB_buffer_storage was true, which proves BufferStorage.create ran
        // set.add("GL_ARB_buffer_storage"), which proves DeviceFeatures.persistentMapping() is true.
        // Yet u_SectionTimeInfo still received 6,374 sub-data writes costing 6,846 ms - and
        // UniformBufferManager.writeMeshTimes only takes that branch when sectionTimeInfoMap is null,
        // which UniformBufferManager.<init> only leaves null when persistentMapping() is false.
        //
        // Both cannot hold, so the remaining candidate is that persistentMapping() is true and the
        // map call itself fails: GlBuffer$Direct.map requests the whole buffer with
        // GL_MAP_WRITE | FLUSH_EXPLICIT | UNSYNCHRONIZED | PERSISTENT, this layer strips
        // FLUSH_EXPLICIT, and if the driver then returns null the field stays null and every write
        // falls back to sub-data. Nothing in the file log would show it, because the existing
        // "Failed to map buffer range" is LOG_W and only LOG_I reaches MG/latest.log.
        //
        // So: record every mapping of a store large enough to matter, with the access bits actually
        // passed to the driver, whether a pointer came back, and the GL error if not.
        uint64_t map_attempts = 0;
        uint64_t map_failures = 0;
        uint64_t map_persistent_attempts = 0;
        uint64_t map_persistent_failures = 0;
        uint64_t map_access_or = 0;      // OR of all access masks passed to the driver
        uint64_t map_fail_access_or = 0; // OR of the access masks that failed
        uint64_t map_fail_buffer = 0;    // parameters of the most recent failure
        uint64_t map_fail_length = 0;
        uint64_t map_fail_error = 0;
        uint64_t map_big_ok_buffer = 0; // most recent SUCCESSFUL map of a >=64 KiB store
        uint64_t map_big_ok_length = 0;
        uint64_t map_big_ok_access = 0;

        // Per-buffer map/unmap accounting for large stores. This is what the previous round's probe
        // should have been: "most recent successful map" was overwritten within its window by the
        // uniform ring's 262,144-byte maps, so it could neither confirm nor deny that the 229,376-byte
        // store was ever mapped.
        //
        // The discriminator this gives: a store whose mapping is HELD shows maps == 1 and unmaps == 0,
        // because GlBuffer$Direct caches the mapping and only unmaps when its refcount reaches zero.
        // A store that is mapped, memset and closed at construction shows maps == 1 and unmaps == 1.
        // UniformBufferManager.<init> does the former when persistentMapping() is true and the latter
        // when it is false, so for the 229,376-byte store these two numbers decide which branch ran -
        // which is exactly the contradiction still open.
        MapDest map_dest[MAP_DEST_SLOTS] = {};
        uint64_t map_dest_overflow = 0;

        // ----- Was a buffer deleted while it was still bound? --------------------------------
        //
        // This is the measurement for the region-misplacement artifact, and it is the first one
        // that has a mechanism behind it rather than a resemblance. gl/buffer.cpp
        // unbind_deleted_buffer has the full chain; the short form is that MobileGlues used to keep
        // naming a deleted fake id in its binding shadow, and the restore paths then handed that
        // fake number to the driver as a driver name, after which a recycled id could make
        // temporarilyBindBuffer skip its rebind and send a write into the wrong buffer.
        //
        // How to read it:
        //   calls == 0            the situation never arises, and this cannot be the artifact.
        //                         Expected on vanilla 26.2 / 26.1.2 / 1.21.11, whose buffers are
        //                         long-lived.
        //   calls > 0 with Sodium the hazard is live and its rate should track region churn -
        //                         near zero while standing still, rising while crossing chunk
        //                         boundaries, highest under elytra flight. That correlation is what
        //                         would make the attribution more than circumstantial.
        //   targets vs vaos       which half fired. targets is GL_ARRAY_BUFFER and friends, i.e.
        //                         vertex and transfer bindings; vaos is the element-array binding
        //                         held per vertex array, i.e. index buffers. Sodium's arena buffers
        //                         carry usage 120 = VERTEX|INDEX|COPY_SRC|COPY_DST, so both are
        //                         possible and they imply different visual damage.
        //
        // Counted after the fix is applied, so a non-zero value means "this would have gone wrong",
        // not "this went wrong". That is the only honest way to measure a hazard that has just been
        // closed, and it is why the artifact verdict needs the frame-by-frame image check as well.
        uint64_t unbind_on_delete_calls = 0;
        uint64_t unbind_on_delete_targets = 0;
        uint64_t unbind_on_delete_vaos = 0;

        // ----- Layer-owned persistent mapping ------------------------------------------------
        //
        // The acceptance criteria for the chunk-boundary stutter fix. See gl/buffer.h for the design
        // and gl/buffer.cpp for the cache.
        //
        // The comparison that decides whether it worked, against the 287 s session of 2026-08-07
        // that motivated it:
        //
        //   before   fb_calls 51,919   fb_us 12,296 ms   of which u_SectionTimeInfo 11,673 ms
        //            below 30 fps that store was 99.4% of all sub-threshold cost; 37 windows under
        //            30 fps and 104 of 287 under 60
        //   after    pmap_writes should carry roughly 9,300 of those calls at microsecond cost,
        //            fb_us should lose about 11,700 ms, and the windows under 30 fps should go away
        //
        // What each number decides:
        //   adopted            should be exactly 1 in a Sodium session and 0 on the three vanilla
        //                      versions, which never take thousands of small writes into one small
        //                      store. More than one or two means the criteria are too loose and
        //                      something unintended is being held mapped.
        //   map_failures       must be 0. The mapping was already proved to succeed on this driver
        //                      (map_persistent_failures = 0, access 0x62), so a failure means the
        //                      flag test in mg_pmap_consider let through a store that cannot hold a
        //                      persistent mapping, and every write on it fell back.
        //   writes / write_ns  the whole point. Expect single-digit microseconds per write in total,
        //                      against the 1,255 us average and 84 ms worst the sub-data path
        //                      measured on the same store.
        //   evict_*            each reason separately, because they mean different things. respecified
        //                      and copy should be rare. app_map non-zero means the application wanted
        //                      the mapping and we correctly stepped aside - but also that adoption is
        //                      fighting the application and the criteria should exclude that buffer
        //                      earlier. element non-zero means an index buffer was barred, which is
        //                      the guard against the base-vertex emulation silently dropping draws.
        //   deleted            expected once per adopted store at world unload.
        uint64_t pmap_adopted = 0;
        uint64_t pmap_map_failures = 0;
        uint64_t pmap_writes = 0;
        uint64_t pmap_write_bytes = 0;
        uint64_t pmap_write_ns = 0;
        uint64_t pmap_write_max_ns = 0;
        uint64_t pmap_evict_respecified = 0;
        uint64_t pmap_evict_deleted = 0;
        uint64_t pmap_evict_app_map = 0;
        uint64_t pmap_evict_element = 0;
        uint64_t pmap_evict_copy = 0;

        // Buffers barred without ever having held a mapping. The pmap_evict_* counters above only
        // fire when a mapping was actually released, so this case - which is the one that went wrong
        // in the first round - read as zero everywhere and looked like nothing had happened.
        uint64_t pmap_barred_app_map = 0;
        uint64_t pmap_barred_element = 0;

        // ----- Which mapping access mask does this driver actually grant? --------------------
        //
        // The question that decided the third failed attempt at this fix, and it had never been
        // asked. The cache requested WRITE | PERSISTENT | COHERENT, the driver returned null, and the
        // buffer was barred for the session - while the justification on record said "a persistent
        // mapping of it succeeds, access 0x62". 0x62 is WRITE | UNSYNCHRONIZED | PERSISTENT: no
        // coherence. The application never asks for coherence on a mapping, so nothing had
        // established that this driver grants it, and RENDER-ADAPTATION.md 6.4 already said as much.
        //
        // NOTE: these live in PmapAcceptance below, NOT in this struct, because Counters is wiped at
        // every window boundary and an adoption happens once per session. Left documented here
        // because this is where the rest of the pmap accounting is.
        //
        // accepted_rung / accepted_access say which rung of PMAP_LADDER in gl/buffer.cpp the driver
        // took. Read it as:
        //   rung 0 or 1   coherent mapping granted; writes need no flush and 6.4's worry is unfounded
        //                 for mappings this layer takes.
        //   rung 2 or 3   coherent refused but explicit flush granted; pmap_flushes then carries the
        //                 real cost of publishing each write, which is the number that decides
        //                 whether this design survives.
        //   rung 4        neither; the mapping is the one the application itself gets, with no way to
        //                 publish a write. Using it relies on 6.4's unproven premise, so this rung
        //                 being taken is a finding in its own right and must not pass unremarked.
        //
        // ladder_failures with the mask and error of each refusal is what turns "the driver said no"
        // into "the driver said no to this bit, with this error".
        // Cost of publishing a write on a non-coherent mapping. Only non-zero on rungs 2 and 3.
        uint64_t pmap_flushes = 0;
        uint64_t pmap_flush_ns = 0;
        uint64_t pmap_flush_max_ns = 0;

        // ----- Writes served through the application's own persistent mapping ----------------
        //
        // The acceptance criteria for the chunk-boundary stutter fix. See gl/buffer.h mg_appmap_record
        // for why this is the only route that can work on the store that matters.
        //
        // The comparison, against the session that motivated it - 174 s, MC 26.2 + Sodium, the build
        // that could not map the store at all:
        //
        //   before   fb_calls 4,925  fb_us 11,198 ms, of which the 229,376-byte store 11,196 ms
        //            (100.0%), mean 2,370 us, worst 93.8 ms, 66 of 173 windows under 60 fps
        //   after    appmap_writes should carry essentially all of that store's calls, appmap_write_us
        //            should be single-digit milliseconds in total, and fb_us should collapse
        //
        // The two honest unknowns, both measured here rather than predicted:
        //   appmap_flush_*        publishing cost. The mapping is persistent but not coherent, so each
        //                         write needs glFlushMappedBufferRange. If that turns out to cost what
        //                         the sub-data it replaces cost, this design fails and these numbers
        //                         are how that is discovered rather than argued.
        //   appmap_unpublished    writes made with no way to publish them. Non-zero means the mapping
        //                         lacked GL_MAP_FLUSH_EXPLICIT_BIT and the bytes rest on the
        //                         storage-level coherence promise alone - the open question in 6.4. It
        //                         should read 0 now that glMapBufferRange keeps that bit for persistent
        //                         mappings; if it does not, the write path is on thin ice and the fade
        //                         animation is where it would show.
        //
        // appmap_out_of_range should read 0: GlBuffer$Direct always maps the whole buffer.
        uint64_t appmap_writes = 0;
        uint64_t appmap_write_bytes = 0;
        uint64_t appmap_write_ns = 0;
        uint64_t appmap_write_max_ns = 0;
        uint64_t appmap_flushes = 0;
        uint64_t appmap_flush_ns = 0;
        uint64_t appmap_flush_max_ns = 0;
        uint64_t appmap_unpublished = 0;
        uint64_t appmap_out_of_range = 0;

        // REMOVED 2026-08-07: md_* (multi-draw), present_*, native_* and inside_*.
        //
        // All four answered their question and were then cost, the last two on every GL entry point in
        // the library. What they established, so it does not have to be re-measured:
        //   * the multi-draw path costs 0.30-0.33 ms per frame and the present 0.29-0.39 ms per frame,
        //     both flat across frame rates, so neither is the stutter;
        //   * of a frame's CPU time in the steady state, 8-10% is in the macro-generated forwards and
        //     14-18% inside this library as a whole - the remaining 82-86% is the application's own
        //     code, so the frame-rate ceiling is not here.
        // See docs/ohos/RENDER-ADAPTATION.md 6.11.

        PmapDest pmap_dest[PMAP_DEST_SLOTS] = {};
        uint64_t pmap_dest_overflow = 0;
#endif

        // REMOVED 2026-08-07: native_*, frame_gap_* and inside_*. See the note above pmap_dest.

        // Uploads that took the ordinary synchronous path.
        uint64_t fallback_calls = 0;
        uint64_t fallback_bytes = 0;
        uint64_t fallback_ns = 0;

        uint64_t subdata_calls = 0;
        uint64_t subdata_bytes = 0;
        uint64_t subdata_ns = 0;
        uint64_t subdata_max_ns = 0;

        uint64_t map_calls = 0;
        uint64_t map_bytes = 0;
        uint64_t map_ns = 0;
        uint64_t map_max_ns = 0;

        // flush_skipped is the interesting one: coherent mappings publish writes without a
        // cache-maintenance command, so a skipped flush is expected rather than a lost one.
        uint64_t flush_calls = 0;
        uint64_t flush_driver_calls = 0;
        uint64_t flush_skipped_calls = 0;
        uint64_t flush_bytes = 0;
        uint64_t flush_ns = 0;
        uint64_t flush_max_ns = 0;

        uint64_t copy_calls = 0;
        uint64_t copy_bytes = 0;
        uint64_t copy_ns = 0;
        uint64_t copy_max_ns = 0;

#if defined(MG_PLATFORM_OHOS)
        // ----- Buffer-to-buffer copies, by identity and bandwidth ----------------------------
        //
        // The leading candidate for the hitch once the upload cost was gone. Measured over 89 windows:
        // glCopyNamedBufferSubData was the slowest entry point in this library in 17 of them, those
        // windows averaged 42.8 fps against about 51 overall, and the worst single copy took 52.3 ms. A
        // single 52 ms copy is a dropped frame on its own, which is what a hitch is - so the total being
        // modest does not clear it.
        //
        // Why identity and flags and not just totals: docs/ohos/RENDER-ADAPTATION.md section 2 already
        // recorded that a copy has to *read* coherent write-combined memory "at only a few megabytes per
        // second", and this library promotes every store carrying GL_DYNAMIC_STORAGE_BIT to
        // COHERENT | PERSISTENT - 2,794 promotions in one session, none refused. Sodium's arena buffers
        // are all in that set. So this library's own promotion is a candidate cause of its slow copies,
        // and the way to tell is bytes over time plus the flags of the two buffers involved.
        //
        // How to read it:
        //   copy_slow_bytes / copy_slow_ns   effective bandwidth of the slow copies. A few MB/s confirms
        //                                    the write-combined read; hundreds of MB/s refutes it and the
        //                                    stall is synchronization instead.
        //   copy_worst_read_flags            0x80 present means the source was coherent, which is the
        //                                    promotion. Absent means the premise does not apply here.
        //   copy_slow_calls vs copy_calls    concentrated or spread. Concentrated means only the region
        //                                    transfers matter.
        uint64_t copy_slow_calls = 0;
        uint64_t copy_slow_ns = 0;
        uint64_t copy_slow_bytes = 0;
        uint64_t copy_worst_ns = 0;
        uint64_t copy_worst_bytes = 0;
        uint64_t copy_worst_read = 0;
        uint64_t copy_worst_write = 0;
        uint64_t copy_worst_read_flags = 0;
        uint64_t copy_worst_write_flags = 0;
        uint64_t copy_flags_read_or = 0;
        uint64_t copy_flags_write_or = 0;

        // The driver's own glClear, timed separately from this library's frame-boundary bookkeeping.
        // glClear was the slowest entry point in 25 windows with a worst of 41.8 ms, and this says
        // whether that is the driver resolving tiles or work this library added.
        uint64_t clear_calls = 0;
        uint64_t clear_ns = 0;
        uint64_t clear_max_ns = 0;
#endif
    };

    inline thread_local Counters g_counters;

#if defined(MG_PLATFORM_OHOS)
    // Deliberately NOT part of Counters, because Counters is zeroed at every window boundary and each
    // of these records a once-per-session event. Keeping them here is what makes them readable from a
    // log window other than the one the adoption happened in - the lack of exactly that property is
    // why the previous round's failure took a whole device session to identify.
    struct PmapAcceptance {
        uint64_t rung = 0;   // 1-based index into PMAP_LADDER; 0 means nothing accepted yet
        uint64_t access = 0; // the mask the driver granted
        uint64_t failures = 0;
        uint64_t fail_access_or = 0; // OR of every mask refused
        uint64_t fail_error_or = 0;  // OR of every GL error given
        uint64_t fail_last_rung = 0; // 1-based rung of the most recent refusal
    };

    inline thread_local PmapAcceptance g_pmap_acc;

    // What the driver says about a buffer at the moment it refuses to map it.
    //
    // Captured once, for the first buffer that fails every rung of PMAP_LADDER. It exists because the
    // spec narrows GL_INVALID_OPERATION from MapBufferRange to a short list, and every remaining
    // candidate is directly queryable - so there is no reason left to reason about it:
    //
    //   storage_flags without 0x40   EXT_buffer_storage requires the buffer's BUFFER_STORAGE_FLAGS to
    //                                contain every one of READ / WRITE / PERSISTENT / COHERENT that
    //                                access asks for. All five rungs set PERSISTENT, so a single
    //                                missing bit here explains all five failures at once, and would
    //                                mean this layer's promotion in glBufferStorage did not take -
    //                                which nothing checks, because glBufferStorage reads no error.
    //   immutable = 0                glBufferStorageEXT never took effect for this buffer at all; it
    //                                is still a mutable store, whose flags per Table 6.3 cannot
    //                                contain PERSISTENT.
    //   mapped = 1                   "The buffer is already in a mapped state" - the one mask
    //                                independent condition in the ES 3.2 list. access_flags then says
    //                                with what.
    //   driver_binding != expected   GL_ARRAY_BUFFER is not actually bound to this buffer, so the map
    //                                applied to something else or to zero. Zero would give exactly
    //                                this error, and temporarilyBindBuffer's fast path trusts a
    //                                shadow rather than the driver.
    //   query_error = 0x502          even the queries failed, which per ES 3.2 6.6 means the target
    //                                has no buffer bound - proving the binding case on its own.
    struct PmapProbe {
        uint64_t captured = 0;
        uint64_t buffer = 0;
        uint64_t recorded_bytes = 0;
        uint64_t driver_binding = 0;
        uint64_t expected_real = 0;
        uint64_t buffer_size = 0;
        uint64_t immutable = 0;
        uint64_t storage_flags = 0;
        uint64_t mapped = 0;
        uint64_t access_flags = 0;
        uint64_t query_error = 0;
    };

    inline thread_local PmapProbe g_pmap_probe;

    // Errors from glBufferStorageEXT, which nothing used to read.
    //
    // failures > 0 means this layer's storage promotion is not taking, and every conclusion drawn from
    // mg_store_effective_flags - which records the request, not the result - is unreliable. That is
    // the difference the persistent-mapping work spent a device round failing to notice.
    struct StorageErrors {
        uint64_t calls = 0;
        uint64_t failures = 0;
        uint64_t error_or = 0;
        uint64_t last_error = 0;
        uint64_t last_bytes = 0;
        uint64_t last_requested = 0;
        uint64_t last_effective = 0;
    };

    inline thread_local StorageErrors g_storage_err;

    // Which buffers this layer is tracking an application mapping for. Sticky: a mapping is taken once
    // per store, at world load, and a per-window table would be empty in every window that matters.
    struct AppMapDest {
        uint64_t buffer = 0;
        uint64_t length = 0;
        uint64_t access = 0;
        uint64_t records = 0;
    };

    constexpr int APPMAP_DEST_SLOTS = 8;

    struct AppMapTracking {
        AppMapDest dest[APPMAP_DEST_SLOTS] = {};
        uint64_t overflow = 0;
        uint64_t forget_unmap = 0;
        uint64_t forget_deleted = 0;
        uint64_t forget_respecified = 0;
        uint64_t forget_remapped = 0;
    };

    inline thread_local AppMapTracking g_appmap_track;
#endif

    inline uint64_t now_ns() {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    // Timestamps are only needed when the counters are on, and clock_gettime is not free on every
    // device. Call sites use this so a disabled build does not read the clock at all.
    inline uint64_t timestamp() {
        return enabled() ? now_ns() : 0;
    }

    inline uint64_t elapsed_ns(uint64_t start_ns) {
        if (!enabled() || start_ns == 0) return 0;
        const uint64_t end = now_ns();
        return end > start_ns ? end - start_ns : 0;
    }

    // GLsizeiptr is signed and callers may pass a negative size straight from the application.
    inline uint64_t non_negative_bytes(GLsizeiptr size) {
        return size > 0 ? static_cast<uint64_t>(size) : 0;
    }

    namespace detail {

        inline void update_max(uint64_t& current, uint64_t value) {
            if (value > current) current = value;
        }

        inline void count_wait_result(GLenum result, uint64_t& already, uint64_t& satisfied, uint64_t& timeout,
                                      uint64_t& failed, uint64_t& other) {
            switch (result) {
            case GL_ALREADY_SIGNALED:
                already++;
                break;
            case GL_CONDITION_SATISFIED:
                satisfied++;
                break;
            case GL_TIMEOUT_EXPIRED:
                timeout++;
                break;
            case GL_WAIT_FAILED:
                failed++;
                break;
            default:
                other++;
                break;
            }
        }

    } // namespace detail

    inline void record_frame_wait(GLenum result, uint64_t elapsed) {
        if (!enabled()) return;
        Counters& c = g_counters;
        c.frame_wait_calls++;
        c.frame_wait_ns += elapsed;
        detail::update_max(c.frame_wait_max_ns, elapsed);
        detail::count_wait_result(result, c.frame_wait_already, c.frame_wait_satisfied, c.frame_wait_timeout,
                                  c.frame_wait_failed, c.frame_wait_other);
    }

    inline void record_sync_wait(GLenum result, GLuint64 timeout_ns, uint64_t elapsed) {
        if (!enabled()) return;
        Counters& c = g_counters;
        c.sync_wait_calls++;
        if (timeout_ns == 0)
            c.sync_wait_zero_timeout++;
        else
            c.sync_wait_positive_timeout++;
        c.sync_wait_ns += elapsed;
        detail::update_max(c.sync_wait_max_ns, elapsed);
        detail::count_wait_result(result, c.sync_wait_already, c.sync_wait_satisfied, c.sync_wait_timeout,
                                  c.sync_wait_failed, c.sync_wait_other);
    }

    inline void record_named_upload(uint64_t bytes, uint64_t buffer_bytes, uint64_t elapsed) {
        if (!enabled()) return;
        Counters& c = g_counters;
        c.named_calls++;
        c.named_bytes += bytes;
        c.named_ns += elapsed;
        detail::update_max(c.named_max_ns, elapsed);
        detail::update_max(c.named_max_buffer_bytes, buffer_bytes);
        detail::update_max(c.named_max_upload_bytes, bytes);
    }

    inline void record_direct_map_attempt(uint64_t elapsed) {
        if (!enabled()) return;
        g_counters.direct_attempts++;
        g_counters.direct_map_ns += elapsed;
    }

    inline void record_direct_map_hit(uint64_t bytes, uint64_t memcpy_ns, uint64_t unmap_ns) {
        if (!enabled()) return;
        Counters& c = g_counters;
        c.direct_hits++;
        c.direct_bytes += bytes;
        c.direct_memcpy_ns += memcpy_ns;
        c.direct_unmap_ns += unmap_ns;
    }

#if defined(MG_PLATFORM_OHOS)

    // result is a glClientWaitSync return value, or 0 when there was no fence to poll.
    inline void record_direct_fence_poll(GLenum result) {
        if (!enabled()) return;
        Counters& c = g_counters;
        switch (result) {
        case GL_ALREADY_SIGNALED:
        case GL_CONDITION_SATISFIED:
            c.direct_poll_already++;
            break;
        case GL_TIMEOUT_EXPIRED:
            c.direct_poll_timeout++;
            break;
        default:
            c.direct_poll_other++;
            break;
        }
    }

    inline void record_direct_dest_copy_target() {
        if (!enabled()) return;
        g_counters.direct_dest_copy_target++;
    }

    inline void record_direct_map_cost(bool unsynchronized, uint64_t elapsed) {
        if (!enabled()) return;
        Counters& c = g_counters;
        if (unsynchronized) {
            c.direct_unsync_map_calls++;
            c.direct_unsync_map_ns += elapsed;
            detail::update_max(c.direct_unsync_map_max_ns, elapsed);
        } else {
            c.direct_sync_map_calls++;
            c.direct_sync_map_ns += elapsed;
            detail::update_max(c.direct_sync_map_max_ns, elapsed);
        }
    }

    // The deferred-upload counters are written from the GL paths, so unlike every other recorder
    // here they must stay correct with diagnostics off - the flush logic reads none of them, but a
    // half-updated set would make a later session's numbers meaningless. They are plain increments,
    // so the enabled() check is kept for consistency of cost, not of state.
    inline void record_deferred_enqueue(uint64_t bytes) {
        if (!enabled()) return;
        g_counters.deferred_enqueued++;
        g_counters.deferred_enqueued_bytes += bytes;
    }

    inline void record_deferred_replay(uint64_t bytes) {
        if (!enabled()) return;
        g_counters.deferred_replayed++;
        g_counters.deferred_replayed_bytes += bytes;
    }

    inline void record_deferred_replay_fallback(uint64_t bytes) {
        if (!enabled()) return;
        g_counters.deferred_replay_fallback++;
        g_counters.deferred_replayed_bytes += bytes;
    }

    inline void record_deferred_overflow() {
        if (!enabled()) return;
        g_counters.deferred_overflow++;
    }

    inline void record_deferred_forced_flush() {
        if (!enabled()) return;
        g_counters.deferred_forced_flush++;
    }

    // Records one sub-threshold upload. Called from the fallback branch of glNamedBufferSubData,
    // in addition to record_fallback, so the two stay comparable across builds.
    //
    // Cost: a linear scan of at most FALLBACK_DEST_SLOTS integer comparisons plus a few adds, on a
    // path measured at 138 calls per second. That is four orders of magnitude below the 400 us mean
    // of the call being measured, which is the margin rule 5.5 asks for after a probe once distorted
    // the very frame rate it was sampling.
    // Which store-creation entry point the application used, and whether this layer promoted it.
    // See the storage_calls comment for what the answer decides.
    inline void record_storage(bool promoted, bool extFnMissing) {
        if (!enabled()) return;
        Counters& c = g_counters;
        c.storage_calls++;
        if (promoted) c.storage_promoted++;
        if (extFnMissing) c.storage_no_ext_fn++;
    }

    inline void record_buffer_data() {
        if (!enabled()) return;
        g_counters.bufferdata_calls++;
    }

    // A sub-data call dropped because the bytes were already there. Counted separately from
    // fallback_calls so the two together give the pre-fix call count, keeping this build comparable
    // with the 2026-08-07 measurement of 6,374 writes costing 6,846 ms.
    inline void record_fallback_deduped(uint64_t bytes) {
        if (!enabled()) return;
        g_counters.fallback_deduped++;
        g_counters.fallback_deduped_bytes += bytes;
    }

    // One mapping attempt, as the driver saw it. See the map_attempts comment for what this decides.
    // access is the mask actually passed to the driver, after this layer's rewrite.
    inline void record_map_result(uint64_t buffer, uint64_t length, uint64_t access, bool ok, uint64_t glError) {
        if (!enabled()) return;
        Counters& c = g_counters;
        const bool persistent = (access & 0x40u) != 0; // GL_MAP_PERSISTENT_BIT
        c.map_attempts++;
        c.map_access_or |= access;
        if (persistent) c.map_persistent_attempts++;
        if (!ok) {
            c.map_failures++;
            c.map_fail_access_or |= access;
            c.map_fail_buffer = buffer;
            c.map_fail_length = length;
            c.map_fail_error = glError;
            if (persistent) c.map_persistent_failures++;
        } else if (length >= 64u * 1024u) {
            c.map_big_ok_buffer = buffer;
            c.map_big_ok_length = length;
            c.map_big_ok_access = access;
        }

        if (length < MAP_DEST_MIN_BYTES) return;
        for (int i = 0; i < MAP_DEST_SLOTS; ++i) {
            MapDest& d = c.map_dest[i];
            if (d.buffer == buffer || d.buffer == 0) {
                d.buffer = buffer;
                d.length = length;
                d.access_or |= access;
                if (ok) d.maps++;
                else d.failures++;
                return;
            }
        }
        c.map_dest_overflow++;
    }

    // One unmap of a large store. Paired with record_map_result so maps vs unmaps can be compared;
    // see Counters::map_dest for what the comparison decides.
    inline void record_unmap(uint64_t buffer) {
        if (!enabled()) return;
        Counters& c = g_counters;
        for (int i = 0; i < MAP_DEST_SLOTS; ++i) {
            MapDest& d = c.map_dest[i];
            if (d.buffer == buffer) {
                d.unmaps++;
                return;
            }
        }
    }

    inline void record_fallback_dest(uint64_t buffer, uint64_t destBytes, uint64_t uploadBytes, uint64_t offset,
                                     uint64_t copyDest, uint64_t elapsed, uint64_t reqFlags, uint64_t effFlags,
                                     uint64_t entry) {
        if (!enabled()) return;
        Counters& c = g_counters;

        int bucket;
        if (destBytes < 64u * 1024u) bucket = 0;
        else if (destBytes < 1u * 1024u * 1024u) bucket = 1;
        else if (destBytes < 4u * 1024u * 1024u) bucket = 2;
        else if (destBytes < 16u * 1024u * 1024u) bucket = 3;
        else bucket = 4; // should be unreachable: at or above the threshold the call is deferred
        c.fallback_bucket_calls[bucket]++;
        c.fallback_bucket_ns[bucket] += elapsed;
        detail::update_max(c.fallback_bucket_max_ns[bucket], elapsed);

        if (elapsed > 1000000ULL) { // 1 ms
            c.fallback_slow_calls++;
            c.fallback_slow_ns += elapsed;
        }

        if (elapsed > c.fallback_worst_ns) {
            c.fallback_worst_ns = elapsed;
            c.fallback_worst_buffer = buffer;
            c.fallback_worst_dest_bytes = destBytes;
            c.fallback_worst_upload_bytes = uploadBytes;
            c.fallback_worst_offset = offset;
        }

        // Match an existing slot, else claim a free one, else count it as overflow. First-come
        // rather than evict-the-cheapest, so the numbers in a slot always describe that one buffer
        // for the whole window instead of a shifting subset.
        for (int i = 0; i < FALLBACK_DEST_SLOTS; ++i) {
            FallbackDest& d = c.fallback_dest[i];
            if (d.buffer == buffer || d.buffer == 0) {
                d.buffer = buffer;
                d.dest_bytes = destBytes;
                d.calls++;
                d.ns += elapsed;
                d.upload_bytes += uploadBytes;
                d.req_flags = reqFlags;
                d.eff_flags = effFlags;
                d.entry = entry;
                if (copyDest) d.copy_dest = 1;
                detail::update_max(d.max_ns, elapsed);
                return;
            }
        }
        c.fallback_overflow_calls++;
        c.fallback_overflow_ns += elapsed;
    }

    inline void record_deferred_midframe_flush() {
        if (!enabled()) return;
        g_counters.deferred_midframe_flush++;
    }

    inline void record_deferred_ordered_replay(uint64_t bytes, uint64_t elapsed) {
        if (!enabled()) return;
        Counters& c = g_counters;
        c.deferred_ordered_replay++;
        c.deferred_ordered_bytes += bytes;
        c.deferred_ordered_ns += elapsed;
        detail::update_max(c.deferred_ordered_max_ns, elapsed);
    }

    // One glDeleteBuffers that found the deleted id still sitting in a binding record. See
    // Counters::unbind_on_delete_calls for what the numbers decide. Called with both totals so a
    // single delete that occupied several binding points counts as one occurrence.
    inline void record_unbind_on_delete(unsigned targets, unsigned vaos) {
        if (!enabled()) return;
        if (targets == 0 && vaos == 0) return;
        Counters& c = g_counters;
        c.unbind_on_delete_calls++;
        c.unbind_on_delete_targets += targets;
        c.unbind_on_delete_vaos += vaos;
    }

    // State of one candidate buffer, as of the moment mg_pmap_consider looked at it. See PmapDest.
    // First-come slot assignment, so a slot always describes the same buffer for the whole window.
    inline void record_pmap_state(uint64_t buffer, uint64_t bytes, uint64_t smallWrites, bool adopted, bool declined,
                                  uint64_t strikes, uint64_t effFlags) {
        if (!enabled()) return;
        Counters& c = g_counters;
        for (int i = 0; i < PMAP_DEST_SLOTS; ++i) {
            PmapDest& d = c.pmap_dest[i];
            if (d.buffer == buffer || d.buffer == 0) {
                d.buffer = buffer;
                d.bytes = bytes;
                d.small_writes = smallWrites;
                d.adopted = adopted ? 1u : 0u;
                d.declined = declined ? 1u : 0u;
                d.strikes = strikes;
                d.eff_flags = effFlags;
                d.considers++;
                return;
            }
        }
        c.pmap_dest_overflow++;
    }

    // reason is one of the MG_PMAP_EVICT_* values in gl/buffer.h.
    inline void record_pmap_barred(int reason) {
        if (!enabled()) return;
        if (reason == 4)
            g_counters.pmap_barred_element++;
        else
            g_counters.pmap_barred_app_map++;
    }

    // rung is 0-based as indexed into PMAP_LADDER; stored 1-based so 0 can mean "never happened".
    // Not gated on enabled(): a once-per-session fact costs nothing to keep and a session that turned
    // diagnostics on late would otherwise have no record of the decision that shaped it.
    // Not gated on enabled(): a mapping is recorded once per store at world load, and a session that
    // turned diagnostics on afterwards would otherwise have no record of what is being tracked.
    inline void record_appmap_tracked(uint64_t buffer, uint64_t length, uint64_t access) {
        AppMapTracking& t = g_appmap_track;
        for (int i = 0; i < APPMAP_DEST_SLOTS; ++i) {
            AppMapDest& d = t.dest[i];
            if (d.buffer == buffer || d.buffer == 0) {
                d.buffer = buffer;
                d.length = length;
                d.access = access;
                d.records++;
                return;
            }
        }
        t.overflow++;
    }

    // reason is one of the MG_APPMAP_FORGET_* values in gl/buffer.h.
    inline void record_appmap_forgot(int reason) {
        AppMapTracking& t = g_appmap_track;
        switch (reason) {
        case 1:
            t.forget_unmap++;
            break;
        case 2:
            t.forget_deleted++;
            break;
        case 3:
            t.forget_respecified++;
            break;
        default:
            t.forget_remapped++;
            break;
        }
    }

    inline void record_appmap_write(uint64_t bytes, uint64_t elapsed) {
        if (!enabled()) return;
        Counters& c = g_counters;
        c.appmap_writes++;
        c.appmap_write_bytes += bytes;
        c.appmap_write_ns += elapsed;
        detail::update_max(c.appmap_write_max_ns, elapsed);
    }

    inline void record_appmap_flush(uint64_t elapsed) {
        if (!enabled()) return;
        Counters& c = g_counters;
        c.appmap_flushes++;
        c.appmap_flush_ns += elapsed;
        detail::update_max(c.appmap_flush_max_ns, elapsed);
    }

    inline void record_appmap_unpublished() {
        if (!enabled()) return;
        g_counters.appmap_unpublished++;
    }

    inline void record_appmap_out_of_range() {
        if (!enabled()) return;
        g_counters.appmap_out_of_range++;
    }

    // One glBufferStorageEXT, with whatever error it produced. See StorageErrors.
    inline void record_storage_error(uint64_t bytes, uint64_t requested, uint64_t effective, uint64_t glError) {
        StorageErrors& s = g_storage_err;
        s.calls++;
        if (glError == 0) return;
        s.failures++;
        s.error_or |= glError;
        s.last_error = glError;
        s.last_bytes = bytes;
        s.last_requested = requested;
        s.last_effective = effective;
    }

    inline void record_pmap_ladder_failure(int rung, uint64_t access, uint64_t glError) {
        PmapAcceptance& a = g_pmap_acc;
        a.failures++;
        a.fail_access_or |= access;
        a.fail_error_or |= glError;
        a.fail_last_rung = static_cast<uint64_t>(rung) + 1u;
    }

    inline void record_pmap_accepted(int rung, uint64_t access) {
        PmapAcceptance& a = g_pmap_acc;
        a.rung = static_cast<uint64_t>(rung) + 1u;
        a.access = access;
    }

    inline void record_pmap_flush(uint64_t elapsed) {
        if (!enabled()) return;
        Counters& c = g_counters;
        c.pmap_flushes++;
        c.pmap_flush_ns += elapsed;
        detail::update_max(c.pmap_flush_max_ns, elapsed);
    }

    inline void record_pmap_adopted() {
        if (!enabled()) return;
        g_counters.pmap_adopted++;
    }

    inline void record_pmap_map_failure() {
        if (!enabled()) return;
        g_counters.pmap_map_failures++;
    }

    inline void record_pmap_write(uint64_t bytes, uint64_t elapsed) {
        if (!enabled()) return;
        Counters& c = g_counters;
        c.pmap_writes++;
        c.pmap_write_bytes += bytes;
        c.pmap_write_ns += elapsed;
        detail::update_max(c.pmap_write_max_ns, elapsed);
    }

    // reason is one of the MG_PMAP_EVICT_* values in gl/buffer.h. Kept as separate counters rather
    // than one total, because the reasons carry different consequences; see Counters::pmap_evict_*.
    inline void record_pmap_evicted(int reason) {
        if (!enabled()) return;
        Counters& c = g_counters;
        switch (reason) {
        case 1:
            c.pmap_evict_respecified++;
            break;
        case 2:
            c.pmap_evict_deleted++;
            break;
        case 3:
            c.pmap_evict_app_map++;
            break;
        case 4:
            c.pmap_evict_element++;
            break;
        default:
            c.pmap_evict_copy++;
            break;
        }
    }

    inline void record_deferred_fence_missing() {
        if (!enabled()) return;
        ++g_counters.deferred_fence_missing;
    }

    inline void record_deferred_fence_wait(GLenum result) {
        if (!enabled()) return;
        Counters& c = g_counters;
        detail::count_wait_result(result, c.deferred_fence_already, c.deferred_fence_satisfied,
                                  c.deferred_fence_timeout, c.deferred_fence_other, c.deferred_fence_other);
    }

#endif // MG_PLATFORM_OHOS

    inline void record_fallback(uint64_t bytes, uint64_t elapsed) {
        if (!enabled()) return;
        Counters& c = g_counters;
        c.fallback_calls++;
        c.fallback_bytes += bytes;
        c.fallback_ns += elapsed;
    }

    inline void record_sub_data(uint64_t bytes, uint64_t elapsed) {
        if (!enabled()) return;
        Counters& c = g_counters;
        c.subdata_calls++;
        c.subdata_bytes += bytes;
        c.subdata_ns += elapsed;
        detail::update_max(c.subdata_max_ns, elapsed);
    }

    inline void record_map(uint64_t bytes, uint64_t elapsed) {
        if (!enabled()) return;
        Counters& c = g_counters;
        c.map_calls++;
        c.map_bytes += bytes;
        c.map_ns += elapsed;
        detail::update_max(c.map_max_ns, elapsed);
    }

    inline void record_flush(uint64_t bytes, bool called_driver, uint64_t elapsed) {
        if (!enabled()) return;
        Counters& c = g_counters;
        c.flush_calls++;
        if (called_driver)
            c.flush_driver_calls++;
        else
            c.flush_skipped_calls++;
        c.flush_bytes += bytes;
        c.flush_ns += elapsed;
        detail::update_max(c.flush_max_ns, elapsed);
    }

    inline void record_copy(uint64_t bytes, uint64_t elapsed) {
        if (!enabled()) return;
        Counters& c = g_counters;
        c.copy_calls++;
        c.copy_bytes += bytes;
        c.copy_ns += elapsed;
        detail::update_max(c.copy_max_ns, elapsed);
    }

#if defined(MG_PLATFORM_OHOS)
    // Identity and size of a buffer-to-buffer copy, so its effective bandwidth can be computed and the
    // slow ones can be named. See Counters::copy_slow_calls for what this decides.
    inline void record_copy_detail(uint64_t bytes, uint64_t elapsed, uint64_t readBuffer, uint64_t writeBuffer,
                                   uint64_t readFlags, uint64_t writeFlags) {
        if (!enabled()) return;
        Counters& c = g_counters;
        if (elapsed > 1000000ULL) { // 1 ms
            c.copy_slow_calls++;
            c.copy_slow_ns += elapsed;
            c.copy_slow_bytes += bytes;
        }
        if (elapsed > c.copy_worst_ns) {
            c.copy_worst_ns = elapsed;
            c.copy_worst_bytes = bytes;
            c.copy_worst_read = readBuffer;
            c.copy_worst_write = writeBuffer;
            c.copy_worst_read_flags = readFlags;
            c.copy_worst_write_flags = writeFlags;
        }
        c.copy_flags_read_or |= readFlags;
        c.copy_flags_write_or |= writeFlags;
    }

    inline void record_clear(uint64_t elapsed) {
        if (!enabled()) return;
        Counters& c = g_counters;
        c.clear_calls++;
        c.clear_ns += elapsed;
        detail::update_max(c.clear_max_ns, elapsed);
    }
#endif

    // Called at every frame boundary. Counts the frame and, once a second has passed, emits the
    // aggregate and starts a new window.
    //
    // 'policy' names the storage and upload behaviour in effect, so records from different builds
    // can be told apart when they are compared later. Pass a stable identifier, not a description.
    void on_frame_boundary(const char* policy);

} // namespace mg::diagnostics

#endif // MOBILEGLUES_DIAGNOSTICS_COUNTERS_H
