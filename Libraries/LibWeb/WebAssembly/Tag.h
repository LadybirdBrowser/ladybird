/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/WebAssembly/WebAssembly.h>

namespace Web::WebAssembly {

// https://webassembly.github.io/spec/js-api/#tag
class Tag : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(Tag, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(Tag);

public:
    static GC::Ref<Tag> create(ReadonlySpan<Wasm::Runtime::ValueType> parameters);
    // The Tag object of a tag, which is the same object for as long as it lives.
    static GC::Ref<Tag> from_address(JS::Realm&, Wasm::Runtime::Entity tag);

    Wasm::Runtime::Entity address() const { return m_tag.ptr(); }

private:
    explicit Tag(Wasm::Runtime::Entity tag);

    // The Tag object of a tag other than the JavaScript exception tag, from the agent's tag object cache.
    static GC::Ref<Tag> from_address_in_agent(Wasm::Runtime::Entity tag);

    virtual void visit_edges(Visitor&) override;

    GC::Ptr<GC::ForeignCell> m_tag;
};

}
