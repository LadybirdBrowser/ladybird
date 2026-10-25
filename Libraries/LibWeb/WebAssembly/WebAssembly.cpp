/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/MemoryStream.h>
#include <AK/NeverDestroyed.h>
#include <AK/ScopeGuard.h>
#include <AK/StringBuilder.h>
#include <AK/Utf16String.h>
#include <LibCrypto/BigInt/SignedBigInteger.h>
#include <LibGC/ConservativeVector.h>
#include <LibGC/DeferGC.h>
#include <LibGC/Heap.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Agent.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/BigInt.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/ErrorConstructor.h>
#include <LibJS/Runtime/Intrinsics.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <LibWeb/Bindings/HostDefined.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/ContentSecurityPolicy/BlockingAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/MIME.h>
#include <LibWeb/Fetch/Response.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/WebAssembly/BindingsGlue.h>
#include <LibWeb/WebAssembly/Exception.h>
#include <LibWeb/WebAssembly/Global.h>
#include <LibWeb/WebAssembly/Instance.h>
#include <LibWeb/WebAssembly/Memory.h>
#include <LibWeb/WebAssembly/Module.h>
#include <LibWeb/WebAssembly/Table.h>
#include <LibWeb/WebAssembly/Tag.h>
#include <LibWeb/WebAssembly/WebAssembly.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/ExceptionOrUtils.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::WebAssembly {

static GC::Ref<WebIDL::Promise> asynchronously_compile_webassembly_module(JS::Realm&, ByteBuffer, HTML::Task::Source = HTML::Task::Source::Unspecified);
static GC::Ref<WebIDL::Promise> instantiate_promise_of_module(JS::Realm&, GC::Ref<WebIDL::Promise>, GC::Ptr<JS::Object> import_object);
static GC::Ref<WebIDL::Promise> asynchronously_instantiate_webassembly_module(JS::Realm&, GC::Ref<Module>, GC::Ptr<JS::Object> import_object);
static GC::Ref<WebIDL::Promise> compile_potential_webassembly_response(JS::Realm&, GC::Ref<WebIDL::Promise>);

namespace Detail {

using WasmValue = Wasm::Runtime::Value;
using WasmValueType = Wasm::Runtime::ValueType;
using Wasm::Runtime::HeapKind;
using Wasm::Runtime::ValueKind;

// Exceptions and GC objects are the cells their references are the addresses of.
static GC::ForeignCell* cell_of_reference(u64 reference)
{
    return reinterpret_cast<GC::ForeignCell*>(static_cast<FlatPtr>(reference));
}

// NB: The objects of the caches are the ones the runtime keeps alive with their cells (Store::set_host_object()). The
//     platform objects among them are C++ cells, and the others foreign cells.
template<typename T>
static GC::Ptr<T> cell_object(void* object)
{
    return static_cast<T*>(static_cast<GC::Cell*>(object));
}

template<typename T>
static GC::Ptr<T> foreign_cell_object(void* object)
{
    return static_cast<T*>(static_cast<GC::ForeignCell*>(object));
}

GC::Ptr<JS::NativeFunction> WebAssemblyCache::function_object(Entity function) const { return foreign_cell_object<JS::NativeFunction>(m_store->host_object(function)); }
void WebAssemblyCache::set_function_object(Entity function, JS::NativeFunction& object) { m_store->set_host_object(function, object); }
GC::Ptr<Global> WebAssemblyCache::global_object(Entity global) const { return cell_object<Global>(m_store->host_object(global)); }
void WebAssemblyCache::set_global_object(Entity global, Global& object) { m_store->set_host_object(global, object); }
GC::Ptr<Memory> WebAssemblyCache::memory_object(Entity memory) const { return cell_object<Memory>(m_store->host_object(memory)); }
void WebAssemblyCache::set_memory_object(Entity memory, Memory& object)
{
    m_store->set_host_object(memory, object);
    if (m_memory_objects.size() >= 2 * m_live_memory_objects_after_pruning + 16) {
        m_memory_objects.remove_all_matching([](auto& weak) { return !weak; });
        m_live_memory_objects_after_pruning = m_memory_objects.size();
    }
    m_memory_objects.append(object);
}
GC::Ptr<Table> WebAssemblyCache::table_object(Entity table) const { return cell_object<Table>(m_store->host_object(table)); }
void WebAssemblyCache::set_table_object(Entity table, Table& object) { m_store->set_host_object(table, object); }
void WebAssemblyCache::for_each_memory_object(Function<void(Memory&)> const& callback) const
{
    for (auto& memory : m_memory_objects) {
        if (memory)
            callback(*memory);
    }
}
GC::Ptr<Tag> WebAssemblyCache::tag_object(Entity tag) const { return cell_object<Tag>(m_store->host_object(tag)); }
void WebAssemblyCache::set_tag_object(Entity tag, Tag& object) { m_store->set_host_object(tag, object); }
GC::Ptr<Exception> WebAssemblyCache::exception_object(u64 exception) const { return cell_object<Exception>(m_store->host_object(cell_of_reference(exception))); }
void WebAssemblyCache::set_exception_object(u64 exception, Exception& object) { m_store->set_host_object(cell_of_reference(exception), object); }
GC::Ptr<JS::Object> WebAssemblyCache::gc_object(u64 object) const { return foreign_cell_object<JS::Object>(m_store->host_object(cell_of_reference(object))); }
void WebAssemblyCache::set_gc_object(u64 object, JS::Object& gc_object) { m_store->set_host_object(cell_of_reference(object), gc_object); }

WebAssemblyCache::~WebAssemblyCache() = default;

WebAssemblyCache& WebAssemblyCache::the()
{
    // NB: Each thread is one agent with one heap. The cache lives as long as the thread, so it is never destroyed after
    //     the heap.
    static thread_local WebAssemblyCache* cache = nullptr;
    if (!cache)
        cache = new WebAssemblyCache(GC::Heap::the());
    return *cache;
}

WebAssemblyCache::WebAssemblyCache(GC::Heap& heap)
    : m_store(make<Wasm::Runtime::Store>(heap))
{
    // NB: The store is the agent's, whose [[CanBlock]] decides whether memory.atomic.wait may block.
    m_store->set_can_block(JS::agent_can_suspend(JS::VM::the()));

    Array<WasmValueType, 1> js_tag_parameters { WasmValueType::reference(HeapKind::Extern) };
    m_js_tag_root = GC::Root<GC::ForeignCell> { m_store->allocate_tag(js_tag_parameters) };

    // https://webassembly.github.io/spec/js-api/#memory-objects
    // Whenever memory.grow is executed, refresh the memory buffer of the memory's Memory object, if it has one.
    m_store->set_memory_grow_callback([this](Entity memory) {
        if (auto object = memory_object(memory))
            refresh_the_memory_buffer(*object);
    });
}

}

// https://webassembly.github.io/spec/js-api/#error-objects
void initialize(JS::Object& self, JS::Realm& realm)
{
    // 1. Let namespaceObject be the namespace object.
    auto& namespace_object = self;

    // 2. For each error of « "CompileError", "LinkError", "RuntimeError" »,
    // 2.1. Let constructor be a new object, implementing the NativeError Object Structure, with NativeError set to error.
    // 2.2. ! DefineMethodProperty(namespaceObject, error, constructor, false).
    u8 attr = JS::Attribute::Writable | JS::Attribute::Configurable;

#define __WASM_ENUMERATE(ClassName, FullClassName)                                                                 \
    namespace_object.define_intrinsic_accessor(#ClassName##_utf16_fly_string, attr, [](auto& realm) -> JS::Value { \
        return &Bindings::ensure_web_constructor<ClassName>(realm, FullClassName##_utf16_fly_string);              \
    });
    WASM_ENUMERATE_NATIVE_ERRORS
#undef __WASM_ENUMERATE

    // https://webassembly.github.io/spec/js-api/#dom-webassembly-jstag
    // NB: A regular attribute of a namespace is an accessor property of the namespace object.
    Function<JS::ThrowCompletionOr<JS::Value>(JS::VM&)> js_tag_getter = [](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
        // 1. Let JSTagAddr be the result of getting the JavaScript exception tag.
        // 2. Let JSTagObject be the result of creating a Tag object from JSTagAddr.
        // 3. Return JSTagObject.
        auto& current_realm = *vm.current_realm();
        auto tag = Tag::from_address(current_realm, Detail::WebAssemblyCache::the().js_tag());
        return Bindings::wrap(Bindings::host_defined_wrapper_world(current_realm), current_realm, tag);
    };
    namespace_object.define_native_accessor(realm, "JSTag"_utf16_fly_string, move(js_tag_getter), Function<JS::ThrowCompletionOr<JS::Value>(JS::VM&)> {}, JS::Attribute::Enumerable | JS::Attribute::Configurable);
}

// https://webassembly.github.io/spec/js-api/#dom-webassembly-validate
bool validate(JS::Realm& realm, WebIDL::BufferSource bytes)
{
    // 1. Let stableBytes be a copy of the bytes held by the buffer bytes.
    auto stable_bytes = WebIDL::get_buffer_source_copy(bytes);
    if (stable_bytes.is_error()) {
        VERIFY(stable_bytes.error().code() == ENOMEM);
        return false;
    }

    // 2. Compile stableBytes as a WebAssembly module and store the results as module.
    (void)realm;
    auto module = Wasm::Runtime::Module::compile(stable_bytes.value());

    // 3. If module is error, return false.
    if (module.is_error())
        return false;

    // 4. Return true.
    return true;
}

// https://webassembly.github.io/spec/js-api/#dom-webassembly-compile
WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> compile(JS::Realm& realm, WebIDL::BufferSource bytes)
{
    auto& vm = realm.vm();

    // 1. Let stableBytes be a copy of the bytes held by the buffer bytes.
    auto stable_bytes = WebIDL::get_buffer_source_copy(bytes);
    if (stable_bytes.is_error()) {
        VERIFY(stable_bytes.error().code() == ENOMEM);
        return WebIDL::create_rejected_promise_from_exception(realm, vm.throw_completion<JS::InternalError>(vm.error_message(JS::VM::ErrorMessage::OutOfMemory)));
    }

    // 2. Asynchronously compile a WebAssembly module from stableBytes and return the result.
    return asynchronously_compile_webassembly_module(realm, stable_bytes.release_value());
}

