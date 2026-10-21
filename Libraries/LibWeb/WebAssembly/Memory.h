/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibJS/Forward.h>
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

class Memory : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(Memory, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(Memory);

    enum class Shared {
        No,
        Yes,
    };

public:
    static WebIDL::ExceptionOr<GC::Ref<Memory>> create(MemoryDescriptor const&);
    static GC::Ref<Memory> create(Wasm::Runtime::Entity memory, Shared);
    static WebIDL::ExceptionOr<GC::Ref<Memory>> create_for_constructor(JS::Object&, MemoryDescriptor const&);

    WebIDL::ExceptionOr<u32> grow(u32 delta);
    WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> to_fixed_length_buffer(JS::Realm&);
    WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> to_resizable_buffer(JS::Realm&);
    WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> buffer(JS::Realm&);

    Wasm::Runtime::Entity address() const { return m_memory; }
    bool is_shared() const { return m_shared == Shared::Yes; }
    static Detail::WebAssemblyCache& cache() { return Detail::WebAssemblyCache::the(); }

private:
    Memory(Wasm::Runtime::Entity memory, Shared shared);

    virtual void visit_edges(Visitor&) override;

    Wasm::Runtime::Entity m_memory { nullptr };
    Shared m_shared { Shared::No };
};

bool memory_has_buffer_object(Memory const&, JS::ArrayBuffer const&);
void refresh_the_memory_buffer(Memory&);

}
