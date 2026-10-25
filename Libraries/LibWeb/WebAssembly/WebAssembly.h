/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <LibGC/ConservativeVector.h>
#include <LibGC/Root.h>
#include <LibGC/Weak.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/HostFunction.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Value.h>
#include <LibURL/URL.h>
#include <LibWasm/Runtime.h>
#include <LibWeb/Bindings/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/Buffers.h>

namespace Web::WebAssembly {

class Exception;
class Tag;

WEB_API void initialize(JS::Object&, JS::Realm&);

WEB_API bool validate(JS::Realm&, WebIDL::BufferSource bytes, Bindings::WebAssemblyCompileOptions const&);
WEB_API WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> compile(JS::Realm&, WebIDL::BufferSource bytes, Bindings::WebAssemblyCompileOptions const&);
WEB_API WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> compile_streaming(JS::Realm&, GC::Ref<WebIDL::Promise> source, Bindings::WebAssemblyCompileOptions const&);

WEB_API WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> instantiate(JS::Realm&, WebIDL::BufferSource bytes, GC::Ptr<JS::Object> import_object, Bindings::WebAssemblyCompileOptions const&);
WEB_API WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> instantiate(JS::Realm&, GC::Ref<Module> module_object, GC::Ptr<JS::Object> import_object, Optional<JS::Value> const& ignored);
WEB_API WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> instantiate_streaming(JS::Realm&, GC::Ref<WebIDL::Promise> source, GC::Ptr<JS::Object> import_object, Bindings::WebAssemblyCompileOptions const&);

namespace Detail {

class ExternValueBox;

struct CompiledWebAssemblyModule : public RefCounted<CompiledWebAssemblyModule> {
    explicit CompiledWebAssemblyModule(NonnullRefPtr<Wasm::Runtime::Module> module)
        : module(move(module))
    {
    }

    NonnullRefPtr<Wasm::Runtime::Module> module;
};

// The surrounding agent's associated store, and the agent's Exported Function, Global, Memory, Table, Tag, Exception, and
// Exported GC Object caches.
// The store holds no wasm entity: each lives while the objects that stand for it, or other entities, refer to it. The
// caches are the runtime's link from a cell to the object that stands for it (Store::set_host_object()), which keeps
// the object alive in turn, so that the object stays the same one for as long as both live, and both go away together.
class WebAssemblyCache {
    AK_MAKE_NONCOPYABLE(WebAssemblyCache);
    AK_MAKE_NONMOVABLE(WebAssemblyCache);
    AK_ALLOC_WITH_KMALLOC;

public:
    using Entity = Wasm::Runtime::Entity;

    // The cache of the current thread's agent.
    static WebAssemblyCache& the();

    Wasm::Runtime::Store& store() { return *m_store; }

    // The tag of the exceptions that carry JavaScript values through WebAssembly code, as their only (externref)
    // value.
    Entity js_tag() const { return m_js_tag_root.ptr(); }

    GC::Ptr<JS::NativeFunction> function_object(Entity function) const;
    void set_function_object(Entity function, JS::NativeFunction&);
    GC::Ptr<WebAssembly::Global> global_object(Entity global) const;
    void set_global_object(Entity global, WebAssembly::Global&);
    GC::Ptr<WebAssembly::Memory> memory_object(Entity memory) const;
    void set_memory_object(Entity memory, WebAssembly::Memory&);
    GC::Ptr<WebAssembly::Table> table_object(Entity table) const;
    void set_table_object(Entity table, WebAssembly::Table&);
    GC::Ptr<WebAssembly::Tag> tag_object(Entity tag) const;
    void set_tag_object(Entity tag, WebAssembly::Tag&);
    // Exceptions and GC objects are references, which are the addresses of their cells.
    GC::Ptr<WebAssembly::Exception> exception_object(u64 exception) const;
    void set_exception_object(u64 exception, WebAssembly::Exception&);
    GC::Ptr<JS::Object> gc_object(u64 object) const;
    void set_gc_object(u64 object, JS::Object&);
    void for_each_memory_object(Function<void(WebAssembly::Memory&)> const&) const;
    // The host value cache, for JavaScript values that are not cells: the box that stands for a value in WebAssembly,
    // which it converts to for as long as the box lives.
    GC::Ptr<ExternValueBox> extern_value_box(JS::Value) const;
    void set_extern_value_box(JS::Value, ExternValueBox&);

private:
    explicit WebAssemblyCache(GC::Heap&);
    ~WebAssemblyCache();

