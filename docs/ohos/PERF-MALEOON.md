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
| ~~Drop the `buffer_coherent_as_flush` promotion for exact `GL_DYNAMIC_STORAGE_BIT`~~ | **This row was wrong and cost a lot of time — see the correction below** | ~~Rejected~~ |
| Drop the promotion for *every* store declaring `GL_DYNAMIC_STORAGE_BIT`, including those the application also requested `MAP_WRITE \| MAP_PERSISTENT` on | Stalls 33 to 42, upload 180 to 219 us per call, median frame rate 55.5 to 42.1; the real `glFlushMappedBufferRange` handed back to the driver measured 5091 us per call | Rejected: a store that *is* mapped every frame needs its coherence |
| Staging ring plus `glCopyBufferSubData` instead of a direct write | Already in the table above | Rejected, and re-implemented once by mistake in the same round; the user's screenshot of black and scrambled terrain is exactly the failure this table already recorded |
| Adaptive routing that picks staged versus direct from observed per-target cost | Two designs, neither worked: the first never triggered because cheap uploads always interleave, the second marked before checking eligibility so every mark was reverted | Rejected: cost is not a usable signal here, eligibility is structural |
| Completing multi-draw-indirect and base-instance support | Nothing in the game calls them: `multiDrawIndexed`, `multiDraw`, `drawIndexedIndirect` and `drawIndirect` have no callers under `net/minecraft/**`, and `gl_DrawID` / `gl_BaseInstance` / `gl_InstanceID` appear in none of the 80 vanilla shaders | Rejected: no reachable caller, so no possible gain |
| Zero-timeout `glClientWaitSync` bypass via `glGetSynciv`, with a `glFlush` when the caller asked for the flush bit | Polling itself became free, but the cost moved to the blocking waits behind it; measured 15.3 ms mean and 190 ms worst single block | Rejected |
| FSR1 upscaling to relieve a GPU-bound frame | Worse on every axis: 53.5 to 44.7 mean fps, 29.6 to 21.8 p10, sync 107.7 to 163.3 ms/s | Rejected, and it is a useful negative: the GPU-behind ratio did not move at all (78% to 79.2%), so the bottleneck is not fill rate |

The pattern is consistent: on this driver, making a host-visible copy correct costs more than
the copy, and removing the synchronization corrupts geometry. Adding another fence anywhere in
this design cannot win.

### 2026-08-04: three upload routes measured against each other, and the conclusion ★

All three were built, shipped to the device and judged by the user on frame rate *and* on
camera smoothness, which is the axis the counters do not show. Terrain correctness was judged
on 26.2 with Sodium.

| Route | Destination storage | What writes it | Terrain | Frame rate | Smooth |
| --- | --- | --- | --- | --- | --- |
| Baseline (`2ea790f`) | promoted to `MAP_WRITE\|COHERENT\|PERSISTENT` | CPU, unsynchronized mapped write | **corrupt** | normal | **yes** |
| Gate v1 (`fa9d6fa`) | same | CPU, synchronous `glBufferSubData` (gate blocked the fast path) | correct | collapsed | no |
| Withhold the promotion (`afa51bf`) | device-local, unmappable | CPU, synchronous `glBufferSubData` | correct | low | no |
| Staged GPU copy (`458b4c5`) | device-local, unmappable | staging store + `glCopyBufferSubData` | correct | **single digits** | no |

Measured single `glBufferSubData` into the 128 MiB terrain store with the promotion withheld,
across 75 one-second windows: in the 21 windows that touched it, mean 65 ms, median 55 ms,
peak 188 ms. In the 54 windows that touched only small stores, mean 149 us. The cost tracks the
destination, not the byte count.

**The only fast configuration is the unsynchronized mapped write into promoted host-coherent
storage, and it is the only incorrect one.** Every route that stops the CPU writing coherent
memory is slower, and the two that remove the mapping entirely are the slowest of all — which
suggests the driver depends on being able to rename a host-visible allocation, and waits for the
readers when it cannot.

Two figures recorded earlier in this file **do not reproduce** and must not be used again to
justify a route: `per ordered glBufferSubData on the same buffers: 11.6 us`, and the
`15.9 us` staging write plus `109 us` copy. Whatever they measured, it was not the terrain store
under load.

