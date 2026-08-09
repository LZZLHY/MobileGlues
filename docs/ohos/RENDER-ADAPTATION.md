# Adapting MobileGlues to Minecraft 26.x and Sodium on a native HarmonyOS GLES driver

Written 2026-08-06. This document replaces `PERF-MALEOON.md` and `DESIGN.md`, both of which are
archived under `docs/ohos/archive/` and **must not be quoted as authority**. They contain correct
measurements mixed with conclusions that later turned out to be wrong, and several of the wrong ones
were load-bearing for months. Anything from them that is still true is restated here from first
principles. If you need a number that is not here, re-measure it rather than lifting it.

---

## 0. Position

Four statements that govern everything below. They are not motivational; each one has changed a
technical decision.

**0.1 MobileGlues decides both frame rate and image correctness. It is not a thin passthrough.**
This was doubted once, on the strength of a whole-process CPU profile showing the library at 0.65 % of
cycles. That figure was real and the inference from it was wrong: it had been measured on a build whose
upload path did almost nothing because it was skipping synchronization. On a same-device, same-scene
A/B where *only* this library differed, presents per second went 54.1 → 36.8 and the library's own
upload cost went 28.7 ms/s → 136.4 ms/s. A translation layer that answers one GL call with a
differently-shaped operation can move the frame rate by a third. Treat it accordingly.

**0.2 Upstream not supporting a configuration is a statement about upstream's scope, not about
feasibility.** MobileGlues upstream marks the Sodium-on-26.2 corruption `Not planned` and its PSA
declares Sodium unsupported. That is a legitimate decision for a project with different priorities. It
is not evidence that the problem is unsolvable, and it must never again appear in this repository as a
reason to stop. We maintain this fork precisely to do the work upstream is not doing. The goal is a
translation layer that is genuinely correct on HarmonyOS, not a stack of patches that make the common
cases look acceptable.

**0.3 Every change must come out of reading source — all four sources.** Not one, not three:
- **Sodium** — the jarjar that actually ships, not the outer mod jar.
- **MobileGlues** — this tree.
- **Minecraft / NeoForge** — the deobfuscated patched client, because the vanilla renderer and the
  mixin surface both matter.
- **Official documentation** — NeoForge's docs for the rendering and mod-loading contracts, Sodium's
  own documentation and issue history for its intended invariants.

A change is not permitted to rest on an assumption about what any of the three parties does. Where the
code cannot be read, say so and stop. This rule exists because every failed round in this area began
with a plausible mechanism instead of a read call graph, and the two most expensive errors were both
assumptions about ordering that a single `javap` run would have refuted.

**0.4 You must understand the three-way interaction before touching it, not just the piece you are
editing.** The bug that survived longest was invisible from inside this library: the same GL buffer
object being handed from one Sodium arena to another with no GL call at all. No amount of care inside
`gl/buffer.cpp` would have found it. Read what the application does with the objects it hands us, and
read what the mod does to the application.

---

## 1. The three-way system

### 1.1 What Minecraft 26.x does

26.x replaced the old renderer with a Blaze3D command-encoder architecture. The parts that matter to a
translation layer:

- **One submit per presented frame.** `Minecraft.renderFrame` calls
  `CommandEncoder.submit()`, which inserts a fence, advances a submit index, and then blocks in
  `awaitSubmit(index - 2, Long.MAX_VALUE)`. `MAX_SUBMITS_IN_FLIGHT = 2`, so the render thread is
  pinned to at most two frames ahead of the GPU. There is no equivalent mechanism in 26.1.2 — that is
  the real architectural difference between the two versions, not the buffer layout.
- **Terrain lives in a small number of very large immutable stores.** Three vertex heaps of 128 MiB
  (one per `ChunkSectionLayer`) and one index heap of 32 MiB for the translucent layer. The
  solid/cutout index heaps are declared but never allocated, because those layers use the shared
  auto-generated quad index buffer.
- **Those stores are declared `GL_DYNAMIC_STORAGE_BIT` and nothing else**, and the application never
  maps them. `GlConst.bufferUsageToGlFlag` never produces `GL_MAP_COHERENT_BIT` for any buffer.
- **Terrain reaches us as `glNamedBufferSubData`.** The staging path selected on this device is a plain
  host allocation (`StagingBuffer$Cpu`), because the alternative is gated on a Windows/D3D12 heuristic
  that is false here. So the bytes arrive as an ordinary ordered sub-data call.
- **Where Minecraft does take on synchronization duty, it says so.** Its own mappable buffers are
  created with mapping flags `WRITE | FLUSH_EXPLICIT | UNSYNCHRONIZED | PERSISTENT`, and it backs that
  with three-slot fenced ring buffers and with recycling deferred behind `submit()`. The contrast
  matters: on the buffers where it does *not* set those bits, it is relying on the driver to order the
  write.

### 1.2 What Sodium replaces

Sodium does not replace `LevelRenderer.render`; it wraps and hollows it. It overwrites the vanilla
draw-list builder to return an empty map, injects its own terrain pass, and drives its own section
manager from a mixin on the extract phase. Consequences:

- Minecraft's own terrain heaps are not used at all when Sodium is loaded. Its dispatcher is nullified.
- Sodium keeps its own arenas, its own allocator, its own staging ring, and its own defragmenter.
- Its terrain draws are `glMultiDrawElementsBaseVertex` with up to ~1800 sub-draws per call.
- Its uploads are normally a copy from a persistently mapped ring into the arena, and fall back to
  `glNamedBufferSubData` when the ring's remaining space cannot hold an upload.

### 1.3 What this library must provide, and where the friction is

The friction is not feature coverage. It is that this driver makes one specific thing catastrophically
expensive, and the honest ways of avoiding that expense all change the semantics of a GL call.

On a Maleoon 920, per call into a store of 16 MiB or more:

| how the bytes are written | cost per call |
| --- | --- |
| mapped write with `GL_MAP_UNSYNCHRONIZED_BIT` | **0.8 µs** |
| the same mapped write without that bit | **24,934 µs**, worst observed 104 ms |
| `glBufferSubData` | 3,946–4,604 µs |
| `glBufferSubData` after refusing to promote the store to host-coherent | 55,000 µs, peak 188 ms |
| a staging buffer plus `glCopyBufferSubData` | collapses to single-digit frame rates |

There are roughly 48 such writes per second in a loaded world. So the only affordable write is the one
that performs no synchronization, and `glNamedBufferSubData` is specified to be ordered. That tension
is the whole problem. Everything in section 4 is an attempt to resolve it.

---

## 2. Established facts

Each entry says how it was established. "Read" means from bytecode via `javap -p -c -constants`;
"measured" means on the device with the counters in `diagnostics/`.

### 2.1 Frame order — vanilla (read)

Inside `LevelRenderer.render`: the draw lists are prepared, then **every terrain draw is issued**, then
translucency re-sorts are scheduled, and **only then** are the terrain buffers uploaded.

So vanilla is **draw-then-upload**, and the bytes uploaded at the end of frame N are not read until
frame N+1. This single fact is what makes the current fix work for vanilla, and it is the opposite of
what this repository assumed for months.

### 2.2 Frame order — Sodium (read)

Sodium's terrain work runs from the **extract** phase, early in `Minecraft.renderFrame`, via a mixin
that calls its world renderer's terrain setup. In order inside that setup: the translucency re-sort
trigger, then a cleanup/flip step that runs the staging-buffer flip **and the arena defragmenter**,
then the two upload steps, and finally the draw-list finalisation. The draws themselves happen later,
in the normal render call.

So Sodium is **upload-then-draw**, within one frame, and its draw lists are built *after* the uploads.
Sodium and vanilla are exactly inverted. Any design that relies on one of the two orderings must
explicitly say what it does about the other.

### 2.3 Sodium's arena lifecycle (read)

The parts that constrain us:

- **Free is immediate and coalescing.** A freed segment merges with its neighbours at once, with no
  fence and no frame quarantine, and can be re-allocated in the same pass.
- **Live segments are relocated every frame.** The defragmenter has a per-frame budget with a floor of
  32 copy commands and 32 MiB of relocated geometry. Every relocation updates a segment's offset **and**
  issues a GL copy, so the data movement is observable from here.
- **An arena can swap its entire backing buffer mid-frame.** On resize, it obtains a buffer, copies the
  live segments across, and releases the old one.
- **The released buffer is pooled, not deleted — BUT THIS IS DEAD CODE IN THE SHIPPING BUILD.**
  The pool exists exactly as described: `ArenaAggregator.releaseBufferForReuse` does not `close()`,
  and `getBufferOfSizeAtLeast` hands an existing buffer object to a *different* arena with no GL call
  at all. What was missed is that `releaseBufferForReuse`'s only caller is
  `SingleOwnerBufferArena.transferSegments`, and `SingleOwnerBufferArena` is only constructed in
  `ArenaAggregator.createDedicatedArena`, which has **zero call sites** across all 732 classes of the
  shipping jarjar. So the pool is never populated, every arena buffer is freshly created, and the
  only way one is retired is `SharedBufferArena.deleteShared` → `GpuBuffer.close()` →
  `GlStateManager._glDeleteBuffers` → `glDeleteBuffers`, issued immediately with no deferred-destruction
  queue anywhere in `GlDevice` or `GlCommandEncoder`.
  **A delete is something this library sees, and already guards.** The invisible-reassignment
  mechanism that §3.3 used to blame for the artifact therefore cannot occur in this build.
- **What is still invisible is owner migration.** `SharedBufferArena.handleResizeUploads` evicts the
  largest owner, `transferOwnerTo` rebinds its handle, and `receiveSegmentsFrom` copies its segments
  into another arena — but `receiveSegmentsFrom` returns 0 without any copy when the segment list is
  empty, and `DataType.update` can delete an emptied arena having issued no copy at all. Those paths
  change which buffer a region's geometry lives in without a copy-out.
- **Enqueue-then-relocate is the normal path, not a corner case.** The upload routine performs the
  uploads and the resize handling in the same call.

### 2.4 Sodium's upload routing (read)

**Precondition that the rest of this section depends on, and that was missing here for months:** the
mapped staging ring exists *only if* `DeviceInfo.features().persistentMapping()` is true.
`RenderRegionManager.createStagingBuffer` builds a `MojangStagingBuffer(32000000)`, whose constructor
allocates a `MappedStagingBuffer` when that flag is true and otherwise sets `staging = null`. With
`staging == null`, `enqueueCopy` sends **every** upload straight to
`CommandEncoder.writeToBuffer` — there is no ring, no fence, no staging→arena copy, and none of the
paragraph below applies.

Measured on device 2026-08-07: **the flag is false on this device.** Two independent confirmations, in
one session. First, `UniformBufferManager.writeMeshTimes` took its `writeToBuffer` branch, which is
guarded on a field that `UniformBufferManager.<init>` only leaves null in the `else` of
`if (persistentMapping())`. Second, and decisively, a `MappedStagingBuffer` would allocate and
persistently map a **32,000,000-byte** store, and across 763 recorded mappings of stores at or above
64 KiB in that session there is no store of that size at all. Observed lengths were 262,144 (685×),
524,288 (28×), 710,178 (3×), 25,165,824, 401,640, 274,432, 229,376 and 65,536.

When the flag *is* true, the routing is as follows. The upload goes to the staging buffer's enqueue
method; if the requested size exceeds the ring's remaining space it writes straight through with a
sub-data call, otherwise it writes into the mapped ring and queues a copy command issued later by
`flush()`. The remaining space is replenished once per frame, in `flip()`, called once from
`RenderRegionManager.update`. So within a frame the ring drains monotonically. Correction to the old
wording: the test is a per-call `len > remaining`, not a sticky flag — with 100 bytes remaining a
200-byte upload becomes a sub-data call *without consuming remaining*, and a following 50-byte upload
still takes the ring. Ring pressure rises with the chunk-build rate, which rises with movement.

### 2.5 The objective frame-rate metric (measured)

The per-present fence in `egl/egl.cpp` fires exactly once per `eglSwapBuffers`, so
`frame_wait_calls` per second **is** presents per second. Use it. Do not ask a human for a frame-rate
number that a counter can give, and do not read a frame rate while chunks are still streaming.

Smoothness is different: it is frame *pacing*, and the counters do not show it. That still needs a
human, and it must be asked about separately from frame rate, because a build can be lower-average and
smoother, or higher-average and janky, and conflating the two has produced wrong conclusions here more
than once.

### 2.6 What `GL_SYNC_FLUSH_COMMANDS_BIT` costs on this driver (measured)

Honouring it makes the driver end the render pass currently being built in order to submit: **3.77 ms
per call**. A fence created and waited on within the same frame, with no present in between, therefore
cannot be waited on cheaply — the zero-timeout poll cannot succeed, because a sync object can only be
signalled once its fence command has *completed*, and an unsubmitted command has not. A fence created
before the last present is a different matter: the present submitted it, so the poll succeeds and
costs nothing.

**These two situations look identical in code and differ by three orders of magnitude in cost.** Any
fence wait added anywhere must state which of the two it is.

---

## 3. Current state

### 3.1 What works, and is the acceptance bar

At the build in the device's hands (`versionCode 1000492`, MG `3de6ef7`):

| configuration | state |
| --- | --- |
| vanilla 26.2 | clean — no flicker, no stutter, frame rate preserved |
| 26.1.2 | clean |
| 1.21.11 | clean |

**These three have not been re-verified since the 2026-08-07 changes** (the deferred-drain rework, the
redundant-write elision, and the probes). Every one of those changes is confined behind
`MG_PLATFORM_OHOS`, and the drain rework touches only a path that measures 0 on these versions, but
"should not affect them" is not the acceptance bar — running them is. Do this before any release.

**These three are the acceptance bar for every future change.** A change that fixes Sodium and
regresses any of them is a failure, not a trade. Verify with `frame_wait_calls` per second and with a
human on pacing, on all three, before shipping anything.

