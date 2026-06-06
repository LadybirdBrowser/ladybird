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
#include <LibJS/Runtime/Value.h>
#include <LibWasm/AbstractMachine/AbstractMachine.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/Table.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/WebAssembly/WebAssembly.h>

namespace Web::HTML {

class WindowOrWorkerGlobalScopeMixin;

}

namespace Web::WebAssembly {

class Table : public Bindings::Wrappable {
    WEB_WRAPPABLE(Table, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(Table);

public:
    static WebIDL::ExceptionOr<GC::Ref<Table>> construct_impl(HTML::WindowOrWorkerGlobalScopeMixin&, Bindings::TableDescriptor& descriptor, Optional<JS::Value> value);

    WebIDL::ExceptionOr<JS::Value> grow(JS::Realm&, JS::Value delta, Optional<JS::Value> value);

    WebIDL::ExceptionOr<JS::Value> get(JS::Realm&, JS::Value index) const;
    WebIDL::ExceptionOr<void> set(JS::Realm&, JS::Value index, Optional<JS::Value> value);

    WebIDL::ExceptionOr<JS::Value> length(JS::Realm&) const;

    Wasm::TableAddress address() const { return m_address; }
    Detail::WebAssemblyCache& cache() { return *m_cache; }
    Detail::WebAssemblyCache& cache() const { return *m_cache; }

private:
    Table(NonnullRefPtr<Detail::WebAssemblyCache>, Wasm::TableAddress);

    virtual void visit_edges(Visitor&) override;

    NonnullRefPtr<Detail::WebAssemblyCache> m_cache;
    Wasm::TableAddress m_address;
};

}
