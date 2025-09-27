/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "DimensionStyleValue.h"
#include <LibWeb/CSS/CSSUnitValue.h>

namespace Web::CSS {

Vector<Parser::ComponentValue> DimensionStyleValue::tokenize() const
{
    return { Parser::Token::create_dimension(raw_value(), FlyString::from_utf8_without_validation(unit_name().bytes())) };
}

// https://drafts.css-houdini.org/css-typed-om-1/#reify-a-numeric-value
GC::Ref<CSSStyleValue> DimensionStyleValue::reify(JS::Realm& realm, FlyString const&) const
{
    // NB: Steps 1 and 2 don't apply here.
    // 3. Return a new CSSUnitValue with its value internal slot set to the numeric value of num, and its unit internal
    //    slot set to "number" if num is a <number>, "percent" if num is a <percentage>, and num’s unit if num is a
    //    <dimension>.
    //    If the value being reified is a computed value, the unit used must be the appropriate canonical unit for the
    //    value’s type, with the numeric value scaled accordingly.
    // FIXME: Reify computed value correctly. That sounds like it should work by computing the value properly before we
    //        reach this point.
    return CSSUnitValue::create(realm, raw_value(), FlyString::from_utf8_without_validation(unit_name().bytes()));
}

}
