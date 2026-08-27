// MobileGlues - gl/frame_stats_core.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#ifndef MOBILEGLUES_FRAME_STATS_CORE_H
#define MOBILEGLUES_FRAME_STATS_CORE_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace mg::frame_stats {

// The always-on part of targeted capture is restricted to buffer and explicit
// synchronization calls. The remaining groups are sampled only for two present
// epochs after a terrain upload or a slow selected call. This keeps the causal
// information that exhaustive capture provided without putting two clock reads
// around 100k+ ordinary state calls every second.
enum class Category : std::uint8_t {
    BufferSubData = 0,
    BufferCopy,
    BufferMap,
    BufferFlush,
    BufferUnmap,
    BufferAllocation,
    StagingOrphan,
    StagingUpload,
    TerrainCopy,
    FramebufferBind,
    Draw,
    Dispatch,
    MemoryBarrier,
    TextureUpload,
    ShaderCompile,
    ProgramLink,
    Fence,
    ClientWait,
    ServerWait,
    Finish,
    Count
};

enum Event : std::uint32_t {
    EventNone = 0,
    EventTerrainUpload = 1U << 0U,
    EventPersistentMap = 1U << 1U,
    EventExplicitFlush = 1U << 2U,
    EventSlowSelectedCall = 1U << 3U,
};

// Client-wait dimensions are deliberately independent. Each one receives the
// same elapsed sample, so every dimension must conserve the legacy
// Category::ClientWait calls/total/max aggregate. The Unclassified buckets make
// a missing wrapper annotation visible instead of silently folding it into a
// real GL parameter/result class.
enum class ClientWaitTimeoutClass : std::uint8_t {
    Zero = 0,
    Finite,
    Int64Max,
    Ignored,
    Other,
    Unclassified,
    Count
};

enum class ClientWaitFlagsClass : std::uint8_t {
    None = 0,
    Flush,
    Other,
    Unclassified,
    Count
};

enum class ClientWaitResultClass : std::uint8_t {
    AlreadySignaled = 0,
    ConditionSatisfied,
    TimeoutExpired,
    WaitFailed,
    Other,
    Unclassified,
    Count
};

constexpr std::size_t kClientWaitTimeoutClassCount =
    static_cast<std::size_t>(ClientWaitTimeoutClass::Count);
constexpr std::size_t kClientWaitFlagsClassCount =
    static_cast<std::size_t>(ClientWaitFlagsClass::Count);
constexpr std::size_t kClientWaitResultClassCount =
    static_cast<std::size_t>(ClientWaitResultClass::Count);

constexpr std::size_t kCategoryCount = static_cast<std::size_t>(Category::Count);
constexpr std::uint64_t kDefaultReportIntervalNs = 1'000'000'000ULL;
constexpr std::uint64_t kSlowSelectedCallNs = 1'000'000ULL;
constexpr std::uint64_t kSlowFrameNs = 25'000'000ULL;
constexpr std::uint64_t kUnarmedTraceFrameNs = 100'000'000ULL;
constexpr std::uint32_t kCaptureFrames = 2;
constexpr std::uint32_t kCaptureCooldownFrames = 30;
constexpr std::uint32_t kMaximumTraceFrames = 64;

// Frame-time buckets are intentionally denser around 16.7/33.3 ms and retain
// long-tail visibility up to five seconds. The final implicit bucket is >5 s.
constexpr std::array<std::uint32_t, 18> kFrameBucketUpperMs{
    4, 8, 12, 16, 20, 25, 33, 50, 67, 100, 150, 250, 500, 1'000, 2'000, 3'000, 4'000, 5'000,
};
constexpr std::size_t kFrameBucketCount = kFrameBucketUpperMs.size() + 1;

struct DurationAggregate {
    std::uint64_t calls{0};
    std::uint64_t total_ns{0};
    std::uint64_t max_ns{0};

    void add(std::uint64_t duration_ns) noexcept {
        ++calls;
        total_ns += duration_ns;
        max_ns = std::max(max_ns, duration_ns);
    }

    void merge(const DurationAggregate& other) noexcept {
        calls += other.calls;
        total_ns += other.total_ns;
        max_ns = std::max(max_ns, other.max_ns);
    }
};