// https://webassembly.github.io/spec/web-api/index.html#dom-webassembly-compilestreaming
WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> compile_streaming(JS::Realm& realm, GC::Ref<WebIDL::Promise> source)
{
    //  The compileStreaming(source) method, when invoked, returns the result of compiling a potential WebAssembly response with source.
    return compile_potential_webassembly_response(realm, source);
}

// https://webassembly.github.io/spec/js-api/#dom-webassembly-instantiate
// https://webassembly.github.io/content-security-policy/js-api/#dom-webassembly-instantiate
WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> instantiate(JS::Realm& realm, WebIDL::BufferSource bytes, GC::Ptr<JS::Object> import_object)
{
    auto& vm = realm.vm();

    // 1. Let stableBytes be a copy of the bytes held by the buffer bytes.
    auto stable_bytes = WebIDL::get_buffer_source_copy(bytes);
    if (stable_bytes.is_error()) {
        VERIFY(stable_bytes.error().code() == ENOMEM);
        return WebIDL::create_rejected_promise_from_exception(realm, vm.throw_completion<JS::InternalError>(vm.error_message(JS::VM::ErrorMessage::OutOfMemory)));
    }

    // 2. Perform HostEnsureCanCompileWasmBytes()
    TRY(Detail::host_ensure_can_compile_wasm_bytes(realm));

    // 3. Asynchronously compile a WebAssembly module from stableBytes and let promiseOfModule be the result.
    auto promise_of_module = asynchronously_compile_webassembly_module(realm, stable_bytes.release_value());

    // 4. Instantiate promiseOfModule with imports importObject and return the result.
    return instantiate_promise_of_module(realm, promise_of_module, import_object);
}

// https://webassembly.github.io/spec/js-api/#dom-webassembly-instantiate-moduleobject-importobject
WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> instantiate(JS::Realm& realm, GC::Ref<Module> module_object, GC::Ptr<JS::Object> import_object)
{
    // 1. Asynchronously instantiate the WebAssembly module moduleObject importing importObject, and return the result.
    return asynchronously_instantiate_webassembly_module(realm, module_object, import_object);
}

// https://webassembly.github.io/spec/web-api/index.html#dom-webassembly-instantiatestreaming
WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> instantiate_streaming(JS::Realm& realm, GC::Ref<WebIDL::Promise> source, GC::Ptr<JS::Object> import_object)
{
    // The instantiateStreaming(source, importObject) method, when invoked, performs the following steps:

    // 1. Let promiseOfModule be the result of compiling a potential WebAssembly response with source.
    auto promise_of_module = compile_potential_webassembly_response(realm, source);

    // 2. Return the result of instantiating the promise of a module promiseOfModule with imports importObject.
    return instantiate_promise_of_module(realm, promise_of_module, import_object);
}

namespace Detail {

// References are encoded so that their kind shows in their low bits: an i31 has bit 0 set, and a host value has bits
// 1:0 equal to 0b10. A host value is the address of a cell with bit 1 set, which keeps the cell alive wherever
// WebAssembly keeps the reference.
static constexpr u64 I31_REFERENCE_BIT = 0b01;
static constexpr u64 HOST_REFERENCE_TAG = 0b10;
static constexpr u64 REFERENCE_TAG_MASK = 0b11;

// A JavaScript value that is not a cell, boxed so that WebAssembly can refer to it as a host value.
class ExternValueBox final : public GC::Cell {
    GC_CELL(ExternValueBox, GC::Cell);
    GC_DECLARE_ALLOCATOR(ExternValueBox);

public:
    using JSValueConversionIsForbidden = void;

    JS::Value value() const { return m_value; }

private:
    explicit ExternValueBox(JS::Value value)
        : m_value(value)
    {
    }

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_value);
    }

    JS::Value m_value;
};

GC_DEFINE_ALLOCATOR(ExternValueBox);

GC::Ptr<ExternValueBox> WebAssemblyCache::extern_value_box(JS::Value value) const
{
    auto box = m_extern_value_boxes.get(value);
    return box.has_value() ? box->ptr() : nullptr;
}

void WebAssemblyCache::set_extern_value_box(JS::Value value, ExternValueBox& box)
{
    if (m_extern_value_boxes.size() >= 2 * m_live_extern_value_boxes_after_pruning + 16) {
        m_extern_value_boxes.remove_all_matching([](auto&, auto& weak) { return !weak; });
        m_live_extern_value_boxes_after_pruning = m_extern_value_boxes.size();
    }
    m_extern_value_boxes.set(value, box);
}

static u64 host_reference_of(JS::VM& vm, JS::Value value)
{
    GC::Cell const* cell = nullptr;
    if (value.is_cell()) {
        cell = &value.as_cell();
    } else {
        // NB: A value gets the same box while references to the box keep it alive, so that it converts to the same
        //     reference.
        auto& cache = WebAssemblyCache::the();
        auto box = cache.extern_value_box(value);
        if (!box) {
            box = vm.heap().allocate<ExternValueBox>(value);
            cache.set_extern_value_box(value, *box);
        }
        cell = box.ptr();
    }
    return static_cast<u64>(reinterpret_cast<FlatPtr>(cell)) | HOST_REFERENCE_TAG;
}

static JS::Value value_of_host_reference(u64 reference)
{
    auto address = static_cast<FlatPtr>(reference & ~REFERENCE_TAG_MASK);
    switch (reinterpret_cast<GC::ForeignCell const*>(address)->cell_kind()) {
    case GC::CellKind::Object:
        return JS::Value(reinterpret_cast<JS::Object const*>(address));
    case GC::CellKind::PrimitiveString:
        return JS::Value(reinterpret_cast<JS::PrimitiveString const*>(address));
    case GC::CellKind::Symbol:
        return JS::Value(reinterpret_cast<JS::Symbol const*>(address));
    case GC::CellKind::BigInt:
        return JS::Value(reinterpret_cast<JS::BigInt const*>(address));
    case GC::CellKind::Other:
        return reinterpret_cast<ExternValueBox const*>(address)->value();
    case GC::CellKind::Accessor:
        break;
    }
    VERIFY_NOT_REACHED();
}

