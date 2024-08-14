/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSStyleValue.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#cssnumericvalue
class CSSNumericValue : public CSSStyleValue {
public:
    virtual ~CSSNumericValue() override = default;

protected:
    explicit CSSNumericValue(Type type)
        : CSSStyleValue(type)
    {
    }
};

}
