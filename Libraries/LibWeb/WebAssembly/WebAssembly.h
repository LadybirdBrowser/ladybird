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
#include <LibGC/Root.h>
#include <LibGC/Weak.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/HostFunction.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Value.h>
#include <LibURL/URL.h>
#include <LibWasm/Runtime/Runtime.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/Buffers.h>

namespace Web::WebAssembly {

WEB_API void initialize(JS::Object&, JS::Realm&);

WEB_API bool validate(JS::Realm&, WebIDL::BufferSource bytes);
WEB_API WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> compile(JS::Realm&, WebIDL::BufferSource bytes);
WEB_API WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> compile_streaming(JS::Realm&, GC::Ref<WebIDL::Promise> source);

WEB_API WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> instantiate(JS::Realm&, WebIDL::BufferSource bytes, GC::Ptr<JS::Object> import_object);
WEB_API WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> instantiate(JS::Realm&, GC::Ref<Module> module_object, GC::Ptr<JS::Object> import_object);
WEB_API WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> instantiate_streaming(JS::Realm&, GC::Ref<WebIDL::Promise> source, GC::Ptr<JS::Object> import_object);

namespace Detail {

struct CompiledWebAssemblyModule : public RefCounted<CompiledWebAssemblyModule> {
    explicit CompiledWebAssemblyModule(NonnullRefPtr<Wasm::Runtime::Module> module)
        : module(move(module))
    {
    }

    NonnullRefPtr<Wasm::Runtime::Module> module;
};

// The surrounding agent's associated store, and the agent's Exported Function, Global, Memory, and Table object caches.
// The store holds no wasm entity: each lives while the objects that stand for it, or other entities, refer to it. The
// caches only map entities to their objects for as long as both live.
class WebAssemblyCache {
    AK_MAKE_NONCOPYABLE(WebAssemblyCache);
    AK_MAKE_NONMOVABLE(WebAssemblyCache);

public:
    using Entity = Wasm::Runtime::Entity;

    // The cache of the current thread's agent.
    static WebAssemblyCache& the();

    Wasm::Runtime::Store& store() { return *m_store; }

    // The tag of the exceptions that carry JavaScript values through WebAssembly code, as their only (externref)
    // value.
    Entity js_tag() const { return m_js_tag; }

    GC::Ptr<JS::NativeFunction> function_object(Entity function) const;
    void set_function_object(Entity function, JS::NativeFunction&);
    GC::Ptr<WebAssembly::Global> global_object(Entity global) const;
    void set_global_object(Entity global, WebAssembly::Global&);
    GC::Ptr<WebAssembly::Memory> memory_object(Entity memory) const;
    void set_memory_object(Entity memory, WebAssembly::Memory&);
    GC::Ptr<WebAssembly::Table> table_object(Entity table) const;
    void set_table_object(Entity table, WebAssembly::Table&);
    void for_each_memory_object(Function<void(WebAssembly::Memory&)> const&) const;

    // The index of a host function: how many functions were imported before it.
    void set_host_function_index(Entity function, size_t index) { m_host_function_indices.set(function, index); }
    Optional<size_t> host_function_index(Entity function) const { return m_host_function_indices.get(function); }

private:
    explicit WebAssemblyCache(GC::Heap&);
    ~WebAssemblyCache();

    // Objects by entity. An entry whose object is gone is stale: its entity may be gone too, and its address reused.
    template<typename T>
    class ObjectCache {
    public:
        GC::Ptr<T> get(Entity) const;
        void set(Entity, T&);
        void for_each(Function<void(T&)> const&) const;

    private:
        HashMap<Entity, GC::Weak<T>> m_objects;
        size_t m_live_after_pruning { 0 };
    };

    NonnullOwnPtr<Wasm::Runtime::Store> m_store;
    Entity m_js_tag { nullptr };
    GC::Root<GC::ForeignCell> m_js_tag_root;
    ObjectCache<JS::NativeFunction> m_function_objects;
    ObjectCache<WebAssembly::Global> m_global_objects;
    ObjectCache<WebAssembly::Memory> m_memory_objects;
    ObjectCache<WebAssembly::Table> m_table_objects;
    // NB: An entry outlives its function, but every host function sets its own, so a live one's entry is right.
    HashMap<Entity, size_t> m_host_function_indices;
};

// https://webassembly.github.io/spec/js-api/#exported-function
GC::Ref<JS::HostFunction> create_exported_function(JS::Realm&, Utf16FlyString name, size_t length, ESCAPING Function<JS::ThrowCompletionOr<JS::Value>(JS::VM&)> behavior, Wasm::Runtime::Entity function);

// The value of the function's [[FunctionAddress]] internal slot, if it has one and is therefore an Exported Function.
Optional<Wasm::Runtime::Entity> exported_function_address(JS::FunctionObject const&);

JS::ThrowCompletionOr<Wasm::Runtime::Entity> instantiate_module(JS::VM&, Wasm::Runtime::Module const&, GC::Ptr<JS::Object> import_object);
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
