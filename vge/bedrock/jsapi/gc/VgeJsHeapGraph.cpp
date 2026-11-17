#include "VgeJsGcInternal.h"

#include <algorithm>

#include "mozilla/ScopeExit.h"
#include "js/Proxy.h"
#include "js/Wrapper.h"
#include "js/friend/CycleCollector.h"

namespace vge::jsapi {
extern std::atomic<int64_t> gLiveStringCells;
extern std::atomic<int64_t> gCreatedStringCells;
}

namespace vge::jsapi::gc {

namespace {

class CellCollector final : public JS::CallbackTracer {
 public:
  CellCollector(JSRuntime* aRuntime, std::vector<JS::GCCellPtr>& aOut)
      : JS::CallbackTracer(aRuntime, JS::TracerKind::Callback,
                           JS::WeakMapTraceAction::TraceKeysAndValues),
        mOut(aOut) {}

  bool onChild(JS::GCCellPtr aThing, const char* aName) override {
    if (aThing && IsVgeCell(aThing.asCell())) {
      mOut.push_back(aThing);
    }
    return true;
  }

 private:
  std::vector<JS::GCCellPtr>& mOut;
};

class HeapGraph {
 public:
  HeapGraph(JSContext* aCx, const v8::HeapSnapshot* aSnapshot)
      : mCx(aCx),
        mProfiler(aCx->isolate->GetHeapProfiler()),
        mSnapshot(aSnapshot) {
    int count = mSnapshot->GetNodesCount();
    mNodes.reserve(count);
    mIndex.reserve(count);
    for (int i = 0; i < count; i++) {
      const v8::HeapGraphNode* node = mSnapshot->GetNode(i);
      mIndex.emplace(node, uint32_t(i));
      mNodes.push_back(node);
    }
    mCellState.assign(count, 0);
    mCells.assign(count, JS::GCCellPtr());
    mStamp.assign(count, 0);
    IndexCells();
  }

  void IndexCells() {
    v8::Isolate* isolate = mCx->isolate;
    v8::HandleScope scope(isolate);
    for (ObjectCell* cell : mCx->runtime->objectCells) {
      if (!CellHasV8(&cell->tail)) {
        continue;
      }
      v8::Local<v8::Value> value = CellV8(isolate, &cell->tail);
      JS::GCCellPtr thing(cell->asObject());
      v8::SnapshotObjectId id = mProfiler->GetObjectId(value);
      if (id != v8::HeapProfiler::kUnknownObjectId) {
        mCellById.emplace(id, thing);
      }
      const JSClass* clasp = cell->shape ? cell->shape->base.clasp : nullptr;
      if (clasp && (clasp->flags & JSCLASS_IS_GLOBAL) && value->IsObject()) {
        v8::Local<v8::Value> inner = v8::InnerGlobalOfProxy(isolate, value);
        v8::SnapshotObjectId innerId = inner.IsEmpty()
                                           ? v8::HeapProfiler::kUnknownObjectId
                                           : mProfiler->GetObjectId(inner);
        if (innerId != v8::HeapProfiler::kUnknownObjectId) {
          mCellById.emplace(innerId, thing);
        }
      }
    }
  }

  size_t size() const { return mNodes.size(); }

  int32_t IndexOfValue(v8::Local<v8::Value> aValue) {
    if (aValue.IsEmpty()) {
      return -1;
    }
    v8::SnapshotObjectId id = mProfiler->GetObjectId(aValue);
    if (id == v8::HeapProfiler::kUnknownObjectId) {
      return -1;
    }
    const v8::HeapGraphNode* node = mSnapshot->GetNodeById(id);
    if (!node) {
      return -1;
    }
    auto it = mIndex.find(node);
    return it == mIndex.end() ? -1 : int32_t(it->second);
  }

  int32_t IndexOfCell(JS::GCCellPtr aThing) {
    v8::HandleScope scope(mCx->isolate);
    return IndexOfValue(HandleOf(mCx->isolate, aThing));
  }

  JS::GCCellPtr CellAt(uint32_t aIndex) {
    if (mCellState[aIndex]) {
      return mCells[aIndex];
    }
    mCellState[aIndex] = 1;
    const v8::HeapGraphNode* node = mNodes[aIndex];
    v8::HeapGraphNode::Type type = node->GetType();
    if (type != v8::HeapGraphNode::kObject &&
        type != v8::HeapGraphNode::kClosure &&
        type != v8::HeapGraphNode::kRegExp &&
        type != v8::HeapGraphNode::kNative) {
      return JS::GCCellPtr();
    }
    auto found = mCellById.find(node->GetId());
    if (found != mCellById.end()) {
      mCells[aIndex] = found->second;
    }
    return mCells[aIndex];
  }

