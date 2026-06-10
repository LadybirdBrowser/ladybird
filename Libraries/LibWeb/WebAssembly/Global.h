/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibWasm/AbstractMachine/AbstractMachine.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/WebAssembly/WebAssembly.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::WebAssembly {

struct GlobalDescriptor {
    Wasm::ValueType value;
    bool mutable_ { false };
};

class Global : public Bindings::Wrappable {
    WEB_WRAPPABLE(Global, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(Global);

public:
    static WebIDL::ExceptionOr<GC::Ref<Global>> create(NonnullRefPtr<Detail::WebAssemblyCache>, GlobalDescriptor const&, Wasm::Value);
    static GC::Ref<Global> create(NonnullRefPtr<Detail::WebAssemblyCache>, Wasm::GlobalAddress);

    WebIDL::ExceptionOr<Wasm::GlobalType> type() const;
    WebIDL::ExceptionOr<Wasm::Value> value() const;
    WebIDL::ExceptionOr<Wasm::ValueType> value_type() const;
    WebIDL::ExceptionOr<void> set_value(Wasm::Value);

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
