/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Heap/GCPtr.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Fetch/Headers.h>

namespace Web::Fetch {

class HeadersIterator final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(HeadersIterator, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(HeadersIterator);

public:
    [[nodiscard]] static JS::NonnullGCPtr<HeadersIterator> create(Headers const&, JS::Object::PropertyKind iteration_kind);

    virtual ~HeadersIterator() override;

    JS::NonnullGCPtr<JS::Object> next();

private:
    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(JS::Cell::Visitor&) override;

    HeadersIterator(Headers const&, JS::Object::PropertyKind iteration_kind);

    JS::NonnullGCPtr<Headers const> m_headers;
    JS::Object::PropertyKind m_iteration_kind;
    size_t m_index { 0 };
};

}
