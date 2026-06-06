/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibGC/Ptr.h>
#include <LibGC/Weak.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibWasm/AbstractMachine/AbstractMachine.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/Memory.h>
#include <LibWeb/Bindings/Wrappable.h>

namespace Web::WebAssembly {

class Memory : public Bindings::Wrappable {
    WEB_WRAPPABLE(Memory, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(Memory);

    enum class Shared {
        No,
        Yes,
    };

public:
    static WebIDL::ExceptionOr<GC::Ref<Memory>> construct_impl(JS::Realm&, Bindings::MemoryDescriptor& descriptor);
    static GC::Ref<Memory> create(JS::Realm&, Wasm::MemoryAddress, Shared);

    JS::ThrowCompletionOr<u32> grow(u32 delta);

    WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> to_fixed_length_buffer();
    WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> to_resizable_buffer();
    WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> buffer() const;
    bool has_buffer_object(JS::ArrayBuffer const&) const;

    Wasm::MemoryAddress address() const { return m_address; }

private:
    Memory(JS::Realm&, Wasm::MemoryAddress, Shared shared);

    virtual void visit_edges(Visitor&) override;

    static void refresh_the_memory_buffer(JS::VM&, JS::Realm&, Wasm::MemoryAddress);
    static GC::Ref<JS::ArrayBuffer> create_a_fixed_length_memory_buffer(JS::VM&, JS::Realm& buffer_realm, JS::Realm& memory_realm, Wasm::MemoryAddress, Shared shared, GC::Ref<GC::Cell> owner);
    static JS::ThrowCompletionOr<GC::Ref<JS::ArrayBuffer>> create_a_resizable_memory_buffer(JS::VM&, JS::Realm& buffer_realm, JS::Realm& memory_realm, Wasm::MemoryAddress, Shared shared, size_t max_size, GC::Ref<GC::Cell> owner);

    GC::Ref<JS::ArrayBuffer> buffer_object_for_realm(JS::Realm&) const;
    void set_buffer_object_for_realm(JS::Realm&, GC::Ref<JS::ArrayBuffer>) const;
    void refresh_buffer_object(JS::VM&, GC::Ref<JS::ArrayBuffer>) const;
    void refresh_buffer_objects(JS::VM&) const;

    Wasm::MemoryAddress m_address;
    Shared m_shared { Shared::No };
    mutable GC::Ptr<JS::ArrayBuffer> m_buffer;
    mutable Vector<GC::Weak<JS::ArrayBuffer>> m_live_buffer_objects;
};

}
