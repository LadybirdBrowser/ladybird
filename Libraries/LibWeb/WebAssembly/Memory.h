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
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibWasm/AbstractMachine/AbstractMachine.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::WebAssembly {

struct MemoryDescriptor {
    u32 initial { 0 };
    Optional<u32> maximum;
    Optional<bool> shared;
};

class Memory : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(Memory, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(Memory);

    enum class Shared {
        No,
        Yes,
    };

public:
    static WebIDL::ExceptionOr<GC::Ref<Memory>> construct_impl(JS::Realm&, MemoryDescriptor& descriptor);

    WebIDL::ExceptionOr<u32> grow(u32 delta);
    WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> buffer() const;

    Wasm::MemoryAddress address() const { return m_address; }

private:
    Memory(JS::Realm&, Wasm::MemoryAddress, Shared shared);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    WebIDL::ExceptionOr<void> reset_the_memory_buffer();
    static WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> create_a_fixed_length_memory_buffer(JS::VM&, JS::Realm&, Wasm::MemoryAddress, Shared shared);

    Wasm::MemoryAddress m_address;
    Shared m_shared { Shared::No };
    mutable GC::Ptr<JS::ArrayBuffer> m_buffer;
};

}