What this makes untrue: that the cost of updating the terrain store can be avoided by picking a
different storage class or a different upload route. Three have now been measured and the
ranking is unambiguous.

What is left, and it is not another upload route:

- **Per-range hazard tracking (Phase 3) is the only remaining candidate**, because the fast
  write is fast precisely by doing no synchronization. It can only be made correct by not
  overwriting bytes an in-flight draw reads, which means knowing which ranges those are. Note
  the difficulty honestly: a vertex array binding gives an offset and a stride but no length, so
  the readable range cannot be derived from bindings alone.
- **Or accept the corruption on 26.2 with Sodium**, which is what upstream did: issue `#432`
  reports the same thing on Android with a Mali-G57 and is marked `Not planned`, and PSA `#313`
  states Sodium is not a supported configuration. An unplayable frame rate for every user is
  worse than corrupt terrain in one configuration.
- **And before either: profile the render thread.** Every round in this area that started from a
  mechanism instead of a profile has been wrong, including all three above. `-DAMCL_STACK_SAMPLER=ON`
  exists for this.

One method note, because it caused a false conclusion mid-round: a 29 fps reading was taken while
chunks were still streaming and treated as steady state; the same build measured 60 fps once the
scene settled. Sample the present interval several times, spaced, after movement stops.

### Correction: one row above was a misclassification, and it hid the answer

"Drop the promotion for exact `GL_DYNAMIC_STORAGE_BIT`" was struck through above because its
recorded verdict pointed at a *different* experiment. The two are not the same change:

| | What the store ends up as | What updates it | Cost of the update |
| --- | --- | --- | --- |
| Non-coherent storage with explicit flush *(rejected)* | host-visible, still **mapped**, coherence removed | a mapped write plus `glFlushMappedBufferRange` | the real flush measured **5091 us** per call |
| Withhold the promotion from the exact request *(this one)* | device-local, **not mappable at all** | `glBufferSubData` only | measured **11.6 us** per call |

The first keeps the mapping and therefore keeps the cache maintenance. The second has no
mapping, so there is nothing to synchronize and nothing to flush. Collapsing them into one row
is what left the second unexplored through several rounds of work.

The rejected 2026-08-03 policy header states the measured result of exactly this half:

> The other half of the policy is kept: an exact `DYNAMIC_STORAGE` request below the threshold
> stays unmapped instead of being promoted to `COHERENT | PERSISTENT`, and that is what removed
> the old ~1300 ms/s implicit-synchronization stall on `glBufferSubData`.

The same header records `per ordered glBufferSubData on the same buffers: 11.6 us` against
`per direct upload 3946 us`, and attributes the 36 % of wall clock spent inside
`glMapBufferRange` to "exactly the stutter felt while moving". It also names a configuration,
`maleoon-unmapped-ordered`, that was never built: the constant selected `maleoon-staged-copy`
instead, which layered a second buffer and a GPU copy on top and was rejected on its own
merits. So the measurements for the unmapped half exist, and the route that uses them does not
appear anywhere in this table.

What generalises: **when an entry here says "already covered above", check that the two changes
produce the same end state.** Both of these drop `GL_MAP_COHERENT_BIT`, and that similarity was
enough to merge them, but one leaves the buffer mappable and the other does not, and on this
driver that difference is the whole cost.

## 2026-08-06: the fix, and the two premises that had to be corrected to find it ★★ read this first

This section supersedes several statements below. Where they conflict, this one is right and the
older text is marked.

### What was actually wrong

MC 26.x declares three 128 MiB terrain vertex heaps and one 32 MiB translucent index heap with
`GL_DYNAMIC_STORAGE_BIT` **only**, never maps them, and writes them with `glNamedBufferSubData`.
This layer answered that call with `glMapBufferRange(MAP_WRITE | UNSYNCHRONIZED)` + `memcpy`.

That is a spec violation, not a tuning choice. `glBufferSubData` is defined to behave as if the data
were consumed at call time — it is an ordered command, and achieving that is the implementation's
problem. `GL_MAP_UNSYNCHRONIZED_BIT` is the opposite contract. Worse, the application never requested
`GL_MAP_WRITE_BIT` on these stores, so under `EXT_buffer_storage` mapping them should have failed with
`GL_INVALID_OPERATION`; it only succeeded because the coherent promotion had rewritten the flags.

