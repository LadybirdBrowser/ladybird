/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibWeb/CSS/CSSStyleValue.h>

namespace Web::CSS {

// An `<opentype-tag>` followed by an optional value.
// For example, <feature-tag-value> ( https://drafts.csswg.org/css-fonts/#feature-tag-value )
// and the `<opentype-tag> <number>` construct for `font-variation-settings`.
class OpenTypeTaggedStyleValue : public StyleValueWithDefaultOperators<OpenTypeTaggedStyleValue> {
public:
    static ValueComparingNonnullRefPtr<OpenTypeTaggedStyleValue> create(FlyString tag, ValueComparingNonnullRefPtr<CSSStyleValue> value)
    {
        return adopt_ref(*new (nothrow) OpenTypeTaggedStyleValue(move(tag), move(value)));
    }
    virtual ~OpenTypeTaggedStyleValue() override = default;

    FlyString const& tag() const { return m_tag; }
    ValueComparingNonnullRefPtr<CSSStyleValue> const& value() const { return m_value; }

    virtual String to_string() const override;

    bool properties_equal(OpenTypeTaggedStyleValue const&) const;

private:
    explicit OpenTypeTaggedStyleValue(FlyString tag, ValueComparingNonnullRefPtr<CSSStyleValue> value)
        : StyleValueWithDefaultOperators(Type::OpenTypeTagged)
        , m_tag(move(tag))
        , m_value(move(value))
    {
    }

    FlyString m_tag;
    ValueComparingNonnullRefPtr<CSSStyleValue> m_value;
};

}