Note that 26.1.2 shares 26.2's buffer architecture — same heap sizes, same host staging, same sub-data
route. What it lacks is the per-frame submit fence. The old claim that "older versions do not reach
this path" is true only of 1.21.x, where the largest buffer we see is tens of kilobytes.

### 3.2 What does not work

26.2 with Sodium. Two symptoms:

- **Regions render as void while the geometry that belonged there appears elsewhere.** Not
  chunk-sized; a Sodium render region is 8×4×8 sections, which matches the observed granularity.
  Unchanged by everything done on 2026-08-07, and its mechanism is **not** currently understood — see
  the second withdrawal in 3.3.
- **Frame rate drops sharply on crossing a chunk boundary**, and only then. The earlier description
  ("falls from ~80 to the low teens whenever the camera or the player moves") was too coarse and sent
  two rounds of work in the wrong direction. Corrected description: standing still is fine; so is
  moving at any speed *within* one section; the drop is a discrete event per section boundary crossed,
  so its apparent frequency scales with speed and reads as continuous under elytra flight. Measured
  100 → 20-30 on the user's device. Mechanism established, see 3.3.

### 3.3 The mechanism of each, as far as it is currently understood

**WITHDRAWN — the mid-frame-flush explanation of the frame collapse.** This section used to say the
collapse was a fence wait at the multi-draw drain paying `GL_SYNC_FLUSH_COMMANDS_BIT` once per moving
frame. The reasoning was sound and the premise was false: instrumented on device 2026-08-07,
`deferred_midframe_flush`, `deferred_forced_flush` and `deferred_ordered_replay` all read **0 across a
124-second session**. The mid-frame drain never fires. Whatever the queue does, it is not the cost.
Do not reinstate this explanation without a counter showing the drain actually running.

> **REASON CORRECTED by 6.11** — the counters are accurate but the inference from them was not. The
> drain does not fire because it is **unreachable**, not because its range test rarely passes:
> `glx/lookup.cpp` rewrites the multi-draw name at `getProcAddress` time, so the application never enters
> the dispatcher the drain lives in. The conclusion "the queue is not the cost" survives; the conclusion
> "the range test almost never fires" is withdrawn. The drain being unreachable is an open defect.

**WITHDRAWN — the buffer-identity reassignment explanation of the artifact.** Per the correction in
2.3 the pool that made a buffer name move between arenas is dead code in the shipping build, so the
mechanism cannot occur. What survives from the old text is only the shape argument: a Sodium render
region is 8×4×8 sections (`REGION_WIDTH/HEIGHT/LENGTH`, `REGION_SIZE = 256`), which matches the observed
granularity.

**SUPERSEDED as of the 2026-08-07 evening round — there is now a candidate mechanism, see 6.10.** A
deleted buffer stayed named in this library's binding shadow, after which a restore path handed the fake
id to the driver as a driver name and a LIFO-recycled id could make `temporarilyBindBuffer` skip its
rebind, sending a write into the wrong buffer. The chain is established in source at every link and the
defect is fixed, but "fixed a real defect" and "explains the artifact" are separate claims and only the
first is settled. The shape argument above is what the candidate has to account for, and it does: an
arena buffer's lifetime *is* a region's lifetime.

**The correctness hole in the deferred wait was real and is fixed.** The wait used to record its
result and then ignore it, so on timeout the unsynchronized write proceeded against draws still in
flight. `deferred_wait_for_fence` now reports whether the wait actually proved anything, and
`deferred_flush` downgrades to an ordered write when it did not.

**What the frame collapse actually is (established 2026-08-07, four device rounds).** It is a
write-after-read stall on one specific store, and it has nothing to do with the deferred queue.

- Trigger is **crossing a chunk/section boundary**, not movement. Walking at 4.317 blocks/s gives one
  stutter every 16/4.317 = 3.7 s; sprinting ~7 blocks/s gives 2.3 s; elytra flight gives under 0.5 s
  and reads as continuous. Inside one section, at any speed, there is none. This was the user's
  observation and it is what turned a diffuse "slow while moving" into a discrete event.
- The store is **`UniformBufferManager.sectionTimeInfo`**, the `u_SectionTimeInfo` texel buffer.
  Identified by three independent properties, not by one coincidence: creation flags `0x142`
  (`MAP_WRITE | PERSISTENT | DYNAMIC_STORAGE`, i.e. a usage mask containing `MAP_WRITE` and
  `COPY_DST`); size **229,376** = 224 × `TIME_BUFFER_SIZE_PER_REGION`(1024), which is exactly
  `UniformBufferManager.<init>`'s `regionsX * regionsY * regionsX * 2 * 1024` at this render distance;
  and the arithmetic exclusion of the only two other `0x142` candidates — Sodium's staging buffer is a
  hard-coded 32,000,000 bytes, and the shared quad index buffer is `primitives × 24`, of which 229,376
  is not a multiple.
- The writes are `writeMeshTimes(regionId, sectionIndex, value)` at byte offset
  `(regionId * 256 + sectionIndex) * 4`, four bytes each. `LocalSectionIndex.pack` is
  `(x&7)<<5 | (z&7)<<2 | (y&3)`, giving byte strides of 4 for Y, 16 for Z and 128 for X — which is why
  the measured offsets are 4-byte aligned but not 16-byte aligned.
- Why it fires at a boundary: the write is guarded on `relativeBuiltTime != -1`, which is only set when
  `RenderSection.consumeFade()` returns true, and that field is set true in the section's constructor
  and false by the one call. **Each RenderSection is written exactly once in its life.** Standing still
  creates no sections; crossing a boundary creates a whole column.
- Cost, measured: 3,196 calls in 127 s costing **4,538 ms**, worst single call **119,689 µs**, mean
  write 8.2 bytes. **94.4 % of all sub-threshold upload cost.** A four-byte write blocking 119 ms is
  the whole stutter. Earlier session, before dedup: 6,374 calls, 6,846 ms, worst 115,972 µs.
- The stall is a write-after-read hazard, not a transfer: the previous frame's terrain draws are still
  reading this buffer when extract writes it, and the per-present frame fence only bounds the GPU to
  two frames behind. Size does not enter into it.
- Ruled out by the same measurements: the frame fence (`frame_wait_timeout` = 0 across sessions, worst
  wait 3.6 ms), Sodium's arena copies (45 ms/s in low-frame windows against 124 ms/s for this path),
  and the deferred queue (see the first withdrawal above).

### 3.4 Why this is a hole in one design, not a proof of impossibility

I previously wrote that deferral "cannot be made sound from inside this layer". That was an
overstatement and it is withdrawn. What is actually true is narrower:

- The *invisible* transition is buffer-object **identity** reassignment.
- The *visible* things are: every offset change (always paired with a GL copy), every arena deletion,
  and every copy in or out of an arena.
- A record only becomes dangerous if it **survives across the transition**.

So the question is not "can we observe the reassignment" — we cannot — but "can we guarantee no record
survives long enough to meet one". That is a different question and it has not been answered. One
concrete unexplored lead is in 6.1.

---

## 4. What has been tried, and what each attempt actually established

Written as axes rather than as a list of patches, because the useful content is which dimensions are
now closed.

**Axis: the size threshold's justification.** `DIRECT_MAP_MIN_SIZE = 16 MiB` excludes small stores
from the fast path, and the comment justifying it says their "upload volume was never the bottleneck".
The volume claim is true and the conclusion is wrong. Measured 2026-08-07: the sub-threshold branch
took 17,133 of 25,339 `glNamedBufferSubData` calls, moved **715 KB in total**, and cost **6,857 ms**
with a worst single call of 116,707 µs. The threshold was chosen on throughput and the cost is
per-call stalls. **Closed: do not justify a threshold with byte counts again.** Note also that simply
lowering it is not the fix — the same comment records that small stores are read again later in the
same frame, which is why they were excluded, and that part still stands.

**Axis: which buffers take the fast path.** Gating the unsynchronized write on whether a buffer had
been bound as a vertex source was built twice. The first version marked from this library's own
internal bind and therefore blocked everything, collapsing the frame rate on all versions. The second
marked only application binds and essentially never fired. Both are now explained structurally rather
than empirically: because vanilla uploads *after* it draws (2.1), a per-frame mark is set before every
upload unconditionally, so there is no safe subset to keep fast. **This axis is closed. Do not
re-open per-buffer or per-frame gating.**

**Axis: where the bytes go.** A staging ring feeding `glCopyBufferSubData` was built properly — one
persistently mapped store, segment rotation, fence-based retirement, no per-call allocation. It made
the image correct and did not recover the frame rate: the copy itself measured ~1,175 µs per call,
worst 78 ms. The lesson worth keeping is methodological: the belief that a GPU copy "only enqueues" came
from dividing the application's copy *bytes* by *time*, which measures throughput and says nothing
about per-call latency. **This axis is closed.**

**Axis: the destination's storage class.** Refusing to promote the terrain stores to host-coherent
leaves them device-local and unmappable, and the sub-data cost rises to 55–188 ms per call. The cost
tracks the destination, not the byte count. **Closed.**

**Axis: the access bits.** Dropping `GL_MAP_UNSYNCHRONIZED_BIT` and letting the driver perform the
write-after-read wait costs 24,934 µs per call — six times worse than not mapping at all. The mapped
write is fast *only* because it is unsynchronized. **Closed, and this is the most important single
measurement in this document.**

**Axis: the draw-side translation.** Forcing the multi-draw backend to the literal per-sub-draw
expansion, which uses no intermediate buffer at all, left the corruption unchanged. The multi-draw
translation and its shared indirect command buffer are both exonerated. Also settled: Sodium's terrain
shaders contain no draw-index or instancing built-ins, so nothing resting on draw-index plumbing
survives either. **Closed.**

**Axis: when the write happens.** The current attempt, and the only axis that has produced a real
result: vanilla 26.2, 26.1.2 and 1.21.11 all went clean. It is incomplete for Sodium for the reasons in
3.3, and two of the three reasons are defects in the implementation rather than in the idea. **This
axis is open and is where the next work belongs.**

**Axis: eliminating redundant writes.** `RenderRegionManager.uploadResults` creates one
`PendingSectionMeshUpload` per terrain pass that has a mesh — up to `DefaultTerrainRenderPasses.ALL
.length`, which is 3 — all carrying the same `relativeBuiltTime`, so one section produces up to three
byte-identical writes to the same offset, adjacent because the pending list is built section by
section. Skipping a write whose bytes are already at that offset is semantically a no-op, and was
implemented with per-buffer shadow invalidation on respecify, map, delete, copy-in and unrecorded
sub-data. It works: 2,585 of 23,563 calls elided, and buffer 64's call count roughly halved.
**It bought no frame rate.** Same-session comparison before and after: average 55.0 → 54.4, windows
below 60 fps 51 % → 58 %, per-second cost 47.8 → 46.9 ms/s. 19 % of the calls removed, 0 % of the cost,
because the cost lives in ~1 % of calls that are slow, and the duplicates are the cheap ones — the
first write has already paid the stall. **Keep the change (it is free and correct) but do not count it
as a fix.** This is the same error shape as the closed "volume" reasoning below, on a different axis:
call count is not cost.

**Non-axis findings worth not rediscovering.** FSR upscaling made everything worse and did not move the
GPU-behind ratio at all, so the bottleneck is not fill rate. The per-present fence itself is nearly
free and already-signalled. The layer's own indirect-command-buffer preparation was measured and is
under 1 % of wall time. Shader translation is cached and does no per-frame work; shader compilation is
a loading-phase cost.

---

## 5. Rules for future work

**5.1 Read before writing. All four sources.** Section 0.3. If a claim about Minecraft, Sodium or
NeoForge appears in a commit message, the commit must be able to point at the code it came from.

**5.2 State which fence you mean.** Every fence wait must say in a comment whether the fence it waits
on was created before the last present or within the current frame, and therefore whether it is
expected to cost nothing or 3.77 ms plus a pipeline drain. See 2.6.

**5.3 Never extrapolate a measurement across a change of site or configuration.** The two errors that
cost the most in this area were both of this shape: a CPU profile taken on a build that skipped
synchronization, used to argue the layer could not matter; and a fence-poll success rate measured at
the present, used to justify a mid-frame poll.

**5.4 Change one variable per device round, and prove which one ran.** Configuration has silently not
applied twice here — once because the config file was rewritten from a template on every start, once
because the parser accepted only a single digit and rejected a minus sign. Both wasted a full round and
both produced confident wrong conclusions. Read the configuration back from the device after launch and
put the effective values in a log line.

**5.5 Instrument before optimising, and check the instrument is not the cost.** One probe that sampled
an expensive alternative on one in eight calls added ~150 ms/s and distorted the frame rate it was
measuring.

**5.6 Judge on three axes, separately: image, frame rate, pacing.** And record all three in any tag or
commit that claims an improvement. A tag that records only "image correct" is how a negative
optimisation once looked like a fix.

**5.7 Keep the platform confined.** Everything platform-specific belongs behind the platform macro, and
the isolation gate must report zero differences against the generic baseline. This is what makes it
possible to reason about, and eventually upstream, the parts that are genuinely general fixes.

**5.8 Preserve rollback at all times.** Tag before building anything that goes to a device, push the
tag, and put the measured image/frame-rate/pacing result in the annotation.

---

## 6. Open questions for the next investigation

Each is phrased so that reading source answers it. None is a plan; do not implement any of them before
the reading is done.

**Status after the 2026-08-07 rounds.** 6.1, 6.2, 6.3, 6.5 and 6.7 are answered below and kept for the
reasoning. 6.4 and 6.6 are still open. One new question is added as 6.8, and it is the important one.

**6.1 ANSWERED — the question was wrong.** Can a deferred record be guaranteed not to survive a
buffer-identity reassignment? The reassignment does not happen: the pool is dead code (see 2.3), and
the real retirement is an immediate `glDeleteBuffers` this library already guards. The mid-frame drain
that the question presupposed also never fires (see 3.3). The residual risk is owner *migration*, which
does copy out except when the segment list is empty. Kept for the record; not on the critical path.

