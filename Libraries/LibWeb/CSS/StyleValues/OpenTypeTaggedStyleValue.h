/*
 * Copyright (c) 2024-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

// An `<opentype-tag>` followed by an optional value.
// For example, <feature-tag-value> ( https://drafts.csswg.org/css-fonts/#feature-tag-value )
// and the `<opentype-tag> <number>` construct for `font-variation-settings`.
class OpenTypeTaggedStyleValue : public StyleValueWithDefaultOperators<OpenTypeTaggedStyleValue> {
public:
    enum class Mode {
        FontFeatureSettings,
        FontVariationSettings,
    };
    virtual ~OpenTypeTaggedStyleValue() override = default;

    Mode mode() const { return static_cast<Mode>(m_value->open_type_tagged.mode); }
    Utf16FlyString tag() const { return css_string_from_rust(&m_value->open_type_tagged.tag_name); }
    ValueComparingNonnullRefPtr<StyleValue const> value() const { return wrap_rust_child(m_value->open_type_tagged.value); }

private:
    friend class StyleValue;

    explicit OpenTypeTaggedStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValueWithDefaultOperators(Type::OpenTypeTagged, data)
    {
    }
};

}
