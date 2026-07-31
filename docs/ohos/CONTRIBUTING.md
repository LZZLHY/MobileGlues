# Contributing to the OpenHarmony line

These are the rules for the `platform/ohos` branch. They exist so that any commit here can
be proposed to upstream MobileGlues without rewriting, and so that platform work stays
reviewable as it grows.

## Branch model

| Branch | Purpose |
| --- | --- |
| `main` | Untouched mirror of upstream `MobileGL-Dev/MobileGlues`. Never commit here. |
| `platform/ohos` | The OpenHarmony line. Must always compile and always be device-verified. |
| `wip/<topic>` | Investigations, instrumentation, throwaway experiments. May be broken. |
| `upstream/<topic>` | A single change rebased onto upstream `main`, used to open a PR upstream. |

This repository is consumed as a git submodule inside the launcher, so a commit here is only
half of a change: the consuming repository pins the exact commit and must be updated in the
same breath. Never leave work only in the working tree, and never leave the pin behind.

`platform/ohos` follows upstream's own naming for platform work (`platform/linux`).
Experiments never land on it; they live on `wip/*` so that bisecting the platform line
stays meaningful.

## Commit format

Upstream's convention, inferred from its history and its accepted pull requests:

```
[Tag] (module): summary in the imperative mood

Why the change is needed, what the driver or platform actually does, and what breaks
without it. Reference concrete symbols, enums and call paths.

Platform: OpenHarmony / HarmonyOS NEXT
Tested-on: <OS version>, <GPU>, <driver path or version>
```

* `Tag` is one of `Feat`, `Fix`, `Improvement`, `Chore`, `Docs`. Combine with `|` when a
  change genuinely spans categories, as upstream does: `[Feat|Fix] (...)`.
* `module` is the area, normally the source directory or file group: `glx`, `texture`,
  `settings`, `buffer`, `DSA`, `CI`, `README`. Combine with `|` for two areas.
* One concern per commit. A commit that fixes a depth format and also advertises an
  extension is two commits.
* Explain the driver behaviour, not the diff. The diff is already in the commit.

## Hard rules

1. No binary artifacts. No `.a`, `.so`, `.o`, `.hap`, no prebuilt third-party libraries.
   Build inputs only.
2. No platform `#ifdef` sprawl in shared logic. Platform differences belong behind a
   narrow interface; see [`DESIGN.md`](DESIGN.md).
3. Do not change Android or Apple behaviour to fix OpenHarmony. Platform-specific policy
   is selected at runtime or configure time, and other platforms keep their existing path.
4. Keep upstream formatting. `MobileGlues-cpp/.clang-format` is authoritative; only the
   lines you touch need to be clean.
5. Never reintroduce a downstream patch series for changes that belong on this branch.
   AMCL's `prebuilt/mobileglues/patches/series` stays empty by design.
6. No secrets, device serials, or account identifiers in commits, logs or docs.

## Performance work

Performance changes carry a higher bar than correctness fixes, because on this platform
they are the ones most likely to trade correctness for frame rate.

* State the hypothesis, then the measurement that supports it. "Feels smoother" is not a
  result.
* Report per-second aggregates, not single frames: call counts, total microseconds, and
  the maximum, for each affected path.
* A change that improves frame pacing while corrupting geometry is a regression, not a
  trade-off. Terrain correctness is not negotiable.
* Record rejected approaches with their numbers in [`PERF-MALEOON.md`](PERF-MALEOON.md).
  Knowing what failed and by how much is the most reusable part of this work.

## Device verification gate

A change to `platform/ohos` is not done until all of the following pass on hardware, in
the same world, with the same view distance and the same mod set:

| Scenario | Duration | Checks |
| --- | --- | --- |
| Idle | 15 s | frame pacing, no stalls, counters stable |
| Look around in place | 15 s | no stalls while the camera rotates |
| Horizontal movement | 15 s | no multi-hundred-millisecond waits, no chunk pop-in stalls |
| Vertical flight | 15 s | same as horizontal, plus arena growth and reallocation |
| Terrain correctness | full pass | no black or missing chunks, no flicker, HUD, hand and entities intact |

Both vanilla and Sodium must be exercised, because they use entirely different upload
paths: vanilla goes through DSA `glNamedBufferSubData`, Sodium through a persistent
staging buffer plus `glCopyBufferSubData`.

## Syncing with upstream

```bash
git fetch upstream
git checkout platform/ohos
git rebase upstream/main          # or merge, if the conflict surface is large
git push --force-with-lease origin platform/ohos
```

After syncing, update AMCL's `deps.lock` `[mobileglues]` `commit` and `base_commit`, rerun
`setup_deps.sh --force --mobileglues-only`, rerun `scripts/check-mg-snapshot.mjs`, rebuild,
and rerun the device gate. A sync is not complete until the gate passes again.

## Proposing a change upstream

1. Branch from upstream `main`: `git checkout -b upstream/<topic> upstream/main`.
2. Cherry-pick the single commit. Drop the `Platform:`/`Tested-on:` trailers only if they
   are not relevant to the platform-neutral form of the change.
3. Confirm the change does not depend on anything else in this line.
4. Open the pull request with the driver-level reasoning, the platform it was found on, and
   how it was verified. Upstream accepts platform fixes with that shape; recent examples are
   the iOS depth-format and symbol-resolution pull requests.
