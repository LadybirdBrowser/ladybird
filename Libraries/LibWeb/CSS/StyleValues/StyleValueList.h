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

    size_t size() const { return m_values.size(); }
    StyleValueVector values() const
    {
        return m_values;
    }
    ValueComparingNonnullRefPtr<StyleValue const> value_at(size_t i, bool allow_loop) const
    {
        if (allow_loop)
            return value_at(i % size());
        return value_at(i);
    }

    void serialize(StringBuilder&, SerializationMode) const;
    Vector<Parser::ComponentValue> tokenize() const;
    GC::Ref<CSSStyleValue> reify(Utf16FlyString const& associated_property) const;
    StyleValueVector subdivide_into_iterations(PropertyNameAndID const&) const;

    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;

    bool properties_equal(StyleValueList const& other) const
    {
        if (separator() != other.separator() || collapsible() != other.collapsible() || size() != other.size())
            return false;
        for (size_t i = 0; i < size(); ++i) {
            if (value_at(i) != other.value_at(i))
                return false;
        }
        return true;
    }

    Separator separator() const { return static_cast<Separator>(m_value->value_list.separator); }

    void set_style_sheet(GC::Ptr<CSSStyleSheet>);

private:
    friend class StyleValue;

    explicit StyleValueList(StyleValueFFI::StyleValueData const* data)
        : StyleValueWithDefaultOperators(Type::ValueList, data)
    {
        auto const& values = data->value_list.values;
        m_values.ensure_capacity(values.length);
        for (size_t i = 0; i < values.length; ++i) {
            auto* child_data = static_cast<StyleValueFFI::StyleValueData const*>(values.pointer[i].pointer);
            m_values.unchecked_append(StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(child_data)));
        }
    }

    StyleValueList(StyleValueVector&& values, Separator separator, Collapsible collapsible = Collapsible::Yes)
        : StyleValueWithDefaultOperators(Type::ValueList, make_value_list_data(values, separator, collapsible))
        , m_values(move(values))
    {
    }

    ValueComparingNonnullRefPtr<StyleValue const> value_at(size_t i) const
    {
        return m_values[i];
    }

    static StyleValueFFI::StyleValueData const* make_value_list_data(StyleValueVector const& values, Separator separator, Collapsible collapsible)
    {
        Vector<StyleValueFFI::StyleValueData const*> pointers;
        pointers.ensure_capacity(values.size());
        for (auto const& value : values)
            pointers.unchecked_append(StyleValueFFI::rust_style_value_retain(value->rust_style_value_data()));
        return StyleValueFFI::rust_style_value_create_value_list(pointers.data(), pointers.size(), to_underlying(separator), collapsible == Collapsible::Yes);
    }

    Collapsible collapsible() const { return m_value->value_list.collapsible ? Collapsible::Yes : Collapsible::No; }

    StyleValueVector m_values;
};

}
