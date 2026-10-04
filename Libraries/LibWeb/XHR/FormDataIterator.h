/*
 * Copyright (c) 2023, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Heap/Cell.h>
#include <LibJS/Runtime/HostObject.h>
#include <LibJS/Runtime/Object.h>
#include <LibWeb/XHR/FormData.h>

namespace Web::XHR {

// The state of a FormData iterator. The iterator's JS object is a host object that carries this cell as its host data.
class FormDataIterator final : public JS::Cell {
    GC_CELL(FormDataIterator, JS::Cell);
    GC_DECLARE_ALLOCATOR(FormDataIterator);

public:
    using JSValueConversionIsForbidden = void;

    [[nodiscard]] static GC::Ref<JS::HostObject> create(JS::Realm&, FormData const&, JS::Object::PropertyKind iterator_kind);

    virtual ~FormDataIterator() override;

    GC::Ref<JS::Object> next(JS::Realm&);

private:
    FormDataIterator(FormData const&, JS::Object::PropertyKind iterator_kind);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    GC::Ref<FormData const> m_form_data;
    JS::Object::PropertyKind m_iterator_kind;
    size_t m_index { 0 };
};

}
