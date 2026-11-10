#include "VgeColdMemory.h"

#include <windows.h>

#include <psapi.h>
#include <stdint.h>
#include <stdio.h>

#include <algorithm>
#include <thread>
#include <unordered_map>

#include "prenv.h"

namespace vge::bedrock {

namespace {

constexpr size_t kPageSize = 4096;
constexpr size_t kBlockPages = 16;
constexpr size_t kBlockSize = kPageSize * kBlockPages;
constexpr size_t kTickPages = 512;
constexpr size_t kTickProbes = 4096;
constexpr uint32_t kTickMs = 50;
constexpr uint32_t kPassPauseMs = 1000;
constexpr uint32_t kGraceMs = 5000;
constexpr uint32_t kBaseIntervalMs = 4000;

struct Block {
  uint32_t nextProbe = 0;
  uint32_t pass = 0;
  uint16_t evicted = 0;
  bool hot = false;
};

class ColdMemoryManager {
 public:
  void Run() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    SetThreadDescription(GetCurrentThread(), L"VgeColdMemory");
    mLog = !!PR_GetEnv("VGE_MEMORY_TRIM_LOG");
    mLowMemory = CreateMemoryResourceNotification(LowMemoryResourceNotification);
    for (;;) {
      Pass();
      Sleep(kPassPauseMs);
    }
  }

 private:
  void Pass() {
    mPass++;
    mEvictedInPass = 0;
    mReturnedInPass = 0;
    BOOL low = FALSE;
    mUnderPressure = mLowMemory &&
                     QueryMemoryResourceNotification(mLowMemory, &low) && low;
    uintptr_t cursor = 0;
    MEMORY_BASIC_INFORMATION info;
    while (VirtualQuery(reinterpret_cast<void*>(cursor), &info, sizeof(info))) {
      uintptr_t base = uintptr_t(info.BaseAddress);
      uintptr_t end = base + info.RegionSize;
      if (info.State == MEM_COMMIT && info.Type == MEM_PRIVATE &&
          !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
        VisitRegion(base, end);
      }
      if (end <= cursor) {
        break;
      }
      cursor = end;
    }
    for (auto it = mBlocks.begin(); it != mBlocks.end();) {
      it = it->second.pass == mPass ? std::next(it) : mBlocks.erase(it);
    }
    if (mLog) {
      printf("coldmem pass %u blocks %zu evicted_pages %zu returned_pages %zu\n",
             mPass, mBlocks.size(), mEvictedInPass, mReturnedInPass);
      fflush(stdout);
    }
  }

  void VisitRegion(uintptr_t aBase, uintptr_t aEnd) {
    for (uintptr_t block = aBase & ~(kBlockSize - 1); block < aEnd;
         block += kBlockSize) {
      uintptr_t from = std::max(block, aBase);
      uintptr_t to = std::min(block + kBlockSize, aEnd);
      VisitBlock(from, (to - from) / kPageSize);
    }
  }

  void VisitBlock(uintptr_t aFrom, size_t aPages) {
    uint32_t now = GetTickCount();
    auto [it, inserted] = mBlocks.try_emplace(aFrom);
    Block& state = it->second;
    state.pass = mPass;
    if (inserted) {
      state.nextProbe = now + kGraceMs;
      return;
    }
    if (state.hot && !mUnderPressure) {
      return;
    }
    if (int32_t(now - state.nextProbe) < 0) {
      return;
    }
    size_t resident = CountResident(aFrom, aPages);
    Pace(mProbes, 1, kTickProbes);
    state.nextProbe = now + kBaseIntervalMs;
    if (!resident) {
      state.evicted = 0;
      return;
    }
    if (state.evicted && !mUnderPressure) {
      state.hot = true;
      state.evicted = 0;
      mReturnedInPass += resident;
      return;
    }
    VirtualUnlock(reinterpret_cast<void*>(aFrom), aPages * kPageSize);
    state.evicted = uint16_t(resident);
    mEvictedInPass += resident;
    Pace(mEvicted, resident, kTickPages);
  }

  size_t CountResident(uintptr_t aFrom, size_t aPages) {
    PSAPI_WORKING_SET_EX_INFORMATION pages[kBlockPages];
    for (size_t i = 0; i < aPages; i++) {
      pages[i].VirtualAddress = reinterpret_cast<void*>(aFrom + i * kPageSize);
      pages[i].VirtualAttributes.Flags = 0;
    }
    if (!QueryWorkingSetEx(GetCurrentProcess(), pages,
                           DWORD(aPages * sizeof(pages[0])))) {
      return 0;
    }
    size_t resident = 0;
    for (size_t i = 0; i < aPages; i++) {
      resident += pages[i].VirtualAttributes.Valid &&
                  !pages[i].VirtualAttributes.Locked;
    }
    return resident;
  }

  void Pace(size_t& aCounter, size_t aAmount, size_t aLimit) {
    aCounter += aAmount;
    if (aCounter >= aLimit) {
      aCounter = 0;
      Sleep(kTickMs);
    }
  }

  std::unordered_map<uintptr_t, Block> mBlocks;
  uint32_t mPass = 0;
  size_t mEvicted = 0;
  size_t mProbes = 0;
  size_t mEvictedInPass = 0;
  size_t mReturnedInPass = 0;
  bool mLog = false;
  bool mUnderPressure = false;
  HANDLE mLowMemory = nullptr;
};

}  // namespace

void StartColdMemoryManager() {
  if (PR_GetEnv("VGE_MEMORY_PRESSURE_DELAY") ||
      PR_GetEnv("VGE_NO_MEMORY_TRIM")) {
    return;
  }
  std::thread([] {
    static ColdMemoryManager manager;
    manager.Run();
  }).detach();
}

}  // namespace vge::bedrock
