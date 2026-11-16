# Embedding the Bedrock runtime (CEF-style)

Reference read before design: CEF 128 headers in
`UE_5.8/Engine/Source/ThirdParty/CEF3/cef_binary_128.4.13.../include`
(`cef_render_handler.h`, `cef_browser.h`, `internal/cef_types_win.h`).

## What CEF offers a host

| CEF | Purpose |
|---|---|
| `CefInitialize`, `CefDoMessageLoopWork`, `CefShutdown` | process start, host-driven pump, stop |
| `CefBrowserHost::CreateBrowser` with `CefWindowInfo` | windowed (child of host HWND) or windowless browser |
| `CefRenderHandler::GetViewRect`, `GetScreenInfo` | host tells size and device scale |
| `OnPaint(type, dirtyRects, buffer, w, h)` | BGRA buffer, upper-left origin, when shared textures are off |
| `OnAcceleratedPaint(type, dirtyRects, info)` | D3D11 shared texture handle from a pool, no keyed mutex; host must reopen and copy inside the callback |
| `SendMouseClickEvent`, `SendMouseMoveEvent`, `SendMouseWheelEvent`, `SendKeyEvent`, `SendTouchEvent`, `ImeSetComposition` | input from the host |
| `WasResized`, `WasHidden`, `NotifyScreenInfoChanged`, `SetFocus`, `Invalidate`, `SetWindowlessFrameRate`, `SendExternalBeginFrame` | view state and frame pacing |
| `CefFrame::LoadURL`, `ExecuteJavaScript`, process messages, `CefV8Handler` | navigation and the JS bridge |

CEF runs a browser process, a GPU process and renderer processes. Every frame of a
windowless browser crosses processes, and the accelerated path still forces the host to
copy the texture inside the callback.

## Bedrock embedding API

One process, one DLL (`VgeBedrock.dll`, the current `xul.dll` plus the runtime), a plain C
header `vge_bedrock.h`. All calls are made on the host's UI thread; the runtime never
creates windows of its own in windowless mode.

```c
typedef struct VgeBedrockSettings {
  const wchar_t* profile_dir;
  const wchar_t* cache_dir;
  int multi_threaded_pump;
  int memory_budget_mb;
} VgeBedrockSettings;

int  vge_bedrock_initialize(const VgeBedrockSettings*);
void vge_bedrock_pump(void);
void vge_bedrock_shutdown(void);

typedef struct VgeView VgeView;

typedef struct VgeFrameInfo {
  void*    shared_texture;
  void*    shared_fence;
  uint64_t fence_value;
  int      width, height;
  const int* dirty_rects;
  int      dirty_rect_count;
} VgeFrameInfo;

typedef struct VgeViewCallbacks {
  void* user;
  void (*paint)(void* user, const void* bgra, int width, int height,
                const int* dirty_rects, int dirty_rect_count);
  void (*accelerated_paint)(void* user, const VgeFrameInfo* frame);
  void (*title_changed)(void* user, const char* utf8);
  void (*load_state)(void* user, int loading, int http_status);
  void (*console)(void* user, int level, const char* utf8);
  void (*host_call)(void* user, uint64_t call_id, const char* name,
                    const char* json_args);
  void (*cursor_changed)(void* user, int cursor);
} VgeViewCallbacks;

VgeView* vge_view_create_windowed(void* parent_hwnd, int x, int y, int w, int h,
                                  const VgeViewCallbacks*);
VgeView* vge_view_create_windowless(int w, int h, float scale, int accelerated,
                                    void* d3d11_device, const VgeViewCallbacks*);
void vge_view_destroy(VgeView*);

void vge_view_load_url(VgeView*, const char* utf8_url);
void vge_view_load_html(VgeView*, const char* utf8_html, const char* base_url);
void vge_view_go(VgeView*, int delta);
void vge_view_reload(VgeView*);

void vge_view_resize(VgeView*, int w, int h, float scale);
void vge_view_set_hidden(VgeView*, int hidden);
void vge_view_set_focus(VgeView*, int focused);
void vge_view_set_frame_rate(VgeView*, int fps);
void vge_view_begin_frame(VgeView*);

void vge_view_mouse_move(VgeView*, int x, int y, int modifiers);
void vge_view_mouse_button(VgeView*, int x, int y, int button, int down,
                           int click_count, int modifiers);
void vge_view_mouse_wheel(VgeView*, int x, int y, float dx, float dy,
                          int modifiers);
void vge_view_key(VgeView*, int type, int windows_key_code, int native_key_code,
                  uint32_t character, int modifiers);
void vge_view_ime_composition(VgeView*, const char* utf8, int cursor);
void vge_view_ime_commit(VgeView*, const char* utf8);

void vge_view_execute_script(VgeView*, const char* utf8_source);
void vge_view_host_reply(VgeView*, uint64_t call_id, const char* json_result,
                         int is_error);
void vge_view_post_message(VgeView*, const char* json);
```

Page side: `window.vge.call(name, args)` returns a promise resolved by
`vge_view_host_reply`; `window.vge.onmessage` receives `vge_view_post_message`.

## Where Bedrock goes further than CEF

- One process: no GPU or renderer process, no IPC per frame, and the memory of one process
  instead of five to ten.
- Accelerated windowless frames are handed over with an explicit D3D11 fence and value, so
  the host can sample the texture directly on its own device without copying and without a
  keyed mutex. The texture stays valid until the next `accelerated_paint`.
- Host-driven frames: with `vge_view_set_frame_rate(view, 0)` the page renders only when the
  host calls `vge_view_begin_frame`, so a game engine can tie page frames to its own frame.
- Memory budget: the runtime returns cold private pages to the OS and follows the OS low
  memory signal; `memory_budget_mb` caps it further.

## Build stages (each part of the final system)

1. Windowed view inside a host HWND (the current window path, re-parented).
2. C API DLL and header, host pump, callbacks for title, load state and console.
3. JS bridge (`window.vge`) on the V8 jsapi layer.
4. Windowless rendering: WebRender renders into a texture owned by the runtime on the host's
   D3D11 device; `accelerated_paint` hands it over with a fence; the CPU path reads back
   only dirty rects for `paint`.
5. Input and IME injection for windowless views.
6. Sample host and a UE integration that replaces CEF in the VGE kit.
