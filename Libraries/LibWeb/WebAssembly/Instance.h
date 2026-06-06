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
#include <LibGC/Weak.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Object.h>
#include <LibWasm/AbstractMachine/AbstractMachine.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/Instance.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/WebAssembly/WebAssembly.h>

namespace Web::HTML {

class WindowOrWorkerGlobalScopeMixin;

}

namespace Web::WebAssembly {

class Instance : public Bindings::Wrappable {
    WEB_WRAPPABLE(Instance, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(Instance);

public:
    static WebIDL::ExceptionOr<GC::Ref<Instance>> construct_impl(HTML::WindowOrWorkerGlobalScopeMixin&, Module& module, GC::Ptr<JS::Object> import_object);
    static GC::Ref<Instance> create(NonnullRefPtr<Detail::WebAssemblyCache>, NonnullRefPtr<Wasm::ModuleInstance>);

    JS::Object const* exports(JS::Realm&);
    Wasm::ModuleInstance const* module_instance() const { return m_module_instance.ptr(); }
    Detail::WebAssemblyCache& cache() { return *m_cache; }
    Detail::WebAssemblyCache& cache() const { return *m_cache; }

private:
    Instance(NonnullRefPtr<Detail::WebAssemblyCache>, NonnullRefPtr<Wasm::ModuleInstance>);

    virtual void visit_edges(Visitor&) override;
    GC::Ref<JS::Object> create_exports_object(JS::Realm&, bool cache_function_exports);

    JS::Object const* exports_for_realm(JS::Realm&);

    GC::Ptr<JS::Object> m_exports;
    Vector<GC::Weak<JS::Object>> m_live_export_objects;
    NonnullRefPtr<Detail::WebAssemblyCache> m_cache;
    NonnullRefPtr<Wasm::ModuleInstance> m_module_instance;
    HashMap<Wasm::FunctionAddress, GC::Ptr<JS::FunctionObject>> m_function_instances;
};

}
