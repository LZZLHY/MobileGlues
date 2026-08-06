# MobileGlues on OpenHarmony / HarmonyOS NEXT

This directory is the entry point for the OpenHarmony platform line of MobileGlues,
developed on the `platform/ohos` branch of [LZZLHY/MobileGlues](https://github.com/LZZLHY/MobileGlues)
and consumed by [AMCL](https://github.com/LZZLHY/amcl), a Minecraft: Java Edition launcher
for HarmonyOS NEXT.

| Document | Purpose |
| --- | --- |
| [`RENDER-ADAPTATION.md`](RENDER-ADAPTATION.md) | **Start here.** The Minecraft 26.x / Sodium / MobileGlues three-way system: position, established facts, closed axes, open questions |
| [`CONTRIBUTING.md`](CONTRIBUTING.md) | Commit, branch, review and upstream-sync rules for this line |
| [`archive/`](archive/) | Superseded documents. Useful for the measurements they contain; **not authoritative for conclusions** |

`RENDER-ADAPTATION.md` replaced `DESIGN.md` and `PERF-MALEOON.md` on 2026-08-06. Those two mixed
sound measurements with conclusions that later proved wrong, and several of the wrong ones were
load-bearing for months — so they were archived wholesale rather than edited. Do not cite them.

## Platform facts

| Item | Value |
| --- | --- |
| OS | HarmonyOS NEXT / OpenHarmony, `aarch64-linux-ohos`, musl libc |
| C++ runtime | `libc++_shared.so` from the OHOS NDK, `__n1` ABI |
| GPU driver | Huawei Maleoon, native GLES 3.2, `/vendor/lib64/passthrough/libhvgr_v200.so` |
| Reference devices | Maleoon 910, Maleoon 920 |
| Logging | `hilog` (`libhilog_ndk.z.so`), not `android/log.h` |
| EGL / GLES | resolved with `dlopen` at runtime, as on the Linux path |
| Consumer | compiled into `libglfw.so`; LWJGL resolves GL through `org.lwjgl.opengl.libname` |

The important difference from most MobileGlues deployments: on Android, launchers such as
FCL and PojavLauncher usually run MobileGlues on top of ANGLE, i.e. GLES emulated over
Vulkan. On HarmonyOS NEXT it runs directly on the vendor GLES driver. Buffer renaming,
coherent memory and synchronization have very different costs in those two setups, so
defaults that are free on ANGLE can dominate the frame here. See
[`RENDER-ADAPTATION.md`](RENDER-ADAPTATION.md).

That difference is also why "upstream does not support this" carries no weight here: upstream is
mostly running on ANGLE, where the expensive operations are cheap. The configurations upstream
declines to support are exactly the ones this platform line exists to make work.

## How it is consumed

MobileGlues is not built standalone for this platform. AMCL checks this repository out as a
git submodule at `prebuilt/mobileglues/mg_src` and compiles the source list in
`prebuilt/mobileglues/cmake-snapshot.txt` directly into `libglfw.so` with the OHOS toolchain.
LWJGL then resolves GL through `org.lwjgl.opengl.libname`.

Because the submodule is a real repository inside the build tree, the development loop has no
copy step: edit, build, then commit and push from the same directory.

```bash
# In the AMCL repository
cd prebuilt/mobileglues/mg_src
git checkout platform/ohos              # setup leaves a detached HEAD at the pinned commit
# edit, then build from the repository root
cd ../../..
DEVECO_SDK_HOME=<sdk> hvigorw assembleHap --mode module -p product=default --no-daemon
```

After pushing, update both the pin in AMCL's `deps.lock` and the superproject gitlink; AMCL's
CI verifies that the two agree, so the pinned version and the version that actually built are
never allowed to diverge.

Only the submodules this build needs are initialized, shallowly: `glslang`, `SPIRV-Cross`,
`glm`, `xxhash` and `FastSTL`. `perfetto` is deliberately skipped, since it is only compiled
with `PROFILING=ON` and cloning it costs minutes.

Cross-compilation flags used by AMCL:

```
--target=aarch64-linux-ohos --sysroot=<ohos-sysroot> -stdlib=libc++
```

## Test

Every change to this line must be validated on a real device. Emulators do not reproduce
Maleoon behaviour, and the problems that matter here are driver-specific.

```bash
hdc -t <serial> install -r entry-default-signed.hap
hdc -t <serial> shell aa start -a EntryAbility -b com.amcl.launcher
hdc -t <serial> shell pidof com.amcl.launcher
hdc -t <serial> shell hilog -x -P <pid> -e AMCL-MG -v time -v msec
```

The required scenario matrix and pass criteria are in
[`CONTRIBUTING.md`](CONTRIBUTING.md#device-verification-gate).
