/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSStyleValue.h>

namespace Web::CSS {

class ContentStyleValue final : public StyleValueWithDefaultOperators<ContentStyleValue> {
public:
    static ValueComparingNonnullRefPtr<ContentStyleValue> create(ValueComparingNonnullRefPtr<StyleValueList> content, ValueComparingRefPtr<StyleValueList> alt_text)
    {
        return adopt_ref(*new (nothrow) ContentStyleValue(move(content), move(alt_text)));
    }
    virtual ~ContentStyleValue() override = default;

    StyleValueList const& content() const { return *m_properties.content; }
    bool has_alt_text() const { return !m_properties.alt_text.is_null(); }
    StyleValueList const* alt_text() const { return m_properties.alt_text; }

    virtual String to_string() const override;

    bool properties_equal(ContentStyleValue const& other) const { return m_properties == other.m_properties; }

private:
    ContentStyleValue(ValueComparingNonnullRefPtr<StyleValueList> content, ValueComparingRefPtr<StyleValueList> alt_text)
        : StyleValueWithDefaultOperators(Type::Content)
        , m_properties { .content = move(content), .alt_text = move(alt_text) }
    {
    }

    struct Properties {
        ValueComparingNonnullRefPtr<StyleValueList> content;
        ValueComparingRefPtr<StyleValueList> alt_text;
        bool operator==(Properties const&) const = default;
    } m_properties;
};

}