static Optional<u64> i31_reference_of(JS::Value value)
{
    static constexpr double I31_MINIMUM = -(1 << 30);
    static constexpr double I31_MAXIMUM = (1 << 30) - 1;
    if (!value.is_number())
        return {};
    auto number = value.as_double();
    if (number < I31_MINIMUM || number > I31_MAXIMUM || number != trunc(number) || (number == 0 && signbit(number)))
        return {};
    return (static_cast<u64>(static_cast<u32>(static_cast<i32>(number)) & 0x7fff'ffff) << 1) | I31_REFERENCE_BIT;
}

static i32 value_of_i31_reference(u64 reference)
{
    // The value is in bits 31:1, so an arithmetic shift of the low 32 bits sign-extends it.
    return static_cast<i32>(static_cast<u32>(reference)) >> 1;
}

// https://webassembly.github.io/spec/js-api/#exported-gc-object
// An Exported GC Object stands for a struct or an array, whose reference is the address of its cell. The object keeps
// that cell as its host data, and has no properties, no prototype, and no way to get either.
struct ExportedGCObjectTraits {
    static JS::ThrowCompletionOr<JS::Object*> get_prototype_of(JS::HostObject const&)
    {
        // 1. Return null.
        return nullptr;
    }

    static JS::ThrowCompletionOr<bool> set_prototype_of(JS::HostObject const& object, JS::Object*)
    {
        // 1. Throw a TypeError.
        return object.vm().throw_completion<JS::TypeError>("Cannot set the prototype of a WebAssembly GC object"_utf16);
    }

    static JS::ThrowCompletionOr<bool> is_extensible(JS::HostObject const&)
    {
        // 1. Return false.
        return false;
    }

    static JS::ThrowCompletionOr<bool> prevent_extensions(JS::HostObject const& object)
    {
        // 1. Throw a TypeError.
        return object.vm().throw_completion<JS::TypeError>("Cannot prevent extensions of a WebAssembly GC object"_utf16);
    }

    static JS::ThrowCompletionOr<Optional<JS::PropertyDescriptor>> get_own_property(JS::HostObject const&, JS::PropertyKey const&)
    {
        // 1. Return undefined.
        return Optional<JS::PropertyDescriptor> {};
    }

    static JS::ThrowCompletionOr<bool> define_own_property(JS::HostObject const& object, JS::PropertyKey const&, JS::PropertyDescriptor&, Optional<JS::PropertyDescriptor>*)
    {
        // 1. Throw a TypeError.
        return object.vm().throw_completion<JS::TypeError>("Cannot define properties of a WebAssembly GC object"_utf16);
    }

    static JS::ThrowCompletionOr<bool> has_property(JS::HostObject const&, JS::PropertyKey const&)
    {
        // 1. Return false.
        return false;
    }

    static JS::ThrowCompletionOr<JS::Value> get(JS::HostObject const&, JS::PropertyKey const&, JS::Value, JS::CacheableGetPropertyMetadata*, JS::Object::PropertyLookupPhase)
    {
        // 1. Return undefined.
        return JS::js_undefined();
    }

    static JS::ThrowCompletionOr<bool> set(JS::HostObject const& object, JS::PropertyKey const&, JS::Value, JS::Value, JS::CacheableSetPropertyMetadata*, JS::Object::PropertyLookupPhase)
    {
        // 1. Throw a TypeError.
        return object.vm().throw_completion<JS::TypeError>("Cannot set properties of a WebAssembly GC object"_utf16);
    }

    static JS::ThrowCompletionOr<bool> delete_property(JS::HostObject const& object, JS::PropertyKey const&)
    {
        // 1. Throw a TypeError.
        return object.vm().throw_completion<JS::TypeError>("Cannot delete properties of a WebAssembly GC object"_utf16);
    }

    static JS::ThrowCompletionOr<GC::RootVector<JS::Value>> own_property_keys(JS::HostObject const&)
    {
        // 1. Let keys be a new empty list.
        // 2. Return keys.
        return GC::RootVector<JS::Value> {};
    }
};

static constexpr JSHostObjectHooks exported_gc_object_hooks = JS::make_host_object_hooks<ExportedGCObjectTraits>();
static constexpr JSHostClass exported_gc_object_host_class = JS::make_host_class(JS_HOST_CLASS_OBJECT, "WebAssemblyGCObject"sv, nullptr, &exported_gc_object_hooks, nullptr, JS_HOST_CLASS_NOT_CACHEABLE_FOR_PROPERTY_ABSENCE);

// https://webassembly.github.io/spec/js-api/#create-a-new-exported-gc-object
static GC::Ref<JS::Object> create_exported_gc_object(JS::VM& vm, u64 reference)
{
    // 1. Assert: objectaddr is a struct or array address.
    // 2. Let map be the current agent's associated exported GC object cache.
    // 3. If map[objectaddr] exists, return map[objectaddr].
    auto& cache = WebAssemblyCache::the();
    if (auto object = cache.gc_object(reference))
        return *object;

    // 4. Let object be a new Exported GC Object, with [[ObjectAddress]] set to objectaddr and [[ObjectKind]] set to
    //    objectKind.
    auto* cell = reinterpret_cast<GC::ForeignCell*>(static_cast<FlatPtr>(reference));
    auto object = JS::HostObject::create(*vm.current_realm(), exported_gc_object_host_class, nullptr, {}, cell);

    // 5. Set map[objectaddr] to object.
    cache.set_gc_object(reference, object);

    // 6. Return object.
    return object;
}

Optional<u64> exported_gc_object_address(JS::Object const& object)
{
    if (!JS::is_host_instance_of(object, exported_gc_object_host_class))
        return {};
    return static_cast<u64>(reinterpret_cast<FlatPtr>(JS::host_data_of(object).ptr()));
}

// https://webassembly.github.io/spec/js-api/#run-a-host-function
static JS::ThrowCompletionOr<void> run_a_host_function(JS::VM& vm, JS::FunctionObject& function, Wasm::Runtime::FunctionType const& type, ReadonlySpan<WasmValue> arguments, Span<WasmValue> results)
{

    // 1. Let functype be the type of the host function.
    // 2. Let [parameters] → [results] be functype.
    // 3. If parameters or results contain v128 or exnref, throw a TypeError.
    // NB: The conversions below throw that TypeError.

    // 4. Let jsArguments be « ».
    GC::RootVector<JS::Value> js_arguments;

    // 5. For each arg of arguments, append ! ToJSValue(arg) to jsArguments.
    for (size_t index = 0; index < arguments.size(); ++index)
        js_arguments.append(TRY(to_js_value(vm, arguments[index], type.parameters[index])));

    // 6. Let ret be ? Call(func, undefined, jsArguments).
    auto ret = TRY(JS::call(vm, function, JS::js_undefined(), js_arguments.span()));

    // 7. Let resultsSize be results's size.
    // 8. If resultsSize is 0, return « ».
    if (type.results.is_empty())
        return {};

    // NB: The converted values may refer to new cells, which this vector keeps alive until they are in the results.
    Wasm::Runtime::Values converted;

    // 9. Otherwise, if resultsSize is 1, return « ? ToWebAssemblyValue(ret, results[0]) ».
    if (type.results.size() == 1) {
        converted.append(TRY(to_webassembly_value(vm, ret, type.results.first())));
    }
    // 10. Otherwise,
    else {
        // 1. Let method be ? GetMethod(ret, %Symbol.iterator%).
        auto method = TRY(ret.get_method(vm, vm.names.iterator));

        // 2. If method is undefined, throw a TypeError.
        if (!method)
            return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotIterable, ret.to_utf16_string_without_side_effects());

        // 3. Let values be ? IteratorToList(? GetIteratorFromMethod(ret, method)).
        auto values = TRY(JS::iterator_to_list(vm, TRY(JS::get_iterator_from_method(vm, ret, *method))));

        // 4. Let wasmValues be a new, empty list.
        // 5. If values's size is not resultsSize, throw a TypeError exception.
        if (values.size() != type.results.size())
            return vm.throw_completion<JS::TypeError>(Utf16String::formatted("Invalid number of return values for multi-value wasm return of {} objects", type.results.size()));

        // 6. For each value and resultType in values and results, paired linearly, append ToWebAssemblyValue(value, resultType) to wasmValues.
        for (size_t index = 0; index < values.size(); ++index)
            converted.append(TRY(to_webassembly_value(vm, values[index], type.results[index])));
    }

    // 7. Return wasmValues.
    for (size_t index = 0; index < converted.size(); ++index)
        results[index] = converted[index];
    return {};
}

// https://webassembly.github.io/spec/js-api/#create-a-host-function
Wasm::Runtime::Entity create_host_function(JS::VM& vm, JS::FunctionObject& function, Wasm::Runtime::FunctionType type, size_t function_index)
{
    auto& cache = WebAssemblyCache::the();

    // 1. Assert: IsCallable(func).
    // 2. Let stored settings be the incumbent settings object.
    // 3. Let hostfunc be a host function which performs the following steps when called with arguments arguments:
    // NB: The function's entity visits what the steps capture, which keeps the JavaScript function alive with it.
    auto host_function = [&vm, function = GC::Ref { function }, type](Wasm::Runtime::Store& store, ReadonlySpan<WasmValue> arguments, Span<WasmValue> results) -> ErrorOr<void, Wasm::Runtime::HostError> {
        auto result = run_a_host_function(vm, function, type, arguments, results);
        if (!result.is_error())
            return {};

        // https://webassembly.github.io/spec/js-api/#create-a-host-function
        // If the steps threw an exception v,
        auto value = result.error_value();

        // 1. If v implements Exception, let address be v.[[Address]].
        if (auto* exception = Bindings::exception_from_value(value))
            return Wasm::Runtime::HostError::exception(exception->address());

        // 2. Otherwise,
        //     1. Let type be the JavaScript exception tag.
        //     2. Let payload be ! ToWebAssemblyValue(v, externref).
        //     3. Let (store, address) be exn_alloc(store, type, « payload »).
        // 3. Throw a WebAssembly exception with address, which WebAssembly code can catch, and which is v again once it
        //    leaves WebAssembly.
        Array<WasmValue, 1> payload { MUST(to_webassembly_value(vm, value, WasmValueType::reference(HeapKind::Extern))) };
        return Wasm::Runtime::HostError::exception(store.allocate_exception(WebAssemblyCache::the().js_tag(), payload));
    };
    return cache.store().allocate_host_function(type, move(host_function), static_cast<u32>(function_index));
}

Wasm::Runtime::FunctionType function_type_in_store(Wasm::Runtime::Module const& module, u32 type_index)
{
    auto& store = WebAssemblyCache::the().store();
    auto type = module.function_type(type_index);
    VERIFY(type.has_value());
    for (auto& parameter : type->parameters)
        parameter = store.canonical_value_type(module, parameter);
    for (auto& result : type->results)
        result = store.canonical_value_type(module, result);
    return type.release_value();
}

JS::ThrowCompletionOr<Wasm::Runtime::Entity> instantiate_module(JS::VM& vm, Wasm::Runtime::Module const& module, GC::Ptr<JS::Object> import_object)
{
    auto& store = WebAssemblyCache::the().store();

    // https://webassembly.github.io/spec/js-api/index.html#read-the-imports
    // 1. If module.imports is not empty, and importObject is undefined, throw a TypeError exception.
    auto import_count = module.import_count();
    if (import_count != 0 && !import_object)
        return vm.throw_completion<JS::TypeError>("ImportObject must be provided when module has imports"_utf16);

    // 2. Let imports be « ».
    // NB: The host functions and globals made for the imports are referenced from here only until the instance
    //     holds them, and the steps below run user code (getters, valueOf) that may collect garbage.
    GC::ConservativeVector<Wasm::Runtime::Extern> imports;
    imports.ensure_capacity(import_count);
    size_t imported_function_count = 0;

    // 3. For each (moduleName, componentName, externtype) of module_imports(module),
    for (size_t import_index = 0; import_index < import_count; ++import_index) {
        auto import = module.import(import_index);
        dbgln_if(LIBWEB_WASM_DEBUG, "Trying to resolve {}::{}", import.module, import.name);

        // 3.1. Let o be ? Get(importObject, moduleName).
        auto value = TRY(import_object->get(Utf16FlyString::from_utf8(import.module)));

        // 3.2. If o is not an Object, throw a TypeError exception.
        if (!value.is_object())
            return vm.throw_completion<JS::TypeError>(JS::ErrorType::IsNotAEvaluatedFrom, value.to_utf16_string_without_side_effects(), "Object"_string, MUST(String::formatted("[wasm import object][\"{}\"]", import.module)));
        auto const& object = value.as_object();

        // 3.3. Let v be ? Get(o, componentName).
        auto import_ = TRY(object.get(Utf16FlyString::from_utf8(import.name)));

        switch (import.type.kind) {
        // 3.4. If externtype is of the form func functype,
        case Wasm::Runtime::ExternKind::Function: {
            // 3.4.1. If IsCallable(v) is false, throw a LinkError exception.
            if (!import_.is_function())
                return vm.throw_completion<LinkError>(JS::ErrorType::IsNotAEvaluatedFrom, import_, "function"_string, MUST(String::formatted("[wasm import object][\"{}\"]", import.name)));
            auto& function = import_.as_function();

            Wasm::Runtime::Entity address = nullptr;
            // 3.4.2. If v has a [[FunctionAddress]] internal slot, and therefore is an Exported Function,
            if (auto exported_address = exported_function_address(function); exported_address.has_value()) {
                // 3.4.2.1. Let funcaddr be the value of v's [[FunctionAddress]] internal slot.
                address = *exported_address;
            }
            // 3.4.3. Otherwise,
            else {
                // 3.4.3.1. Create a host function from v and functype, and let funcaddr be the result.
                // 3.4.3.2. Let index be the number of external functions in imports. This value index is known as the index of the host function funcaddr.
                address = create_host_function(vm, function, function_type_in_store(module, import.type.type_index), imported_function_count);
            }

            // 3.4.4. Let externfunc be the external value func funcaddr.
            // 3.4.5. Append externfunc to imports.
            imports.unchecked_append({ Wasm::Runtime::ExternKind::Function, address });
            ++imported_function_count;
            break;
        }
        // 3.5. If externtype is of the form global mut valtype,
        case Wasm::Runtime::ExternKind::Global: {
            auto value_type = store.canonical_value_type(module, import.type.value_type);
            Wasm::Runtime::Entity address = nullptr;
            // 3.5.1. If v implements Global,
            if (auto* global = Bindings::global_from_value(import_)) {
                // 3.5.1.1. Let globaladdr be v.[[Global]].
                address = global->address();
            }
            // 3.5.2. Otherwise,
            else {
                // 3.5.2.1. If valtype is i64 and v is not a BigInt, throw a LinkError exception.
                if (value_type.kind == ValueKind::I64 && !import_.is_bigint())
                    return vm.throw_completion<LinkError>("Import resolution attempted to use a non-BigInt for an i64 global"sv);
                // 3.5.2.2. If valtype is one of i32, f32 or f64 and v is not a Number, throw a LinkError exception.
                if ((value_type.kind == ValueKind::I32 || value_type.kind == ValueKind::F32 || value_type.kind == ValueKind::F64) && !import_.is_number())
                    return vm.throw_completion<LinkError>("Import resolution attempted to use a non-Number for a numeric global"sv);
                // 3.5.2.3. If valtype is v128, throw a LinkError exception.
                if (value_type.kind == ValueKind::V128)
                    return vm.throw_completion<LinkError>("Import resolution attempted to use a value for a V128 global"sv);
                // 3.5.2.4. If mut is var, throw a LinkError exception.
                if (import.type.is_mutable)
                    return vm.throw_completion<LinkError>("Import resolution attempted to use a value for a mutable global"sv);
                // 3.5.2.5. Let value be ToWebAssemblyValue(v, valtype). If this throws a TypeError, catch it, and throw
                //          a LinkError exception.
                auto value = to_webassembly_value(vm, import_, value_type);
                if (value.is_error())
                    return vm.throw_completion<LinkError>("Import resolution attempted to use a value of the wrong type for a global"sv);
                // 3.5.2.6. Let store be the surrounding agent's associated store.
                // 3.5.2.7. Let (store, globaladdr) be global_alloc(store, const valtype, value).
                // 3.5.2.8. Set the surrounding agent's associated store to store.
                address = store.allocate_global(value_type, false, value.release_value());
            }

            // 3.5.3. Let externglobal be global globaladdr.
            // 3.5.4. Append externglobal to imports.
            imports.unchecked_append({ Wasm::Runtime::ExternKind::Global, address });
            break;
        }
        // 3.6. If externtype is of the form mem memtype,
        case Wasm::Runtime::ExternKind::Memory: {
            // 3.6.1. If v does not implement Memory, throw a LinkError exception.
            auto* memory = Bindings::memory_from_value(import_);
            if (!memory)
                return vm.throw_completion<LinkError>("Expected an instance of WebAssembly.Memory for a memory import"sv);

            // 3.6.2. Let externmem be the external value mem v.[[Memory]].
            // 3.6.3. Append externmem to imports.
            imports.unchecked_append({ Wasm::Runtime::ExternKind::Memory, memory->address() });
            break;
        }
        // 3.7. If externtype is of the form table tabletype,
        case Wasm::Runtime::ExternKind::Table: {
            // 3.7.1. If v does not implement Table, throw a LinkError exception.
            auto* table = Bindings::table_from_value(import_);
            if (!table)
                return vm.throw_completion<LinkError>("Expected an instance of WebAssembly.Table for a table import"sv);

            // 3.7.2. Let tableaddr be v.[[Table]].
            // 3.7.3. Let externtable be the external value table tableaddr.
            // 3.7.4. Append externtable to imports.
            imports.unchecked_append({ Wasm::Runtime::ExternKind::Table, table->address() });
            break;
        }
        // 3.8. If externtype is of the form tag attribute functype,
        case Wasm::Runtime::ExternKind::Tag: {
            // 3.8.1. Assert: attribute is exception.
            // 3.8.2. If v does not implement Tag, throw a LinkError exception.
            auto* tag = Bindings::tag_from_value(import_);
            if (!tag)
                return vm.throw_completion<LinkError>("Expected an instance of WebAssembly.Tag for a tag import"sv);

            // 3.8.3. Let tagaddr be v.[[Address]].
            // 3.8.4. Let externtag be the external value tag tagaddr.
            // 3.8.5. Append externtag to imports.
            imports.unchecked_append({ Wasm::Runtime::ExternKind::Tag, tag->address() });
            break;
        }
        }
    }

    // 4. Return imports.
    return instantiate_core_module(vm, module, imports);
}

// https://webassembly.github.io/spec/js-api/index.html#instantiate-the-core-of-a-webassembly-module
JS::ThrowCompletionOr<Wasm::Runtime::Entity> instantiate_core_module(JS::VM& vm, Wasm::Runtime::Module const& module, ReadonlySpan<Wasm::Runtime::Extern> imports)
{
    // 1. Let store be the surrounding agent's associated store.
    // 2. Let result be module_instantiate(store, module, imports).
    // 3. If result is error, throw an appropriate exception type:
    //     * A LinkError exception for most cases which occur during linking.
    //     * If the error came when running the start function, throw a RuntimeError for most errors which occur from
    //       WebAssembly, or the error object propagated from inner ECMAScript code.
    //     * Another error type if appropriate, for example an out-of-memory exception, as documented in the
    //       WebAssembly error mapping.
    // 4. Let (store, instance) be result.
    // 5. Set the surrounding agent's associated store to store.
    // 6. Return instance.
    auto result = WebAssemblyCache::the().store().instantiate(module, imports);
    if (result.is_error())
        return completion_of_failure(vm, result.error());
    return result.value();
}

// https://webassembly.github.io/spec/js-api/#compile-a-webassembly-module
// https://webassembly.github.io/content-security-policy/js-api/#compile-a-webassembly-module
JS::ThrowCompletionOr<NonnullRefPtr<CompiledWebAssemblyModule>> compile_a_webassembly_module(JS::Realm& realm, ReadonlyBytes data)
{
    auto& vm = realm.vm();
    TRY(host_ensure_can_compile_wasm_bytes(realm));

    auto module = Wasm::Runtime::Module::compile(data);
    if (module.is_error())
        return vm.throw_completion<CompileError>(module.error());
    return make_ref_counted<CompiledWebAssemblyModule>(module.release_value());
}

// https://webassembly.github.io/spec/js-api/#HostResizeArrayBuffer
JS::ThrowCompletionOr<JS::HandledByHost> host_resize_array_buffer(JS::VM& vm, JS::ArrayBuffer& buffer, size_t new_length)
{
    // 1. If buffer.[[ArrayBufferDetachKey]] is "WebAssembly.Memory",
    auto detach_key = buffer.detach_key();
    if (detach_key.is_string() && detach_key.as_string() == JS::PrimitiveString::create(vm, "WebAssembly.Memory"_utf16_fly_string)) {
        // 3. For each memaddr → mem in all Memory object caches,
        GC::Ptr<Memory> memory;
        Detail::WebAssemblyCache::the().for_each_memory_object([&](Memory& candidate) {
            // 1. If SameValue(mem.[[BufferObject]], buffer) is true,
            if (memory_has_buffer_object(candidate, buffer)) {
                // 2. Assert: buffer is the [[BufferObject]] of exactly one value in map.
                VERIFY(!memory);
                memory = candidate;
            }
        });

        // 2. Assert: buffer is the [[BufferObject]] of exactly one value in map.
        VERIFY(memory);

        // NB: We perform the following steps for mem after the loop above, because growing mem can collect garbage, and
        //     that removes finalized Memory objects from the caches we iterate.

        // 1. Assert: buffer.[[ArrayBufferByteLength]] modulo 65536 is 0.
        VERIFY(buffer.byte_length() % page_size == 0);

        // 2. Let lengthDelta be newLength - buffer.[[ArrayBufferByteLength]].
        auto length_delta = new_length - buffer.byte_length();

        // 3. If lengthDelta < 0 or lengthDelta modulo 65536 is not 0,
        if (new_length < buffer.byte_length() || length_delta % page_size != 0) {
            // 1. Throw a RangeError exception.
            return vm.throw_completion<JS::RangeError>("WebAssembly.Memory buffers must be resized by a multiple of the page size"sv);
        }

        // 4. Let delta be lengthDelta ÷ 65536.
        auto delta = length_delta / page_size;

        // 5. Grow the memory buffer associated with memaddr by delta.
        // FIXME: "Grow the memory buffer" is a separate algorithm from the Memory#grow() method.
        TRY(WebIDL::throw_dom_exception_if_needed(vm, buffer.shape().realm(), [&] {
            return memory->grow(delta);
        }));

        // 4. Return handled.
        return JS::HandledByHost::Handled;
    }

    // 2. Otherwise, return unhandled.
    return JS::HandledByHost::Unhandled;
}

// https://webassembly.github.io/threads/js-api/#abstract-operation-hostgrowsharedarraybuffer
JS::ThrowCompletionOr<JS::HandledByHost> host_grow_shared_array_buffer(JS::VM& vm, JS::ArrayBuffer& buffer, size_t new_length)
{
    // AD-HOC: If buffer.[[ArrayBufferDetachKey]] is "WebAssembly.Memory",
    auto detach_key = buffer.detach_key();
    if (detach_key.is_string() && detach_key.as_string() == JS::PrimitiveString::create(vm, "WebAssembly.Memory"_utf16_fly_string)) {
        auto current_byte_length = buffer.byte_length();

        // 3. For each memaddr → mem in all Memory object caches,
        GC::Ptr<Memory> memory;
        Detail::WebAssemblyCache::the().for_each_memory_object([&](Memory& candidate) {
            // 1. If SameValue(mem.[[BufferObject]], buffer) is true,
            if (memory_has_buffer_object(candidate, buffer)) {
                // 2. Assert: buffer is the [[BufferObject]] of exactly one value in map.
                VERIFY(!memory);
                memory = candidate;
            }
        });

        // 2. Assert: buffer is the [[BufferObject]] of exactly one value in map.
        VERIFY(memory);

        // NB: We perform the following steps for mem after the loop above, because growing mem can collect garbage, and
        //     that removes finalized Memory objects from the caches we iterate.

        // 1. Assert: buffer.[[ArrayBufferByteLength]] modulo 65536 is 0.
        VERIFY(buffer.byte_length() % page_size == 0);

        // 2. Let lengthDelta be newLength - buffer.[[ArrayBufferByteLength]].
        auto length_delta = new_length - buffer.byte_length();

        // 3. If lengthDelta < 0 or lengthDelta modulo 65536 is not 0,
        if (new_length < buffer.byte_length() || length_delta % page_size != 0) {
            // 1. Throw a RangeError exception.
            return vm.throw_completion<JS::RangeError>("WebAssembly.Memory buffers must be resized by a multiple of the page size"sv);
        }

        // 4. Let delta be lengthDelta ÷ 65536.
        auto delta = length_delta / page_size;

        // 5. Grow the memory buffer associated with memaddr by delta.
        // FIXME: "Grow the memory buffer" is a separate algorithm from the Memory#grow() method.
        TRY(WebIDL::throw_dom_exception_if_needed(vm, buffer.shape().realm(), [&] {
            return memory->grow(delta);
        }));

        // 4. If newLength < buffer.[[ArrayBufferByteLength]] or newLength > buffer.[[ArrayBufferMaxByteLength]], throw a RangeError exception.
        if (new_length < current_byte_length)
            return vm.throw_completion<JS::RangeError>(JS::ErrorType::ByteLengthLessThanPreviousByteLength, new_length, current_byte_length);
        if (new_length > buffer.max_byte_length())
            return vm.throw_completion<JS::RangeError>(JS::ErrorType::ByteLengthExceedsMaxByteLength, new_length, buffer.max_byte_length());

        // FIXME: 5. Let agentRecord be the surrounding agent’s Agent Record.
        // FIXME: 6. Let isLittleEndian be agentRecord.[[LittleEndian]] .
        // FIXME: 7. Let event be an implementation defined ReadModifyWriteSharedMemory event, whose [[Order]] is "SeqCst", [[Payload]] is NumericToRawBytes(Bigint, newLength, isLittleEndian), [[Block]] is buffer.[[ArrayBufferByteLengthData]], [[ByteIndex]] is 0, and [[ElementSize]] is 8.
        // FIXME: 8. Let eventsRecord be the EventsRecord in agentRecord.[[CandidateExecution]].[[EventsRecords]] that corresponds to the surrounding agent’s signifier agentRecord.[[Signifier]].
        // FIXME: 9. Append event to eventsRecord.[[EventsList]].
        VERIFY(buffer.byte_length() == new_length);

        // 10. Return handled.
        return JS::HandledByHost::Handled;
    }

    // AD-HOC: Otherwise, return unhandled.
    return JS::HandledByHost::Unhandled;
}

namespace {

// The internal slots of an Exported Function: its [[FunctionAddress]], and the steps that call into the store.
class ExportedFunctionHostData final : public GC::Cell {
    GC_CELL(ExportedFunctionHostData, GC::Cell);
    GC_DECLARE_ALLOCATOR(ExportedFunctionHostData);

public:
    using JSValueConversionIsForbidden = void;

    using Behavior = AK::Function<JS::ThrowCompletionOr<JS::Value>(JS::VM&)>;

    Wasm::Runtime::Entity function_address() const { return m_function.ptr(); }

    JS::ThrowCompletionOr<JS::Value> call(JS::VM& vm) const
    {
        VERIFY(m_behavior);
        return m_behavior(vm);
    }

private:
    ExportedFunctionHostData(Behavior behavior, Wasm::Runtime::Entity function)
        : m_behavior(move(behavior))
        , m_function(function)
    {
    }

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit_possible_values(m_behavior.raw_capture_range());
        visitor.visit(m_function);
    }

    Behavior m_behavior;
    // The function, which an Exported Function keeps alive.
    GC::Ptr<GC::ForeignCell> m_function;
};

GC_DEFINE_ALLOCATOR(ExportedFunctionHostData);

struct ExportedFunctionTraits {
    static JS::ThrowCompletionOr<JS::Value> call(JS::HostFunction& function, JS::VM& vm)
    {
        auto* host_data = JS::host_data_if<ExportedFunctionHostData>(function);
        VERIFY(host_data);
        return host_data->call(vm);
    }
};

constexpr JSHostFunctionHooks exported_function_hooks = JS::make_host_function_hooks<ExportedFunctionTraits>();
constexpr JSHostClass exported_function_host_class = JS::make_host_class(JS_HOST_CLASS_FUNCTION, "ExportedWasmFunction"sv, nullptr, &exported_function_hooks, nullptr, 0);

}

