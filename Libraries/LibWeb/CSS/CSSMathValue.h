/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/CSSMathValuePrototype.h>
#include <LibWeb/CSS/CSSNumericValue.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#cssmathvalue
class CSSMathValue : public CSSNumericValue {
    WEB_PLATFORM_OBJECT(CSSMathValue, CSSNumericValue);
    GC_DECLARE_ALLOCATOR(CSSMathValue);

public:
    virtual ~CSSMathValue() override = default;

    Bindings::CSSMathOperator operator_() const { return m_operator; }

    enum class Nested : u8 {
        No,
        Yes,
    };
    enum class Parens : u8 {
        With,
        Without,
    };
    virtual String serialize_math_value(Nested, Parens) const = 0;

    virtual WebIDL::ExceptionOr<NonnullRefPtr<StyleValue const>> create_an_internal_representation(PropertyNameAndID const&, PerformTypeCheck) const final override;

protected:
    explicit CSSMathValue(JS::Realm&, Bindings::CSSMathOperator, NumericType);

    virtual void initialize(JS::Realm&) override;

    Bindings::CSSMathOperator m_operator;
};

}
