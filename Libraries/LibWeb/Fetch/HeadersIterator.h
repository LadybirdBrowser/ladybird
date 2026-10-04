/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Runtime/HostObject.h>
#include <LibJS/Runtime/Object.h>
#include <LibWeb/Fetch/Headers.h>

namespace Web::Fetch {

// The state of a Headers iterator. The iterator's JS object is a host object that carries this cell as its host data.
class HeadersIterator final : public JS::Cell {
    GC_CELL(HeadersIterator, JS::Cell);
    GC_DECLARE_ALLOCATOR(HeadersIterator);

public:
    using JSValueConversionIsForbidden = void;

    [[nodiscard]] static GC::Ref<JS::HostObject> create(JS::Realm&, Headers const&, JS::Object::PropertyKind iteration_kind);

    virtual ~HeadersIterator() override;

    GC::Ref<JS::Object> next(JS::Realm&);

private:
    virtual void visit_edges(GC::Cell::Visitor&) override;

    HeadersIterator(Headers const&, JS::Object::PropertyKind iteration_kind);

    GC::Ref<Headers const> m_headers;
    JS::Object::PropertyKind m_iteration_kind;
    size_t m_index { 0 };
};

}
