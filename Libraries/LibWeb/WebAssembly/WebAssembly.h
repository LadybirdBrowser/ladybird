/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibGC/Root.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/PrototypeObject.h>
#include <LibJS/Runtime/Value.h>
#include <LibWasm/AbstractMachine/AbstractMachine.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::WebAssembly {

WEB_API void visit_edges(JS::Object&, JS::Cell::Visitor&);
WEB_API void finalize(JS::Object&);
WEB_API void initialize(JS::Object&, JS::Realm&);

WEB_API bool validate(JS::VM&, GC::Root<WebIDL::BufferSource>& bytes);
WEB_API WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> compile(JS::VM&, GC::Root<WebIDL::BufferSource>& bytes);
WEB_API WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> compile_streaming(JS::VM&, GC::Root<WebIDL::Promise> const& source);

WEB_API WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> instantiate(JS::VM&, GC::Root<WebIDL::BufferSource>& bytes, Optional<GC::Root<JS::Object>>& import_object);
WEB_API WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> instantiate(JS::VM&, Module const& module_object, Optional<GC::Root<JS::Object>>& import_object);
WEB_API WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> instantiate_streaming(JS::VM&, GC::Root<WebIDL::Promise> const& source, Optional<GC::Root<JS::Object>>& import_object);

namespace Detail {

struct CompiledWebAssemblyModule : public RefCounted<CompiledWebAssemblyModule> {
    explicit CompiledWebAssemblyModule(NonnullRefPtr<Wasm::Module> module)
        : module(move(module))
    {
    }

    NonnullRefPtr<Wasm::Module> module;
};

class WebAssemblyCache {
public:
    void add_compiled_module(NonnullRefPtr<CompiledWebAssemblyModule> const& module) { m_compiled_modules.append(module); }
    void add_function_instance(Wasm::FunctionAddress address, GC::Ptr<JS::NativeFunction> function) { m_function_instances.set(address, function); }
    void add_imported_object(GC::Ptr<JS::Object> object) { m_imported_objects.set(object); }
    void add_extern_value(Wasm::ExternAddress address, JS::Value value)
    {
        if (auto entry = m_extern_values.get(address); entry.has_value())
            m_inverse_extern_values.remove(entry.value());
        m_extern_values.set(address, value);
        m_inverse_extern_values.set(value, address);
    }
    void add_global_instance(Wasm::GlobalAddress address, GC::Ptr<WebAssembly::Global> global) { m_global_instances.set(address, global); }
    void add_memory_instance(Wasm::MemoryAddress address, GC::Ptr<WebAssembly::Memory> memory) { m_memory_instances.set(address, memory); }

    Optional<GC::Ptr<JS::NativeFunction>> get_function_instance(Wasm::FunctionAddress address) { return m_function_instances.get(address); }
    Optional<JS::Value> get_extern_value(Wasm::ExternAddress address) { return m_extern_values.get(address); }
    Optional<GC::Ptr<WebAssembly::Global>> get_global_instance(Wasm::GlobalAddress address) { return m_global_instances.get(address); }
    Optional<GC::Ptr<WebAssembly::Memory>> get_memory_instance(Wasm::MemoryAddress address) { return m_memory_instances.get(address); }

