/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "VgeShaderDiskCache.h"

#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "GLLibraryEGL.h"
#include "prenv.h"

#ifdef XP_WIN
#  include <windows.h>
#endif

namespace mozilla::gl {

#ifdef XP_WIN

namespace {

using BlobSize = intptr_t;
using SetBlobFunc = void (*)(const void*, BlobSize, const void*, BlobSize);
using GetBlobFunc = BlobSize (*)(const void*, BlobSize, void*, BlobSize);
using SetBlobCacheFuncs = void(GLAPIENTRY*)(EGLDisplay, SetBlobFunc,
                                            GetBlobFunc);

constexpr uint64_t kMaxCacheBytes = 512ull * 1024 * 1024;
constexpr uint32_t kFormatVersion = 1;

uint64_t Fnv1a(const uint8_t* aData, size_t aSize, uint64_t aSeed) {
  uint64_t hash = aSeed;
  for (size_t i = 0; i < aSize; i++) {
    hash ^= aData[i];
    hash *= 0x100000001b3ull;
  }
  return hash;
}

class ShaderDiskCache {
 public:
  static ShaderDiskCache& Get() {
    static ShaderDiskCache cache;
    return cache;
  }

  bool Ready() const { return !mDirectory.empty(); }

  void Set(const void* aKey, BlobSize aKeySize, const void* aValue,
           BlobSize aValueSize) {
    if (aKeySize <= 0 || aValueSize <= 0) {
      return;
    }
    std::string name = NameOf(aKey, aKeySize);
    Entry entry;
    entry.key.assign(static_cast<const uint8_t*>(aKey),
                     static_cast<const uint8_t*>(aKey) + aKeySize);
    entry.value.assign(static_cast<const uint8_t*>(aValue),
                       static_cast<const uint8_t*>(aValue) + aValueSize);
    {
      std::lock_guard<std::mutex> lock(mMutex);
      mEntries.erase(name);
    }
    Write(name, entry);
  }

  BlobSize GetBlob(const void* aKey, BlobSize aKeySize, void* aValue,
                   BlobSize aValueSize) {
    if (aKeySize <= 0) {
      return 0;
    }
    std::string name = NameOf(aKey, aKeySize);
    std::lock_guard<std::mutex> lock(mMutex);
    auto it = mEntries.find(name);
    if (it == mEntries.end()) {
      Entry entry;
      if (!Read(name, entry)) {
        return 0;
      }
      it = mEntries.emplace(name, std::move(entry)).first;
    }
    const Entry& entry = it->second;
    if (entry.key.size() != size_t(aKeySize) ||
        memcmp(entry.key.data(), aKey, entry.key.size()) != 0) {
      mEntries.erase(it);
      return 0;
    }
    BlobSize size = BlobSize(entry.value.size());
    if (aValue && aValueSize >= size) {
      memcpy(aValue, entry.value.data(), entry.value.size());
      mEntries.erase(it);
    }
    return size;
  }

 private:
  struct Entry {
    std::vector<uint8_t> key;
    std::vector<uint8_t> value;
  };

  ShaderDiskCache() {
    if (PR_GetEnv("VGE_NO_SHADER_CACHE")) {
      return;
    }
    wchar_t temp[MAX_PATH];
    DWORD length = GetTempPathW(MAX_PATH, temp);
    if (!length || length >= MAX_PATH) {
      return;
    }
    std::wstring profile = std::wstring(temp) + L"vge-bedrock-profile";
    CreateDirectoryW(profile.c_str(), nullptr);
    std::wstring directory = profile + L"\\shader-cache";
    if (!CreateDirectoryW(directory.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
      return;
    }
    mDirectory = directory;
    std::thread([directory]() { Trim(directory); }).detach();
  }

  static std::string NameOf(const void* aKey, BlobSize aKeySize) {
    const auto* bytes = static_cast<const uint8_t*>(aKey);
    uint64_t first = Fnv1a(bytes, size_t(aKeySize), 0xcbf29ce484222325ull);
    uint64_t second = Fnv1a(bytes, size_t(aKeySize), 0x84222325cbf29ce4ull);
    char name[33];
    snprintf(name, sizeof(name), "%016llx%016llx",
             static_cast<unsigned long long>(first),
             static_cast<unsigned long long>(second));
    return name;
  }

  std::wstring PathOf(const std::string& aName, const wchar_t* aSuffix) const {
    return mDirectory + L"\\" + std::wstring(aName.begin(), aName.end()) +
           aSuffix;
  }

  void Write(const std::string& aName, const Entry& aEntry) const {
    std::wstring temporary = PathOf(aName, L".tmp");
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      return;
    }
    uint32_t header[3] = {kFormatVersion, uint32_t(aEntry.key.size()),
                          uint32_t(aEntry.value.size())};
    DWORD written = 0;
    bool ok = WriteFile(file, header, sizeof(header), &written, nullptr) &&
              WriteFile(file, aEntry.key.data(), DWORD(aEntry.key.size()),
                        &written, nullptr) &&
              WriteFile(file, aEntry.value.data(), DWORD(aEntry.value.size()),
                        &written, nullptr);
    CloseHandle(file);
    if (!ok) {
      DeleteFileW(temporary.c_str());
      return;
    }
    if (!MoveFileExW(temporary.c_str(), PathOf(aName, L".bin").c_str(),
                     MOVEFILE_REPLACE_EXISTING)) {
      DeleteFileW(temporary.c_str());
    }
  }

