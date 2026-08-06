# Archived documents

Superseded on 2026-08-06 by [`../RENDER-ADAPTATION.md`](../RENDER-ADAPTATION.md).

**Do not cite anything in this directory as authority, and do not use it as a premise.**

These files are kept for one reason: they contain device measurements that were expensive to obtain
and would be expensive to reproduce. Read them for numbers.

Do not read them for conclusions. They interleave sound measurements with reasoning that later proved
wrong, and the wrong parts were not obviously wrong — several were load-bearing for months and each
one sent a round of work in the wrong direction. The three that did the most damage:

* **"The vanilla terrain uber buffer is fully uploaded before anything is drawn from it."** The
  bytecode order is the exact reverse. Every safety argument built on this was void, which is why
  vanilla flickered too.
* **"Older versions never reach this path."** True only of 1.21.x. 26.1.2 has the same buffer
  architecture, the same sizes and the same upload route as 26.2.
* **A whole-process CPU profile putting this library at 0.65 % of cycles**, used to argue it could not
  affect the frame rate. The profile was real; it had been taken on a build whose upload path skipped
  synchronization. A later same-device A/B changing only this library moved presents per second from
  54.1 to 36.8.

Any number you take from here must be re-checked against the configuration it was measured on. If the
build, the Minecraft version, the mod set or the call site differs, the number does not transfer —
that failure mode is itself one of the lessons, and it is recorded as a standing rule in §5.3 of the
replacement document.
