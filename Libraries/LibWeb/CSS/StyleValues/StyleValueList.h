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

    size_t size() const { return m_value->value_list.values.length; }
    StyleValueVector values() const
    {
        auto const& values = m_value->value_list.values;
        StyleValueVector result;
        result.ensure_capacity(values.length);
        for (size_t i = 0; i < values.length; ++i) {
            auto* child_data = static_cast<StyleValueFFI::StyleValueData const*>(values.pointer[i].pointer);
            result.unchecked_append(StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(child_data)));
        }
        return result;
    }
    ValueComparingNonnullRefPtr<StyleValue const> value_at(size_t i, bool allow_loop) const
    {
        if (allow_loop)
            return value_at(i % size());
        return value_at(i);
    }

    GC::Ref<CSSStyleValue> reify(Utf16FlyString const& associated_property) const;
    StyleValueVector subdivide_into_iterations(PropertyNameAndID const&) const;

    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;

    Separator separator() const { return static_cast<Separator>(m_value->value_list.separator); }

    void set_style_sheet(StyleSheetState*);

private:
    friend class StyleValue;

    explicit StyleValueList(StyleValueFFI::StyleValueData const* data)
        : StyleValueWithDefaultOperators(Type::ValueList, data)
    {
    }

    StyleValueList(StyleValueVector&& values, Separator separator, Collapsible collapsible = Collapsible::Yes)
        : StyleValueWithDefaultOperators(Type::ValueList, make_value_list_data(values, separator, collapsible))
    {
    }

    ValueComparingNonnullRefPtr<StyleValue const> value_at(size_t i) const
    {
        auto* child_data = static_cast<StyleValueFFI::StyleValueData const*>(m_value->value_list.values.pointer[i].pointer);
        return StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(child_data));
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
};

}
