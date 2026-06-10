/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWasm/AbstractMachine/AbstractMachine.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/WebAssembly/WebAssembly.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::WebAssembly {

class Instance : public Bindings::Wrappable {
    WEB_WRAPPABLE(Instance, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(Instance);

public:
    static WebIDL::ExceptionOr<GC::Ref<Instance>> create(JS::Realm&, Module& module, GC::Ptr<JS::Object> import_object);
    static GC::Ref<Instance> create(NonnullRefPtr<Detail::WebAssemblyCache>, NonnullRefPtr<Wasm::ModuleInstance>);

    Wasm::ModuleInstance const* module_instance() const { return m_module_instance.ptr(); }
    GC::Ref<Global> global_instance(Wasm::GlobalAddress);
    GC::Ref<Memory> memory_instance(Wasm::MemoryAddress);
    GC::Ref<Table> table_instance(Wasm::TableAddress);
    Detail::WebAssemblyCache& cache() { return *m_cache; }
    Detail::WebAssemblyCache& cache() const { return *m_cache; }

private:
    Instance(NonnullRefPtr<Detail::WebAssemblyCache>, NonnullRefPtr<Wasm::ModuleInstance>);

    virtual void visit_edges(Visitor&) override;

    NonnullRefPtr<Detail::WebAssemblyCache> m_cache;
    NonnullRefPtr<Wasm::ModuleInstance> m_module_instance;
};

}
