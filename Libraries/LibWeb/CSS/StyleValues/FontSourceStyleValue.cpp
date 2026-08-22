/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/StyleValues/FontSourceStyleValue.h>
#include <LibWeb/CSS/StyleValues/URLStyleValue.h>

namespace Web::CSS {

FontSourceStyleValue::Source FontSourceStyleValue::source() const
{
    auto const& data = m_value->font_source;
    if (data.is_local)
        return Local { wrap_rust_child(data.local_name) };

    return url_from_rust_data(data.url, data.url_type, data.url_modifiers);
}

FontSourceStyleValue::FontSourceStyleValue(StyleValueFFI::StyleValueData const* data)
    : StyleValueWithDefaultOperators(Type::FontSource, data)
{
}

FontSourceStyleValue::~FontSourceStyleValue() = default;

}
