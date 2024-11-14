/*
 * Copyright (c) 2023, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/XHR/FormData.h>

namespace Web::XHR {

class FormDataIterator : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(FormDataIterator, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(FormDataIterator);

public:
    [[nodiscard]] static GC::Ref<FormDataIterator> create(FormData const&, JS::Object::PropertyKind iterator_kind);

    virtual ~FormDataIterator() override;

    JS::Object* next();

private:
    FormDataIterator(FormData const&, JS::Object::PropertyKind iterator_kind);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<FormData const> m_form_data;
    JS::Object::PropertyKind m_iterator_kind;
    size_t m_index { 0 };
};

}