    HashMap<Wasm::FunctionAddress, GC::Ptr<JS::NativeFunction>> const& function_instances() const { return m_function_instances; }
    HashMap<Wasm::ExternAddress, JS::Value> const& extern_values() const { return m_extern_values; }
    HashMap<JS::Value, Wasm::ExternAddress> const& inverse_extern_values() const { return m_inverse_extern_values; }
    HashMap<Wasm::GlobalAddress, GC::Ptr<WebAssembly::Global>> const& global_instances() const { return m_global_instances; }
    HashMap<Wasm::MemoryAddress, GC::Ptr<WebAssembly::Memory>> const& memory_instances() const { return m_memory_instances; }
    HashTable<GC::Ptr<JS::Object>> const& imported_objects() const { return m_imported_objects; }
    Wasm::AbstractMachine& abstract_machine() { return m_abstract_machine; }

private:
    HashMap<Wasm::FunctionAddress, GC::Ptr<JS::NativeFunction>> m_function_instances;
    HashMap<Wasm::ExternAddress, JS::Value> m_extern_values;
    HashMap<JS::Value, Wasm::ExternAddress> m_inverse_extern_values;
    HashMap<Wasm::GlobalAddress, GC::Ptr<WebAssembly::Global>> m_global_instances;
    HashMap<Wasm::MemoryAddress, GC::Ptr<WebAssembly::Memory>> m_memory_instances;
    Vector<NonnullRefPtr<CompiledWebAssemblyModule>> m_compiled_modules;
    HashTable<GC::Ptr<JS::Object>> m_imported_objects;
    Wasm::AbstractMachine m_abstract_machine;
};

class ExportedWasmFunction final : public JS::NativeFunction {
    JS_OBJECT(ExportedWasmFunction, JS::NativeFunction);
    GC_DECLARE_ALLOCATOR(ExportedWasmFunction);

public:
    static GC::Ref<ExportedWasmFunction> create(JS::Realm&, Utf16FlyString name, ESCAPING Function<JS::ThrowCompletionOr<JS::Value>(JS::VM&)>, Wasm::FunctionAddress);
    virtual ~ExportedWasmFunction() override = default;

    Wasm::FunctionAddress exported_address() const { return m_exported_address; }

protected:
    ExportedWasmFunction(Utf16FlyString name, AK::Function<JS::ThrowCompletionOr<JS::Value>(JS::VM&)>, Wasm::FunctionAddress, Object& prototype);

private:
    Wasm::FunctionAddress m_exported_address;
};

WebAssemblyCache& get_cache(JS::Realm&);

JS::ThrowCompletionOr<NonnullOwnPtr<Wasm::ModuleInstance>> instantiate_module(JS::VM&, Wasm::Module const&, GC::Ptr<JS::Object> import_object);
JS::ThrowCompletionOr<NonnullRefPtr<CompiledWebAssemblyModule>> compile_a_webassembly_module(JS::VM&, ByteBuffer);
JS::NativeFunction* create_native_function(JS::VM&, Wasm::FunctionAddress address, Utf16FlyString name, Instance* instance = nullptr);
JS::ThrowCompletionOr<Wasm::Value> to_webassembly_value(JS::VM&, JS::Value value, Wasm::ValueType const& type);
Wasm::Value default_webassembly_value(JS::VM&, Wasm::ValueType type);
JS::Value to_js_value(JS::VM&, Wasm::Value& wasm_value, Wasm::ValueType type);
JS::ThrowCompletionOr<void> host_ensure_can_compile_wasm_bytes(JS::VM&);
JS::ThrowCompletionOr<JS::HandledByHost> host_resize_array_buffer(JS::VM&, JS::ArrayBuffer&, size_t);

extern HashMap<GC::Ptr<JS::Object>, WebAssemblyCache> s_caches;

}

#define WASM_ENUMERATE_NATIVE_ERRORS                                                                                          \
    __WASM_ENUMERATE(CompileError, "WebAssembly.CompileError", compile_error, CompileErrorPrototype, CompileErrorConstructor) \
    __WASM_ENUMERATE(LinkError, "WebAssembly.LinkError", link_error, LinkErrorPrototype, LinkErrorConstructor)                \
    __WASM_ENUMERATE(RuntimeError, "WebAssembly.RuntimeError", runtime_error, RuntimeErrorPrototype, RuntimeErrorConstructor)