**6.2 ANSWERED.** Where does Sodium's upload phase end in terms of a GL call we can see? The
GL-visible calls between the last upload step and the terrain multi-draw, in order, are:
`SharedQuadIndexBuffer.ensureCapacity` (may `close()` + `createBuffer` + `map` + fill),
`CommandEncoder.createRenderPass`, `RenderPass.setPipeline`, `GlStateManager._glUseProgram`,
`GL46C.glGetUniformLocation` ×3, `setIndexBuffer`, `setUniform("u_Globals")`,
`setUniform("u_SectionTimeInfo")`, `bindTexture` ×2, then per region `setVertexBuffer`,
`glUniform3f`/`glUniform1i`/`glUniform1ui`, and finally `multiDrawIndexed`. Any of these is a cheaper
drain point than the multi-draw. Moot for the queue, since the drain never fires, but load-bearing for
6.8: the read of `u_SectionTimeInfo` is the `setUniform` at `DefaultChunkRenderer.render` offset 318,
and the draw that actually reads it is the `multiDrawIndexed` after it.

**6.3 ANSWERED, negatively.** Nothing submits between Sodium's upload phase and its terrain draw.
`Minecraft.renderFrame` calls `GameRenderer.extract` at offset 440 and `GameRenderer.render` at 526,
with only `RenderSystem.executePendingTasks` and two NeoForge hooks between them, and
`CommandEncoder.submit()` has exactly one call site in the whole client — offset 702, after all
drawing. `submit()` itself is `glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0)` plus a
`glClientWaitSync(..., GL_SYNC_FLUSH_COMMANDS_BIT, Long.MAX_VALUE)` on the fence from two submits ago;
`MAX_SUBMITS_IN_FLIGHT = 2`. So a mid-frame fence is always unsubmitted, and there is no free
submission point to exploit.