  int32_t RootIndex() const {
    auto it = mIndex.find(mSnapshot->GetRoot());
    return it == mIndex.end() ? -1 : int32_t(it->second);
  }

  size_t ShallowSize(uint32_t aIndex) const {
    return mNodes[aIndex]->GetShallowSize();
  }

  void CellChildren(uint32_t aStart, std::vector<uint32_t>& aOut) {
    mGeneration++;
    std::vector<uint32_t> stack;
    stack.push_back(aStart);
    mStamp[aStart] = mGeneration;
    while (!stack.empty()) {
      uint32_t n = stack.back();
      stack.pop_back();
      const v8::HeapGraphNode* node = mNodes[n];
      int children = node->GetChildrenCount();
      for (int i = 0; i < children; i++) {
        const v8::HeapGraphEdge* edge = node->GetChild(i);
        if (edge->GetType() == v8::HeapGraphEdge::kWeak) {
          continue;
        }
        auto it = mIndex.find(edge->GetToNode());
        if (it == mIndex.end()) {
          continue;
        }
        uint32_t m = it->second;
        if (mStamp[m] == mGeneration) {
          continue;
        }
        mStamp[m] = mGeneration;
        if (CellAt(m)) {
          aOut.push_back(m);
          continue;
        }
        stack.push_back(m);
      }
    }
  }

 private:
  JSContext* mCx;
  v8::HeapProfiler* mProfiler;
  const v8::HeapSnapshot* mSnapshot;
  std::vector<const v8::HeapGraphNode*> mNodes;
  std::unordered_map<const v8::HeapGraphNode*, uint32_t> mIndex;
  std::vector<uint8_t> mCellState;
  std::vector<JS::GCCellPtr> mCells;
  std::vector<uint32_t> mStamp;
  std::unordered_map<v8::SnapshotObjectId, JS::GCCellPtr> mCellById;
  uint32_t mGeneration = 0;
};

void Color(GcState& aState, JS::GCCellPtr aThing, bool aGray) {
  js::gc::Cell* cell = aThing.asCell();
  if (aGray) {
    SetColorBit(cell, js::gc::ColorBit::BlackBit, false);
    SetColorBit(cell, js::gc::ColorBit::GrayOrBlackBit, true);
    aState.grayCells.push_back(aThing);
  } else {
    SetColorBit(cell, js::gc::ColorBit::BlackBit, true);
  }
  aState.coloredCells.push_back(aThing);
}

void ClearColor(JS::GCCellPtr aThing) {
  SetColorBit(aThing.asCell(), js::gc::ColorBit::BlackBit, false);
  SetColorBit(aThing.asCell(), js::gc::ColorBit::GrayOrBlackBit, false);
}

uint64_t GcCount(GcState& aState) {
  return aState.majorNumber + aState.minorNumber;
}

class GrayRootTracer final : public JS::CallbackTracer {
 public:
  GrayRootTracer(JSRuntime* aRuntime, v8::GrayRootMarker& aMarker)
      : JS::CallbackTracer(aRuntime, JS::TracerKind::Callback,
                           JS::WeakMapTraceAction::TraceKeysAndValues),
        mMarker(aMarker) {}

  bool onChild(JS::GCCellPtr aThing, const char* aName) override {
    if (aThing && IsVgeCell(aThing.asCell())) {
      MarkManagedCell(TailOf(aThing.asCell(), aThing.kind()), 5);
    }
    CellTail* tail = LiveTail(aThing);
    if (tail) {
      MarkCellWith(mMarker, tail);
    }
    return true;
  }

 private:
  v8::GrayRootMarker& mMarker;
};

class AddressIndex final {
 public:
  static constexpr uint32_t kNone = 0xFFFFFFFFu;

  explicit AddressIndex(size_t aExpected) {
    size_t capacity = 16;
    while (capacity < aExpected * 2) {
      capacity <<= 1;
    }
    mSlots.assign(capacity, Slot{0, kNone});
    mMask = capacity - 1;
  }

  bool Insert(uintptr_t aKey, uint32_t aValue) {
    for (size_t i = Hash(aKey);; i = (i + 1) & mMask) {
      Slot& slot = mSlots[i];
      if (slot.key == aKey) {
        return false;
      }
      if (slot.key == 0) {
        slot = Slot{aKey, aValue};
        return true;
      }
    }
  }

  uint32_t Find(uintptr_t aKey) const {
    for (size_t i = Hash(aKey);; i = (i + 1) & mMask) {
      const Slot& slot = mSlots[i];
      if (slot.key == aKey) {
        return slot.value;
      }
      if (slot.key == 0) {
        return kNone;
      }
    }
  }

