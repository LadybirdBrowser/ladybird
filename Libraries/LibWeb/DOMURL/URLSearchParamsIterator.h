/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Object.h>
#include <LibWeb/DOMURL/URLSearchParams.h>

namespace Web::DOMURL {

class URLSearchParamsIterator : public JS::Object {
    JS_OBJECT(URLSearchParamsIterator, JS::Object);
    GC_DECLARE_ALLOCATOR(URLSearchParamsIterator);

public:
    static WebIDL::ExceptionOr<GC::Ref<URLSearchParamsIterator>> create(JS::Realm&, URLSearchParams const&, JS::Object::PropertyKind iteration_kind);

    virtual ~URLSearchParamsIterator() override;

    JS::Object* next();

private:
    URLSearchParamsIterator(JS::Realm&, URLSearchParams const&, JS::Object::PropertyKind iteration_kind);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(GC::Cell::Visitor&) override;

    GC::Ref<URLSearchParams const> m_url_search_params;
    JS::Object::PropertyKind m_iteration_kind;
    size_t m_index { 0 };
};

}
