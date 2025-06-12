/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibWeb/CSS/StyleValues/CSSNumericValue.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#cssunitvalue
class CSSUnitValue : public CSSNumericValue {
public:
    virtual ~CSSUnitValue() override = default;

    virtual double value() const = 0;
    virtual StringView unit() const = 0;

protected:
    explicit CSSUnitValue(Type type)
        : CSSNumericValue(type)
    {
    }
};

}
