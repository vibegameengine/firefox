#include "VgeGpuTrace.h"

#include <windows.h>
#include <d3d11.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "mozilla/StackWalk.h"
#include "nsThreadUtils.h"
#include "nsWindowsDllInterceptor.h"
#include "prenv.h"

namespace vge::bedrock {

namespace {

constexpr int kFrames = 40;
constexpr GUID kTrackerGuid = {
    0x6d2c1f3a, 0x4b8e, 0x4c71, {0x9a, 0x1e, 0x5f, 0x2d, 0x7c, 0x31, 0x88, 0x40}};

struct Site {
  void* frames[kFrames] = {};
  int depth = 0;
  std::string kind;
  int64_t created = 0;
  int64_t createdBytes = 0;
  int64_t live = 0;
  int64_t liveBytes = 0;
};

std::mutex sLock;
std::unordered_map<uint64_t, Site>& Sites() {
  static std::unordered_map<uint64_t, Site> sites;
  return sites;
}

void Note(uint64_t aKey, int64_t aCount, int64_t aBytes) {
  std::lock_guard<std::mutex> guard(sLock);
  auto it = Sites().find(aKey);
  if (it == Sites().end()) {
    return;
  }
  it->second.live += aCount;
  it->second.liveBytes += aBytes;
}

uint64_t Record(const std::string& aKind, int64_t aBytes);

class Tracker final : public IUnknown {
 public:
  Tracker(uint64_t aKey, int64_t aBytes, std::string aKind)
      : mKey(aKey), mBytes(aBytes), mKind(std::move(aKind)) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID aIid, void** aOut) override {
    if (aIid == __uuidof(IUnknown)) {
      AddRef();
      *aOut = static_cast<IUnknown*>(this);
      return S_OK;
    }
    *aOut = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++mRefs; }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG refs = --mRefs;
    if (!refs) {
      Note(mKey, -1, -mBytes);
      if (mBytes >= (1 << 20)) {
        uint64_t key = Record("release:" + mKind, mBytes);
        Note(key, -1, -mBytes);
      }
      delete this;
    }
    return refs;
  }

 private:
  std::atomic<ULONG> mRefs{1};
  uint64_t mKey;
  int64_t mBytes;
  std::string mKind;
};

double BytesPerPixel(DXGI_FORMAT aFormat) {
  switch (aFormat) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_UINT:
      return 16;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
      return 8;
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_A8_UNORM:
    case DXGI_FORMAT_R8_UINT:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC7_UNORM:
      return 1;
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_D16_UNORM:
      return 2;
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC4_UNORM:
      return 0.5;
    case DXGI_FORMAT_NV12:
      return 1.5;
    default:
      return 4;
  }
}

int64_t TextureBytes(const D3D11_TEXTURE2D_DESC* aDesc) {
  double pixels = double(aDesc->Width) * aDesc->Height * aDesc->ArraySize *
                  std::max<UINT>(aDesc->SampleDesc.Count, 1);
  double bytes = pixels * BytesPerPixel(aDesc->Format);
  if (aDesc->MipLevels != 1) {
    bytes *= 4.0 / 3.0;
  }
  return int64_t(bytes);
}

std::string TextureKind(const D3D11_TEXTURE2D_DESC* aDesc) {
  std::string kind = aDesc->Usage == D3D11_USAGE_STAGING ? "tex2d/staging"
                     : aDesc->Usage == D3D11_USAGE_DYNAMIC ? "tex2d/dynamic"
                                                           : "tex2d/default";
  if (aDesc->BindFlags & D3D11_BIND_RENDER_TARGET) kind += "/rt";
  if (aDesc->BindFlags & D3D11_BIND_DEPTH_STENCIL) kind += "/ds";
  if (aDesc->MiscFlags &
      (D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE))
    kind += "/shared";
  if (aDesc->SampleDesc.Count > 1) kind += "/msaa";
  char size[48];
  snprintf(size, sizeof(size), " %ux%u", aDesc->Width, aDesc->Height);
  return kind + size;
}

