/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSMathValue.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#cssmathmin
class CSSMathMin final : public CSSMathValue {
    WEB_PLATFORM_OBJECT(CSSMathMin, CSSMathValue);
    GC_DECLARE_ALLOCATOR(CSSMathMin);

public:
    [[nodiscard]] static GC::Ref<CSSMathMin> create(JS::Realm&, NumericType, GC::Ref<CSSNumericArray>);
    static WebIDL::ExceptionOr<GC::Ref<CSSMathMin>> construct_impl(JS::Realm&, Vector<CSSNumberish>);

    virtual ~CSSMathMin() override;

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    GC::Ref<CSSNumericArray> values() const;

    virtual String serialize_math_value(Nested, Parens) const override;
    virtual bool is_equal_numeric_value(GC::Ref<CSSNumericValue> other) const override;

private:
    CSSMathMin(JS::Realm&, NumericType, GC::Ref<CSSNumericArray>);
    GC::Ref<CSSNumericArray> m_values;
};

}
