/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibJS/Runtime/Object.h>
#include <LibWeb/Fetch/Headers.h>

namespace Web::Fetch {

class HeadersIterator final : public JS::Object {
    JS_OBJECT(HeadersIterator, JS::Object);
    GC_DECLARE_ALLOCATOR(HeadersIterator);

public:
    [[nodiscard]] static GC::Ref<HeadersIterator> create(JS::Realm&, Headers const&, JS::Object::PropertyKind iteration_kind);

    virtual ~HeadersIterator() override;

    GC::Ref<JS::Object> next();

private:
    virtual void visit_edges(GC::Cell::Visitor&) override;

    HeadersIterator(JS::Realm&, Headers const&, JS::Object::PropertyKind iteration_kind);

    GC::Ref<Headers const> m_headers;
    JS::Object::PropertyKind m_iteration_kind;
    size_t m_index { 0 };
};

}