GC::Ref<JS::HostFunction> create_exported_function(JS::Realm& realm, Utf16FlyString name, size_t length, Function<JS::ThrowCompletionOr<JS::Value>(JS::VM&)> behavior, Wasm::Runtime::Entity function)
{
    auto host_data = realm.heap().allocate<ExportedFunctionHostData>(move(behavior), function);
    return JS::HostFunction::create(realm, exported_function_host_class, move(name), static_cast<i32>(length), {}, host_data);
}

Optional<Wasm::Runtime::Entity> exported_function_address(JS::FunctionObject const& function)
{
    if (auto const* host_data = JS::host_data_if<ExportedFunctionHostData>(function))
        return host_data->function_address();
    return {};
}

// https://www.w3.org/TR/wasm-js-api-2/#name-of-the-webassembly-function
Utf16FlyString name_of_webassembly_function(Wasm::Runtime::Entity function)
{
    // 1. Let store be the surrounding agent's associated store.
    // 2. Let funcinst be store.funcs[funcaddr].
    // 3. If funcinst is of the form {type functype, hostcode hostfunc},
    //     1. Assert: hostfunc is a JavaScript object and IsCallable(hostfunc) is true.
    //     2. Let index be the index of the host function funcaddr.
    // 4. Otherwise,
    //     1. Let moduleinst be funcinst.module.
    //     2. Assert: funcaddr is contained in moduleinst.funcaddrs.
    //     3. Let index be the index of moduleinst.funcaddrs where funcaddr is found.
    auto& cache = WebAssemblyCache::the();
    auto index = cache.store().function_index(function);
    if (!index.has_value())
        index = cache.store().host_function_index(function);
    VERIFY(index.has_value());

    // 5. Return ! ToString(index).
    auto name = Utf16String::number(*index);
    return Utf16FlyString::from_utf16(name.utf16_view());
}