struct ClientWaitCounters {
    std::array<DurationAggregate, kClientWaitTimeoutClassCount> timeouts{};
    std::array<DurationAggregate, kClientWaitFlagsClassCount> flags{};
    std::array<DurationAggregate, kClientWaitResultClassCount> results{};

    void add(std::uint64_t duration_ns, ClientWaitTimeoutClass timeout_class,
             ClientWaitFlagsClass flags_class, ClientWaitResultClass result_class) noexcept {
        timeouts[static_cast<std::size_t>(timeout_class)].add(duration_ns);
        flags[static_cast<std::size_t>(flags_class)].add(duration_ns);
        results[static_cast<std::size_t>(result_class)].add(duration_ns);
    }

    void merge(const ClientWaitCounters& other) noexcept {
        for (std::size_t i = 0; i < timeouts.size(); ++i) timeouts[i].merge(other.timeouts[i]);
        for (std::size_t i = 0; i < flags.size(); ++i) flags[i].merge(other.flags[i]);
        for (std::size_t i = 0; i < results.size(); ++i) results[i].merge(other.results[i]);
    }
};

struct SlowCall {
    const char* name{nullptr};
    std::uint64_t duration_ns{0};

    void observe(const char* function_name, std::uint64_t duration) noexcept {
        if (duration <= duration_ns) return;
        name = function_name;
        duration_ns = duration;
    }

    void merge(const SlowCall& other) noexcept { observe(other.name, other.duration_ns); }
};

struct FrameCounters {
    DurationAggregate gl;
    DurationAggregate present;
    std::array<DurationAggregate, kCategoryCount> categories{};
    ClientWaitCounters client_wait{};
    SlowCall slowest{};
    // Time outside the calls selected by the active coverage mode. It includes
    // game/JIT/GC time and, outside a short causal window, ordinary unobserved GL.
    std::uint64_t outside_gl_ns{0};
    std::uint64_t outside_gl_max_ns{0};
    std::uint64_t buffer_bytes{0};
    std::uint64_t map_bytes{0};
    std::uint64_t flush_bytes{0};
    std::uint64_t terrain_bytes{0};
    std::uint64_t staging_bytes{0};
    std::uint64_t terrain_copy_bytes{0};
    std::uint64_t terrain_calls{0};
    std::uint64_t terrain_staged_calls{0};
    std::uint64_t suppressed_flush_calls{0};
    std::uint32_t event_mask{EventNone};
    bool causal_capture{false};

    void merge(const FrameCounters& other) noexcept {
        gl.merge(other.gl);
        present.merge(other.present);
        for (std::size_t i = 0; i < categories.size(); ++i) categories[i].merge(other.categories[i]);
        client_wait.merge(other.client_wait);
        slowest.merge(other.slowest);
        outside_gl_ns += other.outside_gl_ns;
        outside_gl_max_ns = std::max(outside_gl_max_ns, other.outside_gl_max_ns);
        buffer_bytes += other.buffer_bytes;
        map_bytes += other.map_bytes;
        flush_bytes += other.flush_bytes;
        terrain_bytes += other.terrain_bytes;
        staging_bytes += other.staging_bytes;
        terrain_copy_bytes += other.terrain_copy_bytes;
        terrain_calls += other.terrain_calls;
        terrain_staged_calls += other.terrain_staged_calls;
        suppressed_flush_calls += other.suppressed_flush_calls;
        event_mask |= other.event_mask;
        causal_capture = causal_capture || other.causal_capture;
    }
};

struct Report {
    std::uint64_t sequence{0};
    std::uint64_t window_ns{0};
    std::uint64_t frames{0};
    std::uint64_t frame_total_ns{0};
    std::uint64_t frame_max_ns{0};
    std::array<std::uint64_t, kFrameBucketCount> frame_histogram{};
    FrameCounters counters{};
};

struct FrameTrace {
    bool valid{false};
    std::uint64_t sequence{0};
    std::uint64_t frame_ns{0};
    FrameCounters counters{};
};

inline const DurationAggregate& category(const Report& report, Category value) noexcept {
    return report.counters.categories[static_cast<std::size_t>(value)];
}

inline const DurationAggregate& category(const FrameTrace& trace, Category value) noexcept {
    return trace.counters.categories[static_cast<std::size_t>(value)];
}

