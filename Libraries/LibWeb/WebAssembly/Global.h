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

namespace Web::WebAssembly {

class Global : public Bindings::Wrappable {
    WEB_WRAPPABLE(Global, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(Global);

public:
    static WebIDL::ExceptionOr<GC::Ref<Global>> construct_impl(JS::Realm&, Bindings::GlobalDescriptor const&, Optional<JS::Value>);
    static GC::Ref<Global> create(JS::Realm&, Wasm::GlobalAddress);

    WebIDL::ExceptionOr<JS::Value> value_of() const;

    WebIDL::ExceptionOr<void> set_value(JS::Value);
    WebIDL::ExceptionOr<JS::Value> value() const;

    Wasm::GlobalAddress address() const { return m_address; }

private:
    Global(JS::Realm&, Wasm::GlobalAddress);

    Wasm::GlobalAddress m_address;
};

}
