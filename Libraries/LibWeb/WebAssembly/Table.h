/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/GCPtr.h>
#include <LibJS/Runtime/Value.h>
#include <LibWasm/AbstractMachine/AbstractMachine.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/TablePrototype.h>

namespace Web::WebAssembly {

struct TableDescriptor {
    Bindings::TableKind element;
    u32 initial { 0 };
    Optional<u32> maximum;
};

class Table : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(Table, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(Table);

public:
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<Table>> construct_impl(JS::Realm&, TableDescriptor& descriptor, JS::Value value);

    WebIDL::ExceptionOr<u32> grow(u32 delta, JS::Value value);

    WebIDL::ExceptionOr<JS::Value> get(u32 index) const;
    WebIDL::ExceptionOr<void> set(u32 index, JS::Value value);

    WebIDL::ExceptionOr<u32> length() const;

    Wasm::TableAddress address() const { return m_address; }

private:
    Table(JS::Realm&, Wasm::TableAddress);

    virtual void initialize(JS::Realm&) override;

    Wasm::TableAddress m_address;
};

}