// https://webassembly.github.io/spec/js-api/#a-new-exported-function
GC::Ref<JS::NativeFunction> create_native_function(JS::Realm& realm, Wasm::Runtime::Entity function)
{
    auto& cache = WebAssemblyCache::the();
    if (auto object = cache.function_object(function))
        return *object;

    auto type = cache.store().function_type(function);
    auto length = type.parameters.size();

    auto object = create_exported_function(
        realm,
        name_of_webassembly_function(function),
        length,
        [function, type = move(type)](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            // https://webassembly.github.io/spec/js-api/#call-an-exported-function
            // NB: The converted arguments may refer to new cells, which this vector keeps alive during the call.
            Wasm::Runtime::Values arguments;
            arguments.ensure_capacity(type.parameters.size());
            for (size_t index = 0; index < type.parameters.size(); ++index)
                arguments.unchecked_append(TRY(to_webassembly_value(vm, vm.argument(index), type.parameters[index])));

            auto result = WebAssemblyCache::the().store().invoke(function, arguments);
            if (result.is_error())
                return completion_of_failure(vm, result.error());

            auto& values = result.value();
            if (values.is_empty())
                return JS::js_undefined();

            if (values.size() == 1)
                return to_js_value(vm, values.first(), type.results.first());

            GC::RootVector<JS::Value> js_result_values;
            js_result_values.ensure_capacity(values.size());
            for (size_t index = 0; index < values.size(); ++index)
                js_result_values.unchecked_append(TRY(to_js_value(vm, values[index], type.results[index])));
            return JS::Value(JS::Array::create_from(*vm.current_realm(), js_result_values));
        },
        function);

    cache.set_function_object(function, object);
    return object;
}

// The internal form of a JavaScript value as a reference of the any hierarchy: an i31 reference for a Number in its
// range, the struct or array of an Exported GC Object, and otherwise a host reference.
static u64 internalized_reference_of(JS::VM& vm, JS::Value value)
{
    if (auto reference = i31_reference_of(value); reference.has_value())
        return *reference;
    if (value.is_object()) {
        if (auto address = exported_gc_object_address(value.as_object()); address.has_value())
            return *address;
    }
    return host_reference_of(vm, value);
}

// Whether a reference type is of the func hierarchy, whose references are functions.
static bool is_function_reference_type(WasmValueType type)
{
    if (type.heap == HeapKind::Concrete)
        return WebAssemblyCache::the().store().is_function_type(type.type_index);
    return type.heap == HeapKind::Function || type.heap == HeapKind::NoFunction;
}