 private:
  struct Slot {
    uintptr_t key;
    uint32_t value;
  };

  size_t Hash(uintptr_t aKey) const {
    return static_cast<size_t>((aKey >> 3) * 0x9E3779B97F4A7C15ull >> 20) &
           mMask;
  }

  std::vector<Slot> mSlots;
  size_t mMask = 0;
};

struct CellSnapshot {
  JS::GCCellPtr thing;
  uintptr_t address;
  uintptr_t inner;
};

struct GrayInput {
  std::vector<std::vector<uintptr_t>> objectChunks;
  std::vector<std::vector<v8::GrayMarkingEdge>> edgeChunks;
  std::vector<uintptr_t> tables;
  std::vector<v8::GrayMarkingEphemeron> ephemerons;
  std::vector<v8::GrayMarkingWeakRef> weakRefs;
  std::vector<CellSnapshot> cells;
};

JSObject* ObjectOf(const CellSnapshot& aCell) {
  return reinterpret_cast<JSObject*>(aCell.thing.asCell());
}

std::unordered_map<js::gc::Cell*, std::vector<JS::GCCellPtr>> ExpandCellEdges(
    const std::vector<JS::GCCellPtr>& cellAt,
    const std::vector<uint32_t>& offsets,
    const std::vector<uint32_t>& targets) {
  std::unordered_map<js::gc::Cell*, std::vector<JS::GCCellPtr>> result;
  size_t nodeCount = cellAt.size();
  std::vector<uint32_t> stamp(nodeCount, 0);
  std::vector<uint32_t> stack;
  uint32_t generation = 0;
  for (uint32_t start = 0; start < nodeCount; start++) {
    JS::GCCellPtr from = cellAt[start];
    if (!from) {
      continue;
    }
    generation++;
    stamp[start] = generation;
    stack.push_back(start);
    std::vector<JS::GCCellPtr> edges;
    while (!stack.empty()) {
      uint32_t n = stack.back();
      stack.pop_back();
      for (uint32_t e = offsets[n]; e < offsets[n + 1]; e++) {
        uint32_t m = targets[e];
        if (stamp[m] == generation) {
          continue;
        }
        stamp[m] = generation;
        if (!cellAt[m]) {
          stack.push_back(m);
        } else if (cellAt[m] != from) {
          edges.push_back(cellAt[m]);
        }
      }
    }
    if (!edges.empty()) {
      result[from.asCell()] = std::move(edges);
    }
  }
  return result;
}

GrayResult BuildGrayResult(GrayInput&& aIn) {
  mozilla::TimeStamp start = mozilla::TimeStamp::Now();
  GrayResult out;
  constexpr uint32_t kNone = AddressIndex::kNone;

  size_t recorded = 0;
  for (const std::vector<uintptr_t>& chunk : aIn.objectChunks) {
    recorded += chunk.size();
  }
  size_t recordedEdges = 0;
  for (const std::vector<v8::GrayMarkingEdge>& chunk : aIn.edgeChunks) {
    recordedEdges += chunk.size();
  }
  AddressIndex gray(recorded);
  uint32_t nodeCount = 0;
  for (const std::vector<uintptr_t>& chunk : aIn.objectChunks) {
    for (uintptr_t address : chunk) {
      if (gray.Insert(address, nodeCount)) {
        nodeCount++;
      }
    }
  }
  aIn.objectChunks = {};

  const bool needCells = !aIn.ephemerons.empty() || !aIn.weakRefs.empty();
  AddressIndex cellIndex(needCells ? aIn.cells.size() * 2 : 0);
  std::vector<JS::GCCellPtr> cellAt(nodeCount);
  for (uint32_t i = 0; i < aIn.cells.size(); i++) {
    const CellSnapshot& cell = aIn.cells[i];
    uint32_t n = gray.Find(cell.address);
    if (n != kNone) {
      out.gray.push_back(cell.thing);
      if (!cellAt[n]) {
        cellAt[n] = cell.thing;
      }
    }
    if (cell.inner) {
      uint32_t m = gray.Find(cell.inner);
      if (m != kNone && !cellAt[m]) {
        cellAt[m] = cell.thing;
      }
    }
    if (needCells) {
      cellIndex.Insert(cell.address, i);
      if (cell.inner) {
        cellIndex.Insert(cell.inner, i);
      }
    }
  }

  AddressIndex tables(aIn.tables.size());
  for (uint32_t i = 0; i < aIn.tables.size(); i++) {
    tables.Insert(aIn.tables[i], i);
  }
  std::vector<uintptr_t> tableOwner(aIn.tables.size(), 0);
  std::vector<uint32_t> fromIndex;
  std::vector<uint32_t> toIndex;
  fromIndex.reserve(recordedEdges);
  toIndex.reserve(recordedEdges);
  for (const std::vector<v8::GrayMarkingEdge>& chunk : aIn.edgeChunks) {
    for (const v8::GrayMarkingEdge& edge : chunk) {
      uint32_t from = gray.Find(edge.from);
      if (from == kNone) {
        continue;
      }
      uint32_t to = gray.Find(edge.to);
      if (to == kNone) {
        continue;
      }
      if (!aIn.tables.empty()) {
        if (tables.Find(edge.from) != kNone) {
          continue;
        }
        uint32_t table = tables.Find(edge.to);
        if (table != kNone) {
          tableOwner[table] = edge.from;
        }
      }
      fromIndex.push_back(from);
      toIndex.push_back(to);
    }
  }
  aIn.edgeChunks = {};

  auto cellOf = [&](uintptr_t aAddress) -> const CellSnapshot* {
    if (!aAddress) {
      return nullptr;
    }
    uint32_t i = cellIndex.Find(aAddress);
    return i == kNone ? nullptr : &aIn.cells[i];
  };
  auto addEdge = [&](uintptr_t aFrom, uintptr_t aTo) {
    fromIndex.push_back(gray.Find(aFrom));
    toIndex.push_back(gray.Find(aTo));
  };
  for (const v8::GrayMarkingEphemeron& entry : aIn.ephemerons) {
    if (gray.Find(entry.value) == kNone) {
      continue;
    }
    uint32_t table = tables.Find(entry.table);
    uintptr_t owner = table == kNone ? 0 : tableOwner[table];
    const CellSnapshot* key = cellOf(entry.key);
    const CellSnapshot* value = cellOf(entry.value);
    const CellSnapshot* map = cellOf(owner);
    if (key && value && (!owner || map)) {
      out.weakMapEntries.push_back(
          {map ? ObjectOf(*map) : nullptr, key->thing, value->thing});
      continue;
    }
    if (gray.Find(entry.key) != kNone) {
      addEdge(entry.key, entry.value);
    }
    if (gray.Find(entry.table) != kNone) {
      addEdge(entry.table, entry.value);
    }
  }

  AddressIndex seenRefs(aIn.weakRefs.size());
  for (const v8::GrayMarkingWeakRef& ref : aIn.weakRefs) {
    if (gray.Find(ref.target) == kNone || !seenRefs.Insert(ref.ref, 0)) {
      continue;
    }
    const CellSnapshot* refCell = cellOf(ref.ref);
    const CellSnapshot* targetCell = cellOf(ref.target);
    if (refCell && targetCell) {
      out.weakRefTargets.emplace_back(ObjectOf(*refCell),
                                      ObjectOf(*targetCell));
    }
  }

  std::vector<uint32_t> offsets(nodeCount + 1, 0);
  for (uint32_t from : fromIndex) {
    offsets[from + 1]++;
  }
  for (uint32_t i = 0; i < nodeCount; i++) {
    offsets[i + 1] += offsets[i];
  }
  std::vector<uint32_t> targets(fromIndex.size());
  std::vector<uint32_t> fill(offsets.begin(), offsets.end() - 1);
  for (size_t e = 0; e < fromIndex.size(); e++) {
    targets[fill[fromIndex[e]]++] = toIndex[e];
  }
  out.jsEdges = ExpandCellEdges(cellAt, offsets, targets);
  out.buildMs = (mozilla::TimeStamp::Now() - start).ToMilliseconds();
  return out;
}

class GrayApplyTask final : public v8::Task {
 public:
  GrayApplyTask(JSRuntime* aRuntime, uint64_t aEpoch)
      : mRuntime(aRuntime), mEpoch(aEpoch) {}

