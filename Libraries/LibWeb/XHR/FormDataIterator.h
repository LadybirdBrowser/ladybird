/*
 * Copyright (c) 2023, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Object.h>
#include <LibWeb/XHR/FormData.h>

namespace Web::XHR {

class FormDataIterator : public JS::Object {
    JS_OBJECT(FormDataIterator, JS::Object);
    GC_DECLARE_ALLOCATOR(FormDataIterator);

public:
    [[nodiscard]] static GC::Ref<FormDataIterator> create(JS::Realm&, FormData const&, JS::Object::PropertyKind iterator_kind);

    virtual ~FormDataIterator() override;

    JS::Object* next();

private:
    FormDataIterator(JS::Realm&, FormData const&, JS::Object::PropertyKind iterator_kind);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    GC::Ref<FormData const> m_form_data;
    JS::Object::PropertyKind m_iterator_kind;
    size_t m_index { 0 };
};

}
