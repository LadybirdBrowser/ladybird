/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/GCPtr.h>
#include <LibJS/Heap/Handle.h>
#include <LibWasm/AbstractMachine/AbstractMachine.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/WebAssembly/WebAssembly.h>

namespace Web::WebAssembly {

class Instance : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(Instance, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(Instance);

public:
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<Instance>> construct_impl(JS::Realm&, Module& module, Optional<JS::Handle<JS::Object>>& import_object);

    Object const* exports() const { return m_exports.ptr(); }

private:
    Instance(JS::Realm&, NonnullOwnPtr<Wasm::ModuleInstance>);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    JS::NonnullGCPtr<Object> m_exports;
    NonnullOwnPtr<Wasm::ModuleInstance> m_module_instance;
    HashMap<Wasm::FunctionAddress, JS::GCPtr<JS::FunctionObject>> m_function_instances;
    HashMap<Wasm::MemoryAddress, JS::GCPtr<WebAssembly::Memory>> m_memory_instances;
    HashMap<Wasm::TableAddress, JS::GCPtr<WebAssembly::Table>> m_table_instances;
};

}
