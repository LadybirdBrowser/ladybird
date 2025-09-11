/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSMathValue.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#cssmathinvert
class CSSMathInvert final : public CSSMathValue {
    WEB_PLATFORM_OBJECT(CSSMathInvert, CSSMathValue);
    GC_DECLARE_ALLOCATOR(CSSMathInvert);

public:
    [[nodiscard]] static GC::Ref<CSSMathInvert> create(JS::Realm&, NumericType, GC::Ref<CSSNumericValue>);
    static WebIDL::ExceptionOr<GC::Ref<CSSMathInvert>> construct_impl(JS::Realm&, CSSNumberish);

    virtual ~CSSMathInvert() override;

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    GC::Ref<CSSNumericValue> value() const;

    virtual String serialize_math_value(Nested, Parens) const override;
    virtual bool is_equal_numeric_value(GC::Ref<CSSNumericValue> other) const override;
    virtual Optional<SumValue> create_a_sum_value() const override;

private:
    CSSMathInvert(JS::Realm&, NumericType, GC::Ref<CSSNumericValue>);
    GC::Ref<CSSNumericValue> m_value;
};

}
