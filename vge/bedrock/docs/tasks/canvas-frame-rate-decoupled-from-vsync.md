# Task: WebGL and WebGPU canvases run apart from the UI vsync

Status: open, set by the user on 2026-11-17 for later.

What the user asked for:

- The page UI, layout and the JS that drives it stay on vsync by default, as now (Gecko refresh
  driver on the hardware vblank source; `vge/bedrock` sets nothing for it).
- WebGL and especially WebGPU canvases live apart from that setting and give the maximum frame
  rate, not capped by the display refresh.
- An API to control this: per canvas or per page, switch between vsync and uncapped and set a
  target rate.

Notes for whoever picks it up:

- Today `requestAnimationFrame`, painting and canvas presentation all ride the refresh driver,
  so a canvas cannot outrun the display.
- Read first how the reference engines do it before designing: Chromium's
  `--disable-gpu-vsync` / `--disable-frame-rate-limit` paths and how its compositor presents
  canvases, Gecko's `layout.frame_rate` (0 = paint as fast as possible, positive = software
  timer) and its `VsyncSource`, and WebGPU `GPUCanvasContext` presentation.
- The UI must keep its vsync pacing while a canvas runs uncapped: no tearing of page content,
  no extra UI frames.

Discussion with the user on 2026-11-17 (see `docs/decision-log.md`, video memory):

- Frames above the display refresh are wasted work on their own. The real value is: the canvas
  presenting at its own pace (variable refresh G-Sync/FreeSync, monitors faster than the UI rate,
  a game that holds a steady rate below refresh), lower input latency (`desynchronized: true`,
  frame timed to scan-out rather than extra frames), and an uncapped mode for measuring.
- Modes the API should offer per canvas: full vsync (default), half rate (interval 2: 60 on
  120 Hz, smoother than an uneven 80), adaptive vsync, variable refresh, a fixed cap, uncapped
  (measurement).
- Half rate and fixed caps need no new device: the canvas is handed frames every Nth refresh.
  Variable refresh, adaptive and uncapped need the canvas to present itself: its own swap chain
  as a DirectComposition visual, and with it its own D3D11 device.
- This fits `webgl-shared-compositor-device.md`: ordinary canvases share the compositor device;
  only a canvas switched into a self-presenting mode gets its own device and swap chain, for as
  long as the mode is on.