Where MC *does* take on synchronization duty it says so: its own mappable buffers are created with
`mappingFlags = 50|64` (`WRITE|FLUSH_EXPLICIT|UNSYNCHRONIZED|PERSISTENT`) and backed by
`MappableRingBuffer`'s three-slot fences and `GlTransientMemory`'s recycle-behind-`submit()`. Where it
does not say so, this layer assumed it anyway.

### Two corrections that unlocked the fix

**1. The order is draw-then-upload, not upload-then-draw.** `LevelRenderer.render` bytecode: offset
392 `prepareChunkRenders`, offset **603 `FrameGraphBuilder.execute` — every terrain draw**, offset 644
`compileSections`, offset **682 `uploadTerrainBuffersToGpu`**. So *every* terrain upload happens after
*every* terrain draw of its frame.

Consequences: the claim further down that "the vanilla terrain uber buffer is fully uploaded before
anything is drawn from it" is **false and always was**, which is why vanilla 26.2 and 26.1.2 flickered
too. And no per-buffer or per-frame predicate can separate a safe upload from an unsafe one — they are
all unsafe, always. That is why the vertex-source gate could never work, now with a structural reason
rather than only an A/B.

**2. 26.1.2 has the same architecture.** `mc-2612.jar` contains the same `UberGpuBuffer` /
`TlsfAllocator` classes, the same host-staging selector, the same 128 MiB / 32 MiB sizes. The claim
that "older versions use the previous renderer and never hit this path" is true **only for 1.21.8**
(measured `named_max_buffer` 17.7–40 KB). The real 26.1.2-vs-26.2 difference is synchronization:
`GlCommandEncoder.submit()` / `fences[]` / `MAX_SUBMITS_IN_FLIGHT = 2` exist only in 26.2.

### Every route was closed by measurement, so the axis had to change

Per call into a ≥16 MiB store, all measured on a Maleoon 920:

| route | cost per call |
| --- | --- |
| mapped write, `UNSYNCHRONIZED` | **0.8 µs** — the only fast one, and the incorrect one |
| the same write with `UNSYNCHRONIZED` dropped | **24,934 µs**, worst 104 ms |
| `glBufferSubData` | 3,946–4,604 µs |
| `glBufferSubData`, promotion withheld (device-local) | 55,000 µs, peak 188 ms |
| staging buffer + `glCopyBufferSubData` | single-digit frame rates |

At 48.2 such writes per second the cheapest correct route costs ≈188 ms/s, which does not fit a 45 fps
budget. **Three rounds changed the route or the scope. None changed the time.**

### The fix: defer the write to the frame boundary

Correction 1 is what makes deferral free: the bytes written at offset 682 are not read until offset
603 of the *next* frame, so moving the write later within this frame cannot make any draw see stale
data.

`glNamedBufferSubData` now `memcpy`s into a host queue and returns. The queue takes a fence when it
opens — at the first deferred write of a frame, by which point all of that frame's terrain draws are
submitted. `egl/egl.cpp eglSwapBuffers` replays the queue after the present, waiting on that fence
first. The write is still the 0.8 µs unsynchronized mapped write; it simply no longer races anything.

Cost: one extra cached host `memcpy` per upload (≤371 KB per call, ~1.7 MB/s), one fence per frame,
and one wait per present where a wait is already being paid (114 µs/s, 89 % of polls already
signalled). Single-digit ms/s against 188 ms/s.

How it differs from each rejected row below, since the rule is to say: the unsynchronized-copy row
keeps today's timing and changes the source, this keeps the source and changes the timing; the
fence-per-batch row *added* waits (261–454 ms/s), this adds none; the staging-ring row changes storage
class and issues a GPU copy, this issues byte-identical GL calls to today; the gate rows route some
uploads to synchronous `glBufferSubData`, this never does except on overflow.

**Device result (versionCode 1000491):** vanilla 26.2, 26.1.2 and 1.21.11 all clean — no flicker, no
stutter, frame rate preserved. 26.2 + Sodium improved but not fixed; see the next section.