  void Run() override {
    GcState* state = MaybeStateOf(mRuntime);
    if (state && state->grayPending && state->grayEpoch == mEpoch) {
      EnsureGrayBits(*state);
    }
  }

 private:
  JSRuntime* mRuntime;
  uint64_t mEpoch;
};

}  // namespace

void EnsureGrayBits(GcState& aState) {
  if (!aState.grayPending) {
    return;
  }
  mozilla::TimeStamp start = mozilla::TimeStamp::Now();
  bool ready = aState.pendingGray.wait_for(std::chrono::seconds(0)) ==
               std::future_status::ready;
  GrayResult result = aState.pendingGray.get();
  aState.grayPending = false;
  mozilla::TimeStamp got = mozilla::TimeStamp::Now();
  for (JS::GCCellPtr thing : result.gray) {
    Color(aState, thing, true);
  }
  aState.jsEdges = std::move(result.jsEdges);
  aState.weakMapEntries = std::move(result.weakMapEntries);
  aState.weakRefTargets = std::move(result.weakRefTargets);
  aState.grayBitsValid = true;
  if (!aState.v8Marking) {
    SetZoneReadBarrier(aState, false);
  }
  std::vector<JS::GCCellPtr> exposed;
  exposed.swap(aState.exposedWhileGrayPending);
  size_t unmarked = 0;
  for (JS::GCCellPtr thing : exposed) {
    if (IsVgeCell(thing.asCell()) && IsGray(thing.asCell())) {
      unmarked += JS::UnmarkGrayGCThingRecursively(thing);
    }
  }
  static const bool sLog = !!getenv("VGE_GC_LOG");
  if (sLog) {
    mozilla::TimeStamp end = mozilla::TimeStamp::Now();
    fprintf(stderr,
            "VGEGRAYAPPLY build %.3f wait %.3f apply %.3f ready %d gray %zu "
            "exposed %zu unmarked %zu\n",
            result.buildMs, (got - start).ToMilliseconds(),
            (end - got).ToMilliseconds(), ready, result.gray.size(),
            exposed.size(), unmarked);
    fflush(stderr);
  }
}

namespace {

class CellGrayMarkingClient final : public v8::GrayMarkingClient {
 public:
  explicit CellGrayMarkingClient(GcState& aState) : mState(aState) {}