uint64_t Record(const std::string& aKind, int64_t aBytes) {
  void* frames[kFrames];
  USHORT depth = RtlCaptureStackBackTrace(2, kFrames, frames, nullptr);
  uint64_t key = std::hash<std::string>()(aKind);
  for (USHORT i = 0; i < depth; i++) {
    key = key * 1099511628211ull ^ reinterpret_cast<uintptr_t>(frames[i]);
  }
  std::lock_guard<std::mutex> guard(sLock);
  Site& site = Sites()[key];
  if (!site.depth) {
    std::copy(frames, frames + depth, site.frames);
    site.depth = depth;
    site.kind = aKind;
  }
  site.created++;
  site.createdBytes += aBytes;
  site.live++;
  site.liveBytes += aBytes;
  return key;
}

void Attach(ID3D11DeviceChild* aResource, uint64_t aKey, int64_t aBytes,
            const std::string& aKind) {
  auto* tracker = new Tracker(aKey, aBytes, aKind);
  aResource->SetPrivateDataInterface(kTrackerGuid, tracker);
  tracker->Release();
}

using CreateTexture2DFn = HRESULT(STDMETHODCALLTYPE*)(
    ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*,
    ID3D11Texture2D**);
using CreateBufferFn = HRESULT(STDMETHODCALLTYPE*)(
    ID3D11Device*, const D3D11_BUFFER_DESC*, const D3D11_SUBRESOURCE_DATA*,
    ID3D11Buffer**);

std::map<void**, std::pair<CreateBufferFn, CreateTexture2DFn>>& Originals() {
  static std::map<void**, std::pair<CreateBufferFn, CreateTexture2DFn>> map;
  return map;
}

std::pair<CreateBufferFn, CreateTexture2DFn> OriginalsOf(ID3D11Device* aDevice) {
  void** vtable = *reinterpret_cast<void***>(aDevice);
  std::lock_guard<std::mutex> guard(sLock);
  return Originals()[vtable];
}

