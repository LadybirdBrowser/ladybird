/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibGC/Ptr.h>
#include <LibGC/Root.h>
#include <LibJS/Forward.h>
#include <LibWasm/AbstractMachine/AbstractMachine.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/WebAssembly/WebAssembly.h>

namespace Web::WebAssembly {

class Instance : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(Instance, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(Instance);

public:
    static WebIDL::ExceptionOr<GC::Ref<Instance>> construct_impl(JS::Realm&, Module& module, Optional<GC::Root<JS::Object>>& import_object);

    Object const* exports() const { return m_exports.ptr(); }

private:
    Instance(JS::Realm&, NonnullOwnPtr<Wasm::ModuleInstance>);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    GC::Ref<Object> m_exports;
    NonnullOwnPtr<Wasm::ModuleInstance> m_module_instance;
    HashMap<Wasm::FunctionAddress, GC::Ptr<JS::FunctionObject>> m_function_instances;
    HashMap<Wasm::MemoryAddress, GC::Ptr<WebAssembly::Memory>> m_memory_instances;
    HashMap<Wasm::TableAddress, GC::Ptr<WebAssembly::Table>> m_table_instances;
};

}
