#ifndef vge_bedrock_jsapi_VgeJsInternal_h
#define vge_bedrock_jsapi_VgeJsInternal_h

#include <atomic>
#include <mutex>
#include <stdint.h>

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "jsapi.h"
#include "jsfriendapi.h"
#include "js/Class.h"
#include "js/GCAPI.h"
#include "js/Promise.h"
#include "js/Warnings.h"
#include "js/friend/CycleCollector.h"
#include "mozilla/Maybe.h"
#include "js/HeapAPI.h"
#include "js/Proxy.h"
#include "js/shadow/Object.h"
#include "js/shadow/Realm.h"
#include "js/shadow/Shape.h"
#include "js/shadow/String.h"
#include "js/shadow/Symbol.h"
#include "js/shadow/Zone.h"
#include "mozilla/LinkedList.h"
#include "cppgc/allocation.h"
#include "cppgc/garbage-collected.h"
#include "cppgc/persistent.h"
#include "cppgc/prefinalizer.h"
#include "cppgc/visitor.h"
#include "v8-cppgc.h"
#include "v8-gray-marking.h"
#include "v8.h"

namespace vge::jsapi {

class Anchor;
struct Shape;

enum class CellKind : uint8_t { Object, String, Symbol, BigInt };

constexpr size_t kObjectHeaderWords = 3;
constexpr size_t kFixedSlots = JS::shadow::Object::MAX_FIXED_SLOTS;

struct CellTail {
  CellKind kind;
  bool rootedThisGC;
  bool isAtom;
  bool classFinalized;
  bool managed;
  uint8_t marked;
  bool wrapperKey;
  bool pinned;
  uint32_t serial;
  std::atomic<uint32_t> charRefs;
  v8::Global<v8::Value> handle;
  Anchor* anchor;
  CellTail* nextFree;
};

struct ObjectCell {
  Shape* shape;
  uintptr_t words[kObjectHeaderWords - 1 + kFixedSlots];
  CellTail tail;

  JS::Value* fixedSlots() {
    return reinterpret_cast<JS::Value*>(&words[kObjectHeaderWords - 1]);
  }
  js::detail::ProxyDataLayout* proxyData() {
    return reinterpret_cast<js::detail::ProxyDataLayout*>(&words[0]);
  }
  JSObject* asObject() { return reinterpret_cast<JSObject*>(this); }
  static ObjectCell* from(const JSObject* aObj) {
    return reinterpret_cast<ObjectCell*>(const_cast<JSObject*>(aObj));
  }
};

struct StringCell {
  uintptr_t flags;
  const void* chars;
  const JSExternalStringCallbacks* external;
  uintptr_t pad;
  CellTail tail;

  JSString* asString() { return reinterpret_cast<JSString*>(this); }
  static StringCell* from(const JSString* aStr) {
    return reinterpret_cast<StringCell*>(const_cast<JSString*>(aStr));
  }
  uint32_t length() const { return uint32_t(flags >> 32); }
  bool latin1() const {
    return flags & JS::shadow::String::LATIN1_CHARS_BIT;
  }
};

template <typename Base, typename Char>
class CellStringResource final : public Base {
 public:
  CellStringResource(StringCell* aCell, const Char* aChars, size_t aLength,
                     bool aCounted)
      : mCell(aCell), mChars(aChars), mLength(aLength), mCounted(aCounted) {
    if (aCounted) {
      aCell->tail.charRefs.fetch_add(1, std::memory_order_relaxed);
    }
  }
  const Char* data() const override { return mChars; }
  size_t length() const override { return mLength; }
  StringCell* Cell() const { return mCell; }
  void SetCell(StringCell* aCell) { mCell = aCell; }
  void Release() { Dispose(); }

  static const void* Vtable() {
    static CellStringResource probe(nullptr, nullptr, 0, false);
    return *reinterpret_cast<const void* const*>(&probe);
  }

 protected:
  void Dispose() override {
    if (mCounted) {
      mCell->tail.charRefs.fetch_sub(1, std::memory_order_release);
    }
    delete this;
  }