// https://webassembly.github.io/spec/js-api/#towebassemblyvalue
static JS::ThrowCompletionOr<u64> to_webassembly_reference(JS::VM& vm, JS::Value value, WasmValueType type)
{
    auto& store = WebAssemblyCache::the().store();

    // If v is null: if the type is nullable, return ref.null of its heap type, and otherwise throw a TypeError.
    if (value.is_null()) {
        if (!type.nullable)
            return vm.throw_completion<JS::TypeError>("Cannot convert null to a non-nullable reference"_utf16);
        return 0;
    }

    switch (type.heap) {
    case HeapKind::Exception:
    case HeapKind::NoException:
        return vm.throw_completion<JS::TypeError>("Cannot convert a JavaScript value to an exception reference"_utf16);
    case HeapKind::NoFunction:
    case HeapKind::NoExtern:
    case HeapKind::None:
        return vm.throw_completion<JS::TypeError>("Only null converts to a bottom reference type"_utf16);
    case HeapKind::Extern:
        // NB: An externref keeps the bits of the anyref it externalizes.
        return internalized_reference_of(vm, value);
    default:
        break;
    }

    // If the type is of the func hierarchy, only an Exported Function of a matching type converts, to its function.
    if (is_function_reference_type(type)) {
        if (value.is_function()) {
            if (auto address = exported_function_address(value.as_function()); address.has_value()) {
                auto reference = store.function_reference(*address);
                if (store.reference_matches(reference, type))
                    return reference;
            }
        }
        return vm.throw_completion<JS::TypeError>("Cannot convert the value to a function reference of this type"_utf16);
    }

    // Otherwise the type is of the any hierarchy: the internal form of v converts if it matches the type.
    auto reference = internalized_reference_of(vm, value);
    if (!store.reference_matches(reference, type))
        return vm.throw_completion<JS::TypeError>("Cannot convert the value to a reference of this type"_utf16);
    return reference;
}

// https://webassembly.github.io/spec/js-api/#towebassemblyvalue
JS::ThrowCompletionOr<WasmValue> to_webassembly_value(JS::VM& vm, JS::Value value, WasmValueType type)
{

    switch (type.kind) {
    case ValueKind::I64: {
        auto bigint = TRY(value.to_bigint(vm));
        return WasmValue::from_i64(bigint->big_integer().to_i64());
    }
    case ValueKind::I32:
        return WasmValue::from_i32(TRY(value.to_i32(vm)));
    case ValueKind::F64:
        return WasmValue::from_f64(TRY(value.to_double(vm)));
    case ValueKind::F32:
        return WasmValue::from_f32(static_cast<float>(TRY(value.to_double(vm))));
    case ValueKind::V128:
        return vm.throw_completion<JS::TypeError>("Cannot convert a javascript value to a vector value"_utf16);
    case ValueKind::Reference:
        return WasmValue::from_reference(TRY(to_webassembly_reference(vm, value, type)));
    }
    VERIFY_NOT_REACHED();
}

// https://webassembly.github.io/spec/js-api/#defaultvalue
WasmValue default_webassembly_value(JS::VM& vm, WasmValueType type)
{
    // 1. If valuetype equals externref, return ToWebAssemblyValue(undefined, valuetype).
    if (type.kind == ValueKind::Reference && type.heap == HeapKind::Extern)
        return MUST(to_webassembly_value(vm, JS::js_undefined(), type));

    // 2. Return DefaultValue(valuetype).
    return WasmValue::default_for(type);
}

// https://webassembly.github.io/spec/js-api/#tojsvalue
JS::ThrowCompletionOr<JS::Value> to_js_value(JS::VM& vm, WasmValue wasm_value, WasmValueType type)
{

    switch (type.kind) {
    case ValueKind::I64:
        return JS::BigInt::create(vm, ::Crypto::SignedBigInteger { wasm_value.to_i64() });
    case ValueKind::I32:
        return JS::Value(wasm_value.to_i32());
    case ValueKind::F64:
        return JS::Value(wasm_value.to_f64());
    case ValueKind::F32:
        return JS::Value(static_cast<double>(wasm_value.to_f32()));
    case ValueKind::V128:
        return vm.throw_completion<JS::TypeError>("Cannot convert a vector value to a javascript value"_utf16);
    case ValueKind::Reference:
        break;
    }

    if (type.heap == HeapKind::Exception || type.heap == HeapKind::NoException)
        return vm.throw_completion<JS::TypeError>("Cannot convert an exception reference to a javascript value"_utf16);

    auto reference = wasm_value.reference_bits();

    // 1. If w is of the form ref.null t, return null.
    if (reference == 0)
        return JS::js_null();

    // 2. If w is of the form ref.func funcaddr, return the result of creating a new Exported Function from funcaddr.
    if (is_function_reference_type(type))
        return create_native_function(*vm.current_realm(), WebAssemblyCache::the().store().function_from_reference(reference));

    // 3. If w is of the form ref.i31 u31, return 𝔽(signed_31(u31)).
    if (reference & I31_REFERENCE_BIT)
        return JS::Value(value_of_i31_reference(reference));

    // 4. If w is of the form ref.host hostaddr, return the result of retrieving a host value from hostaddr.
    // NB: An externref of a host value keeps the bits of the host reference.
    if ((reference & REFERENCE_TAG_MASK) == HOST_REFERENCE_TAG)
        return value_of_host_reference(reference);

    // 5. If w is of the form ref.struct structaddr or ref.array arrayaddr, return the result of creating a new Exported
    //    GC Object from it.
    return create_exported_gc_object(vm, reference);
}

// https://webassembly.github.io/spec/js-api/#error-objects
JS::Completion completion_of_failure(JS::VM& vm, Wasm::Runtime::Failure const& failure)
{
    switch (failure.kind) {
    case Wasm::Runtime::FailureKind::LinkError:
        return vm.throw_completion<LinkError>(failure.message);
    case Wasm::Runtime::FailureKind::StackExhausted:
        // https://webassembly.github.io/spec/js-api/#stack-overflow
        // 6.1. Whenever a stack overflow occurs in WebAssembly code, the same class of exception is thrown as for a stack overflow in JavaScript.
        return vm.throw_completion<JS::InternalError>(JS::ErrorType::CallStackSizeExceeded);
    case Wasm::Runtime::FailureKind::Exception: {
        // An exception with the JavaScript tag carries a JavaScript value that was thrown into WebAssembly code, which
        // continues as itself.
        auto& cache = WebAssemblyCache::the();
        auto& store = cache.store();
        if (store.exception_tag(failure.exception) == cache.js_tag()) {
            auto payload = store.exception_values(failure.exception);
            return JS::throw_completion(MUST(to_js_value(vm, payload.first(), WasmValueType::reference(HeapKind::Extern))));
        }
        // Otherwise, throw the Exception object of the exception.
        auto& realm = *vm.current_realm();
        auto exception = Exception::from_address(failure.exception);
        return JS::throw_completion(Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, exception));
    }
    case Wasm::Runtime::FailureKind::Trap:
        // FIXME: Use the convoluted mapping of errors defined in the spec.
        return vm.throw_completion<RuntimeError>(Utf16String::formatted("Wasm execution trapped (WIP): {}", failure.message));
    }
    VERIFY_NOT_REACHED();
}

// https://webassembly.github.io/content-security-policy/js-api/#abstract-opdef-hostensurecancompilewasmbytes
JS::ThrowCompletionOr<void> host_ensure_can_compile_wasm_bytes(JS::Realm& realm)
{
    // 2. Perform EnsureCSPDoesNotBlockWasmByteCompilation(realm)
    // This algorithm does not return a value, but raises a CompileError exception if the operation cannot complete
    // successfully.
    return ContentSecurityPolicy::ensure_csp_does_not_block_wasm_byte_compilation(realm);
}

}

// https://webassembly.github.io/spec/js-api/#asynchronously-compile-a-webassembly-module
GC::Ref<WebIDL::Promise> asynchronously_compile_webassembly_module(JS::Realm& realm, ByteBuffer bytes, HTML::Task::Source task_source)
{
    // 1. Let promise be a new Promise.
    auto promise = WebIDL::create_promise(realm);

    // 2. Run the following steps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(GC::Heap::the(), [&realm, bytes = move(bytes), promise, task_source]() mutable {
        HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
        // 1. Compile the WebAssembly module bytes and store the result as module.
        auto module_or_error = Detail::compile_a_webassembly_module(realm, bytes);

        // 2. Queue a task to perform the following steps. If taskSource was provided, queue the task on that task source.
        HTML::queue_a_task(task_source, nullptr, nullptr, GC::create_function(GC::Heap::the(), [&realm, bytes = move(bytes), promise, module_or_error = move(module_or_error)]() mutable {
            HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
            auto& realm = HTML::relevant_realm(*promise->promise());

            // 1. If module is error, reject promise with a CompileError exception.
            if (module_or_error.is_error()) {
                WebIDL::reject_promise(promise, module_or_error.error_value());
            }

            // 2. Otherwise,
            else {
                // 1. Construct a WebAssembly module object from module and bytes, and let moduleObject be the result.
                auto module_object = GC::Heap::the().allocate<Module>(module_or_error.release_value(), move(bytes));

                // 2. Resolve promise with moduleObject.
                Bindings::resolve_webassembly_module_promise(realm, promise, module_object);
            }
        }));
    }));

    // 3. Return promise.
    return promise;
}

