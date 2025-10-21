/*
 * Copyright (c) 2024-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
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
    static ValueComparingNonnullRefPtr<OpenTypeTaggedStyleValue const> create(Mode mode, FlyString tag, ValueComparingNonnullRefPtr<StyleValue const> value)
    {
        return adopt_ref(*new (nothrow) OpenTypeTaggedStyleValue(mode, move(tag), move(value)));
    }
    virtual ~OpenTypeTaggedStyleValue() override = default;

    FlyString const& tag() const { return m_tag; }
    ValueComparingNonnullRefPtr<StyleValue const> const& value() const { return m_value; }

    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const override;

    virtual String to_string(SerializationMode) const override;

    bool properties_equal(OpenTypeTaggedStyleValue const&) const;

private:
    explicit OpenTypeTaggedStyleValue(Mode mode, FlyString tag, ValueComparingNonnullRefPtr<StyleValue const> value)
        : StyleValueWithDefaultOperators(Type::OpenTypeTagged)
        , m_mode(mode)
        , m_tag(move(tag))
        , m_value(move(value))
    {
    }

    Mode m_mode;
    FlyString m_tag;
    ValueComparingNonnullRefPtr<StyleValue const> m_value;
};

}
