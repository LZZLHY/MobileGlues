// MobileGlues - AMCL low-overhead buffer/fence diagnostics
// Copyright (c) 2026 AMCL contributors
// SPDX-License-Identifier: LGPL-2.1-only

#ifndef AMCL_MOBILEGLUES_BUFFER_PERF_H
#define AMCL_MOBILEGLUES_BUFFER_PERF_H

#include "../includes.h"
#include "log.h"
#include <chrono>
#include <cstdint>

namespace amcl::mgperf {

struct Counters {
    uint64_t colorClears = 0;

    uint64_t frameWaitCalls = 0;
    uint64_t frameWaitNs = 0;
    uint64_t frameWaitMaxNs = 0;
    uint64_t frameWaitAlready = 0;
    uint64_t frameWaitSatisfied = 0;
    uint64_t frameWaitTimeout = 0;
    uint64_t frameWaitFailed = 0;
    uint64_t frameWaitOther = 0;

    uint64_t syncWaitCalls = 0;
    uint64_t syncWaitZeroTimeout = 0;
    uint64_t syncWaitPositiveTimeout = 0;
    uint64_t syncWaitNs = 0;
    uint64_t syncWaitMaxNs = 0;
    uint64_t syncWaitAlready = 0;
    uint64_t syncWaitSatisfied = 0;
    uint64_t syncWaitTimeout = 0;
    uint64_t syncWaitFailed = 0;
    uint64_t syncWaitOther = 0;

    uint64_t namedCalls = 0;
    uint64_t namedBytes = 0;
    uint64_t namedNs = 0;
    uint64_t namedMaxNs = 0;
    uint64_t namedMaxBufferBytes = 0;
    uint64_t namedMaxUploadBytes = 0;

    uint64_t directAttempts = 0;
    uint64_t directHits = 0;
    uint64_t directBytes = 0;
    uint64_t directMapNs = 0;
    uint64_t directMemcpyNs = 0;
    uint64_t directUnmapNs = 0;

    uint64_t fallbackCalls = 0;
    uint64_t fallbackBytes = 0;
    uint64_t fallbackNs = 0;

    uint64_t subDataCalls = 0;
    uint64_t subDataBytes = 0;
    uint64_t subDataNs = 0;
    uint64_t subDataMaxNs = 0;

    uint64_t mapCalls = 0;
    uint64_t mapBytes = 0;
    uint64_t mapNs = 0;
    uint64_t mapMaxNs = 0;

    uint64_t flushCalls = 0;
    uint64_t flushDriverCalls = 0;
    uint64_t flushSkippedCalls = 0;
    uint64_t flushBytes = 0;
    uint64_t flushNs = 0;
    uint64_t flushMaxNs = 0;

