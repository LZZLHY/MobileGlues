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
    };

    inline thread_local Counters g_counters;

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

    // Called at every frame boundary. Counts the frame and, once a second has passed, emits the
    // aggregate and starts a new window.
    //
    // 'policy' names the storage and upload behaviour in effect, so records from different builds
    // can be told apart when they are compared later. Pass a stable identifier, not a description.
    void on_frame_boundary(const char* policy);

} // namespace mg::diagnostics

#endif // MOBILEGLUES_DIAGNOSTICS_COUNTERS_H
