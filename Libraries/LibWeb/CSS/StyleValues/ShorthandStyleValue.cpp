/*
 * Copyright (c) 2023, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/StyleValues/ShorthandStyleValue.h>

namespace Web::CSS {

ShorthandStyleValue::ShorthandStyleValue(PropertyID shorthand, Vector<PropertyID> sub_properties, Vector<ValueComparingNonnullRefPtr<StyleValue const>> values)
    : StyleValueWithDefaultOperators(Type::Shorthand, make_shorthand_data(shorthand, sub_properties, values))
    , m_values(move(values))
{
    if (m_value->shorthand.sub_properties.length != m_value->shorthand.values.length) {
        dbgln("ShorthandStyleValue: sub_properties and values must be the same size! {} != {}", m_value->shorthand.sub_properties.length, m_value->shorthand.values.length);
        VERIFY_NOT_REACHED();
    }
}

ShorthandStyleValue::~ShorthandStyleValue() = default;

ValueComparingRefPtr<StyleValue const> ShorthandStyleValue::longhand(PropertyID longhand) const
{
    for (auto i = 0u; i < size(); ++i) {
        if (sub_property_at(i) == longhand)
            return value_at(i);
    }
    return nullptr;
}

void ShorthandStyleValue::set_style_sheet(StyleSheetState* style_sheet)
{
    for (auto& value : values())
        const_cast<StyleValue&>(*value).set_style_sheet(style_sheet);
}

}
