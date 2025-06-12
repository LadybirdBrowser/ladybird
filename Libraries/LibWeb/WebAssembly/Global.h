/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWasm/AbstractMachine/AbstractMachine.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/GlobalPrototype.h>
#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::WebAssembly {

struct GlobalDescriptor {
    Bindings::ValueType value;
    bool mutable_ { false };
};

class Global : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(Global, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(Global);

public:
    static WebIDL::ExceptionOr<GC::Ref<Global>> construct_impl(JS::Realm&, GlobalDescriptor& descriptor, JS::Value v);

    WebIDL::ExceptionOr<JS::Value> value_of() const;

    WebIDL::ExceptionOr<void> set_value(JS::Value);
    WebIDL::ExceptionOr<JS::Value> value() const;

    Wasm::GlobalAddress address() const { return m_address; }

private:
    Global(JS::Realm&, Wasm::GlobalAddress);

    virtual void initialize(JS::Realm&) override;

    Wasm::GlobalAddress m_address;
};

}
