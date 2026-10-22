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
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/WebAssembly/WebAssembly.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::WebAssembly {

class Instance : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(Instance, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(Instance);

public:
    static WebIDL::ExceptionOr<GC::Ref<Instance>> create(JS::Realm&, Module& module, GC::Ptr<JS::Object> import_object);
    static GC::Ref<Instance> create(JS::Realm&, Wasm::Runtime::Entity instance);

    Wasm::Runtime::Entity instance_address() const { return m_instance.ptr(); }
    // The world of the realm the instance was created in, the only one whose realms may read its exports.
    Bindings::WrapperWorld const& wrapper_world() const { return *m_wrapper_world; }
    GC::Ref<Global> global_instance(Wasm::Runtime::Entity);
    GC::Ref<Memory> memory_instance(Wasm::Runtime::Entity);
    GC::Ref<Table> table_instance(Wasm::Runtime::Entity);

private:
    Instance(Bindings::WrapperWorld&, Wasm::Runtime::Entity instance);

    virtual void visit_edges(Visitor&) override;

    GC::Ptr<GC::ForeignCell> m_instance;
    GC::Ref<Bindings::WrapperWorld> m_wrapper_world;
};

}
