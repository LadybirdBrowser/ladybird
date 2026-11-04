/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/CSSUnitValue.h>
#include <LibWeb/CSS/CSSNumericValue.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#cssunitvalue
class WEB_API CSSUnitValue final : public CSSNumericValue {
    WEB_RUST_INTERFACE(CSSUnitValue, css_unit_value)

public:
    static GC::Ref<CSSUnitValue> create(double value, Utf16FlyString const& unit);

    double value() const;
    Utf16FlyString unit() const;
};

}