  void MarkGrayRoots(v8::GrayRootMarker& aMarker) override {
    JSRuntime* rt = mState.runtime;
    if (!rt->grayRootTracer) {
      return;
    }
    mozilla::TimeStamp start = mozilla::TimeStamp::Now();
    GrayRootTracer tracer(rt, aMarker);
    JS::SliceBudget budget = JS::SliceBudget::unlimited();
    rt->grayRootTracer(&tracer, budget, rt->grayRootTracerData);
    mRootsEnd = mozilla::TimeStamp::Now();
    mRootsMs = (mRootsEnd - start).ToMilliseconds();
  }

  bool TracesWrappers() const override { return true; }

  uint16_t GrayHandleClassId() const override { return kCellHandleClassId; }

  void TraceWrapper(void* aWrappable, v8::GrayWrapperMarker& aMarker) override {
    static_cast<Anchor*>(static_cast<v8::Object::Wrappable*>(aWrappable))
        ->TraceGray(mState.runtime, aMarker);
  }

  void GrayMarkingFinished(const v8::GrayMarkingResult& aResult) override {
    mozilla::TimeStamp start = mozilla::TimeStamp::Now();
    GrayInput input;
    size_t count = 0;
    aResult.TakeRecords(&input.objectChunks, &input.edgeChunks);
    size_t nodeCount = 0;
    for (const std::vector<uintptr_t>& chunk : input.objectChunks) {
      nodeCount += chunk.size();
    }
    size_t edgeCount = 0;
    for (const std::vector<v8::GrayMarkingEdge>& chunk : input.edgeChunks) {
      edgeCount += chunk.size();
    }
    const uintptr_t* tables = aResult.EphemeronTables(&count);
    input.tables.assign(tables, tables + count);
    const v8::GrayMarkingEphemeron* ephemerons = aResult.Ephemerons(&count);
    input.ephemerons.assign(ephemerons, ephemerons + count);
    const size_t ephemeronCount = count;
    const v8::GrayMarkingWeakRef* refs = aResult.WeakRefs(&count);
    input.weakRefs.assign(refs, refs + count);
    mozilla::TimeStamp copied = mozilla::TimeStamp::Now();
    size_t handleCount = 0;
    const v8::GrayMarkingHandle* handles = aResult.GrayHandles(&handleCount);
    input.cells.reserve(handleCount);
    for (size_t i = 0; i < handleCount; i++) {
      CellTail* tail = reinterpret_cast<CellTail*>(
          handles[i].global - offsetof(CellTail, handle));
      if (tail->kind != CellKind::Object) {
        continue;
      }
      ObjectCell* cell = reinterpret_cast<ObjectCell*>(
          reinterpret_cast<uint8_t*>(tail) - offsetof(ObjectCell, tail));
      uintptr_t inner = 0;
      const JSClass* clasp = cell->shape ? cell->shape->base.clasp : nullptr;
      if (clasp && (clasp->flags & JSCLASS_IS_GLOBAL)) {
        inner = aResult.InnerGlobalOfProxy(handles[i].object);
      }
      input.cells.push_back(
          {JS::GCCellPtr(cell->asObject()), handles[i].object, inner});
    }
    for (Anchor* anchor : mState.runtime->anchors) {
      uintptr_t address = aResult.AddressOf(anchor->Wrapper());
      if (!address) {
        continue;
      }
      ObjectCell* cell = anchor->Cell();
      uintptr_t inner = 0;
      const JSClass* clasp = cell->shape ? cell->shape->base.clasp : nullptr;
      if (clasp && (clasp->flags & JSCLASS_IS_GLOBAL)) {
        inner = aResult.InnerGlobalOfProxy(address);
      }
      input.cells.push_back({JS::GCCellPtr(cell->asObject()), address, inner});
    }
    mozilla::TimeStamp snapped = mozilla::TimeStamp::Now();
    const size_t snappedCount = input.cells.size();
    JSRuntime* rt = mState.runtime;
    uint64_t epoch = ++mState.grayEpoch;
    std::shared_ptr<v8::TaskRunner> runner =
        CurrentPlatform()->GetForegroundTaskRunner(rt->isolate);
    mState.pendingGray = std::async(
        std::launch::async,
        [input = std::move(input), runner, rt, epoch]() mutable {
          GrayResult result = BuildGrayResult(std::move(input));
          runner->PostTask(std::make_unique<GrayApplyTask>(rt, epoch));
          return result;
        });
    mState.grayPending = true;
    SetZoneReadBarrier(mState, true);
    static const bool sLog = !!getenv("VGE_GC_LOG");
    if (sLog) {
      mozilla::TimeStamp end = mozilla::TimeStamp::Now();
      fprintf(stderr,
              "VGEGRAY roots %.3f closure %.3f client %.3f copy %.3f snap "
              "%.3f gray %zu edges %zu ephemerons %zu cells %zu snapped %zu "
              "young %zu strings %lld created_strings %lld dead %zu\n",
              mRootsMs, (start - mRootsEnd).ToMilliseconds(),
              (end - start).ToMilliseconds(),
              (copied - start).ToMilliseconds(),
              (snapped - copied).ToMilliseconds(), nodeCount, edgeCount,
              ephemeronCount, mState.runtime->objectCells.size(),
              snappedCount, mState.runtime->youngTails.size(),
              static_cast<long long>(gLiveStringCells.load()),
              static_cast<long long>(gCreatedStringCells.load()),
              mState.runtime->deadTails.size());
      fflush(stderr);
    }
  }