**6.5 ANSWERED.** Sodium's index arena reaches the threshold by construction: `ArenaAggregator$1
.calculateArenaSize` floors the first index arena at exactly 16 MiB, the second at 32 MiB and later
ones at 64 MiB, and geometry arenas at 32/128/256 MiB. A translucency re-sort's index upload goes
through the identical staging path as a geometry upload — `RegionAllocatorHandle.upload` →
`BufferArena.upload` → `tryUpload` → `enqueueCopy` → `tryUploads` → `flush()` — with no bypass.

**6.7 ANSWERED.** The sticky "has ever been a copy destination" flag measured 6,474 hits in a Sodium
session, so it is live rather than theoretical, and it correctly reports 0 for `u_SectionTimeInfo`.
Nothing branches on it yet.

**6.1 (original text, kept for the reasoning) Can a deferred record be guaranteed not to survive a
buffer-identity reassignment?**
The reassignment is invisible, but the *events around it* may not be. Specifically: an arena is only a
copy **source** when its contents are being migrated or defragmented — its normal upload traffic reads
the staging ring and writes the arena. So "a copy read from a buffer that has pending records" may be a
rare and precise signal that the buffer is being retired. The current guard tests only range overlap on
the read side; a whole-buffer test there would be far stronger and might cost nothing, because the
signal is rare. **Read**: every path that makes an arena buffer a copy source, and whether any
migration or retirement can occur with no copy out at all (for example when the arena is already
empty). If none can, the hole closes. If one can, say precisely which.

**6.2 Where does Sodium's upload phase end, in terms of a GL call we can see?**
The previous answer was "nowhere except the first terrain draw", which is why the drain was put at the
multi-draw and why it is expensive. Re-derive this rather than trusting it. **Read**: everything
between the last upload step and the draw-list finalisation, and the first GL call the draw path makes
before the terrain multi-draw — including render-pass creation, pipeline and vertex-array setup, and
uniform upload. Any of those would be a cheaper drain point than the multi-draw itself, and one of them
may sit after the arena work has finished.

**6.3 Can the drain be made cheap even mid-frame?**
The cost is the flush, not the wait, and the flush exists only because the fence may be unsubmitted.
There is a discarded idea in the archived material — comparing a present counter captured at fence
creation against its value at wait time, and only requesting the flush when no present has intervened.
**Read**: whether anything in Sodium's or Minecraft's per-frame sequence already submits between the
upload phase and the terrain draw (Minecraft's submit, any application fence with the flush bit, any
present). If something does, the drain's fence is already submitted and the flush bit can be dropped
for it — which would remove the frame collapse without removing the drain.

**6.4 Is there a correctness obligation we are currently failing that would explain residual artifacts
on its own?**
Independent of everything above: this layer strips the explicit-flush bit from mappings and then skips
the driver's flush, on the grounds that the store was promoted to coherent — but coherence is requested
on the *mapping*, and that is never done. Minecraft's own mappable buffers do request explicit flush,
so this affects the working configurations too and is **not** an inert change. **Read**: the extension
specification for what a mapping without the coherent bit guarantees, and every application mapping we
rewrite. Then decide, with a device round of its own and with the flush-call counter as the criterion.

> **RESOLVED 2026-08-07 by reading the specification — and it resolves against the worry above. There
> is no correctness obligation being failed here.** See 6.15 for the text and the consequences. In
> short: coherence is a property of the **store**, not of the mapping. EXT_buffer_storage places
> exactly one requirement on the mapping — that it be made with `MapBufferRange` — and then says that
> with the storage bit set, a client write is visible to subsequent GL commands with no further action.
> The bit in `access` is only a consistency constraint ("must include" the storage bit) and adds
> nothing. So skipping the application's flushes while the store is promoted coherent is legal, the
> 85,418 skipped calls per session are not lost writes, and this cannot be the artifact. Note that this
> conclusion depends on the promotion actually granting `COHERENT`; under promote mode 2 it does not,
> which is why every dependent site is now conditional. Four rounds cited this paragraph as a live
> hazard; it was not one.

**6.5 Does the index arena reach the size threshold, and does the translucency re-sort take a
different route?**
The artifact's dependence on view angle points at translucency sorting, whose re-sort rate tracks camera
movement by construction. **Read**: Sodium's index arena sizing, and whether a re-sort's index upload
goes through the same staging path as a geometry upload or bypasses it.

**6.6 What does the application actually require of `glNamedBufferSubData`, and is there a legal fast
implementation we have not considered?**
The call is specified as ordered, and we currently implement it as unsynchronized. Between those two
extremes the specification permits the implementation to stall, to rename the allocation, or to shadow
the write. Renaming is what a desktop driver would do and is why this is fast elsewhere. **Read**: what
this driver exposes that could stand in for renaming — buffer orphaning behaviour, whether a fresh
allocation plus a copy is cheaper than a synchronized write for the sizes involved, and whether the
sub-range nature of these writes allows anything the whole-buffer case does not.

**6.7 (original text) Verify the discriminator before relying on it.**
If any future design needs to tell a Sodium arena from a Minecraft heap, the existing sticky
"has ever been a copy destination" flag is the candidate, and it measured zero on a session without
Sodium. **Verify on device** that it reads zero on vanilla 26.2 *in the same session* in which
deferred enqueues are non-zero, before any behaviour depends on it.

**6.8 NEW, AND OPEN. Why does Blaze3D behave as though `GL_ARB_buffer_storage` were both present and
absent?** Three measurements say `DeviceFeatures.persistentMapping()` is **false**: Sodium's
`writeMeshTimes` takes the branch that only exists when it is false; no 32,000,000-byte
`MappedStagingBuffer` store exists anywhere in a session; and `GlCommandEncoder` would otherwise build a
`GlTransientMemory$PersistentMapping`. Two say it is **true**: `bufferdata_calls` measured 0 across a
session, and `u_SectionTimeInfo` was recorded as created through `glBufferStorage` (`entry=1`), both of
which imply `BufferStorage.create` returned `Immutable`, which implies `caps.GL_ARB_buffer_storage` was
true, which implies the `set.add("GL_ARB_buffer_storage")` in that same branch ran — and
`GlHeuristics.createDeviceInfo` reads the same `HashSet` afterwards (`astore 6` occurs once in
`GlDevice.<init>`; `BufferStorage.create` at 159, `createDeviceInfo` at 323). Both cannot hold.

Everything cheap has been checked and eliminated: no Sodium or NeoForge mixin touches `DeviceFeatures`,
`DeviceInfo`, `GlDevice`, `GlHeuristics`, `BufferStorage`, `RenderSystem` or `GpuDevice` (91 mixin
targets enumerated); `DeviceFeatures` is a record whose 7th constructor argument does land in
`persistentMapping` and is `set.contains("GL_ARB_buffer_storage")`; `createDeviceInfo` has exactly one
call site in the client; `RenderSystem.DEVICE` is assigned once and `shutdownRenderer` does not clear
it; `DeviceInfo.features()`, `GpuDevice.getDeviceInfo()` and `GlDevice.getDeviceInfo()` are pure
accessors; `GlHeuristics` is byte-identical between the patched and unpatched jars; Sodium's `GPULimits`
only reads the OS. **Do not write a resolution to this in this file without a citation.** It does not
block the fix in 6.9, but it means our model of that code has a hole, and something built on the model
may be wrong.

**6.9a FIRST ATTEMPT MEASURED AND CORRECTED — the cache adopted the wrong buffers.** Device result of
the build below, 174 s of MC 26.2 + Sodium: `pmap_adopted` = 3, `pmap_map_failures` = 0, 24,521 writes
costing **12.2 ms in total** (0.5 us each, worst 58 us), and `fb_calls` down from 51,919 to 4,925. So the
mapping mechanism works exactly as designed. And yet `fb_us` moved only from 12,296 ms to 11,198 ms, and
per second it got *worse* — 42.8 to 64.4 ms/s. Per destination, per second:

| destination | before | after |
| --- | --- | --- |
| 229,376 bytes, `u_SectionTimeInfo` | 32.4 calls/s | **27.1 calls/s** |
| 64 / 56 / 160-byte uniform stores | 148.5 calls/s | 1.2 calls/s |
| `pmap_writes` | — | 141 writes/s |

The 141 writes/s the cache absorbed are the 148.5 calls/s of cheap uniform writes. The one store that
carries **100.0% of all remaining sub-threshold cost** — 4,724 calls, 11,196 ms, mean 2,370 us, worst
93.8 ms, with 5.6% of calls holding 98.6% of the cost — was never adopted at all. This is the third time
on this problem that removing most of the *calls* removed none of the *cost*.

Cause, guessed at the time: `mg_pmap_drop` barred a buffer permanently the first time the application
mapped it. That was a real defect and was fixed, but it was **not** the cause — which is the point of
what follows.

**6.9b THE ACTUAL CAUSE, read from the log rather than argued.** With `PmapDest` in place, the
per-window trace of the 229,376-byte store is unambiguous. From window 14 onward it reads
`small_writes=64 adopted=0 declined=1 strikes=0 eff_flags=0x1c2`, and window 14 is the one window with
`pmap_map_failures=1`. So the store accumulated its 64 writes, the mapping was requested exactly once,
**the driver returned null**, and the buffer was barred for the remaining 42 windows. It was never
adopted in any of the three attempts. `strikes=0` also disproves the application-mapping theory
outright.

The mapping asked for `GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT`. The evidence
cited to justify that — in this document and in the commit — was "a persistent mapping of it succeeds,
`map_persistent_failures` = 0, access `0x62`". **`0x62` is `WRITE | UNSYNCHRONIZED | PERSISTENT`. There
is no coherence bit in it.** The application never requests coherence on a mapping, so no measurement
had ever established that this driver grants it; the step from "the storage has `GL_MAP_COHERENT_BIT`"
to "a mapping of it may have `GL_MAP_COHERENT_BIT`" was an assumption. And it is the *same* assumption
this document already flags as a hazard in 6.4 — that coherence is requested on the mapping while this
layer only ever sets it on storage. The warning was written here, then violated three commits later.

Two process failures worth keeping, because both are recurrences:

- **Call count is not the cost axis.** Removing 90% of the calls removed 9% of the cost, for the third
  time on this problem (threshold volume argument, dedup, and now this). Any future claim about this
  path has to be stated in ms/s per destination, never in calls.
- **A number quoted as support has to be read, not remembered.** `0x62` was in the log, in this file,
  and in the counter comment. Nobody decoded it.

**6.9c THE LADDER RESULT, AND WHAT THE SPECIFICATION LEAVES.** Device, 1,263 lines of MC 26.2 + Sodium:
`pmap_accepted_rung = 1`, `pmap_accepted_access = 0xc2`. **The driver does grant
`WRITE | PERSISTENT | COHERENT`** — so 6.9b's diagnosis was also wrong, and 6.4's worry about coherent
mappings is unfounded at least for mappings this layer takes. But `pmap_ladder_failures = 5` with
`pmap_fail_access_or = 0xf2` and `pmap_fail_last_rung = 5`: one buffer was refused on **every** rung,
and `pmap_fail_error_or = 0x502` is `GL_INVALID_OPERATION`. `PmapDest` names it — the 229,376-byte store
again, `declined=1`, `strikes=0`. So it is refused unconditionally while an identical request succeeds
elsewhere in the same session.

Two candidate explanations were eliminated by data rather than argument. `MG-DIAG-MAPDEST` for the whole
session contains no entry of length 229,376 at all (every entry is 262,144 or 402,504), so the
application never maps this store — `strikes=0` says the same thing. And texture-buffer emulation is off:
the session log reads `OpenGL ES 3.2 (320)` and `gles/loader.cpp` only emulates at `es_version <= 310`,
so `glTexBuffer` goes straight to the driver.

The specification then narrows it sharply. ES 3.2 §6.3 lists `INVALID_OPERATION` for MapBufferRange as:
length zero; buffer already mapped; neither READ nor WRITE set; READ combined with
INVALIDATE_RANGE/INVALIDATE_BUFFER/UNSYNCHRONIZED; FLUSH_EXPLICIT without WRITE. EXT_buffer_storage adds
exactly one: `INVALID_OPERATION` if any of READ / WRITE / `MAP_PERSISTENT_BIT_EXT` /
`MAP_COHERENT_BIT_EXT` is in `access` but **the same bit is not in the buffer's storage flags**. Against
the five rungs — every one sets WRITE, none sets READ, both FLUSH rungs also set WRITE, length is
229,376 — all the mask-dependent entries are eliminated. What survives is: the buffer is already mapped;
nothing is bound to the target; or its real `BUFFER_STORAGE_FLAGS` is missing a requested bit.

And there is a decisive asymmetry: **all five rungs set `GL_MAP_PERSISTENT_BIT`.** A storage missing that
one bit fails all five identically, which is precisely the observed shape. Which matters because
`mg_store_effective_flags` records the flags this layer *passed* to `glBufferStorageEXT`, and
`glBufferStorage` **reads no error after that call** — so `eff_flags = 0x1c2` has only ever proved that
MobileGlues asked, never that the driver agreed. Also worth noting, from the same extension: nothing in
ES 3.2 §8.9, EXT_texture_buffer or EXT_buffer_storage makes mapping a buffer that a texture references
illegal, so the texel-buffer suspicion has no specification basis.

Two probes now answer it without further reasoning. `PmapProbe` queries the driver at the moment of
refusal — `BUFFER_STORAGE_FLAGS`, `BUFFER_IMMUTABLE_STORAGE`, `BUFFER_MAPPED`, `BUFFER_ACCESS_FLAGS`,
`BUFFER_SIZE`, and the real `GL_ARRAY_BUFFER_BINDING` against the name this layer expected. `StorageErrors`
reads the error after every `glBufferStorageEXT`. Between them: a missing `0x40` in the real storage flags,
or `immutable = 0`, means the promotion never took; `mapped = 1` means someone else holds the mapping; a
binding mismatch means the map was aimed at the wrong object; and a non-zero `storage_ext_failures` says
so directly at creation time.

The build asks the driver for the mask too: `PMAP_LADDER` in `gl/buffer.cpp` tries coherent, coherent plus
unsynchronized, explicit-flush, explicit-flush plus unsynchronized, and finally the exact mask the
application is known to receive, recording the accepted rung and the GL error behind each refusal in
`PmapAcceptance` — which is deliberately outside `Counters` so that a once-per-session decision survives
the per-window reset. A non-coherent rung flushes each write and times it separately, so the cost of
publishing becomes a measurement rather than a prediction. If rung 5 is what gets taken, that is itself
the answer to 6.4 and must not pass unremarked.

Second cause, and the reason this took a device round to see rather than being read off the log: the
bar happened on a buffer that had never held a mapping, and `mg_pmap_drop` returned before
`record_pmap_evicted`. So `map_failures` was 0 and every `evict_*` was 0, which between them appeared to
rule out every explanation — and the gap got filled by reasoning. `PmapDest` now reports, per candidate
buffer, its size, accumulated small writes, adopted flag, declined flag, strike count and effective
flags, so each of the early returns in `mg_pmap_consider` is distinguishable from the outside.

**6.9d THE CAUSE, AND 6.8 RESOLVED. The application already holds the one mapping the buffer is
allowed, and Blaze3D took it in a constructor.** `PmapProbe` on device: `BUFFER_IMMUTABLE_STORAGE = 1`,
`BUFFER_STORAGE_FLAGS = 0x1c2` with PERSISTENT, COHERENT and WRITE all present, binding correct,
`BUFFER_SIZE` correct, `glBufferStorageEXT` 2,794 calls with **zero** failures — so 6.9c's "the promotion
did not take" was wrong too. What the probe actually found is `BUFFER_MAPPED = 1` with
`BUFFER_ACCESS_FLAGS = 0x62`.

`GlBuffer$Direct.<init>` explains it exactly. Offsets 82-121: when
`canPersistentMap && (usage & 3) != 0` it maps the whole buffer and then **discards the `MappedView`
with a bare `pop` at offset 121**. Nothing closes it, so `mappingRefCount` never returns to zero and the
buffer stays mapped for its entire life. `BufferStorage$Immutable.createBuffer` passes `iconst_1` for
`canPersistentMap` at offset 23, so this applies to every immutable buffer whose usage has MAP_READ or
MAP_WRITE. The access mask is built at offsets 41-64 — `usage & 2` contributes 0x32,
`canPersistentMap` contributes 0x40 — giving 0x72, which this layer reduces to 0x62 by stripping
`GL_MAP_FLUSH_EXPLICIT_BIT`. Bit for bit the value the driver reports.

**The cross-check that settles it.** `usage & 3 != 0` is equivalent to the requested GL flags containing
0x1 or 0x2, so the bytecode predicts, per buffer, whether this layer can map it:

| store | requested flags | contains 0x1/0x2 | predicted | measured |
| --- | --- | --- | --- | --- |
| 229,376 | 0x142 | yes (0x2) | Blaze3D holds it, every mask refused | `declined=1`, all five rungs failed |
| 64 / 56 / 160 | 0x100 | no | free, this layer can map it | `adopted=3`, accepted at rung 1 |

The prediction was available before the measurement and matches it exactly. That is what makes this a
cause rather than another story.

**And 6.8 was never a contradiction — it was two different switches read as one.** `canPersistentMap`
comes from `BufferStorage$Immutable` and is hard-coded true, which is why the GL side is permanently
mapped. `DeviceFeatures.persistentMapping()` comes from the capability set and was false, which is why
`UniformBufferManager.<init>` took its offset 117 `ifeq 147` branch, set `sectionTimeInfoMap = null`
(offset 149), mapped and **closed** a temporary view (offsets 158 and 179), and why `writeMeshTimes`'
single `ifnull` at offset 37 therefore always reaches the `MemoryStack.malloc(4)` plus `writeToBuffer`
path at offsets 64-114. Every observation fits at once. `maps=2 / unmaps=1` fits too: the previous
manager's `delete()` drove the refcount to zero, the driver unmapped and deleted, the GL name was reused
for the replacement store, and that store's constructor mapped it again. (Still not derivable from
bytecode: why the capability set lacked `GL_ARB_buffer_storage` when `BufferStorage.create` adds it in
the same branch that selects `Immutable`. That remains open — but it no longer blocks anything, because
if it were true there would be no sub-data calls to fix.)

**So the fix is not to take a mapping. It is to use the one that exists.** This layer produced that
pointer: `DirectStateAccess$Core.mapBufferRange` calls `glMapNamedBufferRange`, which reaches
`glMapBufferRange` here, which forwards to the driver and returns the result. `mg_appmap_record` now
remembers it, and `mg_appmap_write` satisfies a sub-threshold write with a memcpy into it. Writing there
is not a liberty: the application left `sectionTimeInfoMap` null, so it is not using that pointer, and on
a device where it does use it, what it does is `memPutInt` into the same address.

One correctness obligation comes with it, and it is 6.4 finally becoming load-bearing. The mapping is
persistent but **not** coherent, and a persistent mapping is never unmapped, so `glFlushMappedBufferRange`
is the only way to publish a write — and it requires the mapping to carry
`GL_MAP_FLUSH_EXPLICIT_BIT`, precisely the bit this layer was stripping. `glMapBufferRange` therefore now
keeps that bit **when the access also has `GL_MAP_PERSISTENT_BIT`**, and nothing else changes: the
application's own `glFlushMappedBufferRange` calls are still skipped exactly as before. Two unknowns are
measured rather than predicted — `appmap_flush_*` for what publishing costs, and `appmap_unpublished`,
which must read 0.

**6.9 BUILT 2026-08-07 evening.** Implemented as `mg_pmap_*` in `gl/buffer.cpp`,
hooked into the fallback branch of `glNamedBufferSubData`. The measurement that justified it, taken on
the build that fixed the binding shadow and therefore with the stutter untouched — 287 s, MC 26.2 +
Sodium, split by window frame rate:

| window frame rate | windows | all sub-threshold cost | of which `u_SectionTimeInfo` | share |
| --- | --- | --- | --- | --- |
| under 30 | 37 | 5,126 ms | 5,093 ms | **99.4%** |
| 30–59 | 67 | 4,682 ms | 4,583 ms | **97.9%** |
| 60–79 | 31 | 1,905 ms | 1,829 ms | 96.0% |
| 80 and above | 152 | 583 ms | 168 ms | 28.8% |

9,301 writes averaging 8 bytes, 11,673 ms total, worst single call 84 ms, worst single second 556 ms on
152 four-byte writes. The user independently reported "stable 80+ and perfectly smooth inside a chunk",
which is the same statement as the last row. That closes the attribution: one 229 KiB store is the
stutter.

Three exclusions were added to the adoption criteria on the strength of the source survey, not guessed:
a buffer ever bound to `GL_ELEMENT_ARRAY_BUFFER` is barred forever, because the base-vertex emulation in
`gl/multidraw.cpp` and `gl/drawing.cpp` maps the application's index buffer with `GL_MAP_READ_BIT` and
its failure branches `continue` — a dropped draw with no error anywhere; a buffer the application maps
is released and barred, because `GlBuffer$Direct.map` throws `IllegalStateException("Failed to map
buffer")` rather than returning null, so losing the race would be a crash; and a store above 4 MiB is
never adopted, which keeps this policy and the 16 MiB direct-write policy from both claiming a buffer.

One known gap, stated rather than papered over: "unmap everything on context destruction" has nowhere to
attach. `eglCreateContext`, `eglDestroyContext` and `eglMakeCurrent` are bare forwards in `egl/egl.cpp`
with no hook, and the buffer tables are initialised once in `gles/loader.cpp` and never reset. The
mapping is released on delete, respecification, application map and GPU copy; a context torn down
without deleting its buffers leaks the mapping along with the buffers themselves.

**6.9 (original text, kept for the reasoning) The fix to build next.** Hold a persistent
coherent mapping of `u_SectionTimeInfo` inside this library and satisfy its small writes with a memcpy,
instead of `glBufferSubData`. The preconditions are already measured, not assumed: the store's effective
flags are `0x1c2` (`MAP_WRITE | PERSISTENT | COHERENT | DYNAMIC_STORAGE`) because this library promoted
it; a persistent mapping of it succeeds (`map_persistent_failures` = 0, access `0x62`); and this is the
same thing upstream does on this buffer when `persistentMapping()` is true. **Read before writing**: the
data is a per-section fade timestamp (`R32_SINT` texel buffer, bound in
`ShaderChunkRenderer.static{}`, consumed against `Options.chunkSectionFadeInTime`), so an unsynchronized
write that races a reader costs one section a wrong fade for one frame — not geometry corruption. State
that in the commit, and verify it on the three working configurations before shipping.

**6.10 NEW, MECHANISM ESTABLISHED, FIXED, ATTRIBUTION STILL OPEN — a deleted buffer stayed named in the
binding shadow.** This replaces "the artifact's mechanism is unknown" in 2.2. It is the first candidate
with a chain that closes in source rather than a resemblance, and it was shipped as a fix because GL's
implicit unbind makes it a defect regardless of whether it explains the artifact. Those two judgements
are kept separate deliberately.

The chain, each link read rather than inferred:

1. `remove_buffer` (`gl/buffer.cpp`) cleared six per-buffer tables and left `g_bound_buffers_arr` and
   `g_element_array_buffer_per_vao` untouched. Nothing anywhere compensated: no sweep on delete, no
   `has_buffer` check before a bind, no resynchronisation from the driver, and the tables are
   initialised once in `gles/loader.cpp` and never reset — `eglCreateContext`, `eglDestroyContext` and
   `eglMakeCurrent` are bare forwards with no hook.
2. `glBindBuffer` writes the shadow *before* testing `has_buffer`, and for a deleted id takes its
   `GLES.glBindBuffer(target, buffer)` branch — handing the **fake** id to the driver as a driver name.
   Both namespaces are small sequential integers, so this usually lands on an unrelated live buffer.
   The two restore paths that do this are `restoreTemporaryBufferBinding` (18 call sites) and
   `deferred_restore_copy_write_binding`.
3. `gen_buffer` recycles from `g_free_buffer_ids` with `back()`/`pop_back()`, i.e. LIFO, so the id just
   released is the next one issued. `temporarilyBindBuffer` then sees `prev == bufferID`, **skips the
   rebind** on the shadow's word alone, and the following `glBufferSubData` writes into whatever link 2
   left bound.
4. `find_bound_buffer` answers `GL_ELEMENT_ARRAY_BUFFER_BINDING` from `get_ibo_by_vao`, so the
   element-array slot of the array is dead and the index-buffer half of this lives entirely in the
   per-VAO table — which `remove_array` also did not sweep, because it clears the slot of a deleted
   *vertex array*, not references to a deleted *buffer*.

Minecraft 26.2 makes it reachable rather than hypothetical. `GlStateManager._glDeleteBuffers` is 26
bytes — an assert, a Tracy counter, `GL33C.glDeleteBuffers` — with no unbind and no shadow of its own,
in pointed contrast to `_glBindFramebuffer` in the same class which does track `readFbo`/`writeFbo`.
`GlCommandEncoder.drawFromBuffers` (offset 5–18) binds `GL_ELEMENT_ARRAY_BUFFER` and never binds 0: the
literal `34963` occurs four times in the whole client and never with 0. `VertexArrayCache$Emulated
.setupCombinedAttributes` (offset 35–42) does the same for `GL_ARRAY_BUFFER`. Sodium issues no native GL
at all — its inner jar has zero references to `glBindBuffer`, `glDeleteBuffers` or `glGetInteger` — and
frees its arena buffers, usage 120 = `VERTEX|INDEX|COPY_SRC|COPY_DST`, through `GpuBuffer.close()` from
`releaseBufferForReuse`, `transferSegments` and `deleteSingleOwner` as regions come and go. Two index
buffers are deleted while bound by construction: `SharedQuadIndexBuffer.grow` offset 11, reached from
`DefaultChunkRenderer.render` offset 182 immediately before `setIndexBuffer` at 300, and
`RenderSystem$AutoStorageIndexBuffer.ensureStorage` offset 155.

That predicts region granularity (arena buffer lifetime is region lifetime), Sodium-only (vanilla's own
buffers are long-lived), and intermittency (the id has to collide). All three match the reported
artifact. **It is not therefore the cause.** What is measured is the *hazard*, by
`unbind_on_delete_calls` — and because the counter sits inside the fix, a non-zero value reads as "this
would have gone wrong here", never as "this went wrong". The attribution needs two more things: the
counter rising with region churn (near zero standing still, higher across chunk boundaries, highest
under elytra flight), and the image on 26.2+Sodium actually improving.

**First device result, 2026-08-07 evening, 287 s of MC 26.2 + Sodium.** The hazard is **live**:
`unbind_on_delete_calls` = 8 across 7 windows, clearing 3 target slots and **17 per-VAO element-array
slots**. So it fires, and it fires mostly on index buffers — which is the half that can misplace
geometry. The user reported the artifact as no longer visible in the same session. Two caveats keep this
short of settled:

- The rate is 8 in 287 s, about one every 36 s. That is consistent with an intermittent artifact but it
  is not obviously enough to account for how often the artifact was described. Either each occurrence
  produces a long-lived wrong binding, or something else contributes.
- Every one of the 7 windows carrying an unbind event had `u_SectionTimeInfo` cost of exactly **0 ms**.
  The two problems are disjoint in time, which fits their mechanisms: buffers are freed when regions go
  empty (moving away), while `writeMeshTimes` runs when new sections finish building (moving toward).
  This is also why the user saw the artifact change and the stutter not change in the same build — and
  it is independent evidence that these are two problems, not two symptoms of one.

Absence of a visible artifact over one session is weak evidence for an intermittent defect. It stays a
candidate until a longer session, or a session that deliberately churns regions, leaves it absent.

Nothing in the application observes the dangling value, which is what makes sweeping every vertex array
safe even though the specification only detaches from the current one: the only `glGetInteger` calls in
the client are for label lengths, timestamps and capability probes, and Blaze3D rebinds
`GL_ELEMENT_ARRAY_BUFFER` before every indexed draw.

**6.11 THE STUTTER SURVIVED THE FIX, AND TWO DEFECTS FOUND WHILE MEASURING WHY.** The
`u_SectionTimeInfo` write cost is gone and stays gone: sub-threshold upload fell from 11,198 ms to
**3 ms**, calls from 4,925 to 201, and slow calls (>1 ms) from 276 to **zero**; `named_total_us` fell
from 67.6 to 3.5 ms/s. `appmap_writes` carried 5,107 writes for 1.2 ms total at 0.24 us each, the
publishing flush cost 1.8 ms total with a worst of 14 us, and `appmap_unpublished` and
`appmap_out_of_range` both read 0. Both unknowns 6.9d listed came back favourable.

The stutter remained. Draw and present timing then ruled out the two obvious remaining suspects, on a
per-frame basis rather than per second - which matters, because a per-second figure rises with frame
count and can look like a cause when it is a consequence:

| per frame | under 30 fps (47.6 ms/frame) | 80+ fps (10 ms/frame) |
| --- | --- | --- |
| multi-draw, all three parts | 0.33 ms | 0.30 ms |
| present | 0.39 ms | 0.29 ms |
| the application's own fence wait | 9.5 ms | 1.8 ms |
| GPU copy | 1.8 ms | 0.25 ms |
| **total measured** | **≈12 ms** | ≈2.7 ms |

Draws and the present are flat across frame rates, so neither is the stutter. About **74% of a slow
frame is in code nothing here times.**

> **WITHDRAWN by 6.13** — the reading placed on this table, and on the follow-up measurement it
> motivated, was that the untimed remainder is "the application's own CPU code" and therefore out of
> reach. It is not. The largest single entry in the table is the application's own fence wait (9.5 ms of
> a 47.6 ms frame), and a fence wait is not the application computing anything - it is the GPU being
> behind. The ceiling is on the GPU side. The per-frame flatness of draws and present still stands; only
> the attribution of the remainder is wrong.