### Still open, 26.2 + Sodium only, and the two leading causes

Reported symptoms: residual terrain corruption, smaller in extent, **view-angle dependent** (same
spot, a small rotation makes it appear or disappear); and **sprint-flight only** frame collapse to
single digits, while standing still is ~80 fps and ordinary flight is fine.

Two candidates, both specific to Sodium and both consistent with those symptoms:

1. **The safety net is flushing on every `glCopyBufferSubData`.** Sodium issues 186–210 copies per
   second (its ring → arena upload path); vanilla issues almost none. The net currently calls
   `mg_deferred_upload_flush_all()`, which can block on the queue fence. That is the
   fence-per-upload pattern this file already measured at 261–454 ms/s and rejected — reintroduced,
   and visible only in the configuration that copies. Sprint flight is when copy traffic peaks.
   Fix: make the net range-overlap based instead of flush-all.
2. **Sodium's persistently mapped ring may never publish its writes.** `glMapBufferRange` strips
   `GL_MAP_FLUSH_EXPLICIT_BIT` and `glFlushMappedBufferRange` then skips the driver call — but
   `GL_MAP_COHERENT_BIT` is never added to the *mapping*. Per `EXT_buffer_storage` a mapping is
   coherent only when coherence is requested on the mapping; allocating the store with
   `GL_MAP_COHERENT_BIT` does not make a later mapping coherent. So the application's flush was
   removed with nothing put in its place, and a persistent mapping is never unmapped, so there is no
   later point at which the writes are published. The copy can then read ring contents the CPU writes
   had not reached — a chunk assembled from whatever was there before, intermittently. This is
   Sodium-only because only Sodium uses a mapped staging ring; MC's own path is a host `malloc` plus
   `glNamedBufferSubData`. This was fixed once in `920f65a` and reverted along with the staging-ring
   work; the fix is independent of that work and should be re-applied on its own.

## 2026-08-06 later: deferral cannot be made sound for Sodium, and two defects I introduced ★★

Investigation only, no code changed. This section supersedes the optimistic parts of the section
above. Device state at `3de6ef7`: vanilla 26.2, 26.1.2 and 1.21.11 **all clean and must stay clean**;
26.2 + Sodium **worse than before**, with the artifact changing shape to *regions rendering as void
while the geometry that belonged there floats elsewhere*, and the frame rate dropping from ~80 to the
low teens whenever the camera or the player moves.

### C1 — the frame collapse is mine: a fence that cannot be signalled, polled as if it could

`glFenceSync` inserts a fence command; it does **not** submit it. The spec is explicit that a sync
object "can be in the signaled state only once the corresponding fence command has completed", and a
command that was never submitted cannot have completed. `GL_SYNC_FLUSH_COMMANDS_BIT` exists precisely
to force the submission.

`deferred_wait_for_fence()` polls first with `flags = 0`, on the stated grounds that the poll is
"measured at 89 % already-signalled". **That measurement came from the other drain site.** The two are
not comparable:

| drain site | fence age at the wait | poll outcome |
| --- | --- | --- |
| `egl/egl.cpp eglSwapBuffers`, after the present | a whole frame; the present submitted it | succeeds, ~89 %, 114 µs/s total |
| `gl/multidraw.cpp`, mid-frame | microseconds, no present, no `glFlush`, no MC `submit()` in between | **always times out** |

So every multi-draw drain falls through to `glClientWaitSync(GL_SYNC_FLUSH_COMMANDS_BIT, 100 ms)`.
The cost of that bit on this device was already measured and written down — in
`gl/submit_epoch.h` on the `wip/maleoon-buffer-diagnostics` backup:

> `GL_SYNC_FLUSH_COMMANDS_BIT` promises exactly one thing: that the commands recorded before the
> fence have been submitted … On a tiled GPU honouring it is not free, because the driver ends the
> render pass currently being built in order to submit — **measured at 3.77 ms per call on Maleoon
> 920**.

