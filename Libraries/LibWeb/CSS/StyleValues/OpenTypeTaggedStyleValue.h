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
    static ValueComparingNonnullRefPtr<OpenTypeTaggedStyleValue const> create(Mode mode, Utf16FlyString tag, ValueComparingNonnullRefPtr<StyleValue const> value)
    {
        return adopt_ref(*new (nothrow) OpenTypeTaggedStyleValue(mode, move(tag), move(value)));
    }
    virtual ~OpenTypeTaggedStyleValue() override = default;

    Mode mode() const { return static_cast<Mode>(m_value->open_type_tagged.mode); }
    Utf16FlyString tag() const { return Utf16FlyString::from_raw(m_value->open_type_tagged.tag.raw); }
    ValueComparingNonnullRefPtr<StyleValue const> value() const { return m_setting_value; }

    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;

    void serialize(StringBuilder&, SerializationMode) const;

    bool properties_equal(OpenTypeTaggedStyleValue const&) const;

private:
    friend class StyleValue;

    explicit OpenTypeTaggedStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValueWithDefaultOperators(Type::OpenTypeTagged, data)
        , m_setting_value(StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(static_cast<StyleValueFFI::StyleValueData const*>(data->open_type_tagged.value.pointer))))
    {
    }

    explicit OpenTypeTaggedStyleValue(Mode mode, Utf16FlyString tag, ValueComparingNonnullRefPtr<StyleValue const> value)
        : StyleValueWithDefaultOperators(Type::OpenTypeTagged, StyleValueFFI::rust_style_value_create_open_type_tagged(to_underlying(mode), tag.to_raw_leaked(), pack_tag(tag), StyleValueFFI::rust_style_value_retain(value->rust_style_value_data())))
        , m_setting_value(move(value))
    {
    }

    static u32 pack_tag(Utf16FlyString const& tag)
    {
        VERIFY(tag.length_in_code_units() == 4);
        return static_cast<u32>(tag.code_unit_at(0)) << 24
            | static_cast<u32>(tag.code_unit_at(1)) << 16
            | static_cast<u32>(tag.code_unit_at(2)) << 8
            | static_cast<u32>(tag.code_unit_at(3));
    }

    ValueComparingNonnullRefPtr<StyleValue const> m_setting_value;
};

}