**Defect 1, and it invalidates an earlier conclusion.** `glx/lookup.cpp handle_multidraw_func_name`
rewrites `glMultiDrawElementsBaseVertex` into `mg_glMultiDrawElementsBaseVertex_<mode>` at
getProcAddress time, so LWJGL receives a pointer straight to the backend and the dispatcher in
`gl/multidraw.cpp` is **never called**. Proven by measurement: `md_calls` read 0 while the per-part
counters inside the backend were non-zero. The dispatcher is also where
`mg_deferred_upload_flush_for_draw()` lives, so **the deferred queue's draw-time drain has never run**.
`deferred_forced_flush` reading 0 was interpreted in 3.3 as "the range test almost never fires"; the
truth is the hook was unreachable. Exposure today is small - `deferred_enqueued` is about 2 per window
now that the big uploads are served elsewhere - but it is a real hole and it is recorded here rather than
fixed in the same build as a measurement.

**Defect 2, a process trap.** `MG_PLATFORM_OHOS=1` is defined only on the `glfw` target in
`MyApplication/entry/src/main/cpp/CMakeLists.txt`. The standalone ninja tree used for quick compile
checks does **not** define it, so every `#if defined(MG_PLATFORM_OHOS)` block - which is where almost all
of this work lives - was never compiled by that check. The shipped `libglfw.so` is a single target with
the macro defined consistently, so there is no ODR hazard and the measurements taken so far are valid;
but the compile check was weaker than it appeared for the whole of this investigation. **The HAP build is
the only real check.** Verify the artifact under `intermediates/stripped_native_libs/`, which is what
enters the HAP.

The next instrument follows from the table above rather than from a hypothesis: timing added to the
macros in `gles/loader.h`, which covers every one of the several hundred forwards `gl/gl_native.cpp`
generates, plus `frame_gap_ns` - present-end to next present-start - as the denominator. If
`native_ns` accounts for most of `frame_gap_ns`, the time is inside this library and `native_worst_name`
says which entry point. If it does not, the time is in the application's own code, which for a chunk
boundary means Sodium's meshing threads, and this line of investigation ends rather than acquiring
another counter.

**6.12 CLOSED AXIS: withholding the storage promotion. Refuted on device, and it is a net loss.**
Hypothesis: this library promotes every store carrying `GL_DYNAMIC_STORAGE_BIT` to
`MAP_WRITE | COHERENT | PERSISTENT`, and Sodium's arena buffers are created with usage 120
(`VERTEX|INDEX|COPY_SRC|COPY_DST`) which contains no MAP bit at all - so the application can never map
them and the promotion looked like pure cost, while putting them in uncached memory. Those same buffers
are both ends of the copies that stall.

Tested with a device-settable switch (`MG_PROMOTE_COHERENT`, driven from `promoteCoherentOverride`;
value 0 promotes only stores the application itself asked to map). The copies did get better - slow
copies 52 → 26, their total 1,063 → 507 ms, worst single 89.1 → 45.1 ms, and the worst copy's buffers
read `flags=0x100`, confirming the promotion really was withheld. Even `glClear` improved, worst
106.3 → 53.7 ms.

**And everything else got much worse:**

| | promotion on | promotion restricted |
| --- | --- | --- |
| mean frame rate | 51.2 | **39.7** |
| windows under 60 fps | 99/188 (53%) | **111/132 (84%)** |
| `fb_calls` / `fb_us` | 201 / 2 ms | **9,469 / 171 ms** |
| `appmap_writes` | 14,777 | **0** |
| `pmap_adopted` | 3 | **0** |
| `deferred_replay_fallback` | 0 | **6,973** |
| `frame_wait_max_us` | 5.5 ms | **50.3 ms** |

So the promotion is load-bearing, and the reasoning that it "buys those buffers nothing" was wrong in a
specific way worth recording: it buys nothing to the *application*, but it is what lets **this library**
map them. Without it the deferred queue's unsynchronized replay cannot map its target and falls back to
ordered `glBufferSubData` 6,973 times, the persistent-mapping cache adopts nothing, and the whole
`u_SectionTimeInfo` fix stops working - the store went straight back to 9,469 sub-data writes.

The slower copies are therefore a price the promotion charges, not a defect to remove. Do not re-open
this axis without a mechanism that keeps the mapping capability while changing the memory type, and
note that the copy stall is a *synchronization* stall in any case: the worst copy moved 47,280 bytes in
89.1 ms, which no bandwidth explanation reaches, while the overall average was 480 MB/s across 1.49 GB.

Also recorded, because the wrong metric produced a confident wrong answer: "slow-copy bytes divided by
slow-copy time" reads as a bandwidth but is not one, because the denominator is dominated by calls that
were waiting rather than transferring. Judge a copy by its own byte count and its own duration.

### 6.13 The probes came out, and the result withdrew two of this document's own conclusions

The instrumentation added to answer "where does a frame's CPU time go" (`inside_*`, `native_*`, `md_*`,
`present_*`, `frame_gap_*`) was removed once it had answered. What it left behind, and what removing it
proved, are two different things, and the second is the more important.

**Removing the probes did not raise the frame rate.** The working assumption had been that the clock
reads on every GL entry point were what dropped steady state from about 100 fps to about 50. They were
not: with them gone the session measured *lower*, not higher. So the probes are exonerated and the
frame-rate deficit has a different cause. (Recorded as an exoneration rather than a regression - the two
sessions differ in more than the probes, and nothing that was removed can change behaviour.)

Thermal throttling is also ruled out, by shape rather than by argument. Both sessions get **faster** as
they run: the no-probe session averaged 38.7 fps over its first 20 windows and 56.4 over its last 20,
+45.6%. A throttling device produces a monotone decline. There is none.

**What is actually happening: the application is blocked on the GPU.** Minecraft's own
`glClientWaitSync` - Blaze3D's `MAX_SUBMITS_IN_FLIGHT = 2` throttle, counted here as `sync_wait_*` -
went from 1.98 ms per frame (10% of a frame) in the earlier session to **6.00 ms per frame (29%)**, and
inside each session it climbs monotonically per frame (1.35 → 7.4 ms) while the frame rate rises. In the
last window it is one real block per frame, 8.7 ms each, against nine cheap polls.

This **withdraws §6.11's conclusion** that the steady-state ceiling lies in the application's own CPU
code. That conclusion came from subtracting this library's measured time from the frame gap and
attributing the remaining 82-86% to "the application's own code". Waiting on a fence is not the
application being slow; it is the GPU being slow, and the GPU's work is whatever this library submitted.
The ceiling is on the GPU side. This library's CPU cost (14-18% of a frame) is measured and remains
correct - it is simply not the binding constraint.

**And it explains the stutter spikes, which are consequences rather than defects.** The 70-106 ms
single-call spikes in `glCopyBufferSubData` and `glClear` occur only when the GPU is behind. Grouping
windows by whether the application ever actually blocked (`sync_wait_positive > 0`), across two sessions
and 330 windows:

| | windows | copies | slow copies | worst single `glClear` |
| --- | --- | --- | --- | --- |
| GPU ahead (application never blocked) | 97 | 5,241 | 3 (0.057%) | **0.6 ms** |
| GPU behind (application blocked) | 233 | 57,304 | 147 (0.257%) | **106.3 ms** |

`glClear` is the decisive column. It touches no buffer, so nothing about promotion, mapping or the copy
path can explain it, and across 97 windows and roughly 9,800 clears not one exceeded 0.6 ms. The only
variable the two share is GPU queue depth.

That split is confounded, though - GPU-behind windows are also the moving windows, with 246 copies per
window against 54, and movement could cause both independently. The confound is broken by holding copy
volume fixed. In the band of 1-99 copies per window, with 94 and 23 windows to compare:

| copies/window 1-99 | windows | slow copies per 1,000 | worst copy | worst `glClear` |
| --- | --- | --- | --- | --- |
| GPU ahead | 94 | **0.00** | 0.8 ms | 0.6 ms |
| GPU behind | 23 | 2.03 | 12.5 ms | 102.6 ms |

Same volume, opposite behaviour. The dose-response inside the GPU-behind group is noisier (0.57 → 2.42 →
1.64 → 1.77 → 10.02 slow per 1,000 as per-frame blocking rises through 0, 2, 5, 10 and >10 ms), so the
relationship reads as a threshold rather than a gradient; the volume-controlled split is the load-bearing
evidence, not the gradient.

**Consequence for the plan: the device-side `hiperf` sample of the copy path is withdrawn before being
run.** It would have profiled a symptom. What needs explaining is why the GPU falls behind.

Two things were ruled out immediately, both from data already in the log rather than by argument:

- **This library's own frame fence is not constraining the pipeline.** `frame_wait_already` equals
  `frame_wait_calls` in every window and the total wait is about 1 µs per frame. It never blocks, so it
  cannot be limiting how far the GPU may run behind. (It reads as free only *because* the application
  already blocked earlier in the frame - but never blocking is exactly what "not a constraint" means.)
- **FSR1 is off.** `fsr1Setting = 0` in the startup dump, so the upscale pass is not in the frame.

Two candidates remain for inflated GPU work, and the cheaper one is testable with no code change at all:

