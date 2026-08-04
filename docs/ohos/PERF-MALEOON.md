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

Later rounds ran on a second device, and the numbers below are not interchangeable between the
two. Say which one a measurement came from:

| Item | Value |
| --- | --- |
| Device | Maleoon 920 tablet, `59JYD25815201311` |
| Surface | 2800x1840, roughly 2.8x the pixels of the 910 phone |
| Driver | `libmaleoon_v200.so`, shader compiler `libbishenggpucompiler_v300.so.15` |
| Game | Minecraft `26.2-neoforge-26.2.0.43-beta`, and `26.1.2` as the regression reference |
| Mods | Sodium `0.9.2-alpha.2+mc26.2`, and vanilla for comparison |

Reference points on the 920, reported by the user from the in-game overlay rather than from
counters: vanilla 26.1.2 about 80 fps, vanilla 26.2 about 80 fps with occasional chunk flicker,
Sodium on 26.2 about 30 fps with scrambled terrain. Any change to this platform line has to be
judged against all three, not against one.

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
| Drop the `buffer_coherent_as_flush` promotion for exact `GL_DYNAMIC_STORAGE_BIT` | Already in the table above as non-coherent storage with explicit flush | Rejected, and re-implemented once by mistake in the 2026-08-03 round without reading this table first |
| Staging ring plus `glCopyBufferSubData` instead of a direct write | Already in the table above | Rejected, and re-implemented once by mistake in the same round; the user's screenshot of black and scrambled terrain is exactly the failure this table already recorded |
| Adaptive routing that picks staged versus direct from observed per-target cost | Two designs, neither worked: the first never triggered because cheap uploads always interleave, the second marked before checking eligibility so every mark was reverted | Rejected: cost is not a usable signal here, eligibility is structural |
| Completing multi-draw-indirect and base-instance support | Nothing in the game calls them: `multiDrawIndexed`, `multiDraw`, `drawIndexedIndirect` and `drawIndirect` have no callers under `net/minecraft/**`, and `gl_DrawID` / `gl_BaseInstance` / `gl_InstanceID` appear in none of the 80 vanilla shaders | Rejected: no reachable caller, so no possible gain |
| Zero-timeout `glClientWaitSync` bypass via `glGetSynciv`, with a `glFlush` when the caller asked for the flush bit | Polling itself became free, but the cost moved to the blocking waits behind it; measured 15.3 ms mean and 190 ms worst single block | Rejected |
| FSR1 upscaling to relieve a GPU-bound frame | Worse on every axis: 53.5 to 44.7 mean fps, 29.6 to 21.8 p10, sync 107.7 to 163.3 ms/s | Rejected, and it is a useful negative: the GPU-behind ratio did not move at all (78% to 79.2%), so the bottleneck is not fill rate |

The pattern is consistent: on this driver, making a host-visible copy correct costs more than
the copy, and removing the synchronization corrupts geometry. Adding another fence anywhere in
this design cannot win.

## The size threshold was the wrong test

The direct-write path gated on buffer size, at 16 MiB. The safety argument behind that number was
"a buffer this large is drawn at most once per frame, so a frame-boundary fence covers every reader
of it". Size was standing in for draw frequency.

The substitution holds for the vanilla terrain uber buffer, which is fully uploaded before anything
is drawn from it. It fails for Sodium, whose terrain arenas are drawn repeatedly within one frame
across the solid, cutout and translucent passes, with fresh chunk uploads interleaved between the
passes. Being large is precisely what those arenas have in common with the uber buffer, so the test
admitted the case it was written to exclude, and Sodium terrain rendered scrambled. Upstream issue
`#432` reports the same corruption on Android with a Mali-G57, so this is not specific to this
platform, and it is marked `Not planned` there.

The fix is to test the condition directly instead of through a proxy. `gl/buffer.cpp` keeps a
frame-scoped mark per buffer, set when the application makes the buffer reachable as a vertex
source, cleared at the frame boundary. `glNamedBufferSubData` takes the unsynchronized path only
for a buffer with no mark, so the only readers that can still be in flight belong to frames the
fence has already accounted for. Marking on bind rather than on draw needs no hook in the draw path
and fails toward the synchronous path, which costs performance rather than correctness. Both
`glBindBuffer(GL_ARRAY_BUFFER, ...)` and `glBindVertexBuffer` have to mark: MC 26.2's
separate-attribute vertex array path uses the latter.

Vanilla keeps the fast path unchanged, because its uploads all happen before the bind.

## The colour clear is not a frame boundary, but the fence stays there anyway

The fence sat on `glClear(GL_COLOR_BUFFER_BIT)`, with a comment asserting the application clears
colour exactly once per rendered frame. Measured on the 920, MC 26.2 issues between 1.5 and 4.3
colour clears per present. Two consequences, and the second one invalidated several rounds of
analysis:

- The fence was created and waited on several times per frame, each wait paying a pipeline round
  trip on a tiled GPU.
- Every `ms/s` and per-frame figure aggregated on that boundary was counting sub-frames. Any
  conclusion drawn from those windows has to be re-derived before it can be trusted.

Moving the fence to `eglSwapBuffers`, which `glfwSwapBuffers` calls exactly once per present, was
implemented and measured. **It is not worth it, and it was reverted.** With diagnostics enabled on
vanilla 1.21.8, the fence in `glClear` costs 20 to 55 ms per second, a few percent of the frame, so
there is nothing meaningful to reclaim. Firing it more often than once per present is also *better*
for the gate above: it keeps the GPU closer to the CPU, so the window in which an unsynchronized
write could reach an in-flight reader is a fraction of a frame rather than a whole one.

