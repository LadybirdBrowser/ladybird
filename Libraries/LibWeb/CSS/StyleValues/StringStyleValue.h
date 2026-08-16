/*
 * Copyright (c) 2022-2024, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class StringStyleValue : public StyleValueWithDefaultOperators<StringStyleValue> {
public:
    static ValueComparingNonnullRefPtr<StringStyleValue const> create(Utf16FlyString string)
    {
        return adopt_ref(*new (nothrow) StringStyleValue(move(string)));
    }
    virtual ~StringStyleValue() override = default;

    Utf16FlyString string_value() const { return Utf16FlyString::from_raw(m_value->string.string.raw); }

private:
    friend class StyleValue;

    explicit StringStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValueWithDefaultOperators(Type::String, data)
    {
    }

    explicit StringStyleValue(Utf16FlyString string)
        : StyleValueWithDefaultOperators(Type::String, [&] {
            auto is_valid_animation_name_custom_ident = !string.equals_ignoring_ascii_case("default"sv)
                && !string.equals_ignoring_ascii_case("none"sv)
                && !CSS::is_css_wide_keyword(string);
            return StyleValueFFI::rust_style_value_create_string(
                string.to_raw_leaked(), is_valid_animation_name_custom_ident);
        }())
    {
    }
};

}