    uint64_t copyCalls = 0;
    uint64_t copyBytes = 0;
    uint64_t copyNs = 0;
    uint64_t copyMaxNs = 0;
};

// OpenGL calls for one context are serialized on its owning thread. Keeping one
// counter set per GL thread avoids atomics on the hot upload path while still
// handling a second context correctly if a mod creates one.
inline thread_local Counters g_counters;
inline thread_local uint64_t g_windowStartNs = 0;
inline thread_local bool g_directMapWriteSinceColorClear = false;

inline uint64_t nowNs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

inline uint64_t nonNegativeBytes(GLsizeiptr size) {
    return size > 0 ? static_cast<uint64_t>(size) : 0;
}

inline void updateMax(uint64_t& current, uint64_t value) {
    if (value > current) current = value;
}

inline void countWaitResult(GLenum result, uint64_t& already, uint64_t& satisfied, uint64_t& timeout,
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

inline void recordFrameWait(GLenum result, uint64_t elapsedNs) {
    Counters& c = g_counters;
    c.frameWaitCalls++;
    c.frameWaitNs += elapsedNs;
    updateMax(c.frameWaitMaxNs, elapsedNs);
    countWaitResult(result, c.frameWaitAlready, c.frameWaitSatisfied, c.frameWaitTimeout, c.frameWaitFailed,
                    c.frameWaitOther);
}

inline void recordSyncWait(GLenum result, GLuint64 timeoutNs, uint64_t elapsedNs) {
    Counters& c = g_counters;
    c.syncWaitCalls++;
    if (timeoutNs == 0)
        c.syncWaitZeroTimeout++;
    else
        c.syncWaitPositiveTimeout++;
    c.syncWaitNs += elapsedNs;
    updateMax(c.syncWaitMaxNs, elapsedNs);
    countWaitResult(result, c.syncWaitAlready, c.syncWaitSatisfied, c.syncWaitTimeout, c.syncWaitFailed,
                    c.syncWaitOther);
}

inline void recordNamedUpload(uint64_t bytes, uint64_t bufferBytes, uint64_t elapsedNs) {
    Counters& c = g_counters;
    c.namedCalls++;
    c.namedBytes += bytes;
    c.namedNs += elapsedNs;
    updateMax(c.namedMaxNs, elapsedNs);
    updateMax(c.namedMaxBufferBytes, bufferBytes);
    updateMax(c.namedMaxUploadBytes, bytes);
}

inline void recordDirectMapAttempt(uint64_t elapsedNs) {
    g_counters.directAttempts++;
    g_counters.directMapNs += elapsedNs;
}

inline void recordDirectMapHit(uint64_t bytes, uint64_t memcpyNs, uint64_t unmapNs,
                               bool requiresFrameFence = true) {
    Counters& c = g_counters;
    c.directHits++;
    c.directBytes += bytes;
    c.directMemcpyNs += memcpyNs;
    c.directUnmapNs += unmapNs;
    if (requiresFrameFence) g_directMapWriteSinceColorClear = true;
}

inline bool consumeDirectMapWrite() {
    const bool value = g_directMapWriteSinceColorClear;
    g_directMapWriteSinceColorClear = false;
    return value;
}

inline void recordFallback(uint64_t bytes, uint64_t elapsedNs) {
    Counters& c = g_counters;
    c.fallbackCalls++;
    c.fallbackBytes += bytes;
    c.fallbackNs += elapsedNs;
}

inline void recordSubData(uint64_t bytes, uint64_t elapsedNs) {
    Counters& c = g_counters;
    c.subDataCalls++;
    c.subDataBytes += bytes;
    c.subDataNs += elapsedNs;
    updateMax(c.subDataMaxNs, elapsedNs);
}

inline void recordMap(uint64_t bytes, uint64_t elapsedNs) {
    Counters& c = g_counters;
    c.mapCalls++;
    c.mapBytes += bytes;
    c.mapNs += elapsedNs;
    updateMax(c.mapMaxNs, elapsedNs);
}

inline void recordFlush(uint64_t bytes, bool calledDriver, uint64_t elapsedNs) {
    Counters& c = g_counters;
    c.flushCalls++;
    if (calledDriver)
        c.flushDriverCalls++;
    else
        c.flushSkippedCalls++;
    c.flushBytes += bytes;
    c.flushNs += elapsedNs;
    updateMax(c.flushMaxNs, elapsedNs);
}

inline void recordCopy(uint64_t bytes, uint64_t elapsedNs) {
    Counters& c = g_counters;
    c.copyCalls++;
    c.copyBytes += bytes;
    c.copyNs += elapsedNs;
    updateMax(c.copyMaxNs, elapsedNs);
}

inline unsigned long long value(uint64_t n) {
    return static_cast<unsigned long long>(n);
}

inline void onColorClear(const char* mode) {
    Counters& c = g_counters;
    c.colorClears++;

    const uint64_t now = nowNs();
    if (g_windowStartNs == 0) {
        g_windowStartNs = now;
        return;
    }
    const uint64_t windowNs = now - g_windowStartNs;
    if (windowNs < 1000000000ULL) return;

    LOG_I("[AMCL-MG-PERF] mode=%s window_ms=%llu color_clears=%llu frame_wait_calls=%llu "
          "frame_wait_total_us=%llu frame_wait_max_us=%llu frame_wait_already=%llu frame_wait_satisfied=%llu "
          "frame_wait_timeout=%llu frame_wait_failed=%llu frame_wait_other=%llu sync_wait_calls=%llu "
          "sync_wait_zero=%llu sync_wait_positive=%llu sync_wait_total_us=%llu sync_wait_max_us=%llu "
          "sync_wait_already=%llu sync_wait_satisfied=%llu sync_wait_timeout=%llu sync_wait_failed=%llu sync_wait_other=%llu",
          mode, value(windowNs / 1000000ULL), value(c.colorClears), value(c.frameWaitCalls),
          value(c.frameWaitNs / 1000ULL), value(c.frameWaitMaxNs / 1000ULL), value(c.frameWaitAlready),
          value(c.frameWaitSatisfied), value(c.frameWaitTimeout), value(c.frameWaitFailed), value(c.frameWaitOther),
          value(c.syncWaitCalls), value(c.syncWaitZeroTimeout), value(c.syncWaitPositiveTimeout),
          value(c.syncWaitNs / 1000ULL), value(c.syncWaitMaxNs / 1000ULL), value(c.syncWaitAlready),
          value(c.syncWaitSatisfied), value(c.syncWaitTimeout), value(c.syncWaitFailed), value(c.syncWaitOther));

    LOG_I("[AMCL-MG-BUFFER] named_calls=%llu named_bytes=%llu named_total_us=%llu named_max_us=%llu "
          "named_max_buffer=%llu named_max_upload=%llu direct_attempts=%llu direct_hits=%llu direct_bytes=%llu "
          "direct_map_us=%llu direct_memcpy_us=%llu direct_unmap_us=%llu fallback_calls=%llu fallback_bytes=%llu "
          "fallback_us=%llu subdata_calls=%llu subdata_bytes=%llu subdata_us=%llu subdata_max_us=%llu "
          "map_calls=%llu map_bytes=%llu map_us=%llu map_max_us=%llu flush_calls=%llu flush_driver=%llu "
          "flush_skipped=%llu flush_bytes=%llu flush_us=%llu flush_max_us=%llu copy_calls=%llu copy_bytes=%llu "
          "copy_us=%llu copy_max_us=%llu",
          value(c.namedCalls), value(c.namedBytes), value(c.namedNs / 1000ULL), value(c.namedMaxNs / 1000ULL),
          value(c.namedMaxBufferBytes), value(c.namedMaxUploadBytes), value(c.directAttempts), value(c.directHits),
          value(c.directBytes), value(c.directMapNs / 1000ULL), value(c.directMemcpyNs / 1000ULL),
          value(c.directUnmapNs / 1000ULL), value(c.fallbackCalls), value(c.fallbackBytes),
          value(c.fallbackNs / 1000ULL), value(c.subDataCalls), value(c.subDataBytes), value(c.subDataNs / 1000ULL),
          value(c.subDataMaxNs / 1000ULL), value(c.mapCalls), value(c.mapBytes), value(c.mapNs / 1000ULL),
          value(c.mapMaxNs / 1000ULL), value(c.flushCalls), value(c.flushDriverCalls), value(c.flushSkippedCalls),
          value(c.flushBytes), value(c.flushNs / 1000ULL), value(c.flushMaxNs / 1000ULL), value(c.copyCalls),
          value(c.copyBytes), value(c.copyNs / 1000ULL), value(c.copyMaxNs / 1000ULL));

    c = Counters{};
    g_windowStartNs = now;
}

} // namespace amcl::mgperf

#endif // AMCL_MOBILEGLUES_BUFFER_PERF_H
