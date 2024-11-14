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
#include <LibJS/Runtime/Value.h>
#include <LibWasm/AbstractMachine/AbstractMachine.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Forward.h>

namespace Web::WebAssembly {

void visit_edges(JS::Object&, JS::Cell::Visitor&);
void finalize(JS::Object&);

bool validate(JS::VM&, GC::Root<WebIDL::BufferSource>& bytes);
WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> compile(JS::VM&, GC::Root<WebIDL::BufferSource>& bytes);
WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> compile_streaming(JS::VM&, GC::Root<WebIDL::Promise> source);

WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> instantiate(JS::VM&, GC::Root<WebIDL::BufferSource>& bytes, Optional<GC::Root<JS::Object>>& import_object);
WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> instantiate(JS::VM&, Module const& module_object, Optional<GC::Root<JS::Object>>& import_object);
WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> instantiate_streaming(JS::VM&, GC::Root<WebIDL::Promise> source, Optional<GC::Root<JS::Object>>& import_object);

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
    void add_compiled_module(NonnullRefPtr<CompiledWebAssemblyModule> module) { m_compiled_modules.append(module); }
    void add_function_instance(Wasm::FunctionAddress address, GC::Ptr<JS::NativeFunction> function) { m_function_instances.set(address, function); }
    void add_imported_object(GC::Ptr<JS::Object> object) { m_imported_objects.set(object); }
    void add_extern_value(Wasm::ExternAddress address, JS::Value value) { m_extern_values.set(address, value); }

    Optional<GC::Ptr<JS::NativeFunction>> get_function_instance(Wasm::FunctionAddress address) { return m_function_instances.get(address); }
    Optional<JS::Value> get_extern_value(Wasm::ExternAddress address) { return m_extern_values.get(address); }

    HashMap<Wasm::FunctionAddress, GC::Ptr<JS::NativeFunction>> function_instances() const { return m_function_instances; }
    HashMap<Wasm::ExternAddress, JS::Value> extern_values() const { return m_extern_values; }
    HashTable<GC::Ptr<JS::Object>> imported_objects() const { return m_imported_objects; }
    Wasm::AbstractMachine& abstract_machine() { return m_abstract_machine; }

private:
    HashMap<Wasm::FunctionAddress, GC::Ptr<JS::NativeFunction>> m_function_instances;
    HashMap<Wasm::ExternAddress, JS::Value> m_extern_values;
    Vector<NonnullRefPtr<CompiledWebAssemblyModule>> m_compiled_modules;
    HashTable<GC::Ptr<JS::Object>> m_imported_objects;
    Wasm::AbstractMachine m_abstract_machine;
};

WebAssemblyCache& get_cache(JS::Realm&);

JS::ThrowCompletionOr<NonnullOwnPtr<Wasm::ModuleInstance>> instantiate_module(JS::VM&, Wasm::Module const&, GC::Ptr<JS::Object> import_object);
JS::ThrowCompletionOr<NonnullRefPtr<CompiledWebAssemblyModule>> compile_a_webassembly_module(JS::VM&, ByteBuffer);
JS::NativeFunction* create_native_function(JS::VM&, Wasm::FunctionAddress address, ByteString const& name, Instance* instance = nullptr);
JS::ThrowCompletionOr<Wasm::Value> to_webassembly_value(JS::VM&, JS::Value value, Wasm::ValueType const& type);
Wasm::Value default_webassembly_value(JS::VM&, Wasm::ValueType type);
JS::Value to_js_value(JS::VM&, Wasm::Value& wasm_value, Wasm::ValueType type);

extern HashMap<GC::Ptr<JS::Object>, WebAssemblyCache> s_caches;

}

}
