/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Utf16String.h>
#include <AK/Variant.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/WebAssembly/WebAssembly.h>

namespace Web::WebAssembly {

class Tag;

// https://webassembly.github.io/spec/js-api/#exceptions
class Exception : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(Exception, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(Exception);

public:
    static GC::Ref<Exception> create(u64 exception, Optional<Utf16String> stack);
    // The Exception object of an exception, which is the same object for as long as it lives.
    static GC::Ref<Exception> from_address(u64 exception);

    u64 address() const { return m_exception; }
    Wasm::Runtime::Entity tag_address() const { return m_tag.ptr(); }

    bool is(Tag const&) const;
    Variant<Utf16String, Empty> stack() const;

private:
    Exception(u64 exception, Wasm::Runtime::Entity tag, Optional<Utf16String> stack);

    virtual void visit_edges(Visitor&) override;

    u64 m_exception { 0 };
    GC::Ptr<GC::ForeignCell> m_tag;
    Optional<Utf16String> m_stack;
};

}
