/*
 * Copyright (c) 2024-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibWeb/CSS/Parser/ComponentValue.h>
#include <LibWeb/CSS/Parser/Token.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class DimensionStyleValue : public StyleValue {
public:
    virtual ~DimensionStyleValue() override = default;

    virtual double raw_value() const = 0;
    virtual StringView unit_name() const = 0;
    virtual Vector<Parser::ComponentValue> tokenize() const override
    {
        return { Parser::Token::create_dimension(raw_value(), FlyString::from_utf8_without_validation(unit_name().bytes())) };
    }

protected:
    explicit DimensionStyleValue(Type type)
        : StyleValue(type)
    {
    }
};

}
