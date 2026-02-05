/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class StyleValueList final : public StyleValueWithDefaultOperators<StyleValueList> {
public:
    enum class Separator {
        Space,
        Comma,
    };
    enum class Collapsible {
        Yes,
        No,
    };
    static ValueComparingNonnullRefPtr<StyleValueList> create(StyleValueVector&& values, Separator separator, Collapsible collapsible = Collapsible::Yes)
    {
        return adopt_ref(*new (nothrow) StyleValueList(move(values), separator, collapsible));
    }

    size_t size() const { return m_properties.values.size(); }
    StyleValueVector const& values() const { return m_properties.values; }
    ValueComparingNonnullRefPtr<StyleValue const> value_at(size_t i, bool allow_loop) const
    {
        if (allow_loop)
            return m_properties.values[i % size()];
        return m_properties.values[i];
    }

    virtual void serialize(StringBuilder&, SerializationMode) const override;
    virtual Vector<Parser::ComponentValue> tokenize() const override;
    virtual GC::Ref<CSSStyleValue> reify(JS::Realm&, FlyString const& associated_property) const override;
    virtual StyleValueVector subdivide_into_iterations(PropertyNameAndID const&) const override;

    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const override;

    bool properties_equal(StyleValueList const& other) const { return m_properties == other.m_properties; }

    Separator separator() const { return m_properties.separator; }

    virtual void set_style_sheet(GC::Ptr<CSSStyleSheet>) override;

private:
    StyleValueList(StyleValueVector&& values, Separator separator, Collapsible collapsible = Collapsible::Yes)
        : StyleValueWithDefaultOperators(Type::ValueList)
        , m_properties {
            .separator = separator,
            .collapsible = collapsible,
            .values = move(values),
        }
    {
    }

    struct Properties {
        Separator separator;
        Collapsible collapsible;
        StyleValueVector values;
        bool operator==(Properties const&) const;
    } m_properties;
};

}
