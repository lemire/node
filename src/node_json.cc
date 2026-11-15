// Experimental node:json bindings: lazy views over a simdjson tape and
// schema-driven ("typed") decoding straight from the tape.
//
// Tape format reminder (see simdjson doc/tape.md). Each entry is 64 bits; the
// top 8 bits hold the type character and the low 56 bits a payload.
//   'r'        root; the first one's payload is the number of tape entries.
//   '{' / '['  payload low 32 bits: index just past the matching '}' / ']';
//              bits 32..55: element count, saturated at 0xFFFFFF.
//   '}' / ']'  payload: index of the opening brace.
//   '"'        payload: offset into the string buffer, which holds a 32-bit
//              length, the UTF-8 bytes and a NUL terminator.
//   'l' 'u' 'd' the next tape word is an int64 / uint64 / double.
//   't' 'f' 'n' true / false / null.
//   'Z'        an integer beyond 64 bits, kept as digits in the string buffer
//              (same layout as '"'); converted to a double like JSON.parse.
// Inside an object, keys and values alternate: '{' key value key value '}'.

#include "base_object-inl.h"
#include "env-inl.h"
#include "fast_float/fast_float.h"
#include "node_errors.h"
#include "node_external_reference.h"
#include "simdjson.h"
#include "util-inl.h"
#include "v8.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace node {
namespace json {

using v8::Array;
using v8::Boolean;
using v8::Context;
using v8::DictionaryTemplate;
using v8::Exception;
using v8::FunctionCallbackInfo;
using v8::FunctionTemplate;
using v8::Global;
using v8::Int32;
using v8::Integer;
using v8::Isolate;
using v8::Local;
using v8::LocalVector;
using v8::MaybeLocal;
using v8::NewStringType;
using v8::Null;
using v8::Number;
using v8::Object;
using v8::String;
using v8::Uint32;
using v8::Undefined;
using v8::Value;

namespace {

constexpr uint64_t kPayloadMask = 0x00FFFFFFFFFFFFFFULL;
constexpr uint32_t kCountMask = 0xFFFFFF;

inline char TapeType(const uint64_t* tape, size_t i) {
  return static_cast<char>(tape[i] >> 56);
}

// For '{' and '[': the index just past the matching closing brace.
inline uint32_t AfterScope(const uint64_t* tape, size_t i) {
  return static_cast<uint32_t>(tape[i]);
}

inline size_t AfterElement(const uint64_t* tape, size_t i) {
  switch (TapeType(tape, i)) {
    case '{':
    case '[':
      return AfterScope(tape, i);
    case 'l':
    case 'u':
    case 'd':
      return i + 2;
    default:
      return i + 1;
  }
}

inline std::string_view TapeString(const uint64_t* tape,
                                   const uint8_t* strings,
                                   size_t i) {
  size_t offset = static_cast<size_t>(tape[i] & kPayloadMask);
  uint32_t len;
  memcpy(&len, strings + offset, sizeof(len));
  return std::string_view(
      reinterpret_cast<const char*>(strings + offset + sizeof(len)), len);
}

inline double TapeDouble(const uint64_t* tape, size_t i) {
  double d;
  memcpy(&d, &tape[i + 1], sizeof(d));
  return d;
}

inline int64_t TapeInt64(const uint64_t* tape, size_t i) {
  int64_t v;
  memcpy(&v, &tape[i + 1], sizeof(v));
  return v;
}

// Number of elements (array) or key/value pairs (object) in a scope.
inline uint32_t ScopeCount(const uint64_t* tape, size_t i) {
  uint32_t count = static_cast<uint32_t>((tape[i] >> 32) & kCountMask);
  if (count < kCountMask) return count;
  // Saturated: count by walking.
  const bool is_object = TapeType(tape, i) == '{';
  const size_t end = AfterScope(tape, i) - 1;
  count = 0;
  for (size_t j = i + 1; j < end; count++) {
    if (is_object) j++;  // skip the key
    j = AfterElement(tape, j);
  }
  return count;
}

// A JSON tape and string buffer, sized to fit one parsed document.
struct Tape {
  std::unique_ptr<uint64_t[]> tape;
  std::unique_ptr<uint8_t[]> strings;
  size_t tape_len = 0;
  size_t strings_len = 0;
};

// Each thread keeps one parser and one scratch document, reused across calls
// so that the (large) simdjson buffers are not reallocated for every parse.
struct ThreadParser {
  simdjson::dom::parser parser;
  simdjson::dom::document doc;
};

ThreadParser& GetThreadParser() {
  // Per thread, not per Environment: the parser holds no JavaScript state
  // and only serves one synchronous call at a time.
  // NOLINTNEXTLINE(runtime/thread_local)
  static thread_local ThreadParser state;
  state.parser.number_as_string(true);  // big integers become 'Z', not errors
  return state;
}

Local<String> OneByteString(Isolate* isolate, std::string_view s) {
  return String::NewFromOneByte(isolate,
                                reinterpret_cast<const uint8_t*>(s.data()),
                                NewStringType::kNormal,
                                static_cast<int>(s.size()))
      .ToLocalChecked();
}

void ThrowParseError(Isolate* isolate, simdjson::error_code error) {
  std::string message = "Invalid JSON: ";
  message += simdjson::error_message(error);
  isolate->ThrowException(
      Exception::SyntaxError(OneByteString(isolate, message)));
}

// Parses args[index] (a string or an ArrayBufferView) into the thread's
// scratch document. Returns false, with an exception pending, on failure.
bool ParseInput(Environment* env,
                Local<Value> input,
                simdjson::dom::document** out) {
  Isolate* isolate = env->isolate();
  ThreadParser& state = GetThreadParser();
  simdjson::error_code error;
  if (input->IsString()) {
    Utf8Value utf8(isolate, input);
    error = state.parser
                .parse_into_document_unpadded(
                    state.doc,
                    reinterpret_cast<const uint8_t*>(*utf8),
                    utf8.length())
                .error();
  } else {
    CHECK(input->IsArrayBufferView());
    ArrayBufferViewContents<uint8_t> contents(input);
    error = state.parser
                .parse_into_document_unpadded(
                    state.doc, contents.data(), contents.length())
                .error();
  }
  if (error != simdjson::SUCCESS) {
    ThrowParseError(isolate, error);
    return false;
  }
  *out = &state.doc;
  return true;
}

// Copies the used part of a parsed document into right-sized buffers. The
// scratch document is sized for the worst case (about ten bytes per input
// byte); a document's used tape is usually much smaller.
Tape CopyTape(const simdjson::dom::document& doc) {
  Tape t;
  const uint64_t* tape = doc.tape.get();
  // The payload of the opening 'r' is the number of tape entries, the last
  // one being the closing 'r'. Entries past it are stale (the scratch tape is
  // reused), so never read beyond.
  t.tape_len = static_cast<size_t>(tape[0] & kPayloadMask);
  CHECK_EQ(TapeType(tape, t.tape_len - 1), 'r');
  size_t strings_end = 0;
  for (size_t i = 1; i < t.tape_len;) {
    char type = TapeType(tape, i);
    if (type == '"' || type == 'Z') {
      size_t offset = static_cast<size_t>(tape[i] & kPayloadMask);
      uint32_t len;
      memcpy(&len, doc.string_buf.get() + offset, sizeof(len));
      size_t end = offset + sizeof(len) + len + 1;
      if (end > strings_end) strings_end = end;
      i++;
    } else if (type == 'l' || type == 'u' || type == 'd') {
      i += 2;
    } else {
      i++;
    }
  }
  t.strings_len = strings_end;
  t.tape.reset(new uint64_t[t.tape_len]);
  memcpy(t.tape.get(), tape, t.tape_len * sizeof(uint64_t));
  t.strings.reset(new uint8_t[strings_end > 0 ? strings_end : 1]);
  if (strings_end > 0)
    memcpy(t.strings.get(), doc.string_buf.get(), strings_end);
  return t;
}

// Converts a scalar tape entry (not '{' or '[') to a JavaScript value.
MaybeLocal<Value> ScalarToJS(Isolate* isolate,
                             const uint64_t* tape,
                             const uint8_t* strings,
                             size_t i) {
  switch (TapeType(tape, i)) {
    case '"': {
      std::string_view s = TapeString(tape, strings, i);
      if (s.size() > static_cast<size_t>(String::kMaxLength)) {
        isolate->ThrowException(ERR_STRING_TOO_LONG(isolate));
        return MaybeLocal<Value>();
      }
      return String::NewFromUtf8(isolate,
                                 s.data(),
                                 NewStringType::kNormal,
                                 static_cast<int>(s.size()));
    }
    case 'l':
      return Number::New(isolate, static_cast<double>(TapeInt64(tape, i)));
    case 'u': {
      uint64_t v;
      memcpy(&v, &tape[i + 1], sizeof(v));
      return Number::New(isolate, static_cast<double>(v));
    }
    case 'd':
      return Number::New(isolate, TapeDouble(tape, i));
    case 'Z': {
      // Digits only (simdjson validated them), so from_chars cannot fail;
      // it rounds correctly, as JSON.parse does.
      std::string_view digits = TapeString(tape, strings, i);
      double d = 0;
      fast_float::from_chars(digits.data(), digits.data() + digits.size(), d);
      return Number::New(isolate, d);
    }
    case 't':
      return Boolean::New(isolate, true);
    case 'f':
      return Boolean::New(isolate, false);
    case 'n':
      return Null(isolate);
    default:
      UNREACHABLE();
  }
}

MaybeLocal<String> KeyToJS(Isolate* isolate, std::string_view key) {
  return String::NewFromUtf8(isolate,
                             key.data(),
                             NewStringType::kInternalized,
                             static_cast<int>(key.size()));
}

// Eagerly converts the value at tape index i, as JSON.parse would.
MaybeLocal<Value> ToJS(Isolate* isolate,
                       Local<Context> context,
                       const uint64_t* tape,
                       const uint8_t* strings,
                       size_t i) {
  switch (TapeType(tape, i)) {
    case '{': {
      Local<Object> object = Object::New(isolate);
      const size_t end = AfterScope(tape, i) - 1;
      for (size_t j = i + 1; j < end;) {
        Local<String> key;
        Local<Value> value;
        if (!KeyToJS(isolate, TapeString(tape, strings, j)).ToLocal(&key) ||
            !ToJS(isolate, context, tape, strings, j + 1).ToLocal(&value) ||
            object->CreateDataProperty(context, key, value).IsNothing()) {
          return MaybeLocal<Value>();
        }
        j = AfterElement(tape, j + 1);
      }
      return object;
    }
    case '[': {
      LocalVector<Value> elements(isolate);
      elements.reserve(ScopeCount(tape, i));
      const size_t end = AfterScope(tape, i) - 1;
      for (size_t j = i + 1; j < end; j = AfterElement(tape, j)) {
        Local<Value> value;
        if (!ToJS(isolate, context, tape, strings, j).ToLocal(&value)) {
          return MaybeLocal<Value>();
        }
        elements.push_back(value);
      }
      return Array::New(isolate, elements.data(), elements.size());
    }
    default:
      return ScalarToJS(isolate, tape, strings, i);
  }
}

// Finds the value of `key` in the object at tape index i. Returns 0 (never a
// valid value index) when the key is absent. With duplicate keys, the first
// one wins.
size_t FindKey(const uint64_t* tape,
               const uint8_t* strings,
               size_t i,
               std::string_view key) {
  const size_t end = AfterScope(tape, i) - 1;
  for (size_t j = i + 1; j < end;) {
    if (TapeString(tape, strings, j) == key) return j + 1;
    j = AfterElement(tape, j + 1);
  }
  return 0;
}

// Returns the tape index of element `index` in the array at i, or 0.
size_t FindIndex(const uint64_t* tape, size_t i, uint32_t index) {
  const size_t end = AfterScope(tape, i) - 1;
  size_t j = i + 1;
  for (uint32_t k = 0; j < end; k++) {
    if (k == index) return j;
    j = AfterElement(tape, j);
  }
  return 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Lazy views.

class JSONDocument : public BaseObject {
 public:
  JSONDocument(Environment* env, Local<Object> wrap, Tape&& tape)
      : BaseObject(env, wrap), tape_(std::move(tape)) {
    MakeWeak();
    env->external_memory_accounter()->Increase(env->isolate(), ByteSize());
  }

  ~JSONDocument() override {
    env()->external_memory_accounter()->Decrease(env()->isolate(), ByteSize());
  }

  size_t ByteSize() const {
    return tape_.tape_len * sizeof(uint64_t) + tape_.strings_len;
  }

  const uint64_t* tape() const { return tape_.tape.get(); }
  const uint8_t* strings() const { return tape_.strings.get(); }

  void MemoryInfo(MemoryTracker* tracker) const override {
    tracker->TrackFieldWithSize("tape", ByteSize());
  }
  SET_MEMORY_INFO_NAME(JSONDocument)
  SET_SELF_SIZE(JSONDocument)

 private:
  Tape tape_;
};

// Internal fields of JSONObject and JSONArray views.
enum ViewField {
  kViewDocument = 0,    // the JSONDocument wrapper object
  kViewIndex = 1,       // tape index of the '{' or '['
  kViewCursorPos = 2,   // arrays: element index of the cursor
  kViewCursorTape = 3,  // arrays: tape index of the cursor element
  kObjectViewFieldCount = 2,
  kArrayViewFieldCount = 4,
};

namespace {

MaybeLocal<Value> NewView(Environment* env,
                          Local<Object> document,
                          const uint64_t* tape,
                          size_t i) {
  Isolate* isolate = env->isolate();
  Local<Context> context = env->context();
  const bool is_array = TapeType(tape, i) == '[';
  Local<FunctionTemplate> tmpl =
      is_array ? env->json_array_view_constructor_template()
               : env->json_object_view_constructor_template();
  Local<Object> view;
  if (!tmpl->InstanceTemplate()->NewInstance(context).ToLocal(&view)) {
    return MaybeLocal<Value>();
  }
  view->SetInternalField(kViewDocument, document);
  view->SetInternalField(
      kViewIndex, Integer::NewFromUnsigned(isolate, static_cast<uint32_t>(i)));
  if (is_array) {
    view->SetInternalField(kViewCursorPos, Integer::New(isolate, 0));
    view->SetInternalField(
        kViewCursorTape,
        Integer::NewFromUnsigned(isolate, static_cast<uint32_t>(i + 1)));
  }
  return view;
}

// Returns a view for containers, a plain value for scalars.
MaybeLocal<Value> Materialize(Environment* env,
                              Local<Object> document_wrap,
                              const JSONDocument* document,
                              size_t i) {
  const uint64_t* tape = document->tape();
  char type = TapeType(tape, i);
  if (type == '{' || type == '[') {
    return NewView(env, document_wrap, tape, i);
  }
  return ScalarToJS(env->isolate(), tape, document->strings(), i);
}

struct ViewState {
  Local<Object> document_wrap;
  JSONDocument* document;
  size_t index;
};

// Reads the internal fields of `this`. Returns false (with an exception
// pending) when the receiver is not a properly initialized view.
bool GetViewState(const FunctionCallbackInfo<Value>& args, ViewState* state) {
  Local<Object> self = args.This();
  Local<Value> document = self->GetInternalField(kViewDocument).As<Value>();
  Local<Value> index = self->GetInternalField(kViewIndex).As<Value>();
  if (!document->IsObject() || !index->IsUint32()) {
    THROW_ERR_INVALID_THIS(Environment::GetCurrent(args));
    return false;
  }
  state->document_wrap = document.As<Object>();
  state->document = BaseObject::Unwrap<JSONDocument>(state->document_wrap);
  state->index = index.As<Uint32>()->Value();
  return state->document != nullptr;
}

void IllegalConstructor(const FunctionCallbackInfo<Value>& args) {
  THROW_ERR_ILLEGAL_CONSTRUCTOR(Environment::GetCurrent(args));
}

// parse(input): returns a view (or a scalar) over a freshly parsed document.
void Parse(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  simdjson::dom::document* doc;
  if (!ParseInput(env, args[0], &doc)) return;
  Local<Object> wrap;
  if (!env->json_document_constructor_template()
           ->InstanceTemplate()
           ->NewInstance(env->context())
           .ToLocal(&wrap)) {
    return;
  }
  JSONDocument* document = new JSONDocument(env, wrap, CopyTape(*doc));
  Local<Value> result;
  // Index 1 is the root value; index 0 is the 'r' entry.
  if (Materialize(env, wrap, document, 1).ToLocal(&result)) {
    args.GetReturnValue().Set(result);
  }
}

// JSONObject.prototype.get(key)
void ObjectGet(const FunctionCallbackInfo<Value>& args) {
  ViewState s;
  if (!GetViewState(args, &s)) return;
  Environment* env = Environment::GetCurrent(args);
  Utf8Value key(env->isolate(), args[0]);
  size_t j = FindKey(
      s.document->tape(), s.document->strings(), s.index, key.ToStringView());
  if (j == 0) return;  // undefined
  Local<Value> result;
  if (Materialize(env, s.document_wrap, s.document, j).ToLocal(&result)) {
    args.GetReturnValue().Set(result);
  }
}

// JSONObject.prototype.has(key)
void ObjectHas(const FunctionCallbackInfo<Value>& args) {
  ViewState s;
  if (!GetViewState(args, &s)) return;
  Utf8Value key(args.GetIsolate(), args[0]);
  args.GetReturnValue().Set(FindKey(s.document->tape(),
                                    s.document->strings(),
                                    s.index,
                                    key.ToStringView()) != 0);
}

// JSONObject.prototype.size / JSONArray.prototype.length
void ViewCount(const FunctionCallbackInfo<Value>& args) {
  ViewState s;
  if (!GetViewState(args, &s)) return;
  args.GetReturnValue().Set(ScopeCount(s.document->tape(), s.index));
}

enum class EntryKind { kKeys, kValues, kEntries };

template <EntryKind kind>
void ObjectEntries(const FunctionCallbackInfo<Value>& args) {
  ViewState s;
  if (!GetViewState(args, &s)) return;
  Environment* env = Environment::GetCurrent(args);
  Isolate* isolate = env->isolate();
  const uint64_t* tape = s.document->tape();
  const uint8_t* strings = s.document->strings();
  LocalVector<Value> out(isolate);
  out.reserve(ScopeCount(tape, s.index));
  const size_t end = AfterScope(tape, s.index) - 1;
  for (size_t j = s.index + 1; j < end; j = AfterElement(tape, j + 1)) {
    Local<String> key;
    Local<Value> value;
    if (kind != EntryKind::kValues &&
        !KeyToJS(isolate, TapeString(tape, strings, j)).ToLocal(&key)) {
      return;
    }
    if (kind != EntryKind::kKeys &&
        !Materialize(env, s.document_wrap, s.document, j + 1).ToLocal(&value)) {
      return;
    }
    switch (kind) {
      case EntryKind::kKeys:
        out.push_back(key);
        break;
      case EntryKind::kValues:
        out.push_back(value);
        break;
      case EntryKind::kEntries: {
        Local<Value> pair[] = {key, value};
        out.push_back(Array::New(isolate, pair, 2));
        break;
      }
    }
  }
  args.GetReturnValue().Set(Array::New(isolate, out.data(), out.size()));
}

// JSONArray.prototype.at(index). Walks from a cursor kept on the view, so a
// forward loop over the indexes is linear overall.
void ArrayAt(const FunctionCallbackInfo<Value>& args) {
  ViewState s;
  if (!GetViewState(args, &s)) return;
  Environment* env = Environment::GetCurrent(args);
  Isolate* isolate = env->isolate();
  const uint64_t* tape = s.document->tape();
  double requested = args[0]->IsNumber() ? args[0].As<Number>()->Value() : 0;
  if (std::isnan(requested)) requested = 0;
  requested = std::trunc(requested);
  const double count = ScopeCount(tape, s.index);
  if (requested < 0) requested += count;
  if (requested < 0 || requested >= count) return;  // undefined
  const uint32_t target = static_cast<uint32_t>(requested);

  Local<Object> self = args.This();
  uint32_t pos =
      self->GetInternalField(kViewCursorPos).As<Value>().As<Uint32>()->Value();
  size_t j =
      self->GetInternalField(kViewCursorTape).As<Value>().As<Uint32>()->Value();
  if (target < pos) {
    pos = 0;
    j = s.index + 1;
  }
  for (; pos < target; pos++) j = AfterElement(tape, j);
  self->SetInternalField(kViewCursorPos,
                         Integer::NewFromUnsigned(isolate, pos));
  self->SetInternalField(
      kViewCursorTape,
      Integer::NewFromUnsigned(isolate, static_cast<uint32_t>(j)));
  Local<Value> result;
  if (Materialize(env, s.document_wrap, s.document, j).ToLocal(&result)) {
    args.GetReturnValue().Set(result);
  }
}

// JSONArray.prototype.values(): an Array of the elements, nested
// containers as views.
void ArrayValues(const FunctionCallbackInfo<Value>& args) {
  ViewState s;
  if (!GetViewState(args, &s)) return;
  Environment* env = Environment::GetCurrent(args);
  Isolate* isolate = env->isolate();
  const uint64_t* tape = s.document->tape();
  LocalVector<Value> out(isolate);
  out.reserve(ScopeCount(tape, s.index));
  const size_t end = AfterScope(tape, s.index) - 1;
  for (size_t j = s.index + 1; j < end; j = AfterElement(tape, j)) {
    Local<Value> value;
    if (!Materialize(env, s.document_wrap, s.document, j).ToLocal(&value)) {
      return;
    }
    out.push_back(value);
  }
  args.GetReturnValue().Set(Array::New(isolate, out.data(), out.size()));
}

// toJSON(): converts the whole subtree, as JSON.parse would.
void ViewToJS(const FunctionCallbackInfo<Value>& args) {
  ViewState s;
  if (!GetViewState(args, &s)) return;
  Environment* env = Environment::GetCurrent(args);
  Local<Value> result;
  if (ToJS(env->isolate(),
           env->context(),
           s.document->tape(),
           s.document->strings(),
           s.index)
          .ToLocal(&result)) {
    args.GetReturnValue().Set(result);
  }
}

// atPointer(pointer): RFC 6901 JSON Pointer, relative to the view.
void ViewAtPointer(const FunctionCallbackInfo<Value>& args) {
  ViewState s;
  if (!GetViewState(args, &s)) return;
  Environment* env = Environment::GetCurrent(args);
  Utf8Value pointer_value(env->isolate(), args[0]);
  std::string_view pointer = pointer_value.ToStringView();
  const uint64_t* tape = s.document->tape();
  const uint8_t* strings = s.document->strings();
  size_t i = s.index;
  if (!pointer.empty() && pointer[0] != '/') {
    THROW_ERR_INVALID_ARG_VALUE(
        env, "A JSON Pointer must be empty or start with '/'");
    return;
  }
  std::string token;
  size_t pos = 0;
  while (pos < pointer.size()) {
    // pointer[pos] == '/'
    size_t next = pointer.find('/', pos + 1);
    if (next == std::string_view::npos) next = pointer.size();
    std::string_view raw = pointer.substr(pos + 1, next - pos - 1);
    pos = next;
    token.clear();
    for (size_t k = 0; k < raw.size(); k++) {
      if (raw[k] == '~') {
        if (k + 1 < raw.size() && (raw[k + 1] == '0' || raw[k + 1] == '1')) {
          token += raw[k + 1] == '0' ? '~' : '/';
          k++;
        } else {
          THROW_ERR_INVALID_ARG_VALUE(env, "Invalid escape in JSON Pointer");
          return;
        }
      } else {
        token += raw[k];
      }
    }
    char type = TapeType(tape, i);
    if (type == '{') {
      i = FindKey(tape, strings, i, token);
    } else if (type == '[') {
      // A canonical decimal index: no sign, no leading zero.
      if (token.empty() || token.size() > 10 ||
          (token.size() > 1 && token[0] == '0')) {
        return;
      }
      uint64_t index = 0;
      for (char c : token) {
        if (c < '0' || c > '9') return;
        index = index * 10 + (c - '0');
      }
      if (index >= UINT32_MAX) return;
      i = FindIndex(tape, i, static_cast<uint32_t>(index));
    } else {
      return;
    }
    if (i == 0) return;  // not found: undefined
  }
  Local<Value> result;
  if (Materialize(env, s.document_wrap, s.document, i).ToLocal(&result)) {
    args.GetReturnValue().Set(result);
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Typed decoding.

// Bits of SchemaNode::mask: which JSON kinds a node accepts.
enum KindBit : uint32_t {
  kNull = 1,
  kBoolean = 2,
  kInteger = 4,  // also an integral double such as 1.0
  kNumber = 8,   // any number; schemas set kInteger | kNumber
  kString = 16,
  kObject = 32,
  kArray = 64,
};

enum DefaultKind : int32_t {
  kNoDefault = 0,
  kValueDefault = 1,
  kJSONDefault = 2
};

struct SchemaNode {
  uint32_t mask = 0;
  // Objects.
  bool has_properties = false;
  bool additional = false;
  bool use_template = true;
  std::vector<std::string> keys;
  std::vector<int32_t> props;
  std::vector<bool> required;
  std::vector<int32_t> default_kinds;
  std::vector<Global<Value>> defaults;
  Global<DictionaryTemplate> tmpl;
  // Arrays.
  int32_t items = -1;
  // enum / const.
  std::vector<Global<Value>> enum_values;
};

class SchemaPlan : public BaseObject {
 public:
  SchemaPlan(Environment* env, Local<Object> wrap) : BaseObject(env, wrap) {
    MakeWeak();
  }
  std::vector<SchemaNode> nodes;

  void MemoryInfo(MemoryTracker* tracker) const override {}
  SET_MEMORY_INFO_NAME(SchemaPlan)
  SET_SELF_SIZE(SchemaPlan)
};

namespace {

bool NeedsGenericObject(std::string_view key) {
  // DictionaryTemplate only takes plain named properties.
  if (key == "__proto__" || key.empty()) return true;
  for (char c : key) {
    if (c < '0' || c > '9') return false;
  }
  return true;  // an array index such as "0"
}

Local<Value> GetField(Local<Context> context,
                      Local<Object> object,
                      const char* name) {
  Isolate* isolate = Isolate::GetCurrent();
  return object->Get(context, OneByteString(isolate, name)).ToLocalChecked();
}

// compileSchema(nodes): `nodes` is the array produced by compileSchema() in
// lib/json.js. Each entry is an object with the fields read below.
void CompileSchema(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  Isolate* isolate = env->isolate();
  Local<Context> context = env->context();
  CHECK(args[0]->IsArray());
  Local<Array> nodes = args[0].As<Array>();
  Local<Object> wrap;
  if (!env->json_schema_plan_constructor_template()
           ->InstanceTemplate()
           ->NewInstance(context)
           .ToLocal(&wrap)) {
    return;
  }
  SchemaPlan* plan = new SchemaPlan(env, wrap);
  plan->nodes.resize(nodes->Length());
  for (uint32_t n = 0; n < nodes->Length(); n++) {
    Local<Object> desc = nodes->Get(context, n).ToLocalChecked().As<Object>();
    SchemaNode& node = plan->nodes[n];
    node.mask = GetField(context, desc, "mask").As<Uint32>()->Value();
    node.items = GetField(context, desc, "items").As<Int32>()->Value();
    node.additional = GetField(context, desc, "additional")->IsTrue();
    Local<Value> keys = GetField(context, desc, "keys");
    if (keys->IsArray()) {
      node.has_properties = true;
      Local<Array> key_array = keys.As<Array>();
      Local<Array> props = GetField(context, desc, "props").As<Array>();
      Local<Array> required = GetField(context, desc, "required").As<Array>();
      Local<Array> default_kinds =
          GetField(context, desc, "defaultKinds").As<Array>();
      Local<Array> defaults = GetField(context, desc, "defaults").As<Array>();
      const uint32_t count = key_array->Length();
      for (uint32_t k = 0; k < count; k++) {
        Utf8Value key(isolate, key_array->Get(context, k).ToLocalChecked());
        node.keys.emplace_back(key.ToStringView());
        if (NeedsGenericObject(node.keys.back())) node.use_template = false;
        node.props.push_back(
            props->Get(context, k).ToLocalChecked().As<Int32>()->Value());
        node.required.push_back(
            required->Get(context, k).ToLocalChecked()->IsTrue());
        node.default_kinds.push_back(default_kinds->Get(context, k)
                                         .ToLocalChecked()
                                         .As<Int32>()
                                         ->Value());
        node.defaults.emplace_back(isolate,
                                   defaults->Get(context, k).ToLocalChecked());
      }
      if (node.use_template) {
        std::vector<std::string_view> names(node.keys.begin(), node.keys.end());
        node.tmpl.Reset(
            isolate,
            DictionaryTemplate::New(isolate, {names.data(), names.size()}));
      }
    }
    Local<Value> enum_values = GetField(context, desc, "enum");
    if (enum_values->IsArray()) {
      Local<Array> values = enum_values.As<Array>();
      for (uint32_t k = 0; k < values->Length(); k++) {
        node.enum_values.emplace_back(isolate,
                                      values->Get(context, k).ToLocalChecked());
      }
    }
  }
  args.GetReturnValue().Set(wrap);
}

const char* KindName(const uint64_t* tape, size_t i) {
  switch (TapeType(tape, i)) {
    case '{':
      return "object";
    case '[':
      return "array";
    case '"':
      return "string";
    case 'l':
    case 'u':
    case 'Z':
      return "integer";
    case 'd':
      return "number";
    case 't':
    case 'f':
      return "boolean";
    default:
      return "null";
  }
}

std::string MaskNames(uint32_t mask) {
  static const struct {
    uint32_t bit;
    const char* name;
  } names[] = {{kObject, "object"},
               {kArray, "array"},
               {kString, "string"},
               {kNumber, "number"},
               {kInteger, "integer"},
               {kBoolean, "boolean"},
               {kNull, "null"}};
  std::string out;
  for (const auto& entry : names) {
    if (!(mask & entry.bit)) continue;
    if (entry.bit == kInteger && (mask & kNumber)) continue;
    if (!out.empty()) out += " or ";
    out += entry.name;
  }
  return out;
}

class Decoder {
 public:
  Decoder(Environment* env,
          const SchemaPlan* plan,
          const simdjson::dom::document& doc)
      : env_(env),
        isolate_(env->isolate()),
        context_(env->context()),
        tape_(doc.tape.get()),
        strings_(doc.string_buf.get()),
        plan_(plan),
        slots_() {}

  MaybeLocal<Value> Decode(size_t i, int32_t n) {
    const SchemaNode& node = plan_->nodes[n];
    const char type = TapeType(tape_, i);
    uint32_t bit;
    switch (type) {
      case '{':
        bit = kObject;
        break;
      case '[':
        bit = kArray;
        break;
      case '"':
        bit = kString;
        break;
      case 'l':
      case 'u':
      case 'Z':
        bit = kInteger;
        break;
      case 'd': {
        double d = TapeDouble(tape_, i);
        bit = (std::trunc(d) == d && std::isfinite(d)) ? (kNumber | kInteger)
                                                       : kNumber;
        break;
      }
      case 't':
      case 'f':
        bit = kBoolean;
        break;
      default:
        bit = kNull;
        break;
    }
    if (!(node.mask & bit)) {
      return Fail("Expected " + MaskNames(node.mask) + ", got " +
                  KindName(tape_, i));
    }
    MaybeLocal<Value> result;
    if (type == '{' && node.has_properties) {
      result = DecodeObject(i, node);
    } else if (type == '[' && node.items >= 0) {
      result = DecodeArray(i, node);
    } else {
      result = ToJS(isolate_, context_, tape_, strings_, i);
    }
    if (!node.enum_values.empty()) {
      Local<Value> value;
      if (!result.ToLocal(&value)) return result;
      for (const auto& allowed : node.enum_values) {
        if (allowed.Get(isolate_)->StrictEquals(value)) return result;
      }
      return Fail("Value is not one of the allowed values");
    }
    return result;
  }

 private:
  struct PathSegment {
    std::string_view key;
    int64_t index;  // -1 for a key
  };

  MaybeLocal<Value> Fail(const std::string& reason) {
    std::string message = reason + " at $";
    for (const PathSegment& segment : path_) {
      if (segment.index >= 0) {
        message += "[" + std::to_string(segment.index) + "]";
      } else {
        message += ".";
        message += segment.key;
      }
    }
    THROW_ERR_JSON_SCHEMA_VALIDATION(isolate_, message);
    return MaybeLocal<Value>();
  }

  MaybeLocal<Value> DefaultValue(const SchemaNode& node, size_t k) {
    Local<Value> stored = node.defaults[k].Get(isolate_);
    if (node.default_kinds[k] == kValueDefault) return stored;
    // Objects and arrays are re-created for each use, like a default factory.
    return v8::JSON::Parse(context_, stored.As<String>());
  }

  MaybeLocal<Value> DecodeObject(size_t i, const SchemaNode& node) {
    const size_t count = node.keys.size();
    const size_t base = slots_.size();
    slots_.resize(base + count);
    LocalVector<Value> extra(isolate_);  // additional properties: key, value
    const size_t end = AfterScope(tape_, i) - 1;
    size_t hint = 0;
    for (size_t j = i + 1; j < end; j = AfterElement(tape_, j + 1)) {
      std::string_view key = TapeString(tape_, strings_, j);
      // Keys usually come in schema order: try the next expected one first.
      size_t k = count;
      if (hint < count && node.keys[hint] == key) {
        k = hint;
      } else {
        for (size_t m = 0; m < count; m++) {
          if (node.keys[m] == key) {
            k = m;
            break;
          }
        }
      }
      if (k == count) {
        if (node.additional) {
          Local<String> js_key;
          Local<Value> value;
          if (!KeyToJS(isolate_, key).ToLocal(&js_key) ||
              !ToJS(isolate_, context_, tape_, strings_, j + 1)
                   .ToLocal(&value)) {
            slots_.resize(base);
            return MaybeLocal<Value>();
          }
          extra.push_back(js_key);
          extra.push_back(value);
        }
        continue;
      }
      hint = k + 1;
      path_.push_back({key, -1});
      MaybeLocal<Value> value = Decode(j + 1, node.props[k]);
      path_.pop_back();
      if (value.IsEmpty()) {
        slots_.resize(base);
        return MaybeLocal<Value>();
      }
      slots_[base + k] = value;
    }
    for (size_t k = 0; k < count; k++) {
      if (!slots_[base + k].IsEmpty()) continue;
      if (node.default_kinds[k] != kNoDefault) {
        slots_[base + k] = DefaultValue(node, k);
        if (slots_[base + k].IsEmpty()) {
          slots_.resize(base);
          return MaybeLocal<Value>();
        }
      } else if (node.required[k]) {
        slots_.resize(base);
        return Fail("Missing required property \"" + node.keys[k] + "\"");
      }
    }
    Local<Object> object;
    if (node.use_template) {
      object = node.tmpl.Get(isolate_)->NewInstance(
          context_, {slots_.data() + base, count});
    } else {
      object = Object::New(isolate_);
      for (size_t k = 0; k < count; k++) {
        Local<Value> value;
        if (!slots_[base + k].ToLocal(&value)) continue;
        Local<String> key;
        if (!KeyToJS(isolate_, node.keys[k]).ToLocal(&key) ||
            object->CreateDataProperty(context_, key, value).IsNothing()) {
          slots_.resize(base);
          return MaybeLocal<Value>();
        }
      }
    }
    slots_.resize(base);
    for (size_t e = 0; e < extra.size(); e += 2) {
      if (object
              ->CreateDataProperty(
                  context_, extra[e].As<String>(), extra[e + 1])
              .IsNothing()) {
        return MaybeLocal<Value>();
      }
    }
    return object;
  }

  MaybeLocal<Value> DecodeArray(size_t i, const SchemaNode& node) {
    LocalVector<Value> elements(isolate_);
    elements.reserve(ScopeCount(tape_, i));
    const size_t end = AfterScope(tape_, i) - 1;
    int64_t index = 0;
    for (size_t j = i + 1; j < end; j = AfterElement(tape_, j), index++) {
      path_.push_back({std::string_view(), index});
      MaybeLocal<Value> value = Decode(j, node.items);
      path_.pop_back();
      Local<Value> v;
      if (!value.ToLocal(&v)) return MaybeLocal<Value>();
      elements.push_back(v);
    }
    return Array::New(isolate_, elements.data(), elements.size());
  }

  Environment* env_;
  Isolate* isolate_;
  Local<Context> context_;
  const uint64_t* tape_;
  const uint8_t* strings_;
  const SchemaPlan* plan_;
  // Property values of the objects being built, one window per nesting
  // level. Handles stay valid: they live in the caller's HandleScope.
  std::vector<MaybeLocal<Value>> slots_;
  std::vector<PathSegment> path_;
};

// decode(input, plan)
void Decode(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  SchemaPlan* plan;
  ASSIGN_OR_RETURN_UNWRAP(&plan, args[1]);
  simdjson::dom::document* doc;
  if (!ParseInput(env, args[0], &doc)) return;
  Decoder decoder(env, plan, *doc);
  Local<Value> result;
  if (decoder.Decode(1, 0).ToLocal(&result)) {
    args.GetReturnValue().Set(result);
  }
}

Local<FunctionTemplate> MakeViewTemplate(Isolate* isolate,
                                         const char* name,
                                         int field_count) {
  Local<FunctionTemplate> tmpl =
      NewFunctionTemplate(isolate, IllegalConstructor);
  tmpl->SetClassName(OneByteString(isolate, name));
  tmpl->InstanceTemplate()->SetInternalFieldCount(field_count);
  return tmpl;
}

void Initialize(Local<Object> target,
                Local<Value> unused,
                Local<Context> context,
                void* priv) {
  Environment* env = Environment::GetCurrent(context);
  Isolate* isolate = env->isolate();

  if (env->json_document_constructor_template().IsEmpty()) {
    Local<FunctionTemplate> doc_tmpl =
        NewFunctionTemplate(isolate, IllegalConstructor);
    doc_tmpl->InstanceTemplate()->SetInternalFieldCount(
        BaseObject::kInternalFieldCount);
    env->set_json_document_constructor_template(doc_tmpl);

    Local<FunctionTemplate> plan_tmpl =
        NewFunctionTemplate(isolate, IllegalConstructor);
    plan_tmpl->InstanceTemplate()->SetInternalFieldCount(
        BaseObject::kInternalFieldCount);
    env->set_json_schema_plan_constructor_template(plan_tmpl);

    Local<FunctionTemplate> object_tmpl =
        MakeViewTemplate(isolate, "JSONObject", kObjectViewFieldCount);
    SetProtoMethodNoSideEffect(isolate, object_tmpl, "get", ObjectGet);
    SetProtoMethodNoSideEffect(isolate, object_tmpl, "has", ObjectHas);
    SetProtoMethodNoSideEffect(
        isolate, object_tmpl, "keys", ObjectEntries<EntryKind::kKeys>);
    SetProtoMethodNoSideEffect(
        isolate, object_tmpl, "values", ObjectEntries<EntryKind::kValues>);
    SetProtoMethodNoSideEffect(
        isolate, object_tmpl, "entries", ObjectEntries<EntryKind::kEntries>);
    SetProtoMethodNoSideEffect(isolate, object_tmpl, "toJSON", ViewToJS);
    SetProtoMethodNoSideEffect(
        isolate, object_tmpl, "atPointer", ViewAtPointer);
    SetProtoMethodNoSideEffect(isolate, object_tmpl, "getSize", ViewCount);
    env->set_json_object_view_constructor_template(object_tmpl);

    Local<FunctionTemplate> array_tmpl =
        MakeViewTemplate(isolate, "JSONArray", kArrayViewFieldCount);
    SetProtoMethodNoSideEffect(isolate, array_tmpl, "at", ArrayAt);
    SetProtoMethodNoSideEffect(isolate, array_tmpl, "values", ArrayValues);
    SetProtoMethodNoSideEffect(isolate, array_tmpl, "toJSON", ViewToJS);
    SetProtoMethodNoSideEffect(isolate, array_tmpl, "atPointer", ViewAtPointer);
    SetProtoMethodNoSideEffect(isolate, array_tmpl, "getLength", ViewCount);
    env->set_json_array_view_constructor_template(array_tmpl);
  }

  SetConstructorFunction(context,
                         target,
                         "JSONObject",
                         env->json_object_view_constructor_template());
  SetConstructorFunction(context,
                         target,
                         "JSONArray",
                         env->json_array_view_constructor_template());
  SetMethod(context, target, "parse", Parse);
  SetMethod(context, target, "decode", Decode);
  SetMethod(context, target, "compileSchema", CompileSchema);
}

void RegisterExternalReferences(ExternalReferenceRegistry* registry) {
  registry->Register(IllegalConstructor);
  registry->Register(Parse);
  registry->Register(Decode);
  registry->Register(CompileSchema);
  registry->Register(ObjectGet);
  registry->Register(ObjectHas);
  registry->Register(ObjectEntries<EntryKind::kKeys>);
  registry->Register(ObjectEntries<EntryKind::kValues>);
  registry->Register(ObjectEntries<EntryKind::kEntries>);
  registry->Register(ViewToJS);
  registry->Register(ViewAtPointer);
  registry->Register(ViewCount);
  registry->Register(ArrayAt);
  registry->Register(ArrayValues);
}

}  // namespace
}  // namespace json
}  // namespace node

NODE_BINDING_CONTEXT_AWARE_INTERNAL(json, node::json::Initialize)
NODE_BINDING_EXTERNAL_REFERENCE(json, node::json::RegisterExternalReferences)
