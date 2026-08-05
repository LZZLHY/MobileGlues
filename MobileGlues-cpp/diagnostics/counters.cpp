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

        c = Counters{};
        g_window_start_ns = now;
    }

} // namespace mg::diagnostics
