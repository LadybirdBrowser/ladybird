/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/WebAssembly/WebAssembly.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::WebAssembly {

struct GlobalDescriptor {
    Wasm::Runtime::ValueType value;
    bool mutable_ { false };
};

struct GlobalType {
    Wasm::Runtime::ValueType value;
    bool mutable_ { false };
};

class Global : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(Global, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(Global);

public:
    static GC::Ref<Global> create(GlobalDescriptor const&, Wasm::Runtime::Value);
    static GC::Ref<Global> create(Wasm::Runtime::Entity global);

    GlobalType type() const;
    WebIDL::ExceptionOr<Wasm::Runtime::Value> value() const;
    WebIDL::ExceptionOr<Wasm::Runtime::ValueType> value_type() const;
    WebIDL::ExceptionOr<void> set_value(Wasm::Runtime::Value);

    Wasm::Runtime::Entity address() const { return m_global; }

private:
    explicit Global(Wasm::Runtime::Entity global);

    virtual void visit_edges(Visitor&) override;

    Wasm::Runtime::Entity m_global { nullptr };
};

}