    NonnullOwnPtr<Wasm::Runtime::Store> m_store;
    GC::Root<GC::ForeignCell> m_js_tag_root;
    // NB: An entry whose object is gone is stale. Stale entries go once there are as many of them as live ones.
    Vector<GC::Weak<WebAssembly::Memory>> m_memory_objects;
    size_t m_live_memory_objects_after_pruning { 0 };
    HashMap<JS::Value, GC::Weak<ExternValueBox>> m_extern_value_boxes;
    size_t m_live_extern_value_boxes_after_pruning { 0 };
};

// https://webassembly.github.io/spec/js-api/#exported-function
GC::Ref<JS::HostFunction> create_exported_function(JS::Realm&, Utf16FlyString name, size_t length, ESCAPING Function<JS::ThrowCompletionOr<JS::Value>(JS::VM&)> behavior, Wasm::Runtime::Entity function);

// The value of the function's [[FunctionAddress]] internal slot, if it has one and is therefore an Exported Function.
Optional<Wasm::Runtime::Entity> exported_function_address(JS::FunctionObject const&);

// The value of the object's [[ObjectAddress]] internal slot, if it has one and is therefore an Exported GC Object.
Optional<u64> exported_gc_object_address(JS::Object const&);

// The function type at `type_index` of a module, in the store's terms (see Store::canonical_value_type()).
Wasm::Runtime::FunctionType function_type_in_store(Wasm::Runtime::Module const&, u32 type_index);
JS::ThrowCompletionOr<GC::ConservativeVector<Wasm::Runtime::Extern>> read_the_imports(JS::VM&, Wasm::Runtime::Module const&, GC::Ptr<JS::Object> import_object, ReadonlySpan<Utf16String> builtin_set_names, Optional<Utf16String> const& imported_string_module);
JS::ThrowCompletionOr<Wasm::Runtime::Entity> instantiate_module(JS::VM&, Wasm::Runtime::Module const&, GC::Ptr<JS::Object> import_object, ReadonlySpan<Utf16String> builtin_set_names, Optional<Utf16String> const& imported_string_module);
JS::ThrowCompletionOr<Wasm::Runtime::Entity> instantiate_core_module(JS::VM&, Wasm::Runtime::Module const&, ReadonlySpan<Wasm::Runtime::Extern> imports);
Wasm::Runtime::Entity create_host_function(JS::VM&, JS::FunctionObject& function, Wasm::Runtime::FunctionType type, size_t function_index);
JS::ThrowCompletionOr<NonnullRefPtr<CompiledWebAssemblyModule>> compile_a_webassembly_module(JS::Realm&, ReadonlyBytes);
Utf16FlyString name_of_webassembly_function(Wasm::Runtime::Entity function);
GC::Ref<JS::NativeFunction> create_native_function(JS::Realm&, Wasm::Runtime::Entity function);
JS::ThrowCompletionOr<Wasm::Runtime::Value> to_webassembly_value(JS::VM&, JS::Value value, Wasm::Runtime::ValueType type);
Wasm::Runtime::Value default_webassembly_value(JS::VM&, Wasm::Runtime::ValueType type);
JS::ThrowCompletionOr<JS::Value> to_js_value(JS::VM&, Wasm::Runtime::Value wasm_value, Wasm::Runtime::ValueType type);
JS::Completion completion_of_failure(JS::VM&, Wasm::Runtime::Failure const&);
JS::ThrowCompletionOr<void> host_ensure_can_compile_wasm_bytes(JS::Realm&);
JS::ThrowCompletionOr<JS::HandledByHost> host_resize_array_buffer(JS::VM&, JS::ArrayBuffer&, size_t);
JS::ThrowCompletionOr<JS::HandledByHost> host_grow_shared_array_buffer(JS::VM&, JS::ArrayBuffer&, size_t);

inline constexpr size_t page_size = 65536;

}

#define WASM_ENUMERATE_NATIVE_ERRORS                           \
    __WASM_ENUMERATE(CompileError, "WebAssembly.CompileError") \
    __WASM_ENUMERATE(LinkError, "WebAssembly.LinkError")       \
    __WASM_ENUMERATE(RuntimeError, "WebAssembly.RuntimeError")

// https://webassembly.github.io/spec/js-api/#error-objects
// Errors of these types are Error objects whose prototype is the error type's prototype object in their realm. The
// types only create such errors, for VM::throw_completion<CompileError>(...) and the like.
#define __WASM_ENUMERATE(ClassName, FullClassName)                         \
    struct WEB_API ClassName {                                             \
        static GC::Ref<JS::Error> create(JS::Realm&);                      \
        static GC::Ref<JS::Error> create(JS::Realm&, Utf16String message); \
        static GC::Ref<JS::Error> create(JS::Realm&, StringView message);  \
    };
WASM_ENUMERATE_NATIVE_ERRORS
#undef __WASM_ENUMERATE

}
