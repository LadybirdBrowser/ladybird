/*
 * Copyright (c) 2024-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibWeb/CSS/Parser/ComponentValue.h>
#include <LibWeb/CSS/Parser/Token.h>
#include <LibWeb/CSS/StyleValues/CSSNumericValue.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#cssunitvalue
class CSSUnitValue : public CSSNumericValue {
public:
    virtual ~CSSUnitValue() override = default;

    virtual double value() const = 0;
    virtual StringView unit() const = 0;
    virtual Vector<Parser::ComponentValue> tokenize() const override
    {
        return { Parser::Token::create_dimension(value(), FlyString::from_utf8_without_validation(unit().bytes())) };
    }

protected:
    explicit CSSUnitValue(Type type)
        : CSSNumericValue(type)
    {
    }
};

}
