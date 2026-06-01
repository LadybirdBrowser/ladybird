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
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/Table.h>

namespace Web::WebAssembly {

class Table : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(Table, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(Table);

public:
    static WebIDL::ExceptionOr<GC::Ref<Table>> construct_impl(JS::Realm&, Bindings::TableDescriptor& descriptor, Optional<JS::Value> value);

    WebIDL::ExceptionOr<JS::Value> grow(JS::Value delta, Optional<JS::Value> value);

    WebIDL::ExceptionOr<JS::Value> get(JS::Value index) const;
    WebIDL::ExceptionOr<void> set(JS::Value index, Optional<JS::Value> value);

    WebIDL::ExceptionOr<JS::Value> length() const;

    Wasm::TableAddress address() const { return m_address; }

private:
    Table(JS::Realm&, Wasm::TableAddress);

    virtual void initialize(JS::Realm&) override;

    Wasm::TableAddress m_address;
};

}