1. **The multi-draw-indirect backend.** `prepare_indirect_buffer` performs, per multi-draw, a
   `glGetIntegerv`, a `glBufferData` orphan, a `glMapBufferRange`, `primcount` command writes, a
   `glUnmapBuffer`, and then `glMultiDrawElementsIndirectEXT`. Its *CPU* cost was measured and is flat
   (0.33 ms/frame under 30 fps against 0.30 ms/frame at 80+), but its GPU cost has never been measured,
   and indirect draw is a path where a mobile driver may fall back. `multidrawMode = 2`
   (`PreferBaseVertex`) removes the whole indirect path in favour of a loop of `glDrawElementsBaseVertex`,
   and `glx/lookup.cpp handle_multidraw_func_name` honours the mode at `getProcAddress` time, so the
   switch really does change which backend the application resolves. A config-only A/B.
2. **`COHERENT` on the terrain arenas.** The copy source and destination buffers carry
   `flags=0x1c2`/`0x2c2`, i.e. `MAP_WRITE|PERSISTENT|COHERENT` on top of `DYNAMIC_STORAGE`, and those are
   the buffers the GPU reads terrain vertices and indices out of every frame. Coherent buffer memory on
   this class of GPU is typically uncached. §6.12 tested withdrawing the promotion entirely and it was a
   clear loss, but that experiment conflated two bits: `PERSISTENT` is what lets this library map the
   store, `COHERENT` is what changes the memory type. Separating them - grant `PERSISTENT`, withhold
   `COHERENT` - is now possible because `mg_appmap_write` publishes with `glFlushMappedBufferRange`
   rather than relying on coherence. It is *not* free, though: `bufferCoherentAsFlush = 1` currently
   skips all 85,418 of the application's own `glFlushMappedBufferRange` calls on the grounds that the
   storage is coherent, and that justification would have to go too. Do candidate 1 first.

### 6.14 Separating the mapping capability from the memory type — promote mode 2

6.12 closed the promotion axis with the condition "do not re-open this without a mechanism that keeps
the mapping capability while changing the memory type". This is that mechanism.

**Why the axis reopens.** 6.13 established that the binding constraint is GPU throughput: the
application blocks on its own fence about 6 ms per frame, 29% of a frame. The GPU reads terrain
vertices and indices out of buffers this library promoted to `flags=0x1c2` every frame, and coherent
buffer memory on this class of GPU is typically uncached. That is the one thing this library does to
those specific buffers that could cost GPU read bandwidth.

**Why 6.12 could not tell.** `MG_PROMOTE_COHERENT=0` withdrew `MAP_WRITE`, `PERSISTENT` and `COHERENT`
together. The failure it produced was a *mapping* failure — the deferred queue could not map its
target and fell back to ordered `glBufferSubData` 6,973 times. Per EXT_buffer_storage, mapping requires
`WRITE` and `PERSISTENT` in the storage flags and does **not** require `COHERENT`. So the two bits that
broke it are not the bit under suspicion.

**Mode 2** grants `MAP_WRITE | PERSISTENT` and withholds `COHERENT`. A full audit of the file found
that no admission test anywhere requires `COHERENT` — `mg_pmap_consider` tests `0x42` and says in so
many words that requiring coherence there is the mistake that made the first version of the
`u_SectionTimeInfo` fix fail; the deferred replay and the direct-map upload both publish through
`glUnmapBuffer` and need only `MAP_WRITE`. What the audit did find is four places that had been
*assuming* coherence unconditionally. All four now ask `mg_coherence_promised()`, so mode 1 stays
bit-identical to today and the three vanilla configurations are outside the blast radius:

| site | under mode 1 | under mode 2 | why it must change |
| --- | --- | --- | --- |
| `glFlushMappedBufferRange` | drops all ~85,418 application flushes | forwards them | the skip's only justification is the coherence promise |
| `glMapBufferRange` | strips `FLUSH_EXPLICIT` from non-persistent mappings | keeps it | forwarding a flush for a mapping without that bit is itself `GL_INVALID_OPERATION`, so this is one change with the row above, not two |
| `PMAP_LADDER` rungs 0/1 (ask `COHERENT`) and rung 4 (`needsFlush=false`, no `FLUSH_EXPLICIT`) | tried; rung 1 is the one the driver grants | skipped for a non-coherent store | rungs 0/1 would be refused predictably; rung 4 has no way to publish a write at all, and a silently unseen write is worse than falling back |
| `mg_appmap_write` unpublishable branch | writes and counts `appmap_unpublished` | counts and returns `GL_FALSE` | with no coherence to rest on, those bytes would never reach the GPU |

The rung gating is keyed on the store's own recorded flags rather than on the global mode, so a store
the application created coherent itself is still eligible for every rung.

**A design smell found on the way, worth fixing separately.** `buffer_coherent_as_flush` gates both
"promote" and "skip the application's flushes", and is derived from `angle == Disabled` rather than
being configurable. One name for two decisions is why they could not be varied independently.

**Acceptance criteria for the next device round.** Proof the change took effect, before any
performance reading is worth anything:

- `[MG-PROMOTE] mode=2 (WRITE|PERSISTENT, COHERENT withheld)` in the log.
- `read_flags_or` / `write_flags_or` on the `MG-DIAG-COPY` line lose `0x80`: `0x1c2 → 0x142`,
  `0x2c2 → 0x242`. This is the direct evidence that the buffers the GPU reads terrain from are no
  longer coherent.
- `flush_driver` goes from 0 to roughly 689 per second.

Then the question this round exists to answer, in order — a "no" on the first line makes the rest moot:

- `sync_wait_positive` and its per-frame cost. **If the GPU-side hypothesis is right this falls from
  6.00 ms/frame.** If it does not move, `COHERENT` is not what is holding the GPU back and this axis
  closes for good.
- Frame rate, and the fraction of windows below 60 fps.
- `fb_calls` must stay near 201, `appmap_writes` non-zero, `appmap_unpublished` **0**,
  `deferred_replay_fallback` **0**, `pmap_adopted` non-zero. Any of these moving the way they moved
  under mode 0 means the mapping capability was lost after all and the separation failed.
- `pmap_accepted_rung` should read 3 rather than 1 (explicit-flush rung instead of the coherent one),
  with `pmap_flushes` now carrying a per-write cost that has never been measured. If that cost is
  large, this is where it shows.

### 6.15 6.4 answered from the specification, and a claim in the changelog corrected

Source: the Khronos registry copy of `EXT_buffer_storage`, which is what this driver actually exposes
(`Detected GL_EXT_buffer_storage!` in the startup log). Paraphrased, with the operative sentences kept
short:

Section 6.2, describing the **storage** flag `MAP_COHERENT_BIT_EXT`: shared access to buffers that are
mapped for client access and used by the server will be coherent, *so long as that mapping is performed
using MapBufferRange*. It then splits into cases, and the two that matter here are:

- with the bit **not** set, a client write followed by `FlushMapped*BufferRange` over the written range
  is seen by the server in subsequent commands;
- with the bit **set**, a client write is seen by the server in subsequent commands.

Section 6.3, describing the **access** bit of the same name: it indicates the mapping should be
performed coherently, "that is, such a mapping follows the rules set forth in section 6.2", and if it is
set then the buffer's `BUFFER_STORAGE_FLAGS_EXT` must include it too. And the `FlushMappedBufferRange`
paragraph attributes the property to the store rather than the mapping: data written to a coherent
*store* will always become visible to the server.

**So coherence is a property of the store.** The only requirement on the mapping is that it be made
with `MapBufferRange` rather than `MapBuffer`; the access bit is a consistency constraint and adds
nothing. Three consequences:

1. **6.4's hazard is not one.** Under promote mode 1 the store carries `COHERENT`, so stripping
   `MAP_FLUSH_EXPLICIT_BIT` and skipping the application's 85,418 flushes loses nothing. Four rounds
   cited that paragraph as a live correctness risk. It was never one.
2. **The artifact is not explained by it.** The leading candidate stays 6.10's binding shadow.
3. **A changelog claim is wrong and must not be acted on.** Entry 1000491 item (ii) says that adding
   `GL_MAP_COHERENT_BIT` to a store does not make a later mapping coherent, concludes that the
   application's flushes were removed without a replacement, predicts intermittent garbage geometry, and
   records that a fix existed at `920f65a`, was reverted along with the staging-ring work, and "should be
   re-applied separately". **It should not.** Re-applying it would add roughly 689 driver flushes per
   second to buy a guarantee the store already provides. The changelog entry is left as written, because
   it is a historical record; this is the correction.

**What it confirms about promote mode 2.** The same text makes all four of 6.14's conditional changes
necessary rather than merely cautious, which is the reason to have read it before shipping them:

- the error list contains "INVALID_OPERATION is generated by MapBufferRange if any of MAP_READ_BIT,
  MAP_WRITE_BIT, MAP_PERSISTENT_BIT_EXT, or MAP_COHERENT_BIT_EXT are included in `access`, but the same
  bit is not included in the buffer's storage flags" — so the two `PMAP_LADDER` rungs that ask for
  coherence are guaranteed to fail on a mode-2 store, and skipping them is correct;
- with the storage bit absent, the *first* case above is the only one available, so a mapping with
  neither `COHERENT` nor `FLUSH_EXPLICIT` — `PMAP_LADDER`'s last rung, and `mg_appmap_write`'s
  unpublishable branch — has no way to make a write visible at all. Gating both is a correctness
  requirement, not tidiness;
- base ES 3.1 already errors on `FlushMappedBufferRange` for a mapping made without
  `MAP_FLUSH_EXPLICIT_BIT`, which is why keeping that bit and forwarding the flush have to be one
  change.

**And it adds a cost prediction for the mode-2 round.** Those 85,418 skipped flushes become real driver
calls. `flush_us` and `flush_max_us` already count them, and the counter has never had a non-zero
`flush_driver` to report, so their cost on this driver is unmeasured. If mode 2 shows a CPU regression
without a GPU improvement, look there first.

**One ambiguity, recorded rather than resolved.** Section 6.2's bullet says a client write to a coherent
store is visible "in subsequent commands", while the `FlushMappedBufferRange` paragraph says visible
"after an unspecified period of time". The first is the specific normative statement for this case and
the second reads as an informative aside, but they are in tension, and a driver that took the weaker
reading would produce exactly the kind of intermittent stale-geometry artifact 6.10 is still chasing.
Not actionable on its own — noted so that it is not rediscovered as a new idea.

### 6.16 Promote mode 2 on device: the mechanism worked, the hypothesis was half right, and the frame rate is not there

**The separation itself is a clean success.** All four acceptance criteria from 6.14 came back as
specified, which is what makes the rest of this reading trustworthy:

| criterion | result |
| --- | --- |
| mode in effect | `[MG-PROMOTE] mode=2 (WRITE\|PERSISTENT, COHERENT withheld)` |
| storage flags on the buffers the GPU reads terrain from | `write_flags_or` 0x1c2 → **0x142**, `read_flags_or` 0x2c2 → **0x242** |
| flushes forwarded | `flush_driver` = `flush_calls`, `flush_skipped` = 0, and cheap: 1,332 flushes in 420 us, mean 0.32 us, worst 1 us |
| mapping capability retained | `deferred_replay_fallback` **0**, `appmap_unpublished` **0**, `pmap_accepted_rung` **3** at access 0x52, `pmap_ladder_failures` **0**, `pmap_flushes` 164 for 108 us |

So mode 0's collapse (`fallback` 6,973, `fb_calls` 9,469) did not recur in any form. `PERSISTENT` and
`MAP_WRITE` are indeed the mapping capability and `COHERENT` is indeed separable. The unmeasured cost the
6.14 criteria flagged - publishing a pmap write by explicit flush instead of relying on coherence - turned
out to be negligible.

**The GPU-side prediction moved, in the predicted direction, by a lot.** The application's own blocking
fence wait:

| session | promote mode | app blocked per frame | share of a frame |
| --- | --- | --- | --- |
| sodium262-copy | 1 | 1.98 ms | 10% |
| sodium262-noprobe | 1 | 6.00 ms | 29% |
| sodium262-nocoherent | **2** | **3.54 ms** | **18%** |

and the session produced two consecutive 13-window segments at **82.6 and 81.8 fps**, the highest
sustained figures in any session so far (previous best segment: 64.8).

**But it is a trade, not a win.** Withholding `COHERENT` makes the server-side copy and the clear pay
cache maintenance:

| per frame | mode 1 | mode 2 |
| --- | --- | --- |
| `glCopyBufferSubData` | 1.0 ms | 1.5 ms |
| `glClear` | 0.13 ms | 0.37 ms (mean per call 22 → 64 us) |
| slow-copy rate | 0.30% of calls | 0.75% |

−2.46 ms of GPU wait against +0.74 ms of CPU. Net about −1.7 ms per frame, and the measured session
average went 45.3 → 47.8 fps. Real, small, and nowhere near the complaint.

**Two methodological corrections found while doing this, both of which affect earlier numbers.**

`window_ms` is not fixed at 1000. Median is 1,004-1,006 but 4 to 6 windows per session run long, up to
6.6 seconds. So `frame_wait_calls` is not the frame rate, and every "fps" figure in these notes taken
that way is inflated by 4.5% (copy), 5.8% (noprobe) and 8.3% (nocoherent). Frames divided by summed
window time gives the real averages: **48.8 / 45.3 / 47.8**. The bias is small enough not to overturn any
comparison, but per-frame figures should be computed as total-over-total, never as a mean of per-window
rates. The per-frame blocking figures above are total-over-total and are unaffected.

Cross-session comparison is the weaker part of all of this. Sessions differ in length (197 / 150 / 112 s)
and the route is hand-flown, so terrain churn differs. Within-session splits - the GPU-ahead versus
GPU-behind grouping, and per-frame normalisation - carry the weight; the raw session averages do not.

**Where the frame rate actually is, and why the next step is not another MG change.** Splitting a frame
by band within the mode-2 session, per frame:

| per frame | 82 fps band | 21 fps band | growth |
| --- | --- | --- | --- |
| unaccounted (render-thread Java, or not being scheduled) | 7.8 ms | **32 ms** | **+24** |
| application's own GPU fence wait | 3.9 ms | 9.7 ms | +5.8 |
| MG `glCopyBufferSubData` | 0.45 ms | 5.8 ms | +5.3 |