// NOTE: This is technically not allowed by ECMA262, as the set of native errors is closed
//       our implementation uses this fact in places, but for the purposes of wasm returning
//       *some* kind of error, named e.g. 'WebAssembly.RuntimeError', this is sufficient.
#define DECLARE_WASM_NATIVE_ERROR(ClassName, FullClassName, snake_name, PrototypeName, ConstructorName) \
    class WEB_API ClassName final : public JS::Error {                                                  \
        JS_OBJECT(ClassName, Error);                                                                    \
        GC_DECLARE_ALLOCATOR(ClassName);                                                                \
                                                                                                        \
    public:                                                                                             \
        static GC::Ref<ClassName> create(JS::Realm&);                                                   \
        static GC::Ref<ClassName> create(JS::Realm&, Utf16String message);                              \
        static GC::Ref<ClassName> create(JS::Realm&, StringView message);                               \
                                                                                                        \
        explicit ClassName(Object& prototype);                                                          \
        virtual ~ClassName() override = default;                                                        \
    };

#define DECLARE_WASM_NATIVE_ERROR_CONSTRUCTOR(ClassName, FullClassName, snake_name, PrototypeName, ConstructorName) \
    class ConstructorName final : public JS::NativeFunction {                                                       \
        JS_OBJECT(ConstructorName, NativeFunction);                                                                 \
        GC_DECLARE_ALLOCATOR(ConstructorName);                                                                      \
                                                                                                                    \
    public:                                                                                                         \
        virtual void initialize(JS::Realm&) override;                                                               \
        virtual ~ConstructorName() override;                                                                        \
        virtual JS::ThrowCompletionOr<JS::Value> call() override;                                                   \
        virtual JS::ThrowCompletionOr<GC::Ref<JS::Object>> construct(JS::FunctionObject& new_target) override;      \
                                                                                                                    \
    private:                                                                                                        \
        explicit ConstructorName(JS::Realm&);                                                                       \
                                                                                                                    \
        virtual bool has_constructor() const override                                                               \
        {                                                                                                           \
            return true;                                                                                            \
        }                                                                                                           \
    };

#define DECLARE_WASM_NATIVE_ERROR_PROTOTYPE(ClassName, FullClassName, snake_name, PrototypeName, ConstructorName) \
    class PrototypeName final : public JS::PrototypeObject<PrototypeName, ClassName> {                            \
        JS_PROTOTYPE_OBJECT(PrototypeName, ClassName, ClassName);                                                 \
        GC_DECLARE_ALLOCATOR(PrototypeName);                                                                      \
                                                                                                                  \
    public:                                                                                                       \
        virtual void initialize(JS::Realm&) override;                                                             \
        virtual ~PrototypeName() override = default;                                                              \
                                                                                                                  \
    private:                                                                                                      \
        explicit PrototypeName(JS::Realm&);                                                                       \
    };

#define __WASM_ENUMERATE(ClassName, FullClassName, snake_name, PrototypeName, ConstructorName) \
    DECLARE_WASM_NATIVE_ERROR(ClassName, FullClassName, snake_name, PrototypeName, ConstructorName)
WASM_ENUMERATE_NATIVE_ERRORS
#undef __WASM_ENUMERATE

#define __WASM_ENUMERATE(ClassName, FullClassName, snake_name, PrototypeName, ConstructorName) \
    DECLARE_WASM_NATIVE_ERROR_CONSTRUCTOR(ClassName, FullClassName, snake_name, PrototypeName, ConstructorName)
WASM_ENUMERATE_NATIVE_ERRORS
#undef __WASM_ENUMERATE

#define __WASM_ENUMERATE(ClassName, FullClassName, snake_name, PrototypeName, ConstructorName) \
    DECLARE_WASM_NATIVE_ERROR_PROTOTYPE(ClassName, FullClassName, snake_name, PrototypeName, ConstructorName)
WASM_ENUMERATE_NATIVE_ERRORS
#undef __WASM_ENUMERATE

#undef DECLARE_WASM_NATIVE_ERROR
#undef DECLARE_WASM_NATIVE_ERROR_PROTOTYPE
#undef DECLARE_WASM_NATIVE_ERROR_CONSTRUCTOR

}