And the drain always fires with a render pass open: `DefaultChunkRenderer.render` creates the pass at
offset 246 and closes it at 500/515, with `nglMultiDrawElementsBaseVertex` at
`GlCommandEncoder.executeDraws` offset 167 in between, with no `primcount == 1` shortcut. So 3.77 ms
of tile teardown at 2800x1840, **plus** a real block for everything recorded before the fence — which
now includes the frame's 32-copy defragmentation budget and all of `MappedStagingBuffer.flush()`'s
consolidated copies. Once per frame that enqueues anything, which is exactly "whenever moving":
standing still there are no chunk builds and no re-sorts, so no enqueues and no drain, so 80 fps.

That is 3.77 ms plus a pipeline drain against the ~65 ms/frame the symptom needs; a single 100 ms
timeout covers it outright. Two rejected rows in this file already said the same thing from different
directions — "`glFlush` after every `glFenceSync`: brief improvement, then back to second-scale submit
waits", and the zero-timeout `glGetSynciv` bypass whose "cost moved to the blocking waits behind it,
15.3 ms mean, 190 ms worst".

### C2 — a correctness hole I introduced in the same function

```c
GLenum result = GLES.glClientWaitSync(g_deferred_fence, 0, 0);
if (result != GL_ALREADY_SIGNALED && result != GL_CONDITION_SATISFIED) {
    result = GLES.glClientWaitSync(g_deferred_fence, GL_SYNC_FLUSH_COMMANDS_BIT, 100000000ULL);
}
mg::diagnostics::record_deferred_fence_wait(result);   // recorded, then discarded
```

The second result is never acted on. On `GL_TIMEOUT_EXPIRED` — reachable within a 100 ms budget under
exactly the load C1 creates — the replay proceeds and performs the `GL_MAP_UNSYNCHRONIZED_BIT` write
against draws that are demonstrably still in flight. Independent of everything else, this alone can
produce corruption.

### C3 — why deferral cannot work for Sodium at all: an ownership change with no GL call

This is the finding that closes the approach, and it is a property of Sodium, not a bug in it.

```
SingleOwnerBufferArena.transferSegments
   28  old = arenaBuffer
   40  parent.getBufferOfSizeAtLeast(bytes)     <- may return a POOLED GpuBuffer
   51  executeCopyCommands(cmds, old, new)
   60  parent.releaseBufferForReuse(old)        <- pools it WITHOUT close()
   66  arenaBuffer = new
```

`ArenaAggregator.releaseBufferForReuse` stores the buffer in a free pool and does **not** call
`close()`; `getBufferOfSizeAtLeast` searches that pool for one sized within
`MAX_BUFFER_REUSE_SIZE_FACTOR = 1.4f` and hands **the same driver buffer object to a different
arena**. There is no `glDeleteBuffers`, no `glBufferData`, no bind — **zero GL calls**. MobileGlues has
nothing to wrap, nothing to shadow and nothing to count.

A queued record holds `(realBuffer, dstOffset)`. If that transition happens between the enqueue and
the drain, the replay writes into whichever arena now owns the object. Sodium bakes the section
position into every vertex, so the foreign block renders at *its own* world coordinates — geometry
floating where nothing should be — while the slot that should have received the bytes stays empty —
a void region. A `RenderRegion` is 8x4x8 sections, which is why the artifact is no longer
chunk-sized. **The symptom matches the mechanism exactly.**

And the window is not exotic: `BufferArena.upload` calls `tryUploads` at offset 122 and
`handleResizeUploads` at offset 141 **in the same call**, so enqueue-then-relocate is the normal path.

By contrast, *offset* changes are always observable, because every `BufferSegment.setOffset` in
`defragmentRightwards` (88, 233) and `buildTransferList` (126) is paired with a `copyToBuffer`, which
MG's range guard can and does intercept. It is only buffer-object **identity** that is invisible.

### The escape hatch I had in mind does not exist

"Defer only until the end of Sodium's upload phase" was checked: `StagingBuffer.flip()` is at the
*start* of the phase (`RenderRegionManager.update` offset 4), `finalizeRenderLists` (502) issues no GL
at all, and the first distinctive GL call after the phase is the terrain multi-draw itself. So "end of
upload phase" and "first terrain draw" are the same point — the point that is already failing for the
C1 reasons above.

### Sodium's frame order, corrected again

An earlier pass had `cleanupAndFlip` missing. `SodiumWorldRenderer.setupTerrain`:

