# Task: send the WebGPU shared texture leak fix upstream to Mozilla

Status: open, do after the performance work. User asked on 2026-11-17.

## The defect

Firefox on Windows with WebGPU presenting through shared textures (the default path,
`dom.webgpu.allow-present-without-readback`) leaks one canvas worth of video memory per
frame. Measured on https://webgpu.github.io/webgpu-samples/sample/computeBoids/:

- stock Firefox 160 (GPU process, own temp profile): 1.7 → 2.7 → 3.6 → 4.5 → 5.5 → 6.4 → 7.5 GB
  of dedicated video memory over 35 s;
- our build before the fix: same rate, about 1.1 GB every 5 s;
- readback present path (`dom.webgpu.allow-present-without-readback=false`): flat 154 MB;
- after the fix: flat 160–170 MB;
- Chromium on the same page: 58 MB.

## Cause

`gfx/wgpu_bindings/src/server.rs`, `create_texture_with_shared_texture_d3d11`, called for every
swap chain texture (every frame): it calls `ID3D12Device::OpenSharedHandle` on the D3D11 shared
texture's NT handle each time. wgpu releases the `ID3D12Resource` (its counters show 1–2 live
textures), yet each reopen of the same handle keeps video memory. The D3D11 shared textures
themselves are recycled (fewer than 50 created per run).

## Fix (in this repo)

- `dom/webgpu/SharedTextureD3D11.{h,cpp}`: `GetD3D12Resource(device, queue)` opens the shared
  texture in D3D12 once per device and keeps the `RefPtr<ID3D12Resource>`; every frame hands
  out an AddRef'd reference.
- `dom/webgpu/ipc/WebGPUParent.cpp`: `wgpu_server_get_shared_texture_d3d12_resource`.
- `gfx/wgpu_bindings/src/server.rs`: uses that resource instead of `OpenSharedHandle`.
- Same change also waits the compositor read fences on the wgpu D3D12 queue
  (`FenceD3D11::WaitOnD3D12Queue`) instead of `ID3D11DeviceContext4::Wait` on an unrelated
  D3D11 device, which blocked the WebGPU thread for about 86 % of its time (the old code
  carried `XXX deliver fences to wgpu`).
- `gfx/layers/d3d11/FenceD3D11.{h,cpp}`: opened wait fences shared between fence clones.

Commits: `ff91fbff91` (resource reuse) and the fence commit that follows it.

## How to send it

Mozilla does not take GitHub pull requests for Gecko. The route is:

1. File a bug in Bugzilla (component Core :: Graphics: WebGPU) with the measurements above and
   the stock Firefox reproduction.
2. Rebase the three parts onto mozilla-central, split into the resource reuse patch and the
   D3D12 fence wait patch, without our `vge/` code and logs.
3. Submit with `moz-phab` to Phabricator, reviewers from the WebGPU module.

Ask the user before filing or submitting anything: it publishes under their account.

## Related: stock Firefox on the WebGL game

Stock Firefox 160 on https://claybound-56949.web.app/ (WebGL 2), same machine, window visible:
dedicated video memory 2.2 → 4.0 → 5.8 → 7.7 → 9.5 → 12.2 GB over 30 s. Our build on the same
page holds 554 MB flat. The cause in Firefox is not yet identified; check it before filing,
it may be the same shared texture reopen pattern on the WebGL or compositor path.
