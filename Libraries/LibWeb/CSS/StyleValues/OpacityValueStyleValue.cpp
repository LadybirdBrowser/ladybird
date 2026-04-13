/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "OpacityValueStyleValue.h"
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-color-4/#serializing-opacity-values
void OpacityValueStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    // If the specified value for an opacity value matches a literal <percentage-token> (i.e. does not use calc()) it
    // should be serialized as the equivalent <number> (0% maps to 0, 100% maps to 1) value. value.

    // NB: We resolve percentages to numbers at computed value time so this will only ever the the specified value.
    if (m_value->is_percentage()) {
        serialize_a_number(builder, m_value->as_percentage().percentage().as_fraction());
        return;
    }

    // Otherwise, the specified value for an opacity value should serialize using the standard serialization for the grammar.
    m_value->serialize(builder, mode);
}

ValueComparingNonnullRefPtr<StyleValue const> OpacityValueStyleValue::absolutized(ComputationContext const& computation_context) const
{
    if (m_value->is_number() && m_value->as_number().number() > 0 && m_value->as_number().number() < 1)
        return *this;

    auto clamped_number_value = clamp(number_from_style_value(m_value->absolutized(computation_context), 1), 0, 1);

    return OpacityValueStyleValue::create(NumberStyleValue::create(clamped_number_value));
}

GC::Ref<CSSStyleValue> OpacityValueStyleValue::reify(JS::Realm& realm, FlyString const& associated_property) const
{
    return m_value->reify(realm, associated_property);
}

}