 private:
  StringCell* mCell;
  const Char* mChars;
  size_t mLength;
  bool mCounted;
};

using CellOneByteResource =
    CellStringResource<v8::String::ExternalOneByteStringResource, char>;
using CellTwoByteResource =
    CellStringResource<v8::String::ExternalStringResource, uint16_t>;

StringCell* CellOfV8String(v8::Isolate* aIsolate,
                           v8::Local<v8::String> aString);

struct SymbolCell {
  void* header;
  uint32_t code;
  uint32_t pad;
  uintptr_t pad2[2];
  CellTail tail;

  JS::Symbol* asSymbol() { return reinterpret_cast<JS::Symbol*>(this); }
  static SymbolCell* from(const JS::Symbol* aSym) {
    return reinterpret_cast<SymbolCell*>(const_cast<JS::Symbol*>(aSym));
  }
};

struct BigIntCell {
  uintptr_t header;
  uintptr_t pad[3];
  CellTail tail;

  JS::BigInt* asBigInt() { return reinterpret_cast<JS::BigInt*>(this); }
  static BigIntCell* from(const JS::BigInt* aBig) {
    return reinterpret_cast<BigIntCell*>(const_cast<JS::BigInt*>(aBig));
  }
};

struct AtomKeyHash {
  using is_transparent = void;
  size_t operator()(std::u16string_view aKey) const {
    return std::hash<std::u16string_view>{}(aKey);
  }
};

struct AtomKeyEqual {
  using is_transparent = void;
  bool operator()(std::u16string_view aLeft, std::u16string_view aRight) const {
    return aLeft == aRight;
  }
};

using AtomTable =
    std::unordered_map<std::u16string, StringCell*, AtomKeyHash, AtomKeyEqual>;

void ForgetAtom(JSRuntime* aRuntime, StringCell* aCell);
JSString* PinAtom(JSString* aAtom);

struct CellPool {
  CellTail* freeList = nullptr;
  uint8_t* cursor = nullptr;
  uint8_t* limit = nullptr;
};

struct Shape {
  JS::shadow::Shape shadow;
  JS::shadow::BaseShape base;
  uint32_t reservedSlots;
  bool isProxy;
  bool isFunction;
};

Shape* ShapeFor(const JSClass* aClass, JS::Realm* aRealm);

extern const JSClass ArrayClass;

namespace buffers {
const JSClass* ClassForV8(v8::Local<v8::Object> aObject);
}

JS::Value* DynamicSlots(ObjectCell* aCell);
JS::Realm* RealmOfScript(const JSScript* aScript);
void FreeObjectCell(ObjectCell* aCell);
void DrainDeferredObjectFrees(JSRuntime* aRuntime, size_t aLimit);
void FreeStringChars(StringCell* aCell);

JSContext* CurrentContext();
v8::Isolate* MainIsolate();
v8::Platform* CurrentPlatform();
void BindCellHandle(JSContext* aCx, CellTail* aTail, v8::Local<v8::Value> aValue, bool aStrong);
void ForgetTail(CellTail* aTail);
constexpr uint16_t kCellHandleClassId = 0x5643;
void TraceAnchoredCell(ObjectCell* aCell, JSTracer* aTracer);
namespace gc {
bool IsVgeCell(const void* aPtr);
}

inline void MarkManagedCell(CellTail* aTail, uint8_t aSite) {
  if (aTail && aTail->managed && !aTail->marked) {
    aTail->marked = aSite;
  }
}
void TraceCellSlots(ObjectCell* aCell, JSTracer* aTracer);
void TraceCellHooks(ObjectCell* aCell, JSTracer* aTracer);
void DetachManagedObjectCell(JSContext* aCx, ObjectCell* aCell);
JSObject* PinCell(JSObject* aObj);
ObjectCell* CellOfExternal(v8::Local<v8::Object> aObject,
                           v8::Local<v8::Value> aExternal);
void ForgetWrapperKey(JSContext* aCx, JSObject* aTarget);
void NoteDeadAnchoredCell(CellTail* aTail);
void MakeCellWeak(CellTail* aTail);
JSString* AtomFromV8(JSContext* aCx, v8::Local<v8::String> aString);
JSString* AtomizeCell(JSContext* aCx, JSString* aStr);

void* AllocCell(size_t aSize);
void FreeCell(void* aCell, size_t aSize);
bool IsCellAddress(const void* aPtr);
void ReleaseCellChunks(JSRuntime* aRuntime);

CellTail* TailOf(js::gc::Cell* aCell, JS::TraceKind aKind);

ObjectCell* NewObjectCell(JSContext* aCx, Shape* aShape,
                          v8::Local<v8::Object> aObject, bool aAnchored);
JSObject* ObjectFromV8(JSContext* aCx, v8::Local<v8::Object> aObject);
v8::Local<v8::Object> ToV8(JSContext* aCx, JSObject* aObj);

JSString* StringFromV8(JSContext* aCx, v8::Local<v8::String> aString);
v8::Local<v8::String> ToV8(JSContext* aCx, JSString* aStr);
JS::Symbol* SymbolFromV8(JSContext* aCx, v8::Local<v8::Symbol> aSymbol);
v8::Local<v8::Symbol> ToV8(JSContext* aCx, JS::Symbol* aSym);
JS::BigInt* BigIntFromV8(JSContext* aCx, v8::Local<v8::BigInt> aBigInt);
v8::Local<v8::BigInt> ToV8(JSContext* aCx, JS::BigInt* aBig);

bool ValueFromV8(JSContext* aCx, v8::Local<v8::Value> aValue,
                 JS::MutableHandleValue aResult);
JS::Value ValueFromV8(JSContext* aCx, v8::Local<v8::Value> aValue);
v8::Local<v8::Value> ToV8(JSContext* aCx, const JS::Value& aValue);

bool IdFromV8(JSContext* aCx, v8::Local<v8::Value> aKey,
              JS::MutableHandleId aResult);
v8::Local<v8::Value> ToV8(JSContext* aCx, jsid aId);

class Anchor final : public v8::Object::Wrappable {
  CPPGC_USING_PRE_FINALIZER(Anchor, PreFinalize);