// https://webassembly.github.io/spec/js-api/#asynchronously-instantiate-a-webassembly-module
GC::Ref<WebIDL::Promise> asynchronously_instantiate_webassembly_module(JS::Realm& realm, GC::Ref<Module> module_object, GC::Ptr<JS::Object> import_object)
{
    // 1. Let promise be a new promise.
    auto promise = WebIDL::create_promise(realm);

    // 2. Let module be moduleObject.[[Module]].
    auto module = module_object->compiled_module();

    // 3. Read the imports of module with imports importObject, and let imports be the result.
    //    If this operation throws an exception, catch it, reject promise with the exception, and return promise.
    // Note: We do this at the same time as instantiation in instantiate_module.

    // 4. Run the following steps in parallel:
    //   1. Queue a task to perform the following steps: Note: Implementation-specific work may be performed here.
    HTML::queue_a_task(HTML::Task::Source::Unspecified, nullptr, nullptr, GC::create_function(GC::Heap::the(), [&realm, promise, module, import_object]() {
        HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
        auto& realm = HTML::relevant_realm(*promise->promise());

        // 1. Instantiate the core of a WebAssembly module module with imports, and let instance be the result.
        //    If this throws an exception, catch it, reject promise with the exception, and terminate these substeps.
        auto result = Detail::instantiate_module(realm.vm(), *module->module, import_object);
        if (result.is_error()) {
            WebIDL::reject_promise(promise, result.error_value());
            return;
        }
        auto instance = result.release_value();

        // 2. Let instanceObject be a new Instance.
        // 3. Initialize instanceObject from module and instance. If this throws an exception, catch it, reject promise with the exception, and terminate these substeps.
        // FIXME: Investigate whether we are doing all the proper steps for "initialize an instance object"
        auto instance_object = Instance::create(realm, instance);

        // 4. Resolve promise with instanceObject.
        Bindings::resolve_webassembly_instance_promise(realm, promise, instance_object);
    }));

    // 5. Return promise.
    return promise;
}

// https://webassembly.github.io/spec/js-api/#instantiate-a-promise-of-a-module
GC::Ref<WebIDL::Promise> instantiate_promise_of_module(JS::Realm& realm, GC::Ref<WebIDL::Promise> promise_of_module, GC::Ptr<JS::Object> import_object)
{
    // 1. Let promise be a new Promise.
    auto promise = WebIDL::create_promise(realm);

    // FIXME: Spec should use react to promise here instead of separate upon fulfillment and upon rejection steps

    // 2. Upon fulfillment of promiseOfModule with value module:
    auto fulfillment_steps = GC::create_function(GC::Heap::the(), [promise, import_object](JS::Value module_value) -> WebIDL::ExceptionOr<JS::Value> {
        auto* module = Bindings::module_from_value(module_value);
        VERIFY(module);
        auto module_object = GC::Ref { *module };

        // 1. Instantiate the WebAssembly module module importing importObject, and let innerPromise be the result.
        auto& realm = HTML::relevant_realm(*promise->promise());
        auto inner_promise = asynchronously_instantiate_webassembly_module(realm, module_object, import_object);

        // 2. Upon fulfillment of innerPromise with value instance.
        auto instantiate_fulfillment_steps = GC::create_function(GC::Heap::the(), [promise, module_object](JS::Value instance_value) -> WebIDL::ExceptionOr<JS::Value> {
            auto& realm = HTML::relevant_realm(*promise->promise());
            auto* instance = Bindings::instance_from_value(instance_value);
            VERIFY(instance);

            // 1. Let result be the WebAssemblyInstantiatedSource value «[ "module" → module, "instance" → instance ]».
            auto result = Bindings::create_webassembly_instantiated_source(realm, module_object, GC::Ref { *instance });

            // 2. Resolve promise with result.
            WebIDL::resolve_promise(promise, result);

            return JS::js_undefined();
        });

        // 3. Upon rejection of innerPromise with reason reason.
        auto instantiate_rejection_steps = GC::create_function(GC::Heap::the(), [promise](JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
            // 1. Reject promise with reason.
            WebIDL::reject_promise(promise, reason);

            return JS::js_undefined();
        });

        WebIDL::react_to_promise(inner_promise, instantiate_fulfillment_steps, instantiate_rejection_steps);

        return JS::js_undefined();
    });

    // 3. Upon rejection of promiseOfModule with reason reason:
    auto rejection_steps = GC::create_function(GC::Heap::the(), [promise](JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
        // 1. Reject promise with reason.
        WebIDL::reject_promise(promise, reason);

        return JS::js_undefined();
    });

    WebIDL::react_to_promise(promise_of_module, fulfillment_steps, rejection_steps);

    // 4. Return promise.
    return promise;
}

// https://webassembly.github.io/spec/web-api/index.html#compile-a-potential-webassembly-response
GC::Ref<WebIDL::Promise> compile_potential_webassembly_response(JS::Realm& realm, GC::Ref<WebIDL::Promise> source)
{
    auto& vm = realm.vm();

    // Note: This algorithm accepts a Response object, or a promise for one, and compiles and instantiates the resulting bytes of the response.
    //       This compilation can be performed in the background and in a streaming manner.
    //       If the Response is not CORS-same-origin, does not represent an ok status, or does not match the `application/wasm` MIME type,
    //       the returned promise will be rejected with a TypeError; if compilation or instantiation fails,
    //       the returned promise will be rejected with a CompileError or other relevant error type, depending on the cause of failure.

    // 1. Let returnValue be a new promise
    auto return_value = WebIDL::create_promise(realm);

    // 2. Upon fulfillment of source with value unwrappedSource:
    auto fulfillment_steps = GC::create_function(GC::Heap::the(), [&vm, return_value](JS::Value unwrapped_source) -> WebIDL::ExceptionOr<JS::Value> {
        auto& realm = HTML::relevant_realm(*return_value->promise());

        // 1. Let response be unwrappedSource’s response.
        auto* response_object = unwrapped_source.is_object()
            ? Bindings::impl_from<Fetch::Response>(&unwrapped_source.as_object())
            : nullptr;
        if (!response_object) {
            WebIDL::reject_promise(return_value, vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "Response").value());
            return JS::js_undefined();
        }
        auto response = response_object->response();

        // 2. Let mimeType be the result of getting `Content-Type` from response’s header list.
        // 3. If mimeType is null, reject returnValue with a TypeError and abort these substeps.
        // 4. Remove all HTTP tab or space byte from the start and end of mimeType.
        // 5. If mimeType is not a byte-case-insensitive match for `application/wasm`, reject returnValue with a TypeError and abort these substeps.
        // Note: extra parameters are not allowed, including the empty `application/wasm;`.
        // FIXME: Validate these extra constraints that are not checked by extract_mime_type()
        if (auto mime = Fetch::Infrastructure::extract_mime_type(response->header_list()); !mime.has_value() || mime.value().essence() != "application/wasm"sv) {
            WebIDL::reject_promise(return_value, vm.throw_completion<JS::TypeError>("Response does not match the application/wasm MIME type"_utf16).value());
            return JS::js_undefined();
        }

        // 6. If response is not CORS-same-origin, reject returnValue with a TypeError and abort these substeps.
        if (!response->is_cors_same_origin()) {
            WebIDL::reject_promise(return_value, vm.throw_completion<JS::TypeError>("Response is not CORS-same-origin"_utf16).value());
            return JS::js_undefined();
        }

        // 7. If response’s status is not an ok status, reject returnValue with a TypeError and abort these substeps.
        if (!response_object->ok()) {
            WebIDL::reject_promise(return_value, vm.throw_completion<JS::TypeError>("Response does not represent an ok status"_utf16).value());
            return JS::js_undefined();
        }

        // 8. Consume response’s body as an ArrayBuffer, and let bodyPromise be the result.
        auto body_promise_or_error = Bindings::array_buffer_for_webassembly_response(realm, *response_object);
        if (body_promise_or_error.is_error()) {
            WebIDL::reject_promise_with_exception(return_value, body_promise_or_error.release_error());
            return JS::js_undefined();
        }
        auto body_promise = body_promise_or_error.release_value();

        // 9. Upon fulfillment of bodyPromise with value bodyArrayBuffer:
        auto body_fulfillment_steps = GC::create_function(GC::Heap::the(), [&vm, return_value](JS::Value body_array_buffer) -> WebIDL::ExceptionOr<JS::Value> {
            // 1. Let stableBytes be a copy of the bytes held by the buffer bodyArrayBuffer.
            VERIFY(body_array_buffer.is_object());
            auto stable_bytes = WebIDL::get_buffer_source_copy(body_array_buffer.as_object());
            if (stable_bytes.is_error()) {
                VERIFY(stable_bytes.error().code() == ENOMEM);
                WebIDL::reject_promise(return_value, vm.throw_completion<JS::InternalError>(vm.error_message(JS::VM::ErrorMessage::OutOfMemory)).value());
                return JS::js_undefined();
            }

            // 2. Asynchronously compile the WebAssembly module stableBytes using the networking task source and resolve returnValue with the result.
            auto result = asynchronously_compile_webassembly_module(HTML::relevant_realm(*return_value->promise()), stable_bytes.release_value(), HTML::Task::Source::Networking);

            // Need to manually convert WebIDL promise to an ECMAScript value here to resolve
            WebIDL::resolve_promise(return_value, result->promise());

            return JS::js_undefined();
        });

        // 10. Upon rejection of bodyPromise with reason reason:
        auto body_rejection_steps = GC::create_function(GC::Heap::the(), [return_value](JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
            // 1. Reject returnValue with reason.
            WebIDL::reject_promise(return_value, reason);
            return JS::js_undefined();
        });

        WebIDL::react_to_promise(body_promise, body_fulfillment_steps, body_rejection_steps);

        return JS::js_undefined();
    });

    // 3. Upon rejection of source with reason reason:
    auto rejection_steps = GC::create_function(GC::Heap::the(), [return_value](JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
        // 1. Reject returnValue with reason.
        WebIDL::reject_promise(return_value, reason);

        return JS::js_undefined();
    });

    WebIDL::react_to_promise(source, fulfillment_steps, rejection_steps);

    // 4. Return returnValue.
    return return_value;
}

