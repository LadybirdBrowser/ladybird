/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/CSSNumericArray.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#cssnumericarray
class CSSNumericArray : public Bindings::Wrappable {
    WEB_WRAPPABLE(CSSNumericArray, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(CSSNumericArray);

public:
    [[nodiscard]] static GC::Ref<CSSNumericArray> create(Vector<GC::Ref<CSSNumericValue>>);

    virtual ~CSSNumericArray() override;

    WebIDL::UnsignedLong length() const;
    virtual Optional<JS::Value> item_value(Bindings::WrapperWorld& wrapper_world, JS::Realm& realm, size_t index) const override;
    Vector<GC::Ref<CSSNumericValue>> values() { return m_values; }

    virtual void visit_edges(GC::Cell::Visitor&) override;
    bool is_equal_numeric_values(GC::Ref<CSSNumericArray> other) const;

private:
    CSSNumericArray(Vector<GC::Ref<CSSNumericValue>>);

    Vector<GC::Ref<CSSNumericValue>> m_values;
};

}
