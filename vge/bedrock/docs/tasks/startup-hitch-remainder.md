# Task: the remaining start-up hitch of the test game

Status: open, deferred by the user on 2026-11-17.

The longest frame of the first 5 seconds of the test game is 775-850 ms with the PGO V8 build,
against 570 ms in Chrome. Ruled out so far (details in `docs/decision-log.md`): GC (59 ms of
pauses in 6 s against 47 in Chrome), waiting on the WebGL host (the main thread holds 742 ms of CPU
in a 752 ms frame), JIT tiering (three.js alone within 5-22% of Chrome cold and warm),
postMessage (made twice faster, no change to the hitch), the V8 C++ runtime (on par with Chrome
after PGO in the runtime test).

Known gaps that still count: `Math.sin/acos/exp` 2.4-2.7x slower (V8 15.7 uses correctly rounded
llvm-libc, Chrome 149's V8 14.9 does not yet; about 3% of start-up ticks; V8 is not changed for
it).

Next step when picked up: profile the hitch with stacks through JIT frames (V8 does not register
unwind info for JIT code with the Windows unwinder in our build, so Gecko profiler stacks stop at
the first JIT frame) and compare per function with `tools/chromeprofile.mjs`.
