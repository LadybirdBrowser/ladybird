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

class StyleValueList final : public StyleValueWithDefaultOperators<StyleValueList> {
public:
    enum class Separator {
        Space,
        Comma,
    };
    static ValueComparingNonnullRefPtr<StyleValueList> create(StyleValueVector&& values, Separator separator)
    {
        return adopt_ref(*new (nothrow) StyleValueList(move(values), separator));
    }

    size_t size() const { return m_properties.values.size(); }
    StyleValueVector const& values() const { return m_properties.values; }
    ValueComparingNonnullRefPtr<CSSStyleValue const> value_at(size_t i, bool allow_loop) const
    {
        if (allow_loop)
            return m_properties.values[i % size()];
        return m_properties.values[i];
    }

    virtual String to_string() const override;

    bool properties_equal(StyleValueList const& other) const { return m_properties == other.m_properties; }

    Separator separator() const { return m_properties.separator; }

private:
    StyleValueList(StyleValueVector&& values, Separator separator)
        : StyleValueWithDefaultOperators(Type::ValueList)
        , m_properties { .separator = separator, .values = move(values) }
    {
    }

    struct Properties {
        Separator separator;
        StyleValueVector values;
        bool operator==(Properties const&) const;
    } m_properties;
};

}
