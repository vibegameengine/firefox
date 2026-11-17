#include "VgeJsInternal.h"

#include <string>
#include <unordered_map>

#include "js/Id.h"
#include "js/Value.h"

namespace vge::jsapi {

std::atomic<int64_t> gLiveStringCells{0};
std::atomic<int64_t> gCreatedStringCells{0};

namespace {

v8::Local<v8::Private> CellKey(JSContext* aCx) {
  return aCx->runtime->cellKey.Get(aCx->isolate);
}

}

void BindCellHandle(JSContext* aCx, CellTail* aTail, v8::Local<v8::Value> aValue,
                bool aStrong) {
  aTail->handle.Reset(aCx->isolate, aValue);
  aTail->handle.SetWrapperClassId(kCellHandleClassId);
  if (!aStrong) {
    MakeCellWeak(aTail);
    aCx->runtime->youngTails.insert(aTail);
  }
}

namespace {

const JSClass* ClassForScriptObject(v8::Local<v8::Object> aObject) {
  if (aObject->IsArrayBuffer() || aObject->IsSharedArrayBuffer() ||
      aObject->IsArrayBufferView()) {
    return buffers::ClassForV8(aObject);
  }
  if (aObject->IsFunction()) {
    return js::FunctionClassPtr;
  }
  if (aObject->IsArray()) {
    return &ArrayClass;
  }
  return js::ObjectClassPtr;
}

StringCell* FillString(JSContext* aCx, v8::Local<v8::String> aString,
                       bool aAtom) {
  StringCell* cell = static_cast<StringCell*>(AllocCell(sizeof(StringCell)));
  ++gLiveStringCells;
  ++gCreatedStringCells;
  uint32_t length = aString->Length();
  uintptr_t flags = JS::shadow::String::LINEAR_BIT;
  if (aString->IsOneByte()) {
    auto* chars = static_cast<uint8_t*>(malloc(length + 1));
    aString->WriteOneByteV2(aCx->isolate, 0, uint32_t(length), chars);
    chars[length] = 0;
    cell->chars = chars;
    flags |= JS::shadow::String::LATIN1_CHARS_BIT;
  } else {
    auto* chars = static_cast<uint16_t*>(malloc((length + 1) * 2));
    aString->WriteV2(aCx->isolate, 0, uint32_t(length), chars);
    chars[length] = 0;
    cell->chars = chars;
  }
  if (aAtom) {
    flags |= JS::shadow::String::ATOM_BIT;
  }
  cell->flags = flags | (uintptr_t(length) << 32);
  cell->tail.kind = CellKind::String;
  cell->tail.isAtom = aAtom;
  return cell;
}

std::u16string Content(StringCell* aCell) {
  std::u16string out;
  uint32_t length = aCell->length();
  out.resize(length);
  if (aCell->latin1()) {
    auto* chars = static_cast<const uint8_t*>(aCell->chars);
    for (uint32_t i = 0; i < length; i++) {
      out[i] = chars[i];
    }
  } else {
    memcpy(out.data(), aCell->chars, length * 2);
  }
  return out;
}

v8::Local<v8::Context> ContextForObject(JSContext* aCx,
                                        v8::Local<v8::Object> aObject) {
  v8::Local<v8::Context> ctx;
  if (aObject->GetCreationContext().ToLocal(&ctx)) {
    return ctx;
  }
  ctx = aCx->v8Context();
  if (!ctx.IsEmpty()) {
    return ctx;
  }
  for (JS::Realm* realm : aCx->runtime->realms) {
    if (!realm->context.IsEmpty()) {
      return realm->context.Get(aCx->isolate);
    }
  }
  return ctx;
}

}

ObjectCell* NewObjectCell(JSContext* aCx, Shape* aShape,
                          v8::Local<v8::Object> aObject, bool aAnchored) {
  ObjectCell* cell = static_cast<ObjectCell*>(AllocCell(sizeof(ObjectCell)));
  cell->shape = aShape;
  cell->tail.kind = CellKind::Object;
  if (aShape->base.realm) {
    aShape->base.realm->liveCells++;
  }
  aCx->runtime->objectCells.insert(cell);
  if (aShape->isProxy) {
    cell->proxyData()->handler = nullptr;
    cell->proxyData()->expando = nullptr;
    cell->proxyData()->values()->init(aShape->reservedSlots);
  } else {
    JS::Value* slots = cell->fixedSlots();
    for (size_t i = 0; i < kFixedSlots; i++) {
      slots[i] = JS::UndefinedValue();
    }
    if (aShape->reservedSlots > kFixedSlots) {
      size_t extra = aShape->reservedSlots - kFixedSlots;
      JS::Value* dynamic = new JS::Value[extra];
      for (size_t i = 0; i < extra; i++) {
        dynamic[i] = JS::UndefinedValue();
      }
      cell->words[0] = reinterpret_cast<uintptr_t>(dynamic);
    }
  }
  v8::Local<v8::Context> ctx = ContextForObject(aCx, aObject);
  aObject->SetPrivate(ctx, CellKey(aCx), v8::External::New(aCx->isolate, cell, v8::kExternalPointerTypeTagDefault))
      .Check();
  if (aAnchored) {
    Anchor* anchor = cppgc::MakeGarbageCollected<Anchor>(
        aCx->isolate->GetCppHeap()->GetAllocationHandle(), cell);
    cell->tail.anchor = anchor;
    RegisterAnchor(aCx->runtime, anchor);
    v8::Object::Wrap(aCx->isolate, aObject, anchor, kAnchorTag);
    anchor->SetWrapper(aCx->isolate, aObject);
    return cell;
  }
  BindCellHandle(aCx, &cell->tail, aObject, false);
  return cell;
}

JSObject* ObjectFromV8(JSContext* aCx, v8::Local<v8::Object> aObject) {
  if (aObject.IsEmpty()) {
    return nullptr;
  }
  v8::Local<v8::Context> ctx = ContextForObject(aCx, aObject);
  v8::Local<v8::Value> existing;
  if (aObject->GetPrivate(ctx, CellKey(aCx)).ToLocal(&existing)) {
    if (ObjectCell* cell = CellOfExternal(aObject, existing)) {
      return cell->asObject();
    }
  }
  JS::Realm* realm = aCx->realm();
  v8::Local<v8::Context> creation;
  if (aObject->GetCreationContext().ToLocal(&creation)) {
    for (JS::Realm* r : aCx->runtime->realms) {
      if (r->context.Get(aCx->isolate) == creation) {
        realm = r;
        break;
      }
    }
  }
  Shape* shape = ShapeFor(ClassForScriptObject(aObject), realm);
  ObjectCell* cell = NewObjectCell(aCx, shape, aObject, false);
  cell->tail.managed = true;
  cell->tail.marked = 7;
  aCx->runtime->managedObjects.push_back(cell);
  return cell->asObject();
}

ObjectCell* CellOfExternal(v8::Local<v8::Object> aObject,
                           v8::Local<v8::Value> aExternal) {
  if (aExternal.IsEmpty() || !aExternal->IsExternal()) {
    return nullptr;
  }
  ObjectCell* cell = static_cast<ObjectCell*>(
      aExternal.As<v8::External>()->Value(v8::kExternalPointerTypeTagDefault));
  if (!CellHasV8(&cell->tail)) {
    return nullptr;
  }
  if (CellIs(&cell->tail, aObject)) {
    return cell;
  }
  const JSClass* clasp = cell->shape ? cell->shape->base.clasp : nullptr;
  if (clasp && (clasp->flags & JSCLASS_IS_GLOBAL)) {
    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    v8::Local<v8::Value> inner =
        v8::InnerGlobalOfProxy(isolate, CellV8(isolate, &cell->tail));
    if (!inner.IsEmpty() && inner == aObject) {
      return cell;
    }
  }
  return nullptr;
}

JSObject* PinCell(JSObject* aObj) {
  if (aObj) {
    ObjectCell::from(aObj)->tail.managed = false;
  }
  return aObj;
}

void DetachManagedObjectCell(JSContext* aCx, ObjectCell* aCell) {
  aCell->tail.handle.Reset();
}

v8::Local<v8::Object> ToV8(JSContext* aCx, JSObject* aObj) {
  if (!aObj) {
    return v8::Local<v8::Object>();
  }
  ObjectCell* cell = ObjectCell::from(aObj);
  return CellV8(aCx->isolate, &cell->tail).As<v8::Object>();
}

StringCell* CellOfV8String(v8::Isolate* aIsolate,
                           v8::Local<v8::String> aString) {
  v8::String::Encoding encoding;
  v8::String::ExternalStringResourceBase* resource =
      aString->GetExternalStringResourceBase(aIsolate, &encoding);
  if (!resource) {
    return nullptr;
  }
  const void* vtable = *reinterpret_cast<const void* const*>(resource);
  if (encoding == v8::String::ONE_BYTE_ENCODING &&
      vtable == CellOneByteResource::Vtable()) {
    return static_cast<CellOneByteResource*>(resource)->Cell();
  }
  if (encoding == v8::String::TWO_BYTE_ENCODING &&
      vtable == CellTwoByteResource::Vtable()) {
    return static_cast<CellTwoByteResource*>(resource)->Cell();
  }
  return nullptr;
}

namespace {

bool ExternalizeIntoCell(JSContext* aCx, v8::Local<v8::String> aString,
                         StringCell* aCell) {
  if (aCell->latin1()) {
    if (!aString->CanMakeExternal(v8::String::ONE_BYTE_ENCODING)) {
      return false;
    }
    auto* resource = new CellOneByteResource(
        aCell, static_cast<const char*>(aCell->chars), aCell->length(), true);
    if (aString->MakeExternal(aCx->isolate, resource)) {
      return true;
    }
    resource->Release();
    return false;
  }
  if (!aString->CanMakeExternal(v8::String::TWO_BYTE_ENCODING)) {
    return false;
  }
  auto* resource = new CellTwoByteResource(
      aCell, static_cast<const uint16_t*>(aCell->chars), aCell->length(), true);
  if (aString->MakeExternal(aCx->isolate, resource)) {
    return true;
  }
  resource->Release();
  return false;
}

v8::Local<v8::String> NewExternalFromCell(JSContext* aCx, StringCell* aCell) {
  v8::Local<v8::String> s;
  if (aCell->latin1()) {
    auto* resource = new CellOneByteResource(
        aCell, static_cast<const char*>(aCell->chars), aCell->length(), true);
    if (!v8::String::NewExternalOneByte(aCx->isolate, resource).ToLocal(&s)) {
      resource->Release();
      return {};
    }
    return s;
  }
  auto* resource = new CellTwoByteResource(
      aCell, static_cast<const uint16_t*>(aCell->chars), aCell->length(), true);
  if (!v8::String::NewExternalTwoByte(aCx->isolate, resource).ToLocal(&s)) {
    resource->Release();
    return {};
  }
  return s;
}

v8::Local<v8::String> NewCopyFromCell(JSContext* aCx, StringCell* aCell) {
  if (aCell->latin1()) {
    return v8::String::NewFromOneByte(
               aCx->isolate, static_cast<const uint8_t*>(aCell->chars),
               v8::NewStringType::kNormal, int(aCell->length()))
        .ToLocalChecked();
  }
  return v8::String::NewFromTwoByte(
             aCx->isolate, static_cast<const uint16_t*>(aCell->chars),
             v8::NewStringType::kNormal, int(aCell->length()))
      .ToLocalChecked();
}

}

JSString* StringFromV8(JSContext* aCx, v8::Local<v8::String> aString) {
  if (StringCell* known = CellOfV8String(aCx->isolate, aString)) {
    MarkManagedCell(&known->tail, 7);
    return known->asString();
  }
  StringCell* cell = FillString(aCx, aString, false);
  cell->tail.managed = true;
  cell->tail.marked = 7;
  aCx->runtime->managedStrings.push_back(cell);
  ExternalizeIntoCell(aCx, aString, cell);
  return cell->asString();
}

JSString* AtomFromV8(JSContext* aCx, v8::Local<v8::String> aString) {
  std::u16string content;
  content.resize(aString->Length());
  aString->WriteV2(aCx->isolate, 0, uint32_t(int(content.size())), reinterpret_cast<uint16_t*>(content.data()));
  auto it = aCx->runtime->atoms.find(content);
  if (it != aCx->runtime->atoms.end()) {
    return it->second->asString();
  }
  v8::Local<v8::String> internalized =
      v8::String::NewFromTwoByte(aCx->isolate,
                                 reinterpret_cast<const uint16_t*>(content.data()),
                                 v8::NewStringType::kInternalized,
                                 int(content.size()))
          .ToLocalChecked();
  StringCell* cell = FillString(aCx, internalized, true);
  BindCellHandle(aCx, &cell->tail, internalized, true);
  aCx->runtime->atoms.emplace(std::move(content), cell);
  return cell->asString();
}

JSString* AtomizeCell(JSContext* aCx, JSString* aStr) {
  StringCell* cell = StringCell::from(aStr);
  if (cell->tail.isAtom) {
    return aStr;
  }
  return AtomFromV8(aCx, ToV8(aCx, aStr));
}

v8::Local<v8::String> ToV8(JSContext* aCx, JSString* aStr) {
  StringCell* cell = StringCell::from(aStr);
  if (cell->tail.handle.IsEmpty()) {
    v8::Local<v8::String> s;
    if (cell->tail.managed && cell->length() > 0) {
      s = NewExternalFromCell(aCx, cell);
    }
    if (s.IsEmpty()) {
      s = NewCopyFromCell(aCx, cell);
    }
    BindCellHandle(aCx, &cell->tail, s, cell->tail.isAtom);
    return s;
  }
  return cell->tail.handle.Get(aCx->isolate).As<v8::String>();
}

JS::Symbol* SymbolFromV8(JSContext* aCx, v8::Local<v8::Symbol> aSymbol) {
  int hash = aSymbol->GetIdentityHash();
  auto range = aCx->runtime->symbols.equal_range(hash);
  for (auto it = range.first; it != range.second; ++it) {
    if (it->second->tail.handle.Get(aCx->isolate) == aSymbol) {
      return it->second->asSymbol();
    }
  }
  SymbolCell* cell = static_cast<SymbolCell*>(AllocCell(sizeof(SymbolCell)));
  cell->tail.kind = CellKind::Symbol;
  cell->code = JS::shadow::Symbol::WellKnownAPILimit;
  BindCellHandle(aCx, &cell->tail, aSymbol, true);
  aCx->runtime->symbols.emplace(hash, cell);
  return cell->asSymbol();
}

v8::Local<v8::Symbol> ToV8(JSContext* aCx, JS::Symbol* aSym) {
  return SymbolCell::from(aSym)->tail.handle.Get(aCx->isolate).As<v8::Symbol>();
}

JS::BigInt* BigIntFromV8(JSContext* aCx, v8::Local<v8::BigInt> aBigInt) {
  BigIntCell* cell = static_cast<BigIntCell*>(AllocCell(sizeof(BigIntCell)));
  cell->tail.kind = CellKind::BigInt;
  BindCellHandle(aCx, &cell->tail, aBigInt, false);
  return cell->asBigInt();
}

v8::Local<v8::BigInt> ToV8(JSContext* aCx, JS::BigInt* aBig) {
  return BigIntCell::from(aBig)->tail.handle.Get(aCx->isolate).As<v8::BigInt>();
}

JS::Value ValueFromV8(JSContext* aCx, v8::Local<v8::Value> aValue) {
  if (aValue.IsEmpty() || aValue->IsUndefined()) {
    return JS::UndefinedValue();
  }
  if (aValue->IsNull()) {
    return JS::NullValue();
  }
  if (aValue->IsBoolean()) {
    return JS::BooleanValue(aValue->IsTrue());
  }
  if (aValue->IsInt32()) {
    return JS::Int32Value(aValue.As<v8::Int32>()->Value());
  }
  if (aValue->IsNumber()) {
    return JS::DoubleValue(aValue.As<v8::Number>()->Value());
  }
  if (aValue->IsString()) {
    return JS::StringValue(StringFromV8(aCx, aValue.As<v8::String>()));
  }
  if (aValue->IsSymbol()) {
    return JS::SymbolValue(SymbolFromV8(aCx, aValue.As<v8::Symbol>()));
  }
  if (aValue->IsBigInt()) {
    return JS::BigIntValue(BigIntFromV8(aCx, aValue.As<v8::BigInt>()));
  }
  return JS::ObjectValue(*ObjectFromV8(aCx, aValue.As<v8::Object>()));
}

bool ValueFromV8(JSContext* aCx, v8::Local<v8::Value> aValue,
                 JS::MutableHandleValue aResult) {
  aResult.set(ValueFromV8(aCx, aValue));
  return true;
}

v8::Local<v8::Value> ToV8(JSContext* aCx, const JS::Value& aValue) {
  v8::Isolate* isolate = aCx->isolate;
  if (aValue.isUndefined() || aValue.isMagic()) {
    return v8::Undefined(isolate);
  }
  if (aValue.isNull()) {
    return v8::Null(isolate);
  }
  if (aValue.isBoolean()) {
    return v8::Boolean::New(isolate, aValue.toBoolean());
  }
  if (aValue.isInt32()) {
    return v8::Integer::New(isolate, aValue.toInt32());
  }
  if (aValue.isDouble()) {
    return v8::Number::New(isolate, aValue.toDouble());
  }
  if (aValue.isString()) {
    return ToV8(aCx, aValue.toString());
  }
  if (aValue.isSymbol()) {
    return ToV8(aCx, aValue.toSymbol());
  }
  if (aValue.isBigInt()) {
    return ToV8(aCx, aValue.toBigInt());
  }
  if (aValue.isObject()) {
    return ToV8(aCx, &aValue.toObject());
  }
  return v8::Undefined(isolate);
}

bool IdFromV8(JSContext* aCx, v8::Local<v8::Value> aKey,
              JS::MutableHandleId aResult) {
  if (aKey->IsSymbol()) {
    aResult.set(JS::PropertyKey::Symbol(SymbolFromV8(aCx, aKey.As<v8::Symbol>())));
    return true;
  }
  if (aKey->IsUint32()) {
    uint32_t index = aKey.As<v8::Uint32>()->Value();
    if (index <= JS::PropertyKey::IntMax) {
      aResult.set(JS::PropertyKey::Int(int32_t(index)));
      return true;
    }
  }
  v8::Local<v8::String> name;
  if (!aKey->ToString(aCx->v8Context()).ToLocal(&name)) {
    return false;
  }
  v8::String::Value chars(aCx->isolate, name);
  uint32_t index = 0;
  bool isIndex = chars.length() > 0 && chars.length() <= 10;
  for (int i = 0; isIndex && i < chars.length(); i++) {
    char16_t c = (*chars)[i];
    if (c < '0' || c > '9' || (i == 0 && c == '0' && chars.length() > 1)) {
      isIndex = false;
      break;
    }
    index = index * 10 + (c - '0');
  }
  if (isIndex && index <= JS::PropertyKey::IntMax) {
    aResult.set(JS::PropertyKey::Int(int32_t(index)));
    return true;
  }
  aResult.set(JS::PropertyKey::NonIntAtom(AtomFromV8(aCx, name)));
  return true;
}

v8::Local<v8::Value> ToV8(JSContext* aCx, jsid aId) {
  if (aId.isInt()) {
    return v8::Integer::New(aCx->isolate, aId.toInt());
  }
  if (aId.isSymbol()) {
    return ToV8(aCx, aId.toSymbol());
  }
  if (aId.isString()) {
    return ToV8(aCx, aId.toString());
  }
  return v8::Undefined(aCx->isolate);
}

}