The unaccounted term dominates the growth by four to one, and it is precisely the part MG's counters
cannot see. Adding more MG counters cannot reach it; neither can `hiperf` on a driver entry point. The
instrument for it already exists in this repository and has been sitting unused:
`-DAMCL_STACK_SAMPLER=ON`, which signals the render thread and walks both its native and its Java stack
via ASGCT.

The archived performance note said this three rounds ago, and it was right: *"before either: profile the
render thread. Every round in this area that started from a mechanism instead of a profile has been
wrong."* Every round since, including 6.14, started from a mechanism.

**Verdict on the promotion axis.** Mode 2 is kept as a switch and left **off by default**. It is a
genuine improvement to GPU wait and it is now known to be safe, but it is small, it costs CPU, and
shipping it would change the memory type of every terrain buffer on the strength of a cross-session
comparison. Revisit it after the profile says what the 24 ms is - if the render thread turns out to be
GPU-bound after all, mode 2 becomes worth its cost; if the 24 ms is Java, mode 2 is noise.

**Sampler configuration for the profiling round**, recorded because the previous settings could not have
caught anything: threshold was 150 ms with an 8-per-second wake, against stutter frames of 40-100 ms - it
would never have fired, and at a 50 ms wake interval a 90 ms stall can fall entirely between two wakes.
Now 60 ms threshold, 8 ms wake, 100 ms emit rate-limit. The build carries
`-XX:+PreserveFramePointer`, so **that round cannot be used to compare frame rates** - only positions.

### 6.17 The render-thread profile. GPU-bound, confirmed by a second instrument, and the copy axis closed for good

`-DAMCL_STACK_SAMPLER=ON`, threshold 60 ms, 8 ms wake, 100 ms emit limit. 107-second session, promote
mode 1 (default), MC 26.2 + Sodium. 254 backtraces, 181 of them after the 25-second loading period.

**Read this from the shape of a stack, because that is where the finding is.** A representative sample:

```
#00  ld-musl-aarch64.so.1+0x1d9c78                       <- blocked in a libc syscall
#04..#07  libEGL_impl.so                                 <- the GLES driver
#08  libglfw.so (glCopyBufferSubDataARB+0xa0)            <- this library
#09  libglfw.so (glCopyNamedBufferSubData+0xc8)
#10+ [anon] [rwxp]                                       <- JIT-compiled Java
#46  libjvm.so (AsyncGetCallTrace+...)                   <- the sampler's own frames, outermost
```

So the innermost frame is where the thread is actually blocked, and the innermost *symbolised* `libglfw`
frame names the GL call it is blocked inside. 86.2% of in-game stall samples have their innermost frame
in libc, i.e. in a kernel syscall reached through the driver.

**Where the render thread is when it stalls** (181 samples, ≥60 ms without a swap):

| share | GL entry point the thread is inside |
| --- | --- |
| **39.8%** | `glClientWaitSync` — the application's own fence |
| 8.8% | `glClear` |
| 7.2% | `mg_deferred_upload_flush_all` — this library's own fence wait at present |
| 6.6% | `glDrawElementsInstancedBaseVertex` |
| 2.8% | `glCopyBufferSubDataARB` |
| 2.8% | `glBlitFramebufferARB` |
| 1.7% | `mg_glMultiDrawElementsBaseVertex_multiindirect` |
| 1.2% | `glDrawArraysInstanced`, `glDrawBuffers` |
| 2.8% | `glShaderSource` / `glLinkProgram` (shader compilation) |
| **26.0%** | no `libglfw` frame at all — application Java or the JVM |

Adding the ones that can only be waiting on the GPU - the fence, the clear, the draws, the blit, and the
deferred queue's fence - gives **about 68% of render-thread stall time spent blocked on the GPU.**

**This confirms 6.13 with an independent instrument, and it closes the copy axis.** 6.13 concluded from
counter grouping that the 70-106 ms spikes in `glCopyBufferSubData` and `glClear` are consequences of GPU
queue depth rather than defects in those paths. The profile says the same thing from the other side:
`glCopyBufferSubDataARB` accounts for **2.8%** of stalls. Several rounds of work went into the copy path
on the strength of it being the largest single item in the per-second counters. It is not the problem.
Neither the `hiperf` plan nor any further change to the copy path is worth pursuing.

**It also revises the verdict on promote mode 2 upward.** 6.16 kept mode 2 off by default because a 41%
cut in GPU wait looked small against a complaint about frame rate. The profile now says the GPU wait *is*
the frame rate, so a 41% cut in it is attacking the right term. That does not make mode 2 the answer - it
bought about 1.7 ms of a 21 ms frame - but it stops being a curiosity.

**`mg_deferred_upload_flush_all` at 7.2% was checked and is not a defect.** `deferred_flush` returns
early on an empty queue and takes no fence, so it is not paying for nothing; the 13 samples are drains
that found the fence unsignalled, which is again the GPU being behind. Left alone.

**What the profile cannot see, and why.** ASGCT returned `-1` (`ticks_no_Java_frame`) on all 254 samples,
so the Java side is blind and the 26% bucket has no name. The native walk works and JIT frames appear as
`[anon] [rwxp]` regions with no symbol, which is why "26% application Java or JVM" is as precise as this
round gets. Fixing it means getting the right `JNIEnv` to `AsyncGetCallTrace` for the sampled thread -
`g_renderEnv` is captured once on the render thread, which is the usual way, so the cause is not obvious
and it needs its own look. Worth doing: 26% is the second-largest bucket, and it is where a
`chunk_builder_threads`-style explanation would show up if it were real.