 private:
  double mRootsMs = 0;
  mozilla::TimeStamp mRootsEnd;
  GcState& mState;
};

}

void InstallGrayMarkingClient(GcState& aState) {
  aState.grayMarkingClient = std::make_unique<CellGrayMarkingClient>(aState);
  v8::SetGrayMarkingClient(aState.runtime->isolate,
                           aState.grayMarkingClient.get());
}

void InvalidateGrayBits(GcState& aState) {
  for (JS::GCCellPtr thing : aState.coloredCells) {
    if (IsVgeCell(thing.asCell())) {
      ClearColor(thing);
    }
  }
  aState.coloredCells.clear();
  aState.grayCells.clear();
  if (aState.pendingGray.valid()) {
    aState.pendingGray.wait();
    aState.pendingGray = {};
  }
  aState.grayPending = false;
  aState.exposedWhileGrayPending.clear();
  if (!aState.v8Marking) {
    SetZoneReadBarrier(aState, false);
  }
  aState.jsEdges.clear();
  aState.weakMapEntries.clear();
  aState.weakRefTargets.clear();
  aState.grayBitsValid = false;
}

void InvalidateUbiGraph(GcState& aState) {
  aState.ubiValid = false;
  aState.ubiEdges.clear();
  aState.ubiSizes.clear();
  aState.ubiRoots.clear();
}

bool BuildFullCellGraph(JSContext* aCx) {
  EnsureHooks(aCx);
  GcState& state = StateOf(aCx->runtime);
  JSRuntime* rt = aCx->runtime;
  v8::Isolate* isolate = aCx->isolate;
  v8::HandleScope outer(isolate);
  const v8::HeapSnapshot* snapshot =
      TakeReportSnapshot(aCx, v8::HeapProfiler::HeapSnapshotOptions());
  if (!snapshot) {
    return false;
  }
  InvalidateUbiGraph(state);
  uint64_t startCount = GcCount(state);
  HeapGraph graph(aCx, snapshot);
  auto deleteSnapshot = mozilla::MakeScopeExit(
      [&] { const_cast<v8::HeapSnapshot*>(snapshot)->Delete(); });

  std::vector<JS::GCCellPtr> embedderRoots;
  {
    CellCollector collector(rt, embedderRoots);
    TraceBlackRoots(rt, &collector);
    if (rt->grayRootTracer) {
      JS::SliceBudget budget = JS::SliceBudget::unlimited();
      rt->grayRootTracer(&collector, budget, rt->grayRootTracerData);
    }
  }

  std::vector<uint8_t> seen(graph.size(), 0);
  std::vector<uint32_t> work;
  std::vector<uint32_t> children;
  for (JS::GCCellPtr thing : embedderRoots) {
    state.ubiRoots.push_back(thing);
    int32_t index = graph.IndexOfCell(thing);
    if (index >= 0 && !seen[index]) {
      seen[index] = 1;
      work.push_back(uint32_t(index));
    }
  }
  int32_t snapshotRoot = graph.RootIndex();
  if (snapshotRoot >= 0) {
    children.clear();
    graph.CellChildren(uint32_t(snapshotRoot), children);
    for (uint32_t m : children) {
      JS::GCCellPtr cell = graph.CellAt(m);
      state.ubiRoots.push_back(cell);
      if (!seen[m]) {
        seen[m] = 1;
        work.push_back(m);
      }
    }
  }
  while (!work.empty()) {
    uint32_t n = work.back();
    work.pop_back();
    JS::GCCellPtr from = graph.CellAt(n);
    if (!from) {
      continue;
    }
    state.ubiSizes[from.asCell()] = graph.ShallowSize(n);
    children.clear();
    graph.CellChildren(n, children);
    std::vector<JS::GCCellPtr>& edges = state.ubiEdges[from.asCell()];
    for (uint32_t m : children) {
      edges.push_back(graph.CellAt(m));
      if (!seen[m]) {
        seen[m] = 1;
        work.push_back(m);
      }
    }
  }
  if (GcCount(state) != startCount) {
    InvalidateUbiGraph(state);
    return false;
  }
  state.ubiValid = true;
  return true;
}

const v8::HeapSnapshot* TakeReportSnapshot(
    JSContext* aCx, const v8::HeapProfiler::HeapSnapshotOptions& aOptions) {
  EnsureHooks(aCx);
  return aCx->isolate->GetHeapProfiler()->TakeHeapSnapshot(aOptions);
}

}

