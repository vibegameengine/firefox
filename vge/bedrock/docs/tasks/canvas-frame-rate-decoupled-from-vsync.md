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