HRESULT STDMETHODCALLTYPE CreateTexture2DHook(
    ID3D11Device* aDevice, const D3D11_TEXTURE2D_DESC* aDesc,
    const D3D11_SUBRESOURCE_DATA* aData, ID3D11Texture2D** aOut) {
  HRESULT hr = OriginalsOf(aDevice).second(aDevice, aDesc, aData, aOut);
  if (SUCCEEDED(hr) && aOut && *aOut && aDesc) {
    int64_t bytes = TextureBytes(aDesc);
    std::string kind = TextureKind(aDesc);
    Attach(*aOut, Record(kind, bytes), bytes, kind);
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE CreateBufferHook(ID3D11Device* aDevice,
                                           const D3D11_BUFFER_DESC* aDesc,
                                           const D3D11_SUBRESOURCE_DATA* aData,
                                           ID3D11Buffer** aOut) {
  HRESULT hr = OriginalsOf(aDevice).first(aDevice, aDesc, aData, aOut);
  if (SUCCEEDED(hr) && aOut && *aOut && aDesc) {
    int64_t bytes = aDesc->ByteWidth;
    std::string kind = aDesc->Usage == D3D11_USAGE_STAGING ? "buffer/staging"
                       : aDesc->Usage == D3D11_USAGE_DYNAMIC ? "buffer/dynamic"
                                                             : "buffer/default";
    Attach(*aOut, Record(kind, bytes), bytes, kind);
  }
  return hr;
}

void PatchDevice(ID3D11Device* aDevice) {
  void** vtable = *reinterpret_cast<void***>(aDevice);
  {
    std::lock_guard<std::mutex> guard(sLock);
    if (Originals().count(vtable)) {
      return;
    }
    Originals()[vtable] = {reinterpret_cast<CreateBufferFn>(vtable[3]),
                           reinterpret_cast<CreateTexture2DFn>(vtable[5])};
  }
  DWORD old = 0;
  VirtualProtect(&vtable[3], sizeof(void*) * 3, PAGE_READWRITE, &old);
  vtable[3] = reinterpret_cast<void*>(&CreateBufferHook);
  vtable[5] = reinterpret_cast<void*>(&CreateTexture2DHook);
  VirtualProtect(&vtable[3], sizeof(void*) * 3, old, &old);
}

using CreateDeviceFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE,
                                        UINT, const D3D_FEATURE_LEVEL*, UINT,
                                        UINT, ID3D11Device**,
                                        D3D_FEATURE_LEVEL*,
                                        ID3D11DeviceContext**);

mozilla::WindowsDllInterceptor sD3D11;
mozilla::WindowsDllInterceptor::FuncHookType<CreateDeviceFn> sCreateDeviceStub;

HRESULT WINAPI CreateDeviceHook(IDXGIAdapter* aAdapter, D3D_DRIVER_TYPE aType,
                                HMODULE aSoftware, UINT aFlags,
                                const D3D_FEATURE_LEVEL* aLevels, UINT aCount,
                                UINT aSdk, ID3D11Device** aDevice,
                                D3D_FEATURE_LEVEL* aLevel,
                                ID3D11DeviceContext** aContext) {
  HRESULT hr = sCreateDeviceStub(aAdapter, aType, aSoftware, aFlags, aLevels,
                                 aCount, aSdk, aDevice, aLevel, aContext);
  if (SUCCEEDED(hr) && aDevice && *aDevice) {
    Record("device", 0);
    PatchDevice(*aDevice);
  }
  return hr;
}

std::string Symbol(void* aAddress) {
  MozCodeAddressDetails details;
  MozDescribeCodeAddress(aAddress, &details);
  if (details.function[0]) {
    return details.function;
  }
  char buffer[160];
  snprintf(buffer, sizeof(buffer), "%s+0x%llx", details.library,
           static_cast<unsigned long long>(details.loffset));
  return buffer;
}

void WriteReport(const char* aPath) {
  std::vector<Site> sites;
  {
    std::lock_guard<std::mutex> guard(sLock);
    for (const auto& [key, site] : Sites()) {
      sites.push_back(site);
    }
  }
  std::sort(sites.begin(), sites.end(), [](const Site& a, const Site& b) {
    return a.liveBytes > b.liveBytes;
  });
  int64_t total = 0;
  for (const Site& site : sites) {
    total += site.liveBytes;
  }
  FILE* file = fopen(aPath, "w");
  if (!file) {
    return;
  }
  fprintf(file, "live_mb %.1f sites %zu\n", total / 1048576.0, sites.size());
  for (const Site& site : sites) {
    if (site.liveBytes < (256 << 10) && site.kind != "device" &&
        site.created < 100) {
      continue;
    }
    fprintf(file, "live_mb %.2f live %lld created %lld created_mb %.1f %s <",
            site.liveBytes / 1048576.0, static_cast<long long>(site.live),
            static_cast<long long>(site.created), site.createdBytes / 1048576.0,
            site.kind.c_str());
    for (int i = 0; i < site.depth && i < 32; i++) {
      fprintf(file, " %s <", Symbol(site.frames[i]).c_str());
    }
    fprintf(file, "\n");
  }
  fclose(file);
}

}  // namespace

void InstallGpuTrace() {
  if (!PR_GetEnv("VGE_GPU_TRACE")) {
    return;
  }
  LoadLibraryW(L"d3d11.dll");
  sD3D11.Init("d3d11.dll");
  sCreateDeviceStub.Set(sD3D11, "D3D11CreateDevice", &CreateDeviceHook);
}

void ScheduleGpuTraceReport() {
  const char* path = PR_GetEnv("VGE_GPU_TRACE");
  if (!path) {
    return;
  }
  const char* delay = PR_GetEnv("VGE_GPU_TRACE_DELAY");
  std::string target = path;
  NS_DelayedDispatchToCurrentThread(
      NS_NewRunnableFunction("VgeGpuTraceReport",
                             [target] { WriteReport(target.c_str()); }),
      (delay ? atoi(delay) : 20) * 1000);
}

}  // namespace vge::bedrock
