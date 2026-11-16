# Task: WebGPU stress testing on the kit RTS, and shader reuse

Status: next after parity with Chrome. User asked on 2026-11-17.

## Scope

- Scene: `C:\projects\web-starter-kit-ue-bake\src\scenes\rts`, and the kit around it, as the
  heavy WebGPU workload that runs at the edge of what the hardware can do.
- Run it in the Bedrock runtime and in Chromium with the same bench tools (`bench.py --watch`,
  `chromebench.mjs` with `BENCH_URL`, visible frame rate from `windowwatch`, profiler, GC log,
  wgpu counters with `VGE_WGPU_LOG`).
- Find the bottlenecks from code and numbers (main thread, CanvasRenderer/wgpu thread,
  WebRender, GPU), fix them in the runtime.

## Shader reuse

Pipelines and shaders should not compile again on every run or every pipeline creation:

- WebGPU: wgpu pipeline cache (`wgpu::PipelineCache` / D3D12 cached PSO blobs) persisted to
  disk, keyed by device and driver; reuse identical shader modules and pipelines inside a run.
- WebGL: ANGLE program cache is already persisted (`gfx/gl/VgeShaderDiskCache.cpp`); check hit
  rates on the kit.
- Measure first-frame and pipeline creation time cold and warm, against Chromium (which keeps
  a Dawn/D3D12 pipeline cache on disk).