```
349 processGFNIMovement        translucency re-sort trigger (scales with camera movement)
429 cleanupAndFlip             -> RenderRegionManager.update -> StagingBuffer.flip() then
                                  ArenaAggregator.update()  <- DEFRAGMENTATION RUNS HERE
439 updateChunks               uploads
461 processChunkBuilds         uploads
502 finalizeRenderLists        this frame's draw lists, built after the uploads
```

Defragmentation runs **before** the uploads, not after. Its budget floor is
`DEFRAG_COPIES_PER_FRAME = 32/1024MiB` and `DEFRAG_BYTES_PER_FRAME = 32MiB/1024MiB` scaled by
`max(totalDeviceAllocated, 1 GiB)`, i.e. **at least 32 copy commands and 32 MiB of live geometry
relocated per frame**.

Also corrected: `BufferArena.tryUpload` offset 60 **always** calls `StagingBuffer.enqueueCopy`. The
`writeToBuffer` fallback that reaches `glNamedBufferSubData` lives one level down, in
`MappedStagingBuffer.enqueueCopy` offsets 6-33, taken when `size > remaining`. `remaining` is only
replenished by `flip()` once per frame, so within a frame the ring drains monotonically and once
exhausted **every** further upload becomes an MG record. Ring pressure scales with chunk-build rate,
which scales with movement — a second, independent reason the symptoms track movement.

### Where this leaves the design

Deferral is sound for Minecraft's own heaps and is what fixed vanilla 26.2, 26.1.2 and 1.21.11: there
the ordering (`LevelRenderer.render` draws at 603, uploads at 682) guarantees the bytes are not read
until the next frame, and the only drain needed is the one at the present, where the fence is already
submitted and the poll is genuinely free.

It cannot be made sound for Sodium's arena from inside this layer, because of C3. The honest options
for that one configuration are to exclude it from the deferral and accept the original race — which
by the user's own comparison is *less* bad than the current state, and is what upstream ships
(`#432` `Not planned`, PSA `#313`) — or to stop trying to fix it in the translation layer.

`is_buffer_copy_destination()` is the discriminator to use if excluding: sticky per buffer, set from
both copy entry points, and measured **0 of 12,776** direct writes on a session without Sodium, while
structurally guaranteed to be set for a Sodium arena because every non-overflow upload is a ring→arena
copy. The one thing to re-verify on device before trusting it is `direct_dest_copy_target == 0`
together with `deferred_enqueued > 0` on vanilla 26.2 in the same session.

### Bookkeeping defect found in passing, not to be bundled

`gl/buffer.cpp glBufferData` calls `set_buffer_data_size(find_bound_buffer(target), size)`, but
`find_bound_buffer` expects a `*_BINDING` enum. `GL_ARRAY_BUFFER` (0x8892) is not
`GL_ARRAY_BUFFER_BINDING` (0x8894), so it falls to `default: target = 0` and the size is recorded
against buffer id 0. Consequence: for any store created through `glBufferData`, `get_buffer_data_size`
returns 0 and `glNamedBufferSubData` pays the driver `glGetBufferParameteri64v` on every call — the
exact round trip the OHOS block was added to remove. Minecraft's terrain heaps come through
`glBufferStorage`, whose OHOS block uses `get_binding_query` correctly, so they are unaffected; the
impact is limited to mutable stores. Touches all three working versions, so it wants its own commit
and its own device round.

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

### Sharper statement, 2026-08-05: the test reads the size of the store, not the size of the upload ★

The section above says size was standing in for draw frequency. It is worse than that, and the
precise shape of it went unnoticed for several rounds of device work, so it is worth writing out.

```c
GLES.glGetBufferParameteri64v(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &bufSize);
if (data && bufSize >= DIRECT_MAP_MIN_SIZE) { ... UNSYNCHRONIZED map ... }
```

`bufSize` is `GL_BUFFER_SIZE` of the whole buffer object. `size` — the length of *this* upload — is
never compared against anything. So a two-kilobyte write into a 256 MiB arena takes the
unsynchronized path on exactly the same terms as a 128 MiB one. Read as a policy it says "once a
buffer is big, every write into it forever is unsynchronized", which is not what the safety argument
above claims for it.

