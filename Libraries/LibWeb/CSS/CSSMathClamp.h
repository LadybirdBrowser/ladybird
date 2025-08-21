/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSMathValue.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#cssmathclamp
class CSSMathClamp : public CSSMathValue {
    WEB_PLATFORM_OBJECT(CSSMathClamp, CSSMathValue);
    GC_DECLARE_ALLOCATOR(CSSMathClamp);

public:
    [[nodiscard]] static GC::Ref<CSSMathClamp> create(JS::Realm&, NumericType, GC::Ref<CSSNumericValue> lower, GC::Ref<CSSNumericValue> value, GC::Ref<CSSNumericValue> upper);
    static WebIDL::ExceptionOr<GC::Ref<CSSMathClamp>> construct_impl(JS::Realm&, CSSNumberish lower, CSSNumberish value, CSSNumberish upper);

    virtual ~CSSMathClamp() override;

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    GC::Ref<CSSNumericValue> lower() const;
    GC::Ref<CSSNumericValue> value() const;
    GC::Ref<CSSNumericValue> upper() const;

    virtual String serialize_math_value(Nested, Parens) const override;

private:
    CSSMathClamp(JS::Realm&, NumericType, GC::Ref<CSSNumericValue> lower, GC::Ref<CSSNumericValue> value, GC::Ref<CSSNumericValue> upper);
    GC::Ref<CSSNumericValue> m_lower;
    GC::Ref<CSSNumericValue> m_value;
    GC::Ref<CSSNumericValue> m_upper;
};

}
