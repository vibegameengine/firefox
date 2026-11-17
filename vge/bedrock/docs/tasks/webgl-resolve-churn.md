# Task: canvas-size texture recreated every WebGL frame

Status: canvas churn fixed on 2026-11-17; WebRender 1024x1024 churn open.

`VGE_GPU_TRACE` on the game showed `tex2d/default/rt 1264x761` created about 60 times per second
(5 GB of driver allocations in 16 s). The release stack (`VGE_ANGLE_MEM`, symbolized with
llvm-symbolizer against xul.dll) was `WebGLContext::Present` (WebGLContext.cpp, the
`mResolvedDefaultFB = nullptr` line) -> `~MozFramebuffer` -> `glDeleteTextures`. `PresentInto`
then recreated it through `GetDefaultFBForRead` for the color-managed present.

`GetDefaultFBForRead` resolves the backbuffer into `mResolvedDefaultFB` on every call, and
`Resize` drops both framebuffers, so the per-frame drop in `Present` only freed the texture.
It is removed. After: no canvas-size storage releases in 40 s of play (about 5400 before).

Still open: WebRender releases 1024x1024 textures a few hundred times in 40 s
(`Renderer::render_impl` -> `GlDevice::delete_texture`). Find which pool frees them.

Video memory: game 547 MB against Chromium 321; ANGLE/WebRender account for about 415 MB
(`VGE_GPU_TRACE` report), the rest is devices, swap chains and DComp.
