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
    virtual FlyString unit_name() const = 0;
    virtual Vector<Parser::ComponentValue> tokenize() const override;
    virtual GC::Ref<CSSStyleValue> reify(JS::Realm&, String const& associated_property) const override;

protected:
    explicit DimensionStyleValue(Type type)
        : StyleValue(type)
    {
    }
};

}