 public:
  explicit Anchor(ObjectCell* aCell) : mCell(aCell) {}
  ~Anchor();
  void Trace(cppgc::Visitor* aVisitor) const override;
  void TraceGray(JSRuntime* aRuntime, v8::GrayWrapperMarker& aMarker) const;
  ObjectCell* Cell() const { return mCell; }
  void TraceSlotValue(cppgc::Visitor* aVisitor, const JS::Value& aValue) const;
  void SetWrapper(v8::Isolate* aIsolate, v8::Local<v8::Object> aWrapper);
  const v8::TracedReference<v8::Object>& Wrapper() const { return mWrapper; }
  uint32_t Index() const { return mIndex; }
  void SetIndex(uint32_t aIndex) { mIndex = aIndex; }

  static constexpr uint32_t kNoIndex = UINT32_MAX;

 private:
  void PreFinalize();

  ObjectCell* mCell;
  uint32_t mIndex = kNoIndex;
  v8::TracedReference<v8::Object> mWrapper;
};

void RegisterAnchor(JSRuntime* aRuntime, Anchor* aAnchor);
void TakeGrayAnchors(std::vector<Anchor*>* aOut);
void ClearGrayAnchors();
extern size_t gBarrierMarksInGC[3];
void UnregisterAnchor(JSRuntime* aRuntime, Anchor* aAnchor);

inline bool CellHasV8(const CellTail* aTail) {
  return aTail->anchor ? !aTail->anchor->Wrapper().IsEmpty()
                       : !aTail->handle.IsEmpty();
}

inline v8::Local<v8::Value> CellV8(v8::Isolate* aIsolate,
                                   const CellTail* aTail) {
  if (aTail->anchor) {
    return aTail->anchor->Wrapper().Get(aIsolate);
  }
  return aTail->handle.Get(aIsolate);
}

inline bool CellIs(const CellTail* aTail, v8::Local<v8::Object> aObject) {
  if (aTail->anchor) {
    return aTail->anchor->Wrapper() == aObject;
  }
  return aTail->handle == aObject;
}

inline void MarkCellDuringMarking(v8::Isolate* aIsolate,
                                  const CellTail* aTail) {
  if (aTail->anchor) {
    v8::MarkTracedDuringMarking(aIsolate, aTail->anchor->Wrapper());
  } else if (!aTail->handle.IsEmpty()) {
    v8::MarkGlobalDuringMarking(aIsolate, aTail->handle);
  }
}

template <typename Marker>
inline void MarkCellWith(Marker& aMarker, const CellTail* aTail) {
  if (aTail->anchor) {
    aMarker.Mark(aTail->anchor->Wrapper());
  } else if (!aTail->handle.IsEmpty()) {
    aMarker.Mark(aTail->handle);
  }
}

constexpr v8::CppHeapPointerTag kAnchorTag =
    v8::CppHeapPointerTag::kFirstEmbedderWrappableTag;

}

