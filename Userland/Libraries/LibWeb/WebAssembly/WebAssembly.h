/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/Handle.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Value.h>
#include <LibWasm/AbstractMachine/AbstractMachine.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Forward.h>

namespace Web::WebAssembly {

void visit_edges(JS::Object&, JS::Cell::Visitor&);
void finalize(JS::Object&);

bool validate(JS::VM&, JS::Handle<WebIDL::BufferSource>& bytes);
WebIDL::ExceptionOr<JS::Value> compile(JS::VM&, JS::Handle<WebIDL::BufferSource>& bytes);

WebIDL::ExceptionOr<JS::Value> instantiate(JS::VM&, JS::Handle<WebIDL::BufferSource>& bytes, Optional<JS::Handle<JS::Object>>& import_object);
WebIDL::ExceptionOr<JS::Value> instantiate(JS::VM&, Module const& module_object, Optional<JS::Handle<JS::Object>>& import_object);

namespace Detail {
struct CompiledWebAssemblyModule : public RefCounted<CompiledWebAssemblyModule> {
    explicit CompiledWebAssemblyModule(Wasm::Module&& module)
        : module(move(module))
    {
    }

    Wasm::Module module;
};

class WebAssemblyCache {
public:
    void add_compiled_module(NonnullRefPtr<CompiledWebAssemblyModule> module) { m_compiled_modules.append(module); }
    void add_function_instance(Wasm::FunctionAddress address, JS::GCPtr<JS::NativeFunction> function) { m_function_instances.set(address, function); }
    void add_imported_object(JS::GCPtr<JS::Object> object) { m_imported_objects.set(object); }

    Optional<JS::GCPtr<JS::NativeFunction>> get_function_instance(Wasm::FunctionAddress address) { return m_function_instances.get(address); }

    HashMap<Wasm::FunctionAddress, JS::GCPtr<JS::NativeFunction>> function_instances() const { return m_function_instances; }
    HashTable<JS::GCPtr<JS::Object>> imported_objects() const { return m_imported_objects; }
    Wasm::AbstractMachine& abstract_machine() { return m_abstract_machine; }

private:
    HashMap<Wasm::FunctionAddress, JS::GCPtr<JS::NativeFunction>> m_function_instances;
    Vector<NonnullRefPtr<CompiledWebAssemblyModule>> m_compiled_modules;
    HashTable<JS::GCPtr<JS::Object>> m_imported_objects;
    Wasm::AbstractMachine m_abstract_machine;
};

WebAssemblyCache& get_cache(JS::Realm&);

JS::ThrowCompletionOr<NonnullOwnPtr<Wasm::ModuleInstance>> instantiate_module(JS::VM&, Wasm::Module const&);
JS::ThrowCompletionOr<NonnullRefPtr<CompiledWebAssemblyModule>> parse_module(JS::VM&, JS::Object* buffer);
JS::NativeFunction* create_native_function(JS::VM&, Wasm::FunctionAddress address, ByteString const& name, Instance* instance = nullptr);
JS::ThrowCompletionOr<Wasm::Value> to_webassembly_value(JS::VM&, JS::Value value, Wasm::ValueType const& type);
JS::Value to_js_value(JS::VM&, Wasm::Value& wasm_value);

extern HashMap<JS::GCPtr<JS::Object>, WebAssemblyCache> s_caches;

}

}