What did move to `eglSwapBuffers` is the clearing of the vertex-source marks, and only that. The
marks have to span a whole frame or the gate can be fooled: a buffer bound as a vertex source before
a mid-frame colour clear and drawn after it would lose its mark and wrongly qualify for an
unsynchronized write.

Two facts from the same round, recorded so they are not re-derived. All three fence entry points
resolve on this driver — `glFenceSync`, `glClientWaitSync` and `glDeleteSync` are all non-null — and
`frame_wait_calls` equals `color_clears` exactly, so the fence really does execute. An earlier
observation of `frame_wait_calls = 0` against tens of colour clears per window came from a local
modification, not from a missing entry point.

## What the counters say about where the frame actually goes

Measured on vanilla MC 1.21.8 with `diagnostics: 1`, 122 consecutive one-second windows, steady state
in a loaded world:

| Quantity | Value |
| --- | --- |
| `color_clears` | 60 to 66 per second, flat, independent of load |
| Frame fence wait | 20 to 55 ms/s |
| Buffer upload (`named_total_us`) | 26 to 36 ms/s |
| Application `glClientWaitSync` | 0.1 ms/s |
| **All instrumented MobileGlues paths** | **5 to 8 % of wall time** |
| `direct_attempts` / `direct_hits` | 0 / 0 |
| `named_max_buffer` | 17.7 to 40 KB |

Two conclusions follow, and both were expensive to reach by any other route.

The direct-write path never triggers on this version: the largest buffer reaching
`glNamedBufferSubData` is tens of kilobytes, far below the 16 MiB threshold. Anything done to that
path — adding it, removing it, gating it — cannot change the frame rate on 1.21.8, which is exactly
what repeated device testing showed.

More importantly, everything this layer is instrumented for accounts for well under a tenth of the
frame. A frame-rate complaint of the form "it used to be 100 and now it is 30" is a 3.3x regression;
this layer's entire measured footprint is 1.06x. **Establish that ratio before optimising anything
here.** Per-thread sampling on the same session put the render thread at roughly 20 % of one core
with 85 of 86 threads sleeping, and `gpu-work-server` busy, which is a GPU-bound frame, not a
translation-layer cost.

## Method errors worth not repeating

These cost more time than any of the code above.

- **Not reading this file first.** Three of the rejected approaches in the table were
  re-implemented from scratch in the 2026-08-03 round, including the one whose recorded verdict
  ("black terrain chunks — in-flight readers race the write") described the exact screenshot the
  user then sent back. Read this table and `DESIGN.md` before writing any code on this platform.
- **Comparing a menu window against an in-game window.** Index-buffer uploads per frame separate
  them cleanly: roughly 2 to 12 is the menu at 27-60 fps, roughly 1100 to 1400 is in-game at
  7-27 fps. Mixing them produced a confident and entirely false claim that disabling the DSA path
  tripled the frame rate. Filter to `ibo per frame > 1000` before comparing anything.
- **Reading `GL_TIME_ELAPSED` as "the GPU is busy".** It spans wall time in which the GPU is
  starved by the CPU, so a large value does not establish GPU-bound.
- **Looking at the wrong member of a counter pair.** `fbo_bind_redundant` was 2.1% and looked
  harmless; `fbo_bind_no_work` in the same dump was 95.7%.
- **Treating a plausible mechanism as a diagnosis.** Missing multi-draw-indirect and base-instance
  support was called the root cause before checking for callers. There are none.

## Direction

Stop promoting device-local resources to host-visible memory in the first place, then upload
into them through the ordered command stream, and track hazards per range instead of per frame.
That is Phases 1 to 3 in [`DESIGN.md`](DESIGN.md).

The vertex-source gate is a coarse, per-buffer, per-frame approximation of Phase 3. It is the
cheapest thing that is actually correct rather than correct-by-assumption. Phase 3 replaces it with
per-range tracking, which would let Sodium's interleaved uploads keep the fast path for ranges no
in-flight draw touches, instead of falling back for the whole buffer after its first bind.

## Upstream status, checked 2026-08-03

There is no 26.2 work upstream to wait for or to rebase onto.

- The fork is 13 commits ahead of `upstream/main`, which is still v1.3.5 `3d60996` with no new
  commits.
- `upstream/draw-indirect` was last touched 2025-03-14 and is abandoned.
- `upstream/direct-state-access` last committed 2025-03-27, and that commit message says
  `framebuffer wip`.
- `upstream/dev` is 4 commits ahead, all EGL virtualization, unrelated to 26.2.
- Issue `#432`, "Invisible Blocks with Sodium 0.9.0+ / 26.2", is marked `Not planned`. PSA `#313`
  states that Sodium, Iris and OptiFine are not supported configurations.

FoldCraftLauncher does not maintain its own translation layer; it is a host for renderer plugins
(GL4ES, NG-GL4ES / Krypton Wrapper, ANGLE, Mesa with virglrenderer, and MobileGlues). Where FCL and
Pojav run the same MobileGlues without this corruption, they are running it on ANGLE, where GLES is
emulated over Vulkan and buffer renaming is cheap. AMCL runs on the native Maleoon GLES driver,
where it is not. Krypton Wrapper's 26.2 work is crash fixes only and it advertises GL 3.1.

The instrumentation and the rejected experiments are preserved on the
`wip/maleoon-buffer-diagnostics` branch. That branch does not compile as-is and is not a
candidate for the platform line; it exists so the measurements above can be reproduced.
