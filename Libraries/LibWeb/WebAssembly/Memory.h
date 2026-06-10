/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibJS/Forward.h>
#include <LibWasm/AbstractMachine/AbstractMachine.h>
#include <LibWeb/Bindings/Memory.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/WebAssembly/WebAssembly.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace JS {

class ArrayBuffer;
class Realm;

}

namespace Web::WebAssembly {

using MemoryDescriptor = Bindings::MemoryDescriptor;

class Memory : public Bindings::Wrappable {
    WEB_WRAPPABLE(Memory, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(Memory);

    enum class Shared {
        No,
        Yes,
    };

public:
    static WebIDL::ExceptionOr<GC::Ref<Memory>> create(NonnullRefPtr<Detail::WebAssemblyCache>, MemoryDescriptor const&);
    static GC::Ref<Memory> create(NonnullRefPtr<Detail::WebAssemblyCache>, Wasm::MemoryAddress, Shared);
    static WebIDL::ExceptionOr<GC::Ref<Memory>> construct_impl(JS::Realm&, MemoryDescriptor const&);

    WebIDL::ExceptionOr<u32> grow(u32 delta);
    WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> to_fixed_length_buffer(JS::Realm&);
    WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> to_resizable_buffer(JS::Realm&);
    WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> buffer(JS::Realm&);

    Wasm::MemoryAddress address() const { return m_address; }
    bool is_shared() const { return m_shared == Shared::Yes; }
    Detail::WebAssemblyCache& cache() { return *m_cache; }
    Detail::WebAssemblyCache& cache() const { return *m_cache; }

private:
    Memory(NonnullRefPtr<Detail::WebAssemblyCache>, Wasm::MemoryAddress, Shared shared);

    virtual void visit_edges(Visitor&) override;

    NonnullRefPtr<Detail::WebAssemblyCache> m_cache;
    Wasm::MemoryAddress m_address;
    Shared m_shared { Shared::No };
};

bool memory_has_buffer_object(Memory const&, JS::ArrayBuffer const&);
void refresh_the_memory_buffer(Detail::WebAssemblyCache&, Wasm::MemoryAddress);

}