Measured on device, with `diagnostics` on and Sodium loaded: `named_max_buffer` = **256 MiB** with
`direct_hits` > 0. The terrain arena is being written through an unsynchronized mapping.

Two premises that had to be corrected to get here, both of which had been keeping Sodium out of
suspicion for this function:

- **Sodium does reach `glNamedBufferSubData`.** It normally moves bytes with `glCopyBufferSubData`
  out of a persistently mapped ring, which is why it looked exempt. But `GlBufferArena` falls back
  to `writeToBuffer` → `glNamedBufferSubData` whenever an upload does not fit the ring's remaining
  space, and Minecraft's own uploader uses this entry point unconditionally.
- **`GlBufferArena.free()` merges neighbouring free segments immediately, with no fence and no
  frame quarantine.** A segment can therefore be freed, reallocated and written in the same frame it
  was drawn from. The frame fence in `eglSwapBuffers` bounds only the cross-frame case, so it does
  nothing here.

Put together, this explains the observed corruption without needing anything from the draw path.
An unsynchronized `memcpy` overwrites bytes a submitted draw is still reading; the section that was
reading them renders another section's complete vertex block. Because Sodium bakes the section index
into every vertex (`16.0 * unpack(a_LightAndData.w)`, added to the per-region `u_RegionOffset`), the
displaced data carries its own position with it — so the result is a whole, correctly textured chunk
sitting at the wrong section's coordinates, which is exactly the reported symptom rather than
torn triangles or garbage.

The threshold is now runtime-tunable rather than a compile-time constant, so the hypothesis can be
tested on the device without a rebuild: `MG_DIRECT_MAP_MIN_MIB`, read once, **negative disables the
direct write entirely**, absent keeps the historical 16 MiB so default behaviour is unchanged. AMCL
drives it from the `directMapMinMibOverride` key in the MG config.

### Ruled out on device: the multidraw translation backend ★do not re-open

Before the store-size finding, the leading hypothesis was that the corruption came from MobileGlues'
`glMultiDrawElementsBaseVertex` translation, since Sodium's terrain goes entirely through it
(`GLDrawBatch.draw` → `GlRenderPass.multiDrawIndexed` → `GlCommandEncoder.executeDraws`, up to 1793
sub-draws per region pass, `GL_TRIANGLES` / `GL_UNSIGNED_INT`). The reasoning was sound: for opaque
passes `storesIndices == false`, so `firstIndex` is 0 for every sub-draw and the only things
distinguishing them are `count` and `baseVertex` — so a wrong `baseVertex` produces displaced whole
chunks, the exact symptom.

It was tested by forcing `multidrawMode = 2` (`PreferBaseVertex`), the literal spec expansion that
emits one `glDrawElementsBaseVertex(mode, counts[i], type, indices[i], basevertex[i])` per sub-draw
and uses no intermediate buffer at all. Device log confirmed the path:
`multidrawMode = Unroll` → `PreferBaseVertex` → `-> BaseVertex (OK)`. **Terrain was still wrong.**
Since that backend has no shared state left to corrupt, the multidraw translation is exonerated;
so is the shared indirect command buffer used by mode 3.

Also settled while looking: there is no `gl_DrawID`, no `baseInstance` and no instancing anywhere in
Sodium's terrain shaders — a grep of all six shader files found zero such built-ins — so no theory
resting on draw-index plumbing survives either.

The first attempt at this experiment produced no signal at all, for a reason worth remembering:
`multidrawMode` was written straight into the device `config.json`, and AMCL's own
`entry/src/main/cpp/platform/mg_config.cpp` **rewrites that file from a template on every start**, so
the key was erased before MobileGlues read it. A whole device round was spent measuring the default
configuration while believing otherwise. Overrides now go through dedicated `*Override` keys that the
template preserves, and `.tmp-devlogs/set-mg-config.ps1` reads the file back after launch. A second
instance of the same class of error was found in the same code on 2026-08-05: `readIntKey` parsed a
single digit and rejected a leading `-`, so `"directMapMinMibOverride": -1` would have silently read
back as the fallback 16 and the "disabled" experiment would have run the unchanged threshold.
**Whenever a device experiment is configured, prove from the device which configuration ran.**

