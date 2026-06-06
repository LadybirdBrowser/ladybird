/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWasm/AbstractMachine/AbstractMachine.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/Global.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/WebAssembly/WebAssembly.h>

namespace Web::HTML {

class WindowOrWorkerGlobalScopeMixin;

}

namespace Web::WebAssembly {

class Global : public Bindings::Wrappable {
    WEB_WRAPPABLE(Global, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(Global);

public:
    static WebIDL::ExceptionOr<GC::Ref<Global>> construct_impl(HTML::WindowOrWorkerGlobalScopeMixin&, Bindings::GlobalDescriptor const&, Optional<JS::Value>);
    static GC::Ref<Global> create(NonnullRefPtr<Detail::WebAssemblyCache>, Wasm::GlobalAddress);

    WebIDL::ExceptionOr<JS::Value> value_of(JS::Realm&) const;

    WebIDL::ExceptionOr<void> set_value(JS::Realm&, JS::Value);
    WebIDL::ExceptionOr<JS::Value> value(JS::Realm&) const;

    Wasm::GlobalAddress address() const { return m_address; }
    Detail::WebAssemblyCache& cache() { return *m_cache; }
    Detail::WebAssemblyCache& cache() const { return *m_cache; }

private:
    Global(NonnullRefPtr<Detail::WebAssemblyCache>, Wasm::GlobalAddress);

    virtual void visit_edges(Visitor&) override;

    NonnullRefPtr<Detail::WebAssemblyCache> m_cache;
    Wasm::GlobalAddress m_address;
};

}
