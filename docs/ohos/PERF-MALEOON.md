# Maleoon performance: method, measurements, and rejected approaches

Everything here was measured on hardware. The purpose of this document is that nobody has to
rediscover it, and that future changes are compared against numbers rather than impressions.

## Environment

| Item | Value |
| --- | --- |
| OS | HarmonyOS NEXT 6.x |
| GPU | Huawei Maleoon 910, native GLES 3.2 |
| Driver | `/vendor/lib64/passthrough/libhvgr_v200.so` |
| MobileGlues | 1.3.5 base (`3d60996`) plus this platform line |
| Game | Minecraft `26.2-neoforge-26.2.0.40-beta` |
| Mods | Sodium `0.9.2-alpha.1+mc26.2`, and vanilla for comparison |
| Launcher | AMCL, MobileGlues compiled into `libglfw.so` |

## Method

Counters are accumulated per GL thread with no allocation on the hot path, and flushed as one
aggregate log line per second. Per-frame logging is useless here: the stalls are bursty and
logging them per frame changes the timing being measured.

Recorded per window: color clears (a proxy for frames), and for each interesting path the
call count, total microseconds and maximum microseconds. Synchronization results are counted
separately by outcome, because `GL_TIMEOUT_EXPIRED` versus `GL_CONDITION_SATISFIED` versus
`GL_ALREADY_SIGNALED` tells you whether a wait was real, hopeless, or free.

Scenarios are always the same four, 15 seconds each, in one world with a fixed view distance:
idle, look around in place, horizontal movement, vertical flight. Terrain correctness is
checked visually across the whole pass.

## Baseline: unconditional per-frame fence

The original platform fast path wrote large terrain buffers through an unsynchronized mapping
and protected the cross-frame race with one fence per color clear. Measured over the last
15 windows of each scenario:

| Scenario | Color clears/s | Wait per second | Max single wait | Timeouts |
| --- | --- | --- | --- | --- |
| Idle | 216.9 | 395.9 ms (39.6%) | 10.5 ms | 0 |
| Look in place | 267.6 | 391.6 ms (39.2%) | 10.6 ms | 0 |
| Horizontal | 113.4 | 720.7 ms (72.1%) | 1000.3 ms | 8 |
| Vertical | 209.7 | 590.6 ms (59.1%) | 1000.4 ms | 3 |

In the same windows, buffer copies cost 7-12 ms/s, flushes 0.1-0.2 ms/s, and application
`glClientWaitSync` calls 0.1 ms/s. The upload work itself was never the problem: the frame
was being spent waiting on the fence, and under movement the wait regularly hit the one
second timeout.

## Root cause

Sodium allocates its terrain arenas with `GL_DYNAMIC_STORAGE_BIT` only, and its staging
buffer with `GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_DYNAMIC_STORAGE_BIT`.
MobileGlues promotes *both* to `GL_MAP_WRITE_BIT | GL_MAP_COHERENT_BIT |
GL_MAP_PERSISTENT_BIT`, because `buffer_coherent_as_flush` defaults to enabled whenever ANGLE
is off (`config/settings.cpp`).

That promotion is reasonable on ANGLE, where GLES is emulated over Vulkan and buffer renaming
is cheap. On native Maleoon it means the terrain arena — a pure GPU-read resource the
application never maps — lives in host-coherent memory. Copying into it, and synchronizing
those copies, is what dominates the frame. Profiling with `hiperf` puts the time in the vendor
driver's `ioctl`, allocation and synchronization paths, with a busy `gpu-work-server`.

## Rejected approaches

Each was implemented and measured on device. None is a candidate; the numbers are why.

| Approach | Result | Verdict |
| --- | --- | --- |
| Unconditional fence per frame | 720 ms/s waiting under movement, 1 s timeouts, 113 clears/s | Rejected: the baseline problem itself |
| Candidate fence created every clear, waited only when needed | Submit waits grew to 1.13-1.16 s per round; unflushed sync objects that are deleted are not free on this driver | Rejected |
| Fence only after a real unsynchronized direct write | Fixes vanilla; Sodium still stalls, because its path never goes through the direct write | Insufficient alone, kept for vanilla |
| `glFlush` after every `glFenceSync` | Brief improvement, then back to second-scale submit waits | Rejected |
| Per-buffer non-coherent storage with explicit flush | Flushes did reach the driver, but even idle regressed badly: one window showed 14.3 s wall time for 3 color clears | Rejected |
| Unsynchronized CPU copy from the staging mapping into the arena | Large throughput win, 237-270 clears/s | Rejected: black terrain chunks — in-flight readers race the write |
| Same, with one synchronizing fence per staging batch | Correct output restored | Rejected: 261-454 ms/s of explicit waiting, 41-66 waits/s, single waits up to 18.8 ms; the user reported stalling in every scenario |

The pattern is consistent: on this driver, making a host-visible copy correct costs more than
the copy, and removing the synchronization corrupts geometry. Adding another fence anywhere in
this design cannot win.

## Direction

Stop promoting device-local resources to host-visible memory in the first place, then upload
into them through the ordered command stream, and track hazards per range instead of per frame.
That is Phases 1 to 3 in [`DESIGN.md`](DESIGN.md).

The instrumentation and the rejected experiments are preserved on the
`wip/maleoon-buffer-diagnostics` branch. That branch does not compile as-is and is not a
candidate for the platform line; it exists so the measurements above can be reproduced.