### The gate is a dead end at this granularity, measured twice on device ★read before touching it

Two versions of it were built, shipped and measured. They are the two ends of one knob, and
neither end is acceptable:

| Build | What the gate did | Terrain | Frame rate (26.1.2-neoforge, Maleoon 920) |
| --- | --- | --- | --- |
| v1, `fa9d6fa`, versionCode 1000476 | Marked from `glBindBuffer`, which this layer calls itself on every upload, so it blocked nearly every large upload | Corruption and flicker **fixed** | **Collapsed**, on every version, with and without Sodium |
| v2, `bb4f560`, versionCode 1000477 | Marked only from application binds | Corruption **back** | **Normal**: steady 60, bursts to 120 |

v2's counters say why it is not a fix: `direct_attempts` fell to **4 to 12 per second** against
about 400 `named_calls`, so the gate essentially never fires and the build is behaviourally the
same as no gate. The reason is that MC attaches the vertex buffer to a vertex array object once
and afterwards only rebinds the VAO, so no per-frame bind reaches `glBindBuffer` or
`glBindVertexBuffer` and the arena is never re-marked. v1 only appeared to work because the
self-marking bug it contained was, accidentally, the thing that made it correct.

Marking from VAO reachability instead would close that hole and land straight back on v1's
behaviour: the arena would be marked every frame, every interleaved upload would take the
synchronous path, and the frame rate would collapse again. **Correctness and throughput are the
same knob at per-buffer, per-frame granularity.** Do not spend another round moving the marking
around; the granularity is the problem, not the placement.

Also record two measurement errors from that round, both of which produced confident wrong
answers:

- **A frame rate read during world load.** A 29 fps sample was taken while chunks were still
  streaming and treated as steady state; the same build measured 60 fps a few minutes later.
  Always confirm the scene is settled, and prefer several spaced samples of the present interval
  from `hidumper -s RenderService -a 'ScreenNode fps'` over a single reading.
- **Treating `named_total_us` as MobileGlues' share of the frame.** It measures CPU time inside
  the call. A synchronous write into coherent storage costs a GPU serialization that is charged
  later, to the fence wait in `glClear`, so summing the instrumented counters and concluding
  "this layer is only 28 % of wall time, therefore it cannot explain the frame rate" is invalid.
  It was used to argue the gate was innocent, and the device A/B above says the opposite.

### How v1 failed in detail: it marked from our own bind

The analysis above is still correct. The implementation was not, and the way it failed is worth
more than the code was.

`glNamedBufferSubData` does not receive a binding, it establishes one, by calling
`temporarilyBindBuffer`, which calls **this layer's own** `glBindBuffer(GL_ARRAY_BUFFER, ...)`.
The marking hook was placed in exactly that function, so every upload marked its own target before
the gate read the mark:

```text
temporarilyBindBuffer(buffer)                       -> glBindBuffer -> mark_vertex_source(buffer)
mg_buffer_used_as_vertex_source_this_frame(buffer)  -> GL_TRUE, set two lines earlier
```

The mark is supposed to mean "the application made this buffer reachable as a vertex source", but
`glBindBuffer` is both the public entry point and the internal mechanism and cannot tell the two
apart. The early-out in `temporarilyBindBuffer` does not save it either: that only skips the bind
when the buffer is already the current `GL_ARRAY_BUFFER` binding, which it is not, because the
previous upload restored the prior binding.

So the direct write was off from the first upload onward, every large upload fell back to a
synchronous `glBufferSubData` into `COHERENT | PERSISTENT` storage at about 4604 us a call, and MC
26.1.2 and 26.2 both became unplayable, with and without Sodium. The corruption did disappear,
because the unsynchronized write stopped happening at all.

Two requirements for the next attempt:

- **Separate the internal bind from the public one.** An internal helper must update the shadow
  binding state without marking, and `temporarilyBindBuffer` and every other internal rebind must
  use it. Until that separation exists, any hook in `glBindBuffer` fires on our own traffic.
- **Do not accept `direct_hits` falling to zero as evidence the gate works.** It reads identically
  to "the gate never lets anything through", which is what actually happened. The device check has
  to assert a frame rate and `direct_hits > 0` on vanilla, not only the Sodium numbers.

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
