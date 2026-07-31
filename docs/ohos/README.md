# MobileGlues on OpenHarmony / HarmonyOS NEXT

This directory is the entry point for the OpenHarmony platform line of MobileGlues,
developed on the `platform/ohos` branch of [LZZLHY/MobileGlues](https://github.com/LZZLHY/MobileGlues)
and consumed by [AMCL](https://github.com/LZZLHY/amcl), a Minecraft: Java Edition launcher
for HarmonyOS NEXT.

| Document | Purpose |
| --- | --- |
| [`CONTRIBUTING.md`](CONTRIBUTING.md) | Commit, branch, review and upstream-sync rules for this line |
| [`DESIGN.md`](DESIGN.md) | Platform abstraction and the phased plan for the OHOS backend |
| [`PERF-MALEOON.md`](PERF-MALEOON.md) | Measurement method, counters, and the accepted/rejected experiment log |

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
defaults that are free on ANGLE can dominate the frame here. See `PERF-MALEOON.md`.

## Build

MobileGlues is not built standalone for this platform. AMCL's CMake compiles the sources
listed in `prebuilt/mobileglues/cmake-snapshot.txt` into `libglfw.so` with the OHOS
toolchain. To reproduce a build:

```bash
# In the AMCL repository, with deps.lock pointing at this branch
bash setup_deps.sh --force --mobileglues-only
DEVECO_SDK_HOME=<sdk> hvigorw assembleHap --mode module -p product=default --no-daemon
```

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