namespace {

// The name of each error type, and the name under which Bindings::Intrinsics keeps its prototype and constructor.
template<typename NativeError>
struct NativeErrorNames;

#define __WASM_ENUMERATE(ClassName, FullClassName)                                          \
    template<>                                                                              \
    struct NativeErrorNames<ClassName> {                                                    \
        static constexpr StringView constructor_class_name = #ClassName "Constructor"sv;    \
        static Utf16FlyString name() { return #ClassName##_utf16_fly_string; }              \
        static Utf16FlyString intrinsic_name() { return FullClassName##_utf16_fly_string; } \
    };
WASM_ENUMERATE_NATIVE_ERRORS
#undef __WASM_ENUMERATE

template<typename NativeError>
JS::Object& native_error_prototype(JS::Realm& realm)
{
    return Bindings::ensure_web_prototype<NativeError>(realm, NativeErrorNames<NativeError>::intrinsic_name());
}

// https://tc39.es/ecma262/#sec-ordinarycreatefromconstructor
// The LibJS implementation looks the intrinsic default prototype up in JS::Intrinsics, whereas these error types keep
// theirs in Bindings::Intrinsics.
template<typename NativeError>
JS::ThrowCompletionOr<GC::Ref<JS::Error>> ordinary_create_native_error_from_constructor(JS::VM& vm, JS::FunctionObject const& constructor)
{
    auto& realm = *vm.current_realm();

    // 2. Let proto be ? GetPrototypeFromConstructor(constructor, intrinsicDefaultProto).
    auto prototype = TRY([&]() -> JS::ThrowCompletionOr<GC::Ref<JS::Object>> {
        // https://tc39.es/ecma262/#sec-getprototypefromconstructor

        // 2. Let proto be ? Get(constructor, "prototype").
        auto prototype = TRY(constructor.get(vm.names.prototype));

        // 3. If Type(proto) is not Object, then
        if (!prototype.is_object()) {
            // a. Let realm be ? GetFunctionRealm(constructor).
            auto* realm = TRY(JS::get_function_realm(vm, constructor));

            // b. Set proto to realm's intrinsic object named intrinsicDefaultProto.
            prototype = &native_error_prototype<NativeError>(*realm);
        }

        // 4. Return proto.
        return prototype.as_object();
    }());

    // 3. If internalSlotsList is present, let slotsList be internalSlotsList.
    // 4. Else, let slotsList be a new empty List.
    // 5. Return OrdinaryObjectCreate(proto, slotsList).
    return JS::Error::create(realm, prototype);
}

template<typename NativeError>
struct NativeErrorConstructorTraits {
    // 20.5.6.1.1 NativeError ( message [ , options ] ), https://tc39.es/ecma262/#sec-nativeerror
    static JS::ThrowCompletionOr<JS::Value> call(JS::HostFunction& constructor, JS::VM& vm)
    {
        // 1. If NewTarget is undefined, let newTarget be the active function object; else let newTarget be NewTarget.
        return TRY(construct(constructor, vm, constructor));
    }

    // 20.5.6.1.1 NativeError ( message [ , options ] ), https://tc39.es/ecma262/#sec-nativeerror
    static JS::ThrowCompletionOr<GC::Ref<JS::Object>> construct(JS::HostFunction&, JS::VM& vm, JS::FunctionObject& new_target)
    {
        auto message = vm.argument(0);
        auto options = vm.argument(1);

        // 2. Let O be ? OrdinaryCreateFromConstructor(newTarget, "%NativeError.prototype%", « [[ErrorData]] »).
        auto error = TRY(ordinary_create_native_error_from_constructor<NativeError>(vm, new_target));

        // 3. If message is not undefined, then
        if (!message.is_undefined()) {
            // a. Let msg be ? ToString(message).
            auto msg = TRY(message.to_utf16_string(vm));

            // b. Perform CreateNonEnumerableDataPropertyOrThrow(O, "message", msg).
            error->create_non_enumerable_data_property_or_throw(vm.names.message, JS::PrimitiveString::create(vm, move(msg)));
        }

        // 4. Perform ? InstallErrorCause(O, options).
        TRY(error->install_error_cause(options));

        // 5. Return O.
        return error;
    }
};

template<typename NativeError>
constexpr JSHostFunctionHooks native_error_constructor_hooks = JS::make_host_function_hooks<NativeErrorConstructorTraits<NativeError>>();

template<typename NativeError>
constexpr JSHostClass native_error_constructor_host_class = JS::make_host_class(JS_HOST_CLASS_FUNCTION, NativeErrorNames<NativeError>::constructor_class_name, nullptr, &native_error_constructor_hooks<NativeError>, nullptr, JS_HOST_CLASS_HAS_CONSTRUCTOR);

struct NativeErrorPrototypeAndConstructor {
    GC::Ref<JS::Object> prototype;
    GC::Ref<JS::HostFunction> constructor;
};

template<typename NativeError>
NativeErrorPrototypeAndConstructor create_native_error_prototype_and_constructor(JS::Realm& realm)
{
    auto& vm = realm.vm();
    auto name = NativeErrorNames<NativeError>::name();
    u8 const attr = JS::Attribute::Writable | JS::Attribute::Configurable;

    // 20.5.6.3 Properties of the NativeError Prototype Objects, https://tc39.es/ecma262/#sec-properties-of-the-nativeerror-prototype-objects
    auto prototype = JS::Object::create(realm, realm.intrinsics().error_prototype());
    prototype->define_direct_property(vm.names.name, JS::PrimitiveString::create(vm, name), attr);
    prototype->define_direct_property(vm.names.message, JS::PrimitiveString::create(vm, Utf16String {}), attr);

    // 20.5.6.2 Properties of the NativeError Constructors, https://tc39.es/ecma262/#sec-properties-of-the-nativeerror-constructors
    auto constructor = JS::HostFunction::create_without_own_properties(realm, native_error_constructor_host_class<NativeError>, name, realm.intrinsics().error_constructor());
    constructor->define_direct_property(vm.names.prototype, prototype, 0);
    constructor->define_direct_property(vm.names.name, JS::PrimitiveString::create(vm, name), JS::Attribute::Configurable);
    constructor->define_direct_property(vm.names.length, JS::Value(1), JS::Attribute::Configurable);

    prototype->define_direct_property(vm.names.constructor, constructor, attr);
    return { prototype, constructor };
}

}

#define __WASM_ENUMERATE(ClassName, FullClassName)                                                \
    GC::Ref<JS::Error> ClassName::create(JS::Realm& realm)                                        \
    {                                                                                             \
        return JS::Error::create(realm, native_error_prototype<ClassName>(realm));                \
    }                                                                                             \
                                                                                                  \
    GC::Ref<JS::Error> ClassName::create(JS::Realm& realm, Utf16String message)                   \
    {                                                                                             \
        return JS::Error::create(realm, native_error_prototype<ClassName>(realm), move(message)); \
    }                                                                                             \
                                                                                                  \
    GC::Ref<JS::Error> ClassName::create(JS::Realm& realm, StringView message)                    \
    {                                                                                             \
        return create(realm, Utf16String::from_utf8(message));                                    \
    }
WASM_ENUMERATE_NATIVE_ERRORS
#undef __WASM_ENUMERATE

}

namespace Web::Bindings {

WebAssembly::Global* global_from_value(JS::Value value)
{
    return value.is_object() ? Bindings::impl_from<WebAssembly::Global>(&value.as_object()) : nullptr;
}

WebAssembly::Instance* instance_from_value(JS::Value value)
{
    return value.is_object() ? Bindings::impl_from<WebAssembly::Instance>(&value.as_object()) : nullptr;
}

WebAssembly::Memory* memory_from_value(JS::Value value)
{
    return value.is_object() ? Bindings::impl_from<WebAssembly::Memory>(&value.as_object()) : nullptr;
}

WebAssembly::Module* module_from_value(JS::Value value)
{
    return value.is_object() ? Bindings::impl_from<WebAssembly::Module>(&value.as_object()) : nullptr;
}

WebAssembly::Table* table_from_value(JS::Value value)
{
    return value.is_object() ? Bindings::impl_from<WebAssembly::Table>(&value.as_object()) : nullptr;
}

void resolve_webassembly_module_promise(JS::Realm& realm, WebIDL::Promise& promise, GC::Ref<WebAssembly::Module> module)
{
    WebIDL::resolve_promise(promise, Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, module));
}

void resolve_webassembly_instance_promise(JS::Realm& realm, WebIDL::Promise& promise, GC::Ref<WebAssembly::Instance> instance)
{
    WebIDL::resolve_promise(promise, Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, instance));
}

GC::Ref<JS::Object> create_webassembly_instantiated_source(JS::Realm& realm, GC::Ref<WebAssembly::Module> module, GC::Ref<WebAssembly::Instance> instance)
{
    // NB: The WebAssemblyInstantiatedSource dictionary converts to an ordinary object, whose prototype is
    //     %Object.prototype%.
    auto result = JS::Object::create(realm, realm.intrinsics().object_prototype());
    result->define_direct_property("module"_utf16_fly_string, Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, module), JS::default_attributes);
    result->define_direct_property("instance"_utf16_fly_string, Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, instance), JS::default_attributes);
    return result;
}

WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> array_buffer_for_webassembly_response(JS::Realm& realm, Fetch::Response const& response)
{
    auto promise = WebIDL::create_promise(realm);
    response.array_buffer(promise);
    return promise;
}

#define __WASM_ENUMERATE(ClassName, FullClassName)                                                                                 \
    template<>                                                                                                                     \
    void Intrinsics::create_web_prototype_and_constructor<WebAssembly::ClassName>(JS::Realm & realm)                               \
    {                                                                                                                              \
        auto [prototype, constructor] = WebAssembly::create_native_error_prototype_and_constructor<WebAssembly::ClassName>(realm); \
        m_prototypes.set(FullClassName##_utf16_fly_string, prototype);                                                             \
        m_constructors.set(FullClassName##_utf16_fly_string, constructor);                                                         \
    }
WASM_ENUMERATE_NATIVE_ERRORS
#undef __WASM_ENUMERATE

}