inline const DurationAggregate& clientWaitTimeout(const FrameCounters& counters,
                                                  ClientWaitTimeoutClass value) noexcept {
    return counters.client_wait.timeouts[static_cast<std::size_t>(value)];
}

inline const DurationAggregate& clientWaitFlags(const FrameCounters& counters,
                                                ClientWaitFlagsClass value) noexcept {
    return counters.client_wait.flags[static_cast<std::size_t>(value)];
}

inline const DurationAggregate& clientWaitResult(const FrameCounters& counters,
                                                 ClientWaitResultClass value) noexcept {
    return counters.client_wait.results[static_cast<std::size_t>(value)];
}

inline std::uint32_t percentileUpperMs(const Report& report, std::uint32_t percentile) noexcept {
    if (report.frames == 0) return 0;
    const std::uint64_t target = (report.frames * percentile + 99) / 100;
    std::uint64_t seen = 0;
    for (std::size_t i = 0; i < report.frame_histogram.size(); ++i) {
        seen += report.frame_histogram[i];
        if (seen < target) continue;
        if (i < kFrameBucketUpperMs.size()) return kFrameBucketUpperMs[i];
        const std::uint64_t max_ms = (report.frame_max_ns + 999'999ULL) / 1'000'000ULL;
        return static_cast<std::uint32_t>(std::min<std::uint64_t>(max_ms, std::numeric_limits<std::uint32_t>::max()));
    }
    return 0;
}

// Pure state machine: callers provide monotonic timestamps. Keeping clock and
// logging out of this type makes every accounting and arming rule host-testable.
class Collector {
public:
    explicit Collector(std::uint64_t report_interval_ns = kDefaultReportIntervalNs) noexcept
        : report_interval_ns_(report_interval_ns == 0 ? 1 : report_interval_ns) {}

    bool insideGlCall() const noexcept { return gl_depth_ != 0; }
    bool causalCaptureActive() const noexcept { return capture_frames_remaining_ != 0; }

    // Called by the lightweight scope installed on every wrapper. The first
    // branch is the normal path; no name classification or clock read follows it.
    bool shouldObserveCausal(const char* function_name) const noexcept {
        return causalCaptureActive() && causalCategory(classify(function_name));
    }

    void glEnter(std::uint64_t now_ns, const char* function_name) noexcept {
        if (gl_depth_++ != 0) return;
        addOutsideGap(now_ns);
        outer_gl_start_ns_ = now_ns;
        outer_function_name_ = function_name;
        outer_category_ = classify(function_name);
        outer_buffer_bytes_recorded_ = false;
        outer_client_wait_recorded_ = false;
        outer_client_wait_timeout_ = ClientWaitTimeoutClass::Unclassified;
        outer_client_wait_flags_ = ClientWaitFlagsClass::Unclassified;
        outer_client_wait_result_ = ClientWaitResultClass::Unclassified;
    }

    void glExit(std::uint64_t now_ns) noexcept {
        if (gl_depth_ <= 0) {
            gl_depth_ = 0;
            return;
        }
        if (--gl_depth_ != 0) return;

        const std::uint64_t elapsed = elapsedNs(outer_gl_start_ns_, now_ns);
        frame_.gl.add(elapsed);
        frame_.slowest.observe(outer_function_name_, elapsed);
        if (outer_category_ != Category::Count) {
            frame_.categories[static_cast<std::size_t>(outer_category_)].add(elapsed);
        }
        if (outer_category_ == Category::ClientWait) {
            frame_.client_wait.add(elapsed, outer_client_wait_timeout_, outer_client_wait_flags_,
                                   outer_client_wait_result_);
        }
        if (isAlwaysSelected(outer_category_) && elapsed >= kSlowSelectedCallNs) {
            frame_.event_mask |= EventSlowSelectedCall;
            armCapture();
        }
        last_gl_exit_ns_ = now_ns;
    }

    // The wrapper records these after the backend returns, while the original
    // GL scope is still active. A nested wrapper cannot steal ownership from the
    // outer public call, and duplicate annotations retain first-writer identity.
    void recordClientWait(std::uint32_t flags, std::uint64_t timeout, std::uint32_t result) noexcept {
        if (gl_depth_ != 1 || outer_category_ != Category::ClientWait || outer_client_wait_recorded_) return;
        outer_client_wait_timeout_ = classifyClientWaitTimeout(timeout);
        outer_client_wait_flags_ = classifyClientWaitFlags(flags);
        outer_client_wait_result_ = classifyClientWaitResult(result);
        outer_client_wait_recorded_ = true;
    }

    // Some DSA functions implement one public operation through a nested GL
    // wrapper. Only the outer call owns attribution; the first annotation wins.
    void recordBufferBytes(std::uint64_t bytes) noexcept {
        if (gl_depth_ <= 0 || outer_buffer_bytes_recorded_) return;
        switch (outer_category_) {
        case Category::BufferSubData:
        case Category::BufferCopy:
            frame_.buffer_bytes += bytes;
            break;
        case Category::BufferMap:
            frame_.map_bytes += bytes;
            break;
        case Category::BufferFlush:
            frame_.flush_bytes += bytes;
            break;
        default:
            return;
        }
        outer_buffer_bytes_recorded_ = true;
    }

    void recordTerrainUpload(std::uint64_t bytes) noexcept {
        ++frame_.terrain_calls;
        frame_.terrain_bytes += bytes;
        frame_.event_mask |= EventTerrainUpload;
        armCapture();
    }

    void recordTerrainStaged() noexcept {
        if (frame_.terrain_staged_calls < frame_.terrain_calls) ++frame_.terrain_staged_calls;
    }

    void recordPersistentMap() noexcept {
        frame_.event_mask |= EventPersistentMap;
        armCapture();
    }

    void recordExplicitFlush(bool suppressed) noexcept {
        frame_.event_mask |= EventExplicitFlush;
        if (suppressed) ++frame_.suppressed_flush_calls;
        armCapture();
    }

    // Records a raw backend phase nested inside a public wrapper. It is a subset
    // of outer GL time and therefore must not be added to frame_.gl.
    void recordPhase(Category value, std::uint64_t duration_ns, std::uint64_t bytes) noexcept {
        if (value == Category::Count) return;
        frame_.categories[static_cast<std::size_t>(value)].add(duration_ns);
        frame_.slowest.observe(categoryName(value), duration_ns);
        if (value == Category::StagingUpload) frame_.staging_bytes += bytes;
        if (value == Category::TerrainCopy) frame_.terrain_copy_bytes += bytes;
    }

    void presentBegin(std::uint64_t now_ns) noexcept {
        addOutsideGap(now_ns);
        if (causalCaptureActive()) frame_.causal_capture = true;
        present_start_ns_ = now_ns;
        present_in_progress_ = true;
    }

    bool presentEnd(std::uint64_t now_ns, Report& out, FrameTrace* trace = nullptr) noexcept {
        if (trace != nullptr) *trace = {};
        if (present_in_progress_) {
            frame_.present.add(elapsedNs(present_start_ns_, now_ns));
            present_in_progress_ = false;
        }

        // Loading screens may issue GL before the first present. Discard that
        // partial interval: it has no preceding frame boundary.
        if (!primed_) {
            primed_ = true;
            last_present_end_ns_ = now_ns;
            last_gl_exit_ns_ = now_ns;
            window_start_ns_ = now_ns;
            frame_ = {};
            return false;
        }

        const std::uint64_t frame_ns = elapsedNs(last_present_end_ns_, now_ns);
        ++frame_sequence_;
        const bool event_trace = frame_.event_mask != EventNone || frame_.causal_capture;
        if (trace != nullptr && emitted_trace_frames_ < kMaximumTraceFrames &&
            ((event_trace && frame_ns >= kSlowFrameNs) || frame_ns >= kUnarmedTraceFrameNs)) {
            trace->valid = true;
            trace->sequence = frame_sequence_;
            trace->frame_ns = frame_ns;
            trace->counters = frame_;
            ++emitted_trace_frames_;
        }

        mergeFrame(frame_ns);
        frame_ = {};
        last_present_end_ns_ = now_ns;
        last_gl_exit_ns_ = now_ns;

        if (capture_frames_remaining_ != 0) {
            --capture_frames_remaining_;
            if (capture_frames_remaining_ == 0) capture_cooldown_frames_ = kCaptureCooldownFrames;
        } else if (capture_cooldown_frames_ != 0) {
            --capture_cooldown_frames_;
        }

        const std::uint64_t window_ns = elapsedNs(window_start_ns_, now_ns);
        if (window_ns < report_interval_ns_) return false;

        report_.sequence = ++report_sequence_;
        report_.window_ns = window_ns;
        out = report_;
        report_ = {};
        window_start_ns_ = now_ns;
        return true;
    }

private:
    static std::uint64_t elapsedNs(std::uint64_t begin_ns, std::uint64_t end_ns) noexcept {
        return end_ns >= begin_ns ? end_ns - begin_ns : 0;
    }

    static bool startsWith(const char* value, const char* prefix) noexcept {
        if (value == nullptr || prefix == nullptr) return false;
        return std::strncmp(value, prefix, std::strlen(prefix)) == 0;
    }

    static bool equals(const char* value, const char* expected) noexcept {
        return value != nullptr && std::strcmp(value, expected) == 0;
    }

    static Category classify(const char* name) noexcept {
        if (equals(name, "glBufferSubData") || equals(name, "glNamedBufferSubData") ||
            equals(name, "glNamedBufferSubDataEXT")) return Category::BufferSubData;
        if (equals(name, "glCopyBufferSubData") || equals(name, "glCopyNamedBufferSubData"))
            return Category::BufferCopy;
        if (equals(name, "glMapBufferRange") || equals(name, "glMapNamedBufferRange") ||
            equals(name, "glMapNamedBufferRangeEXT")) return Category::BufferMap;
        if (equals(name, "glFlushMappedBufferRange") || equals(name, "glFlushMappedNamedBufferRange") ||
            equals(name, "glFlushMappedNamedBufferRangeEXT")) return Category::BufferFlush;
        if (equals(name, "glUnmapBuffer") || equals(name, "glUnmapNamedBuffer") ||
            equals(name, "glUnmapNamedBufferEXT")) return Category::BufferUnmap;
        if (equals(name, "glBufferData") || equals(name, "glBufferStorage") ||
            equals(name, "glNamedBufferData") || equals(name, "glNamedBufferStorage"))
            return Category::BufferAllocation;
        if (equals(name, "glBindFramebuffer")) return Category::FramebufferBind;
        if (startsWith(name, "glDraw") || startsWith(name, "glMultiDraw") ||
            startsWith(name, "mg_glMultiDraw")) return Category::Draw;
        if (startsWith(name, "glDispatchCompute")) return Category::Dispatch;
        if (startsWith(name, "glMemoryBarrier")) return Category::MemoryBarrier;
        if (startsWith(name, "glTexImage") || startsWith(name, "glTexSubImage") ||
            startsWith(name, "glCompressedTex") || startsWith(name, "glCopyTex"))
            return Category::TextureUpload;
        if (equals(name, "glCompileShader")) return Category::ShaderCompile;
        if (equals(name, "glLinkProgram")) return Category::ProgramLink;
        if (equals(name, "glFenceSync")) return Category::Fence;
        if (equals(name, "glClientWaitSync")) return Category::ClientWait;
        if (equals(name, "glWaitSync")) return Category::ServerWait;
        if (equals(name, "glFinish")) return Category::Finish;
        return Category::Count;
    }

    static ClientWaitTimeoutClass classifyClientWaitTimeout(std::uint64_t timeout) noexcept {
        constexpr std::uint64_t int64_max =
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        if (timeout == 0) return ClientWaitTimeoutClass::Zero;
        if (timeout < int64_max) return ClientWaitTimeoutClass::Finite;
        if (timeout == int64_max) return ClientWaitTimeoutClass::Int64Max;
        if (timeout == std::numeric_limits<std::uint64_t>::max()) return ClientWaitTimeoutClass::Ignored;
        return ClientWaitTimeoutClass::Other;
    }

    static ClientWaitFlagsClass classifyClientWaitFlags(std::uint32_t flags) noexcept {
        constexpr std::uint32_t sync_flush_commands_bit = 0x00000001U;
        if (flags == 0) return ClientWaitFlagsClass::None;
        if (flags == sync_flush_commands_bit) return ClientWaitFlagsClass::Flush;
        return ClientWaitFlagsClass::Other;
    }

    static ClientWaitResultClass classifyClientWaitResult(std::uint32_t result) noexcept {
        constexpr std::uint32_t already_signaled = 0x911AU;
        constexpr std::uint32_t timeout_expired = 0x911BU;
        constexpr std::uint32_t condition_satisfied = 0x911CU;
        constexpr std::uint32_t wait_failed = 0x911DU;
        if (result == already_signaled) return ClientWaitResultClass::AlreadySignaled;
        if (result == condition_satisfied) return ClientWaitResultClass::ConditionSatisfied;
        if (result == timeout_expired) return ClientWaitResultClass::TimeoutExpired;
        if (result == wait_failed) return ClientWaitResultClass::WaitFailed;
        return ClientWaitResultClass::Other;
    }

    static bool isAlwaysSelected(Category value) noexcept {
        return value == Category::BufferSubData || value == Category::BufferCopy ||
               value == Category::BufferMap || value == Category::BufferFlush ||
               value == Category::BufferUnmap || value == Category::Fence ||
               value == Category::ClientWait || value == Category::ServerWait ||
               value == Category::Finish;
    }

    static bool causalCategory(Category value) noexcept {
        return value == Category::BufferAllocation || value == Category::FramebufferBind ||
               value == Category::Draw || value == Category::Dispatch ||
               value == Category::MemoryBarrier || value == Category::TextureUpload ||
               value == Category::ShaderCompile || value == Category::ProgramLink;
    }

    static const char* categoryName(Category value) noexcept {
        switch (value) {
        case Category::StagingOrphan: return "mgStagingOrphan";
        case Category::StagingUpload: return "mgStagingUpload";
        case Category::TerrainCopy: return "mgTerrainCopy";
        default: return "mgBackendPhase";
        }
    }

    void armCapture() noexcept {
        if (capture_frames_remaining_ != 0 || capture_cooldown_frames_ != 0) return;
        capture_frames_remaining_ = kCaptureFrames;
        frame_.causal_capture = true;
    }

    void addOutsideGap(std::uint64_t now_ns) noexcept {
        if (!primed_ || last_gl_exit_ns_ == 0) return;
        const std::uint64_t gap = elapsedNs(last_gl_exit_ns_, now_ns);
        frame_.outside_gl_ns += gap;
        frame_.outside_gl_max_ns = std::max(frame_.outside_gl_max_ns, gap);
    }

    void mergeFrame(std::uint64_t frame_ns) noexcept {
        ++report_.frames;
        report_.frame_total_ns += frame_ns;
        report_.frame_max_ns = std::max(report_.frame_max_ns, frame_ns);
        report_.counters.merge(frame_);

        const std::uint64_t frame_ms = (frame_ns + 999'999ULL) / 1'000'000ULL;
        std::size_t bucket = 0;
        while (bucket < kFrameBucketUpperMs.size() && frame_ms > kFrameBucketUpperMs[bucket]) ++bucket;
        ++report_.frame_histogram[bucket];
    }

    std::uint64_t report_interval_ns_;
    bool primed_{false};
    bool present_in_progress_{false};
    int gl_depth_{0};
    std::uint64_t outer_gl_start_ns_{0};
    std::uint64_t last_gl_exit_ns_{0};
    std::uint64_t last_present_end_ns_{0};
    std::uint64_t present_start_ns_{0};
    std::uint64_t window_start_ns_{0};
    std::uint64_t frame_sequence_{0};
    std::uint64_t report_sequence_{0};
    const char* outer_function_name_{nullptr};
    Category outer_category_{Category::Count};
    bool outer_buffer_bytes_recorded_{false};
    bool outer_client_wait_recorded_{false};
    ClientWaitTimeoutClass outer_client_wait_timeout_{ClientWaitTimeoutClass::Unclassified};
    ClientWaitFlagsClass outer_client_wait_flags_{ClientWaitFlagsClass::Unclassified};
    ClientWaitResultClass outer_client_wait_result_{ClientWaitResultClass::Unclassified};
    std::uint32_t capture_frames_remaining_{0};
    std::uint32_t capture_cooldown_frames_{0};
    std::uint32_t emitted_trace_frames_{0};
    FrameCounters frame_{};
    Report report_{};
};

} // namespace mg::frame_stats

#endif // MOBILEGLUES_FRAME_STATS_CORE_H
