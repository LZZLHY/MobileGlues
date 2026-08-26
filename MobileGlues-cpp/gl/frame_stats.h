// MobileGlues - gl/frame_stats.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#ifndef MOBILEGLUES_FRAME_STATS_H
#define MOBILEGLUES_FRAME_STATS_H

#include "frame_stats_core.h"

#ifndef AMCL_MG_FRAME_STATS
#define AMCL_MG_FRAME_STATS 0
#endif

#ifndef AMCL_MG_FRAME_STATS_EXHAUSTIVE
#define AMCL_MG_FRAME_STATS_EXHAUSTIVE 0
#endif

#if AMCL_MG_FRAME_STATS

#include <ctime>

namespace mg::frame_stats {

inline std::uint64_t monotonicNowNs() noexcept {
    timespec value{};
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return static_cast<std::uint64_t>(value.tv_sec) * 1'000'000'000ULL + static_cast<std::uint64_t>(value.tv_nsec);
}

// GL contexts are thread-affine in the path under test, so thread-local state is
// both the correct ownership model and the cheapest one: no locks, allocation or
// cross-thread cache traffic is added to a GL call.
inline thread_local Collector g_collector{};

class GlCallScope {
public:
    enum class Mode { Selected, Causal, Exhaustive };

    GlCallScope(const char* function_name, Mode mode) noexcept {
        // Nested DSA forwarding belongs to the outer public operation. Avoiding
        // the inner clock reads matters in the same hot paths this observer is
        // intended to measure.
        if (g_collector.insideGlCall()) return;
        if (mode == Mode::Causal && !g_collector.shouldObserveCausal(function_name)) return;
        active_ = true;
        g_collector.glEnter(monotonicNowNs(), function_name);
    }
    ~GlCallScope() {
        if (active_) g_collector.glExit(monotonicNowNs());
    }

    GlCallScope(const GlCallScope&) = delete;
    GlCallScope& operator=(const GlCallScope&) = delete;

private:
    bool active_{false};
};

inline void recordBufferBytes(std::uint64_t bytes) noexcept { g_collector.recordBufferBytes(bytes); }
inline void recordTerrainUpload(std::uint64_t bytes) noexcept { g_collector.recordTerrainUpload(bytes); }
inline void recordTerrainStaged() noexcept { g_collector.recordTerrainStaged(); }
inline void recordPersistentMap() noexcept { g_collector.recordPersistentMap(); }
inline void recordExplicitFlush(bool suppressed) noexcept { g_collector.recordExplicitFlush(suppressed); }
inline void recordPhase(Category category, std::uint64_t begin_ns, std::uint64_t bytes = 0) noexcept {
    const std::uint64_t end_ns = monotonicNowNs();
    g_collector.recordPhase(category, begin_ns != 0 && end_ns >= begin_ns ? end_ns - begin_ns : 0, bytes);
}
inline void presentBegin() noexcept { g_collector.presentBegin(monotonicNowNs()); }
inline bool presentEnd(Report& report, FrameTrace* trace = nullptr) noexcept {
    return g_collector.presentEnd(monotonicNowNs(), report, trace);
}

} // namespace mg::frame_stats

#define MG_FRAME_STATS_JOIN_INNER(a, b) a##b
#define MG_FRAME_STATS_JOIN(a, b) MG_FRAME_STATS_JOIN_INNER(a, b)
#define MG_FRAME_STATS_SCOPE(mode)                                                                                     \
    ::mg::frame_stats::GlCallScope MG_FRAME_STATS_JOIN(_mg_frame_stats_scope_, __COUNTER__)(                          \
        __FUNCTION__, ::mg::frame_stats::GlCallScope::Mode::mode);
#define MG_FRAME_STATS_PHASE_BEGIN(name) const std::uint64_t name = ::mg::frame_stats::monotonicNowNs()
#define MG_FRAME_STATS_PHASE_END(name, category, bytes)                                                               \
    ::mg::frame_stats::recordPhase(::mg::frame_stats::Category::category, name, static_cast<std::uint64_t>(bytes))

#if AMCL_MG_FRAME_STATS_EXHAUSTIVE
#define MG_FRAME_STATS_ALL_GL_SCOPE() MG_FRAME_STATS_SCOPE(Exhaustive)
#define MG_FRAME_STATS_SELECTED_GL_SCOPE() ((void)0);
#else
// In selective mode every wrapper pays only a predictable inactive branch.
// During a two-frame causal window the scope clocks only draw/FBO/dispatch/
// barrier/texture/shader calls; ordinary state/query calls remain untouched.
#define MG_FRAME_STATS_ALL_GL_SCOPE() MG_FRAME_STATS_SCOPE(Causal)
#define MG_FRAME_STATS_SELECTED_GL_SCOPE() MG_FRAME_STATS_SCOPE(Selected)
#endif

#else

namespace mg::frame_stats {
inline void recordBufferBytes(std::uint64_t) noexcept {}
inline void recordTerrainUpload(std::uint64_t) noexcept {}
inline void recordTerrainStaged() noexcept {}
inline void recordPersistentMap() noexcept {}
inline void recordExplicitFlush(bool) noexcept {}
inline void presentBegin() noexcept {}
inline bool presentEnd(Report&, FrameTrace* = nullptr) noexcept { return false; }
} // namespace mg::frame_stats

// The disabled form is a statement that the optimiser removes completely. It
// intentionally does not construct an empty RAII object on every GL call.
#define MG_FRAME_STATS_ALL_GL_SCOPE() ((void)0);
#define MG_FRAME_STATS_SELECTED_GL_SCOPE() ((void)0);
#define MG_FRAME_STATS_PHASE_BEGIN(name) ((void)0)
#define MG_FRAME_STATS_PHASE_END(name, category, bytes) ((void)0)

#endif

#endif // MOBILEGLUES_FRAME_STATS_H
