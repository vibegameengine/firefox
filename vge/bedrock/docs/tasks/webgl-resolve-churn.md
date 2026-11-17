# Task: canvas-size texture recreated every WebGL frame

Status: open, parked on 2026-11-17 for the GC pause work.

`VGE_GPU_TRACE` on the game shows `tex2d/default/rt 1264x761` created about 60 times per second
(5 GB of driver allocations in 16 s). ANGLE releases the storage from
`MozFramebuffer::~MozFramebuffer` under `ScopedDrawCallWrapper::~ScopedDrawCallWrapper`
(WebGLContext::Clear), then `GetDefaultFBForRead` creates `mResolvedDefaultFB` again for the
color-managed present (`PresentInto`, CMS mode TaggedOnly, sRGB canvas, Display surface).

Done: the wrapper no longer drops `mResolvedDefaultFB`; the invalidate after the LUT pass now
targets the drawing buffer. The churn is still there, so another path drops the framebuffer:
find it with `VGE_ANGLE_MEM` storage-release stacks (now in TextureD3D.cpp).

Video memory: game 547 MB against Chromium 321; ANGLE/WebRender account for about 415 MB
(`VGE_GPU_TRACE` report), the rest is devices, swap chains and DComp.