namespace vge::jsapi::gc {
struct GcState;
}

struct JSRuntime {
  JSRuntime();

  std::atomic<vge::jsapi::gc::GcState*> gcState{nullptr};

  v8::Isolate* isolate = nullptr;
  v8::CppHeap* cppHeap = nullptr;
  JSContext* mainContext = nullptr;
  JS::Zone* zone = nullptr;
  vge::jsapi::CellPool cellPools[2];
  std::vector<void*> cellChunks;
  v8::Global<v8::Private> cellKey;
  v8::Global<v8::Private> slotsKey;
  mozilla::LinkedList<js::PersistentRootedBase>
      heapRoots[size_t(JS::RootKind::Limit)];
  std::vector<std::pair<JSTraceDataOp, void*>> blackRootTracers;
  bool (*blackGlobalCallback)(JSObject*) = nullptr;
  JSGrayRootsTracer grayRootTracer = nullptr;
  void* grayRootTracerData = nullptr;
  std::vector<std::pair<JSGCCallback, void*>> gcCallbacks;
  std::vector<std::pair<JSFinalizeCallback, void*>> finalizeCallbacks;
  JSGCCallback gcCallback = nullptr;
  void* gcCallbackData = nullptr;
  const JSWrapObjectCallbacks* wrapObjectCallbacks = nullptr;
  JSDestroyCompartmentCallback destroyCompartmentCallback = nullptr;
  JSDestroyZoneCallback destroyZoneCallback = nullptr;
  JSSizeOfIncludingThisCompartmentCallback sizeOfCompartmentCallback = nullptr;
  JS::DestroyRealmCallback destroyRealmCallback = nullptr;
  JS::RealmNameCallback realmNameCallback = nullptr;
  const JSSecurityCallbacks* securityCallbacks = nullptr;
  const JSPrincipals* trustedPrincipals = nullptr;
  std::vector<vge::jsapi::CellTail*> strongThisGC;
  std::unordered_set<vge::jsapi::CellTail*> youngTails;
  std::vector<vge::jsapi::CellTail*> deadTails;
  std::vector<vge::jsapi::StringCell*> managedStrings;
  std::vector<vge::jsapi::ObjectCell*> managedObjects;
  bool sweepingManaged = false;
  bool phantomSweepPosted = false;
  std::vector<JS::Realm*> realms;
  std::vector<JS::Realm*> dyingRealms;
  std::unordered_set<vge::jsapi::ObjectCell*> objectCells;
  std::vector<std::pair<vge::jsapi::ObjectCell*, JS::Value*>> deferredObjectFrees;
  std::vector<vge::jsapi::ObjectCell*> pendingFinalize;
  std::vector<vge::jsapi::ObjectCell*> deadAnchored;
  std::vector<vge::jsapi::Anchor*> anchors;
  std::vector<cppgc::Persistent<vge::jsapi::Anchor>> rootedAnchors;
  void (*weakPointerSweep)(JSRuntime*) = nullptr;
  bool sweepingWeakPointers = false;
  std::unordered_map<const JSClass*, std::vector<vge::jsapi::Shape*>> shapes;
  vge::jsapi::AtomTable atoms;
  std::unordered_multimap<int, vge::jsapi::SymbolCell*> symbols;
  uint32_t gcNumber = 0;
  bool inGC = false;
};

