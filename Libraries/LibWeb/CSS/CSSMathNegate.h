/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSMathValue.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#cssmathnegate
class CSSMathNegate : public CSSMathValue {
    WEB_PLATFORM_OBJECT(CSSMathNegate, CSSMathValue);
    GC_DECLARE_ALLOCATOR(CSSMathNegate);

public:
    [[nodiscard]] static GC::Ref<CSSMathNegate> create(JS::Realm&, NumericType, GC::Ref<CSSNumericValue>);
    static WebIDL::ExceptionOr<GC::Ref<CSSMathNegate>> construct_impl(JS::Realm&, CSSNumberish);

    virtual ~CSSMathNegate() override;

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    GC::Ref<CSSNumericValue> value() const;

    virtual String serialize_math_value(Nested, Parens) const override;
    virtual bool is_equal_numeric_value(GC::Ref<CSSNumericValue> other) const override;

private:
    CSSMathNegate(JS::Realm&, NumericType, GC::Ref<CSSNumericValue>);
    GC::Ref<CSSNumericValue> m_value;
};

}
