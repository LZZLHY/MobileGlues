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

// Whether the per-GL-call scopes are compiled at all. This is a second axis, not
// a second name for AMCL_MG_FRAME_STATS.
//
// AMCL_MG_FRAME_STATS used to control two things that cost four orders of
// magnitude apart: the present-boundary frame accounting (one clock pair per
// frame -- tens per second) and the scope installed on GL wrappers (tens to
// hundreds of thousands per second). Turning the observer off to measure its own
// cost therefore also removed the frame rate it was being measured against,
// which is the one number an A/B of this needs to keep.
//
// With this at 0 the wrapper scopes become the same statement the optimiser
// removes that AMCL_MG_FRAME_STATS=0 produces, while presentBegin/presentEnd and
// the report keep emitting: fps, frame_ms percentiles and present_ms stay
// comparable against every session recorded before the switch existed.
// observed_calls and the per-category rows read zero, and that zero means "not
// instrumented", never "did not happen" -- the same reading rule the causal
// coverage already required.
//
// ⭐ What the first A/B this made possible actually said (2026-08-30, Maleoon 920,
// MC 1.18.2 vanilla, same world and settings, active-world present rate):
//
//   obs_calls(max)=2      weighted 24.48   p50 23.39     (instrumentation absent)
//   obs_calls(max)=81903  weighted 46.18   p50 43.89     (everything on)
//   obs_calls(max)=0      weighted 42.38   p50 39.54     (this switch at 0)
//
// The run with no instrumentation is the slowest of the three. Turning the scopes
// off did not beat leaving them on. Whatever these scopes cost on this client, it
// is not what sets its frame rate -- so this switch is a measurement tool and a
// default-hygiene decision, not a performance fix. Do not cite it as one.
#ifndef AMCL_MG_FRAME_STATS_GL_SCOPES
#define AMCL_MG_FRAME_STATS_GL_SCOPES 1
#endif

#if AMCL_MG_FRAME_STATS_EXHAUSTIVE && !AMCL_MG_FRAME_STATS_GL_SCOPES
#error "AMCL_MG_FRAME_STATS_EXHAUSTIVE times every wrapped call and cannot be combined with AMCL_MG_FRAME_STATS_GL_SCOPES=0"
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
inline void recordClientWait(std::uint32_t flags, std::uint64_t timeout, std::uint32_t result) noexcept {
    g_collector.recordClientWait(flags, timeout, result);
}
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
#elif AMCL_MG_FRAME_STATS_GL_SCOPES
// In selective mode every wrapper pays only a predictable inactive branch.
// During a two-frame causal window the scope clocks only draw/FBO/dispatch/
// barrier/texture/shader calls; ordinary state/query calls remain untouched.
//
// "Only a branch" describes the unarmed state, and what arms the window is not
// under this file's control: presentEnd() arms on any frame at or over
// kSlowFrameNs, so below that frame rate the causal window is open for its
// bounded duty cycle continuously rather than around an event. The always-on
// Selected scope has no window at all -- it clocks every call it is installed on,
// and on a pre-1.20 client glBufferData carries the whole per-frame buffer path.
// Both are deliberate; this switch is how their combined cost gets measured
// instead of argued about.
#define MG_FRAME_STATS_ALL_GL_SCOPE() MG_FRAME_STATS_SCOPE(Causal)
#define MG_FRAME_STATS_SELECTED_GL_SCOPE() MG_FRAME_STATS_SCOPE(Selected)
#else
// Frame-boundary accounting only. No wrapper installs a scope, so no GL call
// reads a clock, classifies a function name, or touches the collector's
// thread-local -- while the report still names the frame rate those calls are
// being blamed for.
#define MG_FRAME_STATS_ALL_GL_SCOPE() ((void)0);
#define MG_FRAME_STATS_SELECTED_GL_SCOPE() ((void)0);
#endif

#else

namespace mg::frame_stats {
inline void recordBufferBytes(std::uint64_t) noexcept {}
inline void recordClientWait(std::uint32_t, std::uint64_t, std::uint32_t) noexcept {}
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
