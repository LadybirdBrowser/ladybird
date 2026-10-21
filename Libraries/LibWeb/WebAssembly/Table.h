/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/WebAssembly/WebAssembly.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::WebAssembly {

class Table : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(Table, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(Table);

public:
    // A table's elements are references, as their bits.
    static WebIDL::ExceptionOr<GC::Ref<Table>> create(Wasm::Runtime::ValueType element_type, bool address64, u64 initial, Optional<u64> maximum, u64 reference);
    static GC::Ref<Table> create(Wasm::Runtime::Entity table);

    WebIDL::ExceptionOr<u64> grow(u64 delta, u64 reference);
    WebIDL::ExceptionOr<u64> get(u64 index) const;
    WebIDL::ExceptionOr<void> set(u64 index, u64 reference);
    u64 length() const;
    bool is_address64() const;
    Wasm::Runtime::ValueType element_type() const;

    Wasm::Runtime::Entity address() const { return m_table; }
    static Detail::WebAssemblyCache& cache() { return Detail::WebAssemblyCache::the(); }

private:
    explicit Table(Wasm::Runtime::Entity table);

    virtual void visit_edges(Visitor&) override;

    Wasm::Runtime::Entity m_table { nullptr };
};

}
