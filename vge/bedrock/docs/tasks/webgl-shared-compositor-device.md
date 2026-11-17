# Task: WebGL on the compositor's D3D11 device

Status: open, deferred by the user on 2026-11-17 (WebGPU first).

Why: about 112 MB of the video memory gap to Chrome (458 MB against 323 MB on the test game) sits
outside D3D11 resources. We run three D3D11 devices (WebGL's ANGLE display, the WebRender
compositor, the content device for Direct2D), each costing about 11 MB on this GPU before ANGLE's
own buffers, plus a copy of every WebGL frame between devices. Chrome runs WebGL, compositing and
presentation on one ANGLE device in one GPU thread.

What is known (details and numbers in `docs/decision-log.md`, video memory):

- ANGLE in our build uses per-context mutexes (`ANGLE_ENABLE_CONTEXT_MUTEX`,
  `ANGLE_ENABLE_SHARE_CONTEXT_LOCK`), and contexts in different share groups do not lock each
  other, but the D3D11 back end keeps one state per device and resyncs it on make-current
  (`Context11::onMakeCurrent`). Two threads on one device are unsafe; it needs one GPU thread
  (Chrome's model) or one lock around every GL call of both.
- Gecko keeps WebGL and WebRender displays apart on purpose (`RenderThread.cpp`, comment about
  thread safety).
- Cost of serialising measured on steady play: per 120 Hz frame the WebGL host thread spends
  0.81 ms CPU and the WebRender renderer 0.56 ms (+0.26 ms backend), about 1.4 ms of 8.3 ms.
- The content device carries Direct2D and canvas 2D; dropping it moves canvas 2D to the CPU, so
  it goes onto the shared device too, not away.
- A canvas in a self-presenting frame-rate mode keeps its own device and swap chain
  (`canvas-frame-rate-decoupled-from-vsync.md`).

Done when: the test game runs with one D3D11 device for WebGL, compositing and canvas 2D, frame
times unchanged, and the video memory gap is measured again.
