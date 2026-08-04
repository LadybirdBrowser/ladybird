/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
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
    GC::Ptr<CSSNumericValue> value_at(size_t index) const;
    Vector<GC::Ref<CSSNumericValue>> values() { return m_values; }

    virtual void visit_edges(GC::Cell::Visitor&) override;
    bool is_equal_numeric_values(GC::Ref<CSSNumericArray> other) const;

private:
    CSSNumericArray(Vector<GC::Ref<CSSNumericValue>>);

    Vector<GC::Ref<CSSNumericValue>> m_values;
};

}