**Two device facts gathered on the side, both recorded rather than acted on.** The game log for this
session says, verbatim, `[ChunkBuilder/]: Started 2 worker threads` - Sodium is meshing with two threads,
because `-XX:ActiveProcessorCount=8` (set in `jvm_common_args.cpp` to work around the sandbox reporting a
single core, which used to deadlock Forge's parallel mod loading) feeds Sodium's
`clamp(max(procs/3, procs-6), 1, 10)`. That is a candidate for the 26%, not a conclusion: Sodium meshes
asynchronously, so too few threads mainly makes chunks appear late, and what lands on the render thread is
the upload. `/proc/cpuinfo` and `/sys/devices/system/cpu/present` are both permission-denied, so the real
core count is still unknown. Sodium's own `chunk_builder_threads` option would be the surgical way to test
it, without the JVM-wide side effects of changing `ActiveProcessorCount`.

**Three tooling errors found and fixed while doing this, all of the kind that produce confident wrong
answers.** Recorded because each was caught by a self-test or a sanity check rather than by luck:

- The stall durations reported by `STALL detected (no swap for N ms)` are **detection latency, not stall
  length**: the loop wakes every 8 ms and logs on first detection, so N is always in [threshold,
  threshold+interval) - here 60 to 68 ms for every one of 151 stalls. The real length is the `stalled Nms`
  field on subsequent backtraces of the same stall. Reading the first number as a duration would have
  produced a beautifully tight and completely meaningless distribution.
- PowerShell's `[int]` **rounds**, and rounds halves to even. `[int]2.7` is 3, so a p90 index ran off the
  end of the array and printed blank; `[int]1.5` is 2, so a median of three elements took the wrong
  element. Use `[Math]::Floor`.
- PowerShell variable names are **case-insensitive**, so a local `$top` hash table silently overwrote the
  `-Top` parameter and the script died binding a hash table to an `int`.

**And one substantive correction to how these notes have been reading frame rates**, from 6.16 but worth
repeating here because it affects every earlier number: `window_ms` is not fixed at 1000. Per-frame
figures must be computed as total-over-total, never as a mean of per-window rates.

### 6.18 The Java-side profile, and one mechanism that would explain both symptoms at once

The sampler's ASGCT path returns `-1` on every sample on this JDK, so a **JVMTI `GetStackTrace`
fallback** was added: called from the sampler thread rather than the signal handler, so it is not bound
by async-signal-safety, and a thread blocked in native is safepoint-safe so its Java stack is walkable
from the last-Java-frame anchor. It works — 224 backtraces, 6,467 Java frames. Implementation notes worth
keeping: the render thread's `jthread` global ref must be taken **on the render thread**
(`GetCurrentThread` returns the caller's), and the sampler thread must attach with
`AttachCurrentThreadAsDaemon` or the JVM will not exit and the game cannot be closed.

**First: the session has three phases, and mixing them produced a wrong answer that had to be
withdrawn within the same round.** Stall counts per 10 s: t=10–59 s **124** (startup, world load, shader
compilation — all 48 shader-compile stalls fall in t=10–49), t=60–350 s **17 total**, t=355–397 s **85**.
So an aggregate over the whole session reported "shader compilation is 26.6% of in-world stalls", which is
a loading cost, not the stutter. `maxGlslCacheSize = 0` remains a real and unfixed load-time defect, but
it is not this. **The stutter population is the dense tail**, which is when the player was walking.

**The dense segment (84 samples, t≥355 s):**

| share | top of Java stack |
| --- | --- |
| 23.8% | `nglClientWaitSync` via `GlCommandEncoder.awaitSubmit` ← `CommandEncoder.submit` |
| 14.3% | the present path (`GlSurface.present`) |
| 9.5% | `glBindFramebuffer` |
| 9.5% | `glCopyNamedBufferSubData`, of which 8.4% has `DefragmentingBufferArena.defragmentLeftwards/Rightwards` as its outer frame |
| 2.4% | `nglMultiDrawElementsBaseVertex` |
| 84.5% | **no Sodium frame anywhere in the stack** |

**A measurement conflict that must be resolved before either number is used again.** 6.17's native-stack
profile put `glCopyBufferSubDataARB` at **2.8%** and concluded "the copy axis is closed for good". This
round's Java stack puts the same call at **9.5%**. A factor of 3.4. Either the attribution rule differs
(innermost symbolised `libglfw` frame versus top-of-Java-stack) or the two sessions differ in workload.
**Until that is reconciled, neither figure supports a conclusion**, and 6.17's "closed for good" is
downgraded to "not established".

**What a full source audit of Sodium against MG's translation ruled out** (via sub-agent, with
line-number citations; Sodium bytecode read from `alpha.1`/`alpha.2` dumps, so `alpha.3` is not directly
verified):

- **Texel-buffer emulation does not run.** `gles/loader.cpp:100-102` sets `emulate_texture_buffer` only
  at `es_version <= 310`; this device reports 320, so `glTexBuffer` forwards natively
  (`gl/buffer.cpp:1804`) and the emulation — 6 `glGetIntegerv` + `glGetBufferParameteriv` +
  `glTexImage2D` + height×`glTexSubImage2D` — is dead code here. The same flag makes `prepareForDraw()`
  an empty function, so each multi-draw begins with zero driver calls.
- **SSBO and compute are not used by Sodium's GL path** — only by its `VK*Context` classes, gated behind
  `instanceof VulkanDevice`.
- **MG's `glBindFramebuffer` is a strict 1:1 forward with no synchronisation point**
  (`gl/framebuffer.cpp:46-69`; `ensure_max_attachments` queries the driver only on the first call ever,
  and with FSR1 off the FBO-0 redirect is the identity). So the 9.5% is inside the driver, presumably
  tile resolve/restore. *Not* an MG defect. `glDrawBuffers` (`framebuffer.cpp:124-160`) does expand one
  logical call into up to 8 `glFramebufferTexture2D` re-attachments, which could dirty the FBO — but
  whether that affects the following bind is unmeasured.
- **Sodium hardly calls GL directly at all** — 8 LWJGL references in the whole jar. It works through
  Blaze3D, so "Sodium renders chunks differently" reaches MG as a *different capability mix*, not as
  different call sites.

**And the mechanism that now leads, because it explains both symptoms with one cause.**
`gl/multidraw.cpp` keeps **one** indirect command buffer for the whole process:

```cpp
static GLuint g_indirectbuffer = 0;   // one global, shared by every multi-draw in a frame
...
glMapBufferRange(GL_DRAW_INDIRECT_BUFFER, 0, primcount * 20,
                 GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);   // no UNSYNCHRONIZED
```

Sodium issues one multi-draw per render region per pass, so a frame contains many, and **each rewrites
this same memory and then immediately issues a draw that reads it.** Multi-draw N+1's map must not land
while multi-draw N's `glMultiDrawElementsIndirectEXT` is still reading. Without
`GL_MAP_UNSYNCHRONIZED_BIT` the driver has to wait for that read to finish — and that wait *is* the cost.
If instead the driver renames the storage, correctness is preserved; if it does neither, the commands read
are stale or half-written, meaning wrong `baseVertex`/`firstIndex`, meaning **geometry drawn from the
wrong place in the arena: a region renders empty while its geometry appears elsewhere.** Which is 3.2's
artifact, word for word.

One mechanism, and it accounts for every discriminator the user established:

- only with Sodium — vanilla Minecraft issues no multi-draw at all, so this buffer is never touched;
- not on PC — desktop GL has native `glMultiDrawElementsBaseVertex` and needs no indirect buffer;
- worse across a chunk boundary — more visible regions means more multi-draws per frame, hence more
  serialisation points on the one shared buffer;
- frame rate and artifact always appearing together — same cause, two faces of it.

Note also that §4 already measured what a missing `GL_MAP_UNSYNCHRONIZED_BIT` costs on this driver —
24,934 µs per call, six times worse than not mapping at all, recorded there as "the most important single
measurement in this document" — but it measured it on the terrain store and **never on the indirect
command buffer**.

**Falsification, with no code change.** `multidrawMode = 2` (`PreferBaseVertex`) removes the indirect path
entirely: no indirect buffer, no `glGetIntegerv`, no map, just `primcount` calls to
`glDrawElementsBaseVertex`. `glx/lookup.cpp handle_multidraw_func_name` honours the mode at
`getProcAddress` time, so the switch really does change which backend the application resolves. Three
outcomes, all informative:

- artifact **and** stutter gone ⇒ mechanism confirmed; the fix is a rotating set of indirect buffers (or
  one per multi-draw slot) rather than shipping the unrolled backend, since unrolling costs one driver
  call per section;
- artifact gone, frame rate worse ⇒ mechanism right, this remedy too expensive; implement the rotation;
- neither changes ⇒ refuted, and the next suspect is `SharedQuadIndexBuffer.grow`, below.

**Second suspect, if the first is refuted.** `SharedQuadIndexBuffer.grow` closes the old index buffer,
creates a new one, maps it, and has the CPU fill the **entire** block element by element, with doubling
growth — and it is triggered from `DefaultChunkRenderer.render` exactly when new sections arrive and
capacity runs short. MG promotes that store to `COHERENT` (`gl/buffer.cpp:2087-2088`), which on this class
of GPU is typically uncached or write-combined, so the fill writes element-at-a-time into uncached memory,
and **its cost doubles as the world loads**. That last property is the one that would explain a discrete
event whose severity grows through a session. Every link is readable in source; none of it has been
measured. Testable with the existing `MG_PROMOTE_COHERENT=2` switch.

### 6.19 6.8 solved: `persistentMapping()` is false because **this launcher makes it false**

Not a MobileGlues defect, and not a Blaze3D capability failure. `AmclLauncher.java:140` calls
`MaleoonPersistentMappingCompat.install(...)`, which uses reflection plus
`Unsafe.putObjectVolatile` to overwrite the final field `GlDevice.deviceInfo` with an otherwise
identical `DeviceInfo` whose `persistentMapping` is `false`.

**Both sides of 6.8's contradiction were correct all along.** LWJGL really does see the extension, the
constructed value really is `true`, and Sodium really reads `false`. Something rewrote it in between,
and 6.8's elimination list covered Sodium's mixins, NeoForge and Minecraft's own classes — it never
covered the launcher's own code, which is where the writer lives.

The bytecode chain is now closed rather than inferred:

- `DeviceFeatures` is a record; `putfield` order gives `persistentMapping` as the **7th** constructor
  parameter (offset 40). Previously an assumption, now read directly.
- `GlHeuristics.createDeviceInfo(GLCapabilities, int, Set<String>)` passes
  `extensions.contains("GL_ARB_buffer_storage")` in that position (offset 100-103). Not a
  `GLCapabilities` boolean, not a function-pointer check, not a version test.
- `BufferStorage.create` **adds** that very string to the same set when
  `caps.GL_ARB_buffer_storage` is true (offset 13-16).
- `GlDevice.<init>` calls `create` at offset 159 and `createDeviceInfo` at offset 323 with the **same**
  local 6. So the two are not different criteria; they are the upstream and downstream of one.

So `storage_calls > 0` (Immutable chosen) *entails* `persistentMapping == true` at construction. And the
hook's own guard proves it independently: it returns early with `already-disabled` if it reads false, so
its `applied:` log line can only be reached from `true`. Device log, most recent sessions
(08-08 01:28 and 01:31): `applied: ... now reports persistentMapping=false`.

**Why this is the upstream cause of everything in 6.18.** With `persistentMapping()` false,
`MojangStagingBuffer` leaves `staging = null`, so `enqueueCopy` goes to
`CommandEncoder.writeToBuffer` — a direct `glNamedBufferSubData` into the arena, for **every** terrain
upload. Both arenas exceed the 16 MiB threshold (index first block 16 MiB, geometry 32 MiB), so all of
it enters this library's deferred queue and lands only at the present. The translucent pass then reads
its per-section sorted indices out of the index arena in the same frame they were written
(`DefaultChunkRenderer` offset 438-453), which is the water holes; and a batch that loses its fence is
replayed with synchronous `glBufferSubData` at 3,424 us per record, worst single 97 ms.

With it true, Sodium uses its own `MappedStagingBuffer` — persistently mapped 32 MB ring, memcpy,
`glCopyBufferSubData`, its own fence recycling — and **this library does not participate in the write at
all.** That is what the desktop build does, and the desktop build has neither symptom.

**MobileGlues' extension reporting was checked and is correct**, so this is not a case of the capability
being reported inconsistently: `gles/loader.cpp:217-219` appends `GL_ARB_buffer_storage`,
`gl/getter.cpp:390-393` returns it from `glGetString(GL_EXTENSIONS)`, `GL_NUM_EXTENSIONS`
(`getter.cpp:32-51`) counts tokens of that same string, and `glGetStringi` (`getter.cpp:455-457`)
splits that same string. All three are the same source and agree.

**This is a trade to re-measure, not simply a bug to remove.** The hook's own comment says it exists
because Maleoon was "unusually slow when Sodium copies terrain from its persistently mapped 32 MiB
staging ring". That may have been right when written, but it predates the entire deferred-queue design
and everything after it. So it is now a device-settable switch rather than a fixed decision:
`MG/config.json` `"persistentMappingOverride"` → `AMCL_PERSISTENT_MAPPING`, read by the hook.
`0` (default) applies the hook as today; `1` skips it and lets Minecraft's own `true` stand.

**Open, and it blocks the experiment:** with `persistentMappingOverride: 1` written and read back, and
`MG_CONFIG` plus the hook logging from the **same pid**, the hook still logged `applied` rather than
`skipped by AMCL_PERSISTENT_MAPPING=1`. The jar is re-extracted with `TRUNC` on every launch
(`McGamePage.ets:717-730`) so staleness is ruled out, and `setenv` runs at NAPI init before the JVM is
created, so `System.getenv` should see it. One link is still unaccounted for. If the next session
repeats it, move the flag to a JVM system property on the command line, which cannot be missed.

### 6.20 The artifact is fixed at its root, and the trade the launcher hook existed for is now measured

`persistentMappingOverride: 1` on device. Both proofs came back:

- `skipped by AMCL_PERSISTENT_MAPPING=1: leaving Minecraft's own persistentMapping value untouched`
- `[MG-DIAG-MAPDEST] slot=3 buffer=64 length=32000000 maps=1 unmaps=0 failures=0 access_or=0x72` — the
  32 MB store exists and is persistently mapped, so Sodium is running its own `MappedStagingBuffer` and
  this library is **out of the terrain write path entirely**. `deferred_enqueued` is 0 for the whole
  session, against 7,733 in the previous one.

**And the water artifact is gone**, which closes the chain 6.19 laid out: `persistentMapping` false →
`staging = null` → every terrain upload is a `glNamedBufferSubData` into an arena → deferred queue →
lands at the present → the translucent pass reads its per-section sorted indices from the index arena in
the same frame they were written → rectangular holes in water. Two independent artifacts have now been
traced to root and removed: the shared indirect command buffer (6.18) and this.

**The stutter survives, and the discriminator moved again — this time to something the hook's own comment
predicted.** Good/bad window diff over 111 windows (32 bad under 25 fps, 32 good at 70+), per frame:

| per frame | bad | good | ratio |
| --- | --- | --- | --- |
| `sync_wait_total_us` | **24,411 us** | 5,636 us | 4.3x |
| `sync_wait_max_us` | 3,546 us | 255 us | 13.9x |
| `copy_us` | 2,390 us | 241 us | 9.9x |
| `copy_slow_us` | 1,863 us | 59 us | 31.6x |
| `copy_slow_bytes` | 147,519 B | 58 B | 2,556x |
| `clear_us` | 477 us | 67 us | 7.2x |

Two things to read here. First, `clear_us` **collapsed** from 3,246 us/frame in the previous session to
477 — so whatever made `glClear` 51x more expensive there is gone, and it travelled with the deferred
queue rather than being independent. Second, the application's fence wait is now the dominant term at
24.4 ms of a sub-25-fps frame, about 60% of it, where in the previous session it was flat between good
and bad windows. The good windows are almost unchanged across the two sessions (5,636 against 4,770); it
is only the bad ones that moved.

**So the launcher hook's premise is confirmed by measurement.** Its comment says it exists because
Maleoon "is unusually slow when Sodium copies terrain from its persistently mapped 32 MiB staging ring".
Sodium is now doing exactly that, and the copies are 31.6x slower in the windows that stutter. The hook
was not superstition; it traded a real throughput problem for the artifact we have just fixed properly.

**But the slowness may be this library's own doing, and the switch for it already exists.** The 32 MB
staging store is promoted to `COHERENT` by `glBufferStorage`, i.e. into uncached or write-combined
memory, and `glCopyBufferSubData` has to **read** it. That is the textbook reason a GPU-side copy runs
far below its byte count. `MG_PROMOTE_COHERENT=2` withholds exactly that bit while keeping `MAP_WRITE`
and `PERSISTENT`, which is what the mapping needs.

It is safe here for a reason specific to this path: `MappedStagingBuffer.flush` **itself** calls
`flushMappedBufferRange` (bytecode offset 40-133), and under mode 2 this library forwards those flushes
instead of dropping them. Under mode 1 they are all dropped — `flush_calls` 78,390 against
`flush_driver` 0 — which is legal only because the storage is coherent (6.15). So mode 2 replaces one
publishing mechanism with another that the application already uses, rather than removing one.

Next round is therefore `persistentMappingOverride: 1` **plus** `promoteCoherentOverride: 2`, config
only. Criteria: the water artifact must stay gone; `copy_slow_us` and `sync_wait_total_us` in bad windows
must fall; `flush_driver` must become non-zero (proof the pairing took effect); and the 32,000,000-byte
store must still appear with a successful mapping — if it stops appearing, mode 2 has broken Sodium's
ring and the combination is invalid.

**If that fails**, the remaining honest options are to reinstate the hook and instead fix the deferred
queue so it does not defer the index arena (the artifact's actual mechanism), or to accept the hook and
fix only the translucent path. Both are code changes and both should wait for this measurement.

> **REFUTED on device, and the promotion axis is now closed for good.** `promoteCoherentOverride: 2`
> together with `persistentMappingOverride: 1` **lowered the frame rate on every configuration**, vanilla
> included. Reverted to defaults immediately.
>
> The reasoning error is worth naming because it is elementary and it is the second time this axis has
> produced one. `MG_PROMOTE_COHERENT` is not scoped to the buffer under discussion: it changes the
> storage flags of **every** store that passes through `glBufferStorage`, in every version, Sodium or
> not. The argument for it looked only at one consumer — the 32 MB staging buffer whose copies were
> measured 31.6x slower — and never asked what withholding `COHERENT` costs the hundreds of other stores
> that were relying on it. 6.12 made the same mistake with mode 0 and its lesson ("the promotion is
> load-bearing") was recorded and then not applied.
>
> **Do not re-open this axis.** Both directions have now been measured and both are losses. If the
> staging copies need to be faster, the change has to be scoped to that one store — for example keyed on
> `mg_store_requested_flags`, so nothing else moves — and even then only after the scoping is proven by a
> counter rather than assumed.
>
> Restored state: all override keys back to their defaults, `diagnostics: 0`. The only code change in
> this session that alters default behaviour is the 8-slot indirect command ring, which affects nothing
> but applications that issue `glMultiDrawElementsBaseVertex` — vanilla Minecraft issues none — and which
> is the device-confirmed fix for the geometry-displacement artifact.

---

## 7. Where to look

| what | where |
| --- | --- |
| upload entry point and the size test | `gl/ExtWrappers/DSAWrapper.cpp`, `glNamedBufferSubData` |
| storage promotion, mapping rewrite, flush skipping | `gl/buffer.cpp`, `glBufferStorage` / `glMapBufferRange` / `glFlushMappedBufferRange` |
| deferred queue and its drains | `gl/buffer.cpp`, the deferred-upload section |
| per-present fence and the present-time drain | `egl/egl.cpp`, `eglSwapBuffers` |
| multi-draw dispatch and backends | `gl/multidraw.cpp` |
| counters and the per-second records | `diagnostics/counters.h`, `diagnostics/counters.cpp` |
| redundant-write elision and its shadow invalidation | `gl/buffer.cpp`, `mg_subdata_*`; sites in `glBufferData` / `glBufferStorage` / `glDeleteBuffers` / `glMapBufferRange` / `diagnostics/instrumented_gl.cpp` |
| store creation record, for naming a buffer after the fact | `gl/buffer.cpp`, `mg_record_store` / `mg_store_*` |
| implicit unbind on delete, and the artifact candidate in 6.10 | `gl/buffer.cpp`, `unbind_deleted_buffer` called from `remove_buffer` |
| layer-owned persistent mapping, the chunk-boundary stutter fix | `gl/buffer.cpp`, `mg_pmap_*`; hot path in `gl/ExtWrappers/DSAWrapper.cpp`; release sites in `glBufferData` / `glBufferStorage` / `glMapBufferRange` / `glUnmapBuffer` / `glBindBuffer` / `remove_buffer` / `diagnostics/instrumented_gl.cpp` |
| the binding shadow itself, and why a stale entry is dangerous | `gl/buffer.cpp`, `g_bound_buffers_arr` / `g_element_array_buffer_per_vao` / `find_bound_buffer`; consumer in `gl/ExtWrappers/DSAWrapper.cpp`, `temporarilyBindBuffer` |
| regression harness for the three vanilla configurations | `.tmp-devlogs/regress-vanilla.ps1` |
| device scripts added in the 2026-08-07 rounds | `.tmp-devlogs/analyze-stutter.ps1`, `analyze-fallback.ps1`, `analyze-persistentmap.ps1`, `analyze-dedup.ps1`, `analyze-identify.ps1`, `grab-bufstorage.ps1` |
| device scripts: config, pull, analyse | `.tmp-devlogs/` in the superproject |
| archived earlier documents | `docs/ohos/archive/` — read for measurements, not for conclusions |

Superproject `docs/CHANGELOG.md` carries a per-versionCode record of what was shipped, what the device
said, and what was withdrawn. It is the chronological account; this file is the current one.
