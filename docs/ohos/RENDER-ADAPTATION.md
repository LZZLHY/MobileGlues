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
- **The released buffer is pooled, not deleted.** The release path does not call `close()`. The
  acquisition path searches that pool and hands an existing buffer object to a *different* arena.
  **This transition involves no GL call whatsoever** — no delete, no respecify, no bind. It is
  invisible to this library by construction.
- **Enqueue-then-relocate is the normal path, not a corner case.** The upload routine performs the
  uploads and the resize handling in the same call.

### 2.4 Sodium's upload routing (read)

The upload always goes to the staging buffer's enqueue method. Inside that method, if the requested
size exceeds the ring's remaining space, it writes straight through with a sub-data call; otherwise it
writes into the mapped ring and queues a copy command to be issued later. The ring's remaining space is
replenished once per frame. So within a frame the ring drains monotonically, and **once it is
exhausted every further upload becomes a sub-data call** into our path. Ring pressure rises with the
chunk-build rate, which rises with movement.

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

**These three are the acceptance bar for every future change.** A change that fixes Sodium and
regresses any of them is a failure, not a trade. Verify with `frame_wait_calls` per second and with a
human on pacing, on all three, before shipping anything.

Note that 26.1.2 shares 26.2's buffer architecture — same heap sizes, same host staging, same sub-data
route. What it lacks is the per-frame submit fence. The old claim that "older versions do not reach
this path" is true only of 1.21.x, where the largest buffer we see is tens of kilobytes.

### 3.2 What does not work

26.2 with Sodium. Two symptoms, both movement-dependent:

- **Regions render as void while the geometry that belonged there appears elsewhere.** Not
  chunk-sized; a Sodium render region is 8×4×8 sections, which matches the observed granularity.
- **Frame rate falls from ~80 to the low teens** whenever the camera or the player moves. Standing
  still is fine.

### 3.3 The mechanism of each, as far as it is currently understood

**The frame collapse is ours, and it is understood.** The current design drains a deferred-write queue
at the multi-draw entry point, and that drain waits on a fence taken microseconds earlier in the same
frame. Per 2.6 that wait cannot be cheap: the poll necessarily fails and the fallback pays the flush,
mid-frame, with a render pass open, on a 2800×1840 target. It fires once per frame that enqueues
anything, and "enqueues anything" is exactly "is moving". The error that produced it was extrapolating
a "89 % of polls succeed" measurement from the present-time drain to a mid-frame drain.

**A second, independent correctness hole is ours and is understood.** The same wait records its result
and then ignores it, so on timeout the unsynchronized write proceeds against draws that are still in
flight.

**The void-and-floating artifact is understood as a mechanism but not yet closed.** A queued record
holds a driver buffer name and an absolute offset. Per 2.3 the buffer name can be reassigned to a
different arena with no GL call, so a record that survives that transition writes into a live foreign
arena. Sodium bakes the section position into every vertex, so the foreign block draws at its own world
coordinates — geometry where nothing should be — while the slot that should have received the bytes
stays empty. That is the reported shape exactly.

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

**6.1 Can a deferred record be guaranteed not to survive a buffer-identity reassignment?**
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

**6.7 Verify the discriminator before relying on it.**
If any future design needs to tell a Sodium arena from a Minecraft heap, the existing sticky
"has ever been a copy destination" flag is the candidate, and it measured zero on a session without
Sodium. **Verify on device** that it reads zero on vanilla 26.2 *in the same session* in which
deferred enqueues are non-zero, before any behaviour depends on it.

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
| device scripts: config, pull, analyse | `.tmp-devlogs/` in the superproject |
| archived earlier documents | `docs/ohos/archive/` — read for measurements, not for conclusions |

Superproject `docs/CHANGELOG.md` carries a per-versionCode record of what was shipped, what the device
said, and what was withdrawn. It is the chronological account; this file is the current one.