  bool Read(const std::string& aName, Entry& aEntry) const {
    std::wstring path = PathOf(aName, L".bin");
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      return false;
    }
    uint32_t header[3] = {};
    DWORD read = 0;
    bool ok = ReadFile(file, header, sizeof(header), &read, nullptr) &&
              read == sizeof(header) && header[0] == kFormatVersion;
    if (ok) {
      aEntry.key.resize(header[1]);
      aEntry.value.resize(header[2]);
      ok = ReadFile(file, aEntry.key.data(), header[1], &read, nullptr) &&
           read == header[1] &&
           ReadFile(file, aEntry.value.data(), header[2], &read, nullptr) &&
           read == header[2];
    }
    CloseHandle(file);
    if (ok) {
      FILETIME now;
      GetSystemTimeAsFileTime(&now);
      HANDLE touch = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
                                 FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
      if (touch != INVALID_HANDLE_VALUE) {
        SetFileTime(touch, nullptr, &now, &now);
        CloseHandle(touch);
      }
    }
    return ok;
  }

  static void Trim(const std::wstring& aDirectory) {
    struct Found {
      std::wstring path;
      uint64_t size;
      uint64_t time;
    };
    std::vector<Found> found;
    uint64_t total = 0;
    WIN32_FIND_DATAW data;
    HANDLE search =
        FindFirstFileW((aDirectory + L"\\*.bin").c_str(), &data);
    if (search == INVALID_HANDLE_VALUE) {
      return;
    }
    do {
      uint64_t size =
          (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
      uint64_t time = (uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32) |
                      data.ftLastWriteTime.dwLowDateTime;
      found.push_back({aDirectory + L"\\" + data.cFileName, size, time});
      total += size;
    } while (FindNextFileW(search, &data));
    FindClose(search);
    if (total <= kMaxCacheBytes) {
      return;
    }
    std::sort(found.begin(), found.end(),
              [](const Found& a, const Found& b) { return a.time < b.time; });
    for (const Found& file : found) {
      if (total <= kMaxCacheBytes) {
        break;
      }
      if (DeleteFileW(file.path.c_str())) {
        total -= file.size;
      }
    }
  }

  std::wstring mDirectory;
  std::mutex mMutex;
  std::unordered_map<std::string, Entry> mEntries;
};

void SetBlob(const void* aKey, BlobSize aKeySize, const void* aValue,
             BlobSize aValueSize) {
  ShaderDiskCache::Get().Set(aKey, aKeySize, aValue, aValueSize);
}

BlobSize GetBlob(const void* aKey, BlobSize aKeySize, void* aValue,
                 BlobSize aValueSize) {
  return ShaderDiskCache::Get().GetBlob(aKey, aKeySize, aValue, aValueSize);
}

}  // namespace

void InstallShaderDiskCache(const GLLibraryEGL& aLib, EGLDisplay aDisplay,
                            const char* aDisplayExtensions) {
  if (!aDisplayExtensions ||
      !strstr(aDisplayExtensions, "EGL_ANDROID_blob_cache")) {
    return;
  }
  auto setFuncs = reinterpret_cast<SetBlobCacheFuncs>(
      aLib.fGetProcAddress("eglSetBlobCacheFuncsANDROID"));
  if (!setFuncs || !ShaderDiskCache::Get().Ready()) {
    return;
  }
  setFuncs(aDisplay, SetBlob, GetBlob);
}

#else

void InstallShaderDiskCache(const GLLibraryEGL&, EGLDisplay, const char*) {}

#endif

}  // namespace mozilla::gl
