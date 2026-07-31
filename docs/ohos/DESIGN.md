# OpenHarmony backend design

This describes where OpenHarmony support belongs inside MobileGlues, and the order in which
the work is being done. It is a plan of record: phases are marked as they land.

## Principle

MobileGlues is not an Android library that happens to run elsewhere. Its core job —
translating desktop OpenGL to OpenGL ES — is platform-neutral. What is platform-specific is
narrow and identifiable:

1. how log records are emitted,
2. how EGL and GLES entry points are found,
3. where configuration and caches live,
4. which GPU and driver are underneath, and what that driver makes cheap or expensive.

Items 1 to 3 already have platform seams upstream: `MobileGlues-cpp/CMakeLists.txt`
branches on `MACOS`, `ANDROID` and otherwise, and `gl/log.h` stubs
`__android_log_print` when the platform is not Android. Item 4 has no seam at all today,
and that is where the HarmonyOS problems concentrate.

So the design goal is not "an OHOS fork of the renderer". It is: keep one translation core,
give it an explicit driver-policy seam, and put the HarmonyOS answers behind it.

## Current state of the platform seams

| Concern | Upstream today | What OpenHarmony needs |
| --- | --- | --- |
| Logging | `__android_log_print`, stubbed off-Android | `hilog` with a stable tag, severity mapping |
| GL/EGL loading | `dlopen` on the non-Android path | works as-is |
| Symbol lookup | `dlsym(RTLD_DEFAULT, ...)` in `glx/lookup.cpp` | own-handle first; the system ICD can shadow the wrappers |
| Config directory | `MG_DIR_PATH` or `/sdcard/MG` | app sandbox path supplied by the launcher |
| Depth formats | unsized desktop enums passed through | map to sized GLES formats |
| Extension string | conservative | advertise what GLES 3.x provides and MG forwards |
| GPU identification | vendor string heuristics in `config/gpu_utils.cpp` | recognize Maleoon and expose a driver profile |
| Buffer storage | `buffer_coherent_as_flush` promotes dynamic buffers to coherent/persistent host-visible | must be a per-driver decision, not a global default |
| Hazard tracking | none; correctness relies on driver ordering or a global fence | per-range tracking so waits are rare and local |

The last two rows are the current blocker. Details and measurements are in
[`PERF-MALEOON.md`](PERF-MALEOON.md).

## Target structure

```
MobileGlues-cpp/
  platform/
    platform.h            interface: log sink, library loading, paths, clock
    platform_android.cpp
    platform_ohos.cpp     hilog sink, sandbox paths
  config/
    driver_profile.h      identified GPU/driver + the policies derived from it
    driver_profile.cpp    detection, including Maleoon
  gl/
    storage_policy.h      buffer storage decisions, driven by DriverProfile
    upload_scheduler.h    how host data reaches a device-local resource
    hazard_tracker.h      per-range writer/reader tracking and fence recycling
  diagnostics/
    counters.h            allocation-free per-GL-thread counters, opt-in aggregation
```

Nothing above changes behaviour for existing platforms: each new policy has a default that
reproduces today's Android path bit for bit, and only the OHOS/Maleoon profile selects the
new one.

## Phases

### Phase 0 — seams and instrumentation

* Add the `platform/` interface and route MobileGlues logging through `hilog` on OHOS.
* Add `elseif(OHOS)` to CMake with the OHOS link set.
* Promote the diagnostics counters from `wip/maleoon-buffer-diagnostics` into
  `diagnostics/`, behind a setting, with the aggregation format documented.
* Extend GPU identification to report Maleoon and its driver library.

Rationale: every later decision is a measurement, so the measurement path ships first.

### Phase 1 — storage policy

Replace the blanket promotion of dynamic buffers to coherent host-visible storage with an
explicit decision per allocation:

```
StorageDecision decide(const BufferRequest& request, const DriverProfile& driver);
```

Inputs: caller storage flags, size, target, usage hint, and the driver profile.
Outputs: device-local or host-visible, coherent or explicitly flushed, persistently mapped
or not.

For the Maleoon profile: a large persistently mapped write buffer stays host-visible and
coherent, because that is the application's staging arena and the CPU writes it every frame.
A buffer requested with only `GL_DYNAMIC_STORAGE_BIT` stays device-local, because it is a
GPU-read resource that the application never maps.

Android and ANGLE profiles keep the current promotion.

### Phase 2 — upload scheduling

With device-local destinations, uploads become explicit rather than implicit:

* Host data reaches a device-local resource through an ordered driver upload, so the GL
  command stream provides the ordering and no fence is needed.
* Staging memory is recycled through a ring with a fence per generation, so the CPU never
  overwrites bytes the GPU may still read.
* Small, intra-frame reused buffers — GUI, held items, entity models — keep the existing
  synchronous path. They are not a bandwidth problem, and unsynchronized writes to them
  corrupt the same frame that reads them.

### Phase 3 — hazard tracking

Replace whole-frame and whole-batch synchronization with per-range bookkeeping: record the
last writer and last readers of a buffer range, and wait only when a range is about to be
reused while still referenced. A wait must be the exception, and its cost must be visible in
the counters when it happens.

### Phase 4 — validation

Turn the scenario matrix in [`CONTRIBUTING.md`](CONTRIBUTING.md#device-verification-gate)
into a repeatable procedure with recorded baselines, so a regression is a number, not an
impression. Vanilla and Sodium are both required, since they exercise different upload paths.

### Phase 5 — upstreaming

Split into independently reviewable pull requests, in increasing order of intrusiveness:

1. sized depth internal formats,
2. extension advertisement,
3. own-handle symbol resolution, generalized across platforms,
4. the `platform/` seam plus the OpenHarmony backend and its CI,
5. the driver profile and storage policy mechanism, with all existing platforms keeping
   their current defaults.

Items 1 to 3 are already implemented on this branch and are platform-neutral in substance.
Item 5 is the valuable one for every non-ANGLE GLES driver, not only Maleoon, which is why
it is designed as a mechanism instead of a HarmonyOS special case.