using namespace vge::jsapi;
using namespace vge::jsapi::gc;

namespace {

class UnmarkGrayTracer final : public JS::CallbackTracer {
 public:
  explicit UnmarkGrayTracer(JSRuntime* aRuntime)
      : JS::CallbackTracer(aRuntime, JS::TracerKind::UnmarkGray,
                           JS::WeakMapTraceAction::TraceKeysAndValues) {}

  bool onChild(JS::GCCellPtr aThing, const char* aName) override {
    if (aThing && IsVgeCell(aThing.asCell()) && IsGray(aThing.asCell())) {
      SetColorBit(aThing.asCell(), js::gc::ColorBit::BlackBit, true);
      mUnmarked = true;
      mStack.push_back(aThing);
    }
    return true;
  }

  bool Run(JS::GCCellPtr aThing) {
    onChild(aThing, "root");
    while (!mStack.empty()) {
      JS::GCCellPtr next = mStack.back();
      mStack.pop_back();
      TraceCellChildren(this, next);
    }
    return mUnmarked;
  }

 private:
  std::vector<JS::GCCellPtr> mStack;
  bool mUnmarked = false;
};

}

JS_PUBLIC_API bool JS::UnmarkGrayGCThingRecursively(JS::GCCellPtr thing) {
  if (!thing || !IsVgeCell(thing.asCell())) {
    return false;
  }
  JSRuntime* rt = vge::jsapi::Runtime();
  if (!rt) {
    return false;
  }
  if (GcState* state = MaybeStateOf(rt)) {
    EnsureGrayBits(*state);
  }
  UnmarkGrayTracer tracer(rt);
  return tracer.Run(thing);
}

JS_PUBLIC_API bool js::gc::detail::CanCheckGrayBits(
    const js::gc::TenuredCell* cell) {
  JSRuntime* rt = vge::jsapi::Runtime();
  GcState* state = rt ? MaybeStateOf(rt) : nullptr;
  if (state) {
    EnsureGrayBits(*state);
  }
  return state && state->grayBitsValid && !state->v8Marking;
}

JS_PUBLIC_API bool js::gc::detail::CellIsMarkedGrayIfKnown(
    const js::gc::TenuredCell* cell) {
  js::gc::Cell* c =
      reinterpret_cast<js::gc::Cell*>(const_cast<js::gc::TenuredCell*>(cell));
  if (!IsVgeCell(c)) {
    return false;
  }
  if (!CanCheckGrayBits(cell)) {
    return false;
  }
  return IsGray(c);
}

