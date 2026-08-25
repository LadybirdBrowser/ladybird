/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/Export.h>

namespace Web::CSS {

class WEB_API RadialSizeStyleValue : public StyleValueWithDefaultOperators<RadialSizeStyleValue> {
public:
    using Component = Variant<RadialExtent, NonnullRefPtr<StyleValue const>>;
    static ValueComparingNonnullRefPtr<RadialSizeStyleValue const> create(Vector<Component> components)
    {
        VERIFY(components.size() == 1 || components.size() == 2);
        return adopt_ref(*new (nothrow) RadialSizeStyleValue(move(components)));
    }

    virtual ~RadialSizeStyleValue() override = default;

    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;

    Vector<Component> components() const
    {
        auto const& data = m_value->radial_size;
        Vector<Component> components;
        components.ensure_capacity(data.component_count);
        if (data.is_extent_0)
            components.unchecked_append(static_cast<RadialExtent>(data.extent_0));
        else
            components.unchecked_append(NonnullRefPtr<StyleValue const> { wrap_rust_child(data.value_0) });
        if (data.component_count == 2) {
            if (data.is_extent_1)
                components.unchecked_append(static_cast<RadialExtent>(data.extent_1));
            else
                components.unchecked_append(NonnullRefPtr<StyleValue const> { wrap_rust_child(data.value_1) });
        }
        return components;
    }

private:
    friend class StyleValue;

    explicit RadialSizeStyleValue(Vector<Component> components)
        : StyleValueWithDefaultOperators(Type::RadialSize, make_radial_size_data(components))
    {
    }

    explicit RadialSizeStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValueWithDefaultOperators(Type::RadialSize, data)
    {
    }

    static StyleValueFFI::StyleValueData const* make_radial_size_data(Vector<Component> const& components)
    {
        // The Rust allocation takes ownership of one strong reference to each component value.
        auto is_extent = [](Component const& component) { return component.has<RadialExtent>(); };
        auto extent_of = [&](Component const& component) -> u8 {
            return is_extent(component) ? to_underlying(component.get<RadialExtent>()) : 0;
        };
        auto value_of = [&](Component const& component) -> StyleValueFFI::StyleValueData const* {
            if (is_extent(component))
                return nullptr;
            auto const& value = component.get<NonnullRefPtr<StyleValue const>>();
            return StyleValueFFI::rust_style_value_retain(value->rust_style_value_data());
        };
        bool has_second = components.size() == 2;
        return StyleValueFFI::rust_style_value_create_radial_size(
            static_cast<u8>(components.size()),
            is_extent(components[0]), extent_of(components[0]), value_of(components[0]),
            has_second && is_extent(components[1]), has_second ? extent_of(components[1]) : 0, has_second ? value_of(components[1]) : nullptr);
    }
};

}
