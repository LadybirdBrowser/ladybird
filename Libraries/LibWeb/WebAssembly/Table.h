/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibGC/Ptr.h>
#include <LibWasm/AbstractMachine/AbstractMachine.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/WebAssembly/WebAssembly.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::WebAssembly {

class Table : public Bindings::Wrappable {
    WEB_WRAPPABLE(Table, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(Table);

public:
    static WebIDL::ExceptionOr<GC::Ref<Table>> create(NonnullRefPtr<Detail::WebAssemblyCache>, Wasm::ValueType, Wasm::AddressType, u64 initial, Optional<u64> maximum, Wasm::Reference);

    WebIDL::ExceptionOr<u64> grow(u64 delta, Wasm::Reference);

    WebIDL::ExceptionOr<Wasm::Reference> get(u64 index) const;
    WebIDL::ExceptionOr<void> set(u64 index, Wasm::Reference);

    WebIDL::ExceptionOr<u64> length() const;
    WebIDL::ExceptionOr<Wasm::AddressType> address_type() const;
    WebIDL::ExceptionOr<Wasm::ValueType> element_type() const;

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