JS_PUBLIC_API void JS_SetGrayGCRootsTracer(JSContext* cx,
                                           JSGrayRootsTracer traceOp,
                                           void* data) {
  EnsureHooks(cx);
  GcState& state = StateOf(cx->runtime);
  state.embedderGrayTracer = traceOp;
  state.embedderGrayData = data;
  cx->runtime->grayRootTracer = traceOp;
  cx->runtime->grayRootTracerData = data;
}

JS_PUBLIC_API void JS_SetObjectsTenuredCallback(JSContext* cx,
                                                JSObjectsTenuredCallback cb,
                                                void* data) {
  EnsureHooks(cx);
  GcState& state = StateOf(cx->runtime);
  state.tenuredCallback = cb;
  state.tenuredData = data;
}

JS_PUBLIC_API JS::DoCycleCollectionCallback JS::SetDoCycleCollectionCallback(
    JSContext* cx, JS::DoCycleCollectionCallback callback) {
  GcState& state = StateOf(cx->runtime);
  JS::DoCycleCollectionCallback prior = state.cycleCollectionCallback;
  state.cycleCollectionCallback = callback;
  return prior;
}

JS_PUBLIC_API void JS::MaybeClearWeakRefTargets(
    JSRuntime* runtime, JS::ShouldClearWeakRefTargetCallback callback,
    void* data) {
  GcState& state = StateOf(runtime);
  EnsureGrayBits(state);
  std::vector<std::pair<JSObject*, JSObject*>> kept;
  for (auto& [ref, target] : state.weakRefTargets) {
    if (!callback(JS::GCCellPtr(target), data)) {
      kept.emplace_back(ref, target);
    }
  }
  state.weakRefTargets = std::move(kept);
}

JS_PUBLIC_API void js::TraceWeakMaps(js::WeakMapTracer* trc) {
  GcState& state = StateOf(trc->runtime);
  EnsureGrayBits(state);
  for (const WeakMapEntry& entry : state.weakMapEntries) {
    trc->trace(entry.map, entry.key, entry.value);
  }
}

JS_PUBLIC_API bool js::AreGCGrayBitsValid(JSRuntime* rt) {
  GcState* state = MaybeStateOf(rt);
  if (state) {
    EnsureGrayBits(*state);
  }
  return state && state->grayBitsValid;
}

JS_PUBLIC_API bool js::ZoneGlobalsAreAllGray(JS::Zone* zone) {
  JSRuntime* rt = JS::shadow::Zone::from(zone)->runtimeFromAnyThread();
  if (GcState* state = MaybeStateOf(rt)) {
    EnsureGrayBits(*state);
  }
  for (JS::Realm* realm : rt->realms) {
    if (realm->contextDead) {
      continue;
    }
    JSObject* global = realm->global;
    if (!global || !IsGray(reinterpret_cast<js::gc::Cell*>(global))) {
      return false;
    }
  }
  return true;
}

JS_PUBLIC_API void js::TraceGrayWrapperTargets(JSTracer* trc, JS::Zone* zone) {
  JSRuntime* rt = JS::shadow::Zone::from(zone)->runtimeFromAnyThread();
  GcState& state = StateOf(rt);
  EnsureGrayBits(state);
  std::vector<JS::GCCellPtr> gray = state.grayCells;
  for (JS::GCCellPtr thing : gray) {
    if (!thing.is<JSObject>()) {
      continue;
    }
    JSObject* wrapper = &thing.as<JSObject>();
    if (!js::IsCrossCompartmentWrapper(wrapper)) {
      continue;
    }
    JSObject* target = js::GetProxyTargetObject(wrapper);
    if (target && IsVgeCell(target) &&
        IsGray(reinterpret_cast<js::gc::Cell*>(target))) {
      js::UnsafeTraceManuallyBarrieredEdge(trc, &target, "gray CCW target");
    }
  }
}

JS_PUBLIC_API void js::IterateGrayObjects(JS::Zone* zone,
                                          js::IterateGCThingCallback cellCallback,
                                          void* data) {
  JSRuntime* rt = JS::shadow::Zone::from(zone)->runtimeFromAnyThread();
  GcState& state = StateOf(rt);
  EnsureGrayBits(state);
  JS::AutoCheckCannotGC nogc;
  std::vector<JS::GCCellPtr> gray = state.grayCells;
  for (JS::GCCellPtr thing : gray) {
    if (thing.is<JSObject>() && IsGray(thing.asCell()) && LiveTail(thing)) {
      cellCallback(data, thing, nogc);
    }
  }
}
