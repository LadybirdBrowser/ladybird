/*
 * Copyright (c) 2023-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "IntegerStyleValue.h"
#include <LibWeb/CSS/CSSUnitValue.h>
#include <LibWeb/CSS/Parser/ComponentValue.h>

namespace Web::CSS {

String IntegerStyleValue::to_string(SerializationMode) const
{
    return String::number(m_value);
}

Vector<Parser::ComponentValue> IntegerStyleValue::tokenize() const
{
    return { Parser::Token::create_number(Number { Number::Type::Integer, static_cast<double>(m_value) }) };
}

// https://drafts.css-houdini.org/css-typed-om-1/#reify-a-numeric-value
GC::Ref<CSSStyleValue> IntegerStyleValue::reify(JS::Realm& realm, FlyString const& associated_property) const
{
    // NB: Step 1 doesn't apply here.
    // 2. If num is the unitless value 0 and num is a <dimension>, return a new CSSUnitValue with its value internal
    //    slot set to 0, and its unit internal slot set to "px".
    if (m_value == 0) {
        // NB: Determine whether the associated property expects 0 to be a <length>.
        // FIXME: Do this for registered custom properties.
        if (auto property_id = property_id_from_string(associated_property); property_id.has_value()
            && property_id != PropertyID::Custom
            && property_accepts_type(*property_id, ValueType::Length)) {
            return CSSUnitValue::create(realm, 0, "px"_fly_string);
        }
    }

    // 3. Return a new CSSUnitValue with its value internal slot set to the numeric value of num, and its unit internal
    //    slot set to "number" if num is a <number>, "percent" if num is a <percentage>, and num’s unit if num is a
    //    <dimension>.
    //    If the value being reified is a computed value, the unit used must be the appropriate canonical unit for the
    //    value’s type, with the numeric value scaled accordingly.
    return CSSUnitValue::create(realm, m_value, "number"_fly_string);
}

}
