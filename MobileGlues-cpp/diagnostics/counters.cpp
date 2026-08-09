// MobileGlues - diagnostics/counters.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#include "counters.h"

#include <cstdio>

// The logging macros expand to printf and write_log, so both declarations have to be in scope
// before gl/log.h is used: log.h defines the macros but does not pull in what they call.
#include "../gl/mg.h"

#include "../gl/log.h"

namespace mg::diagnostics {

    bool g_enabled = false;

    void configure(bool enable) {
        g_enabled = enable;
    }

    namespace {

        // Records are emitted at most once per second per GL thread.
        constexpr uint64_t WINDOW_NS = 1000000000ULL;

        thread_local uint64_t g_window_start_ns = 0;

        // The logging macros take varargs, so the widths have to be pinned explicitly rather than
        // relying on uint64_t matching any particular printf length modifier.
        unsigned long long v(uint64_t n) {
            return static_cast<unsigned long long>(n);
        }

        unsigned long long us(uint64_t ns) {
            return static_cast<unsigned long long>(ns / 1000ULL);
        }

        unsigned long long ms(uint64_t ns) {
            return static_cast<unsigned long long>(ns / 1000000ULL);
        }

    } // namespace

    void on_frame_boundary(const char* policy) {
        if (!enabled()) return;

        Counters& c = g_counters;
        c.color_clears++;

        const uint64_t now = now_ns();
        if (g_window_start_ns == 0) {
            g_window_start_ns = now;
            return;
        }
        const uint64_t window_ns = now - g_window_start_ns;
        if (window_ns < WINDOW_NS) return;

        // Two records rather than one: synchronization and memory traffic are read separately, and
        // a single line long enough to hold both gets truncated by log transports.
        LOG_I("[MG-DIAG-SYNC] policy=%s window_ms=%llu color_clears=%llu frame_wait_calls=%llu "
              "frame_wait_total_us=%llu frame_wait_max_us=%llu frame_wait_already=%llu frame_wait_satisfied=%llu "
              "frame_wait_timeout=%llu frame_wait_failed=%llu frame_wait_other=%llu sync_wait_calls=%llu "
              "sync_wait_zero=%llu sync_wait_positive=%llu sync_wait_total_us=%llu sync_wait_max_us=%llu "
              "sync_wait_already=%llu sync_wait_satisfied=%llu sync_wait_timeout=%llu sync_wait_failed=%llu "
              "sync_wait_other=%llu",
              policy ? policy : "(none)", ms(window_ns), v(c.color_clears), v(c.frame_wait_calls), us(c.frame_wait_ns),
              us(c.frame_wait_max_ns), v(c.frame_wait_already), v(c.frame_wait_satisfied), v(c.frame_wait_timeout),
              v(c.frame_wait_failed), v(c.frame_wait_other), v(c.sync_wait_calls), v(c.sync_wait_zero_timeout),
              v(c.sync_wait_positive_timeout), us(c.sync_wait_ns), us(c.sync_wait_max_ns), v(c.sync_wait_already),
              v(c.sync_wait_satisfied), v(c.sync_wait_timeout), v(c.sync_wait_failed), v(c.sync_wait_other))

        LOG_I("[MG-DIAG-BUFFER] policy=%s named_calls=%llu named_bytes=%llu named_total_us=%llu named_max_us=%llu "
              "named_max_buffer=%llu named_max_upload=%llu direct_attempts=%llu direct_hits=%llu direct_bytes=%llu "
              "direct_map_us=%llu direct_memcpy_us=%llu direct_unmap_us=%llu fallback_calls=%llu "
              "fallback_bytes=%llu fallback_us=%llu subdata_calls=%llu subdata_bytes=%llu subdata_us=%llu "
              "subdata_max_us=%llu map_calls=%llu map_bytes=%llu map_us=%llu map_max_us=%llu flush_calls=%llu "
              "flush_driver=%llu flush_skipped=%llu flush_bytes=%llu flush_us=%llu flush_max_us=%llu "
              "copy_calls=%llu copy_bytes=%llu copy_us=%llu copy_max_us=%llu",
              policy ? policy : "(none)", v(c.named_calls), v(c.named_bytes), us(c.named_ns), us(c.named_max_ns),
              v(c.named_max_buffer_bytes), v(c.named_max_upload_bytes), v(c.direct_attempts), v(c.direct_hits),
              v(c.direct_bytes), us(c.direct_map_ns), us(c.direct_memcpy_ns), us(c.direct_unmap_ns),
              v(c.fallback_calls), v(c.fallback_bytes), us(c.fallback_ns), v(c.subdata_calls), v(c.subdata_bytes),
              us(c.subdata_ns), us(c.subdata_max_ns), v(c.map_calls), v(c.map_bytes), us(c.map_ns), us(c.map_max_ns),
              v(c.flush_calls), v(c.flush_driver_calls), v(c.flush_skipped_calls), v(c.flush_bytes), us(c.flush_ns),
              us(c.flush_max_ns), v(c.copy_calls), v(c.copy_bytes), us(c.copy_ns), us(c.copy_max_ns))

#if defined(MG_PLATFORM_OHOS)
        // A separate record so the two above stay byte-identical to every earlier measurement, and
        // because the line above is already close to the length at which log transports truncate.
        //
        // How to read this:
        //   poll_already vs poll_timeout  -> is the previous frame's fence signalled when a direct
        //                                    write happens? already >> timeout means a cheap
        //                                    provably-correct gate exists; the reverse kills it.
        //   dest_copy_target              -> compare against direct_hits. Near-equal under Sodium
        //                                    and ZERO on vanilla 26.2 is what makes "is a copy
        //                                    destination" a usable discriminator. Non-zero on
        //                                    vanilla means it is not.
        //   sync_map vs unsync_map        -> per-call cost of the same mapped write with and
        //                                    without GL_MAP_UNSYNCHRONIZED_BIT. This is the price
        //                                    of the only affordable fallback; glBufferSubData into
        //                                    the same store costs 3,946-4,604 us for comparison.
        LOG_I("[MG-DIAG-PROBE] policy=%s direct_poll_already=%llu direct_poll_timeout=%llu "
              "direct_poll_other=%llu direct_dest_copy_target=%llu direct_sync_map_calls=%llu "
              "direct_sync_map_us=%llu direct_sync_map_max_us=%llu direct_unsync_map_calls=%llu "
              "direct_unsync_map_us=%llu direct_unsync_map_max_us=%llu",
              policy ? policy : "(none)", v(c.direct_poll_already), v(c.direct_poll_timeout), v(c.direct_poll_other),
              v(c.direct_dest_copy_target), v(c.direct_sync_map_calls), us(c.direct_sync_map_ns),
              us(c.direct_sync_map_max_ns), v(c.direct_unsync_map_calls), us(c.direct_unsync_map_ns),
              us(c.direct_unsync_map_max_ns))

        // The acceptance criteria for the deferred upload.
        //
        // deferred_fence_timeout must read 0: the present-time drain polls a fence the present has
        // already submitted, so it should never block. A non-zero count means that premise is wrong.
        //
        // deferred_midframe_flush is the version discriminator. Vanilla 26.2, 26.1.2 and 1.21.11
        // upload after they draw, so their queue is empty at every draw and this must read 0 - if it
        // does not, a draw is reaching the queue mid-frame on a configuration where the frame order
        // says it cannot. Under Sodium it is expected to be non-zero, and deferred_ordered_ns is
        // then the whole cost of the fix: it replaces a 3.77 ms flush plus pipeline drain per
        // occurrence with one ordered write per queued record.
        //
        // deferred_forced_flush keeps its old meaning - any drain that is not the present-time one,
        // whether triggered by a draw or by a guard site - so it stays comparable with earlier device
        // logs. It differs from deferred_midframe_flush only by the overflow drain.
        LOG_I("[MG-DIAG-DEFER] policy=%s deferred_enqueued=%llu deferred_enqueued_bytes=%llu "
              "deferred_replayed=%llu deferred_replayed_bytes=%llu deferred_replay_fallback=%llu "
              "deferred_overflow=%llu deferred_forced_flush=%llu deferred_fence_already=%llu "
              "deferred_fence_satisfied=%llu deferred_fence_timeout=%llu deferred_fence_other=%llu "
              "deferred_fence_missing=%llu "
              "deferred_midframe_flush=%llu deferred_ordered_replay=%llu deferred_ordered_bytes=%llu "
              "deferred_ordered_us=%llu deferred_ordered_max_us=%llu",
              policy ? policy : "(none)", v(c.deferred_enqueued), v(c.deferred_enqueued_bytes), v(c.deferred_replayed),
              v(c.deferred_replayed_bytes), v(c.deferred_replay_fallback), v(c.deferred_overflow),
              v(c.deferred_forced_flush), v(c.deferred_fence_already), v(c.deferred_fence_satisfied),
              v(c.deferred_fence_timeout), v(c.deferred_fence_other), v(c.deferred_fence_missing),
              v(c.deferred_midframe_flush),
              v(c.deferred_ordered_replay), v(c.deferred_ordered_bytes), us(c.deferred_ordered_ns),
              us(c.deferred_ordered_max_ns))

        // Sub-threshold uploads, which the 2026-08-06 round identified as the dominant per-second
        // cost while crossing chunk boundaries. Three questions, in order of what they decide:
        //
        //   slow_calls / calls  - is the total concentrated in a few stalls or spread over all of
        //                         them? Concentrated means only those need handling; spread means
        //                         the DIRECT_MAP_MIN_SIZE threshold itself is what must change.
        //   bucket_*            - the size class of the stalling stores.
        //   dest[i]             - the actual destinations, with copy_dest separating Sodium's
        //                         buffers from Minecraft's own.
        //
        // Which store-creation path the application took. storage_calls > 0 means
        // BufferStorage$Immutable, i.e. LWJGL saw GL_ARB_buffer_storage; bufferdata_calls > 0 with
        // storage_calls == 0 means BufferStorage$Mutable, i.e. it did not. That single fact decides
        // whether Sodium can use its persistent-mapping fast paths at all.
        if (c.storage_calls != 0 || c.bufferdata_calls != 0) {
            LOG_I("[MG-DIAG-STORE] policy=%s storage_calls=%llu storage_promoted=%llu storage_no_ext_fn=%llu "
                  "bufferdata_calls=%llu",
                  policy ? policy : "(none)", v(c.storage_calls), v(c.storage_promoted), v(c.storage_no_ext_fn),
                  v(c.bufferdata_calls))
        }

        // Whether glBufferStorageEXT is actually succeeding. Sticky, so it survives the window reset.
        if (g_storage_err.calls != 0) {
            LOG_I("[MG-DIAG-STOREERR] storage_ext_calls=%llu storage_ext_failures=%llu error_or=0x%llx "
                  "last_error=0x%llx last_bytes=%llu last_requested=0x%llx last_effective=0x%llx",
                  v(g_storage_err.calls), v(g_storage_err.failures), v(g_storage_err.error_or),
                  v(g_storage_err.last_error), v(g_storage_err.last_bytes), v(g_storage_err.last_requested),
                  v(g_storage_err.last_effective))
        }

        // Whether the persistent mapping succeeds. map_persistent_failures > 0 is the finding: it
        // would mean persistentMapping() is true, Sodium asked for the mapping, the driver refused,
        // and every section-time write therefore fell back to sub-data. See counters.h map_attempts.
        if (c.map_attempts != 0) {
            LOG_I("[MG-DIAG-MAP] policy=%s map_attempts=%llu map_failures=%llu map_persistent_attempts=%llu "
                  "map_persistent_failures=%llu map_access_or=0x%llx map_fail_access_or=0x%llx "
                  "map_fail_buffer=%llu map_fail_length=%llu map_fail_error=0x%llx "
                  "map_big_ok_buffer=%llu map_big_ok_length=%llu map_big_ok_access=0x%llx",
                  policy ? policy : "(none)", v(c.map_attempts), v(c.map_failures), v(c.map_persistent_attempts),
                  v(c.map_persistent_failures), v(c.map_access_or), v(c.map_fail_access_or), v(c.map_fail_buffer),
                  v(c.map_fail_length), v(c.map_fail_error), v(c.map_big_ok_buffer), v(c.map_big_ok_length),
                  v(c.map_big_ok_access))

            // One line per large store. maps without a matching unmap means the mapping is being
            // held, which is what UniformBufferManager.<init> does only when persistentMapping() is
            // true. For the 229,376-byte store that is the open question.
            for (int i = 0; i < MAP_DEST_SLOTS; ++i) {
                const MapDest& d = c.map_dest[i];
                if (d.buffer == 0) continue;
                LOG_I("[MG-DIAG-MAPDEST] slot=%d buffer=%llu length=%llu maps=%llu unmaps=%llu failures=%llu "
                      "access_or=0x%llx",
                      i, v(d.buffer), v(d.length), v(d.maps), v(d.unmaps), v(d.failures), v(d.access_or))
            }
            if (c.map_dest_overflow != 0) {
                LOG_I("[MG-DIAG-MAPDEST] overflow=%llu", v(c.map_dest_overflow))
            }
        }

        // Printed once anything has been adopted, and then every window, so that a session where the
        // cache never engaged is silent and one where it did shows its per-second cost. See
        // counters.h pmap_adopted for the numbers this is compared against.
        // Printed as soon as any buffer has been *considered*, not only once one has been adopted.
        // Gating on adoption is what hid the first round's failure: nothing was adopted, so nothing
        // was printed, so there was no record of the several buffers that had been looked at and
        // refused.
        if (c.pmap_dest[0].buffer != 0 || c.pmap_adopted != 0 || c.pmap_writes != 0 ||
            c.pmap_map_failures != 0 || g_pmap_acc.rung != 0 || g_pmap_acc.failures != 0 ||
            g_pmap_probe.captured != 0) {
            LOG_I("[MG-DIAG-PMAP] policy=%s pmap_adopted=%llu pmap_map_failures=%llu pmap_writes=%llu "
                  "pmap_write_bytes=%llu pmap_write_us=%llu pmap_write_max_us=%llu "
                  "pmap_evict_respecified=%llu pmap_evict_deleted=%llu pmap_evict_app_map=%llu "
                  "pmap_evict_element=%llu pmap_evict_copy=%llu",
                  policy ? policy : "(none)", v(c.pmap_adopted), v(c.pmap_map_failures), v(c.pmap_writes),
                  v(c.pmap_write_bytes), us(c.pmap_write_ns), us(c.pmap_write_max_ns), v(c.pmap_evict_respecified),
                  v(c.pmap_evict_deleted), v(c.pmap_evict_app_map), v(c.pmap_evict_element), v(c.pmap_evict_copy))

            LOG_I("[MG-DIAG-PMAPBAR] policy=%s pmap_barred_app_map=%llu pmap_barred_element=%llu "
                  "pmap_dest_overflow=%llu",
                  policy ? policy : "(none)", v(c.pmap_barred_app_map), v(c.pmap_barred_element),
                  v(c.pmap_dest_overflow))

            // Which access mask the driver granted, and what it refused on the way there. This is the
            // record that was missing when a refused COHERENT bit silently disabled the whole fix.
            LOG_I("[MG-DIAG-PMAPACC] policy=%s pmap_accepted_rung=%llu pmap_accepted_access=0x%llx "
                  "pmap_ladder_failures=%llu pmap_fail_access_or=0x%llx pmap_fail_error_or=0x%llx "
                  "pmap_fail_last_rung=%llu pmap_flushes=%llu pmap_flush_us=%llu pmap_flush_max_us=%llu",
                  policy ? policy : "(none)", v(g_pmap_acc.rung), v(g_pmap_acc.access), v(g_pmap_acc.failures),
                  v(g_pmap_acc.fail_access_or), v(g_pmap_acc.fail_error_or), v(g_pmap_acc.fail_last_rung),
                  v(c.pmap_flushes), us(c.pmap_flush_ns), us(c.pmap_flush_max_ns))

            // What the driver said about the buffer it refused. See counters.h PmapProbe for how to
            // read each field; the short version is that storage_flags missing 0x40 or immutable=0
            // means this layer's promotion did not take, mapped=1 means someone else holds the
            // mapping, and driver_binding != expected_real means the map was aimed at the wrong
            // buffer.
            if (g_pmap_probe.captured != 0) {
                LOG_I("[MG-DIAG-PMAPPROBE] buffer=%llu recorded_bytes=%llu driver_binding=%llu expected_real=%llu "
                      "buffer_size=%llu immutable=%llu storage_flags=0x%llx mapped=%llu access_flags=0x%llx "
                      "query_error=0x%llx",
                      v(g_pmap_probe.buffer), v(g_pmap_probe.recorded_bytes), v(g_pmap_probe.driver_binding),
                      v(g_pmap_probe.expected_real), v(g_pmap_probe.buffer_size), v(g_pmap_probe.immutable),
                      v(g_pmap_probe.storage_flags), v(g_pmap_probe.mapped), v(g_pmap_probe.access_flags),
                      v(g_pmap_probe.query_error))
            }

            // One line per candidate buffer. This is what says WHY a buffer was or was not adopted;
            // the aggregate line above could not, which cost a device round.
            for (int i = 0; i < PMAP_DEST_SLOTS; ++i) {
                const PmapDest& d = c.pmap_dest[i];
                if (d.buffer == 0) continue;
                LOG_I("[MG-DIAG-PMAPDEST] slot=%d buffer=%llu bytes=%llu small_writes=%llu adopted=%llu "
                      "declined=%llu strikes=%llu eff_flags=0x%llx considers=%llu",
                      i, v(d.buffer), v(d.bytes), v(d.small_writes), v(d.adopted), v(d.declined), v(d.strikes),
                      v(d.eff_flags), v(d.considers))
            }
        }

        // Buffer-to-buffer copies by identity and bandwidth, and the driver's own clear. See
        // counters.h copy_slow_calls: the question is whether this library's COHERENT promotion is what
        // makes Sodium's arena copies slow, and bytes over time is what answers it.
        if (c.copy_calls != 0 || c.clear_calls != 0) {
            LOG_I("[MG-DIAG-COPY] policy=%s copy_calls=%llu copy_bytes=%llu copy_us=%llu copy_max_us=%llu "
                  "copy_slow_calls=%llu copy_slow_us=%llu copy_slow_bytes=%llu copy_worst_us=%llu "
                  "copy_worst_bytes=%llu copy_worst_read=%llu copy_worst_write=%llu "
                  "copy_worst_read_flags=0x%llx copy_worst_write_flags=0x%llx read_flags_or=0x%llx "
                  "write_flags_or=0x%llx clear_calls=%llu clear_us=%llu clear_max_us=%llu",
                  policy ? policy : "(none)", v(c.copy_calls), v(c.copy_bytes), us(c.copy_ns), us(c.copy_max_ns),
                  v(c.copy_slow_calls), us(c.copy_slow_ns), v(c.copy_slow_bytes), us(c.copy_worst_ns),
                  v(c.copy_worst_bytes), v(c.copy_worst_read), v(c.copy_worst_write), v(c.copy_worst_read_flags),
                  v(c.copy_worst_write_flags), v(c.copy_flags_read_or), v(c.copy_flags_write_or), v(c.clear_calls),
                  us(c.clear_ns), us(c.clear_max_ns))
        }

        // REMOVED 2026-08-07: the [MG-DIAG-DRAW] and [MG-DIAG-FRAME] records. Both answered their
        // question and were then cost. What they established: the multi-draw path and the present cost
        // 0.30-0.39 ms per frame regardless of frame rate, so neither is the stutter; and only 14-18% of
        // a frame is inside this library at all in the steady state. See RENDER-ADAPTATION.md 6.11.

        // Writes served through the application's own mapping. This is the acceptance record for the
        // chunk-boundary stutter fix; see counters.h for the numbers it is compared against and for the
        // two unknowns - flush cost and unpublished writes - that it exists to measure.
        if (c.appmap_writes != 0 || g_appmap_track.dest[0].buffer != 0) {
            LOG_I("[MG-DIAG-APPMAP] policy=%s appmap_writes=%llu appmap_write_bytes=%llu appmap_write_us=%llu "
                  "appmap_write_max_us=%llu appmap_flushes=%llu appmap_flush_us=%llu appmap_flush_max_us=%llu "
                  "appmap_unpublished=%llu appmap_out_of_range=%llu forget_unmap=%llu forget_deleted=%llu "
                  "forget_respecified=%llu forget_remapped=%llu track_overflow=%llu",
                  policy ? policy : "(none)", v(c.appmap_writes), v(c.appmap_write_bytes), us(c.appmap_write_ns),
                  us(c.appmap_write_max_ns), v(c.appmap_flushes), us(c.appmap_flush_ns), us(c.appmap_flush_max_ns),
                  v(c.appmap_unpublished), v(c.appmap_out_of_range), v(g_appmap_track.forget_unmap),
                  v(g_appmap_track.forget_deleted), v(g_appmap_track.forget_respecified),
                  v(g_appmap_track.forget_remapped), v(g_appmap_track.overflow))

            for (int i = 0; i < APPMAP_DEST_SLOTS; ++i) {
                const AppMapDest& d = g_appmap_track.dest[i];
                if (d.buffer == 0) continue;
                LOG_I("[MG-DIAG-APPMAPDEST] slot=%d buffer=%llu length=%llu access=0x%llx records=%llu", i,
                      v(d.buffer), v(d.length), v(d.access), v(d.records))
            }
        }

        // Printed only when it happened, so the three vanilla versions - where buffers are
        // long-lived and this is expected to stay at zero - produce no line at all. Silence here is
        // itself the result. See counters.h unbind_on_delete_calls.
        if (c.unbind_on_delete_calls != 0) {
            LOG_I("[MG-DIAG-UNBIND] policy=%s unbind_on_delete_calls=%llu unbind_on_delete_targets=%llu "
                  "unbind_on_delete_vaos=%llu",
                  policy ? policy : "(none)", v(c.unbind_on_delete_calls), v(c.unbind_on_delete_targets),
                  v(c.unbind_on_delete_vaos))
        }

        // Printed only when the path was taken, so a version that never reaches it stays quiet.
        if (c.fallback_calls != 0) {
            LOG_I("[MG-DIAG-FBSUM] policy=%s fb_calls=%llu fb_us=%llu fb_slow_calls=%llu fb_slow_us=%llu "
                  "fb_worst_us=%llu fb_worst_buffer=%llu fb_worst_dest_bytes=%llu fb_worst_upload_bytes=%llu "
                  "fb_worst_offset=%llu fb_deduped=%llu fb_deduped_bytes=%llu "
                  "fb_overflow_calls=%llu fb_overflow_us=%llu "
                  "b0_lt64k=%llu/%llu/%llu b1_lt1m=%llu/%llu/%llu b2_lt4m=%llu/%llu/%llu "
                  "b3_lt16m=%llu/%llu/%llu b4_ge16m=%llu/%llu/%llu",
                  policy ? policy : "(none)", v(c.fallback_calls), us(c.fallback_ns), v(c.fallback_slow_calls),
                  us(c.fallback_slow_ns), us(c.fallback_worst_ns), v(c.fallback_worst_buffer),
                  v(c.fallback_worst_dest_bytes), v(c.fallback_worst_upload_bytes), v(c.fallback_worst_offset),
                  v(c.fallback_deduped), v(c.fallback_deduped_bytes),
                  v(c.fallback_overflow_calls), us(c.fallback_overflow_ns),
                  v(c.fallback_bucket_calls[0]), us(c.fallback_bucket_ns[0]), us(c.fallback_bucket_max_ns[0]),
                  v(c.fallback_bucket_calls[1]), us(c.fallback_bucket_ns[1]), us(c.fallback_bucket_max_ns[1]),
                  v(c.fallback_bucket_calls[2]), us(c.fallback_bucket_ns[2]), us(c.fallback_bucket_max_ns[2]),
                  v(c.fallback_bucket_calls[3]), us(c.fallback_bucket_ns[3]), us(c.fallback_bucket_max_ns[3]),
                  v(c.fallback_bucket_calls[4]), us(c.fallback_bucket_ns[4]), us(c.fallback_bucket_max_ns[4]))

            // One line per destination, so a wide record cannot be truncated by the log transport.
            for (int i = 0; i < FALLBACK_DEST_SLOTS; ++i) {
                const FallbackDest& d = c.fallback_dest[i];
                if (d.buffer == 0 || d.calls == 0) continue;
                LOG_I("[MG-DIAG-FBDEST] slot=%d buffer=%llu dest_bytes=%llu calls=%llu us=%llu max_us=%llu "
                      "mean_upload_bytes=%llu copy_dest=%llu req_flags=0x%llx eff_flags=0x%llx entry=%llu",
                      i, v(d.buffer), v(d.dest_bytes), v(d.calls), us(d.ns), us(d.max_ns),
                      v(d.calls ? d.upload_bytes / d.calls : 0), v(d.copy_dest), v(d.req_flags), v(d.eff_flags),
                      v(d.entry))
            }
        }
#endif

        c = Counters{};
        g_window_start_ns = now;
    }

} // namespace mg::diagnostics


// REMOVED 2026-08-07: the out-of-line shims behind the entry-point and native-forward timers.
// They answered their question - see gl/log.h and docs/ohos/RENDER-ADAPTATION.md 6.11 - and were
// then two calls per GL entry point on the hottest path in the library.