namespace JS {

class Zone : public JS::shadow::Zone {
 public:
  explicit Zone(JSRuntime* aRuntime)
      : JS::shadow::Zone(aRuntime, nullptr, NormalZone) {}
  void* data = nullptr;
};

class Compartment {
 public:
  explicit Compartment(JS::Zone* aZone) : zone(aZone) {}
  JS::Zone* zone;
  void* data = nullptr;
  std::vector<JS::Realm*> realms;
  bool systemCompartment = false;
};

class Realm : public JS::shadow::Realm {
 public:
  Realm(JS::Compartment* aCompartment, JSRuntime* aRuntime)
      : JS::shadow::Realm(aCompartment), runtime(aRuntime) {}
  JSRuntime* runtime;
  v8::Global<v8::Context> context;
  JSObject* global = nullptr;
  const JSClass* globalClass = nullptr;
  JSPrincipals* principals = nullptr;
  void* data = nullptr;
  bool isSystem = false;
  bool contextDead = false;
  uint32_t enterDepth = 0;
  size_t liveCells = 0;
  RealmCreationOptions creationOptions;
  RealmBehaviors behaviors;
};

}

struct JSContext : public JS::RootingContext {
  explicit JSContext(JSRuntime* aRuntime);

  JSRuntime* runtime;
  v8::Isolate* isolate;
  void* data = nullptr;
  alignas(v8::HandleScope) unsigned char taskScope[sizeof(v8::HandleScope)];
  JS::Value pendingException;
  bool exceptionPending = false;
  bool throwingOutOfMemory = false;
  std::vector<JS::Realm*> realmStack;
  JS::WarningReporter warningReporter = nullptr;
  JSInterruptCallback interruptCallbacks[4] = {};
  size_t interruptCallbackCount = 0;
  JS::JobQueue* jobQueue = nullptr;

  void enterRealm(JS::Realm* aRealm);
  void leaveRealm(JS::Realm* aOld);
  JS::Realm* realm() const { return realm_; }
  JS::Zone* zone() const { return zone_; }
  v8::Local<v8::Context> v8Context();
  void setPending(v8::Local<v8::Value> aException);
};

namespace vge::jsapi {

class AutoV8 {
 public:
  explicit AutoV8(JSContext* aCx);
  ~AutoV8();

  v8::Isolate* isolate() const { return mIsolate; }
  v8::Local<v8::Context> context() const { return mContext; }
  bool Check(const v8::TryCatch& aTry);

 private:
  JSContext* mCx;
  v8::Isolate* mIsolate;
  v8::HandleScope mScope;
  v8::Local<v8::Context> mContext;
  mozilla::Maybe<v8::Context::Scope> mContextScope;
};

bool ReportPending(JSContext* aCx, v8::TryCatch& aTry);

void PushNoResolve(JSContext* aCx, JSObject* aObj, jsid aId);
void PopNoResolve(JSContext* aCx);

class AutoNoResolve {
 public:
  AutoNoResolve(JSContext* aCx, JSObject* aObj, jsid aId) : mCx(aCx) {
    PushNoResolve(aCx, aObj, aId);
  }
  ~AutoNoResolve() { PopNoResolve(mCx); }

 private:
  JSContext* mCx;
};

JSObject* NewClassObject(JSContext* aCx, const JSClass* aClass,
                         JS::HandleObject aProto);
v8::Local<v8::FunctionTemplate> ClassTemplate(JSContext* aCx,
                                              const JSClass* aClass);
JSFunction* NewNativeFunction(JSContext* aCx, JSNative aNative,
                              unsigned aNargs, unsigned aFlags,
                              JS::HandleString aName, const JSJitInfo* aInfo);
JSObject* NewProxyCell(JSContext* aCx, const js::BaseProxyHandler* aHandler,
                       JS::HandleValue aPrivate, JS::HandleObject aProto,
                       const JSClass* aClass);
JS::Realm* RealmOfV8Context(JSContext* aCx, v8::Local<v8::Context> aContext);

void WatchRealmContext(JS::Realm* aRealm);
void TraceEnteredRealmGlobals(JSRuntime* aRuntime, JSTracer* aTracer);
void ForgetRealmClasses(JS::Realm* aRealm);
void ForgetCompartmentClasses(JS::Compartment* aCompartment);
void ForgetRealmProxies(JS::Realm* aRealm);
void ForgetCompartmentProxies(JS::Compartment* aCompartment);
void ForgetRealmScripts(JS::Realm* aRealm);

JSRuntime* Runtime();

}

#endif
