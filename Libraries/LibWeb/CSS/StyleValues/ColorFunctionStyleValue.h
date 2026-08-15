/*
 * Copyright (c) 2024, Lucas Chollet <lucas.chollet@serenityos.org>
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/Optional.h>
#include <AK/Utf16FlyString.h>
#include <LibWeb/CSS/ColorFunctionDescriptor.h>
#include <LibWeb/CSS/StyleValues/ColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/ComputationContext.h>
#include <LibWeb/Export.h>

namespace Web::CSS {

class WEB_API ColorFunctionStyleValue final : public ColorStyleValue {
public:
    static ValueComparingNonnullRefPtr<ColorFunctionStyleValue const> create(
        ColorType,
        ValueComparingNonnullRefPtr<StyleValue const> c1,
        ValueComparingNonnullRefPtr<StyleValue const> c2,
        ValueComparingNonnullRefPtr<StyleValue const> c3,
        ValueComparingRefPtr<StyleValue const> alpha = {},
        ColorSyntax = ColorSyntax::Modern,
        Optional<Utf16FlyString> name = {},
        ValueComparingRefPtr<StyleValue const> origin_color = {});

    virtual ~ColorFunctionStyleValue() override = default;

    ValueComparingNonnullRefPtr<StyleValue const> channel(size_t index) const
    {
        auto const& color_function = m_value->color_function;
        switch (index) {
        case 0:
            return wrap_rust_child(color_function.channel_0);
        case 1:
            return wrap_rust_child(color_function.channel_1);
        case 2:
            return wrap_rust_child(color_function.channel_2);
        }
        VERIFY_NOT_REACHED();
    }
    Array<ValueComparingNonnullRefPtr<StyleValue const>, 3> channels() const
    {
        return { channel(0), channel(1), channel(2) };
    }
    ValueComparingRefPtr<StyleValue const> alpha() const { return wrap_rust_child_or_null(m_value->color_function.alpha); }
    Optional<Utf16FlyString> name() const
    {
        if (!m_value->color_function.has_name)
            return {};
        return Utf16FlyString::from_raw(m_value->color_function.name.raw);
    }
    ValueComparingRefPtr<StyleValue const> origin_color() const { return wrap_rust_child_or_null(m_value->color_function.origin_color); }

    ColorFunctionDescriptor const& descriptor() const { return color_function_descriptor_for(*color_type()); }

    Optional<Color> to_color(ColorResolutionContext) const;
    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;
    bool equals(StyleValue const&) const;

    ValueComparingRefPtr<StyleValue const> resolve_relative_form(ColorResolutionContext const&) const;

    ValueComparingNonnullRefPtr<StyleValue const> computed_value_form() const;

    bool serializes_as_color_function() const
    {
        return descriptor().serialization_behavior == SerializationBehavior::ColorFunction;
    }

private:
    friend class StyleValue;

    ColorFunctionStyleValue(
        ColorType color_type,
        ValueComparingNonnullRefPtr<StyleValue const> c1,
        ValueComparingNonnullRefPtr<StyleValue const> c2,
        ValueComparingNonnullRefPtr<StyleValue const> c3,
        ValueComparingRefPtr<StyleValue const> alpha,
        ColorSyntax color_syntax,
        Optional<Utf16FlyString> name,
        ValueComparingRefPtr<StyleValue const> origin_color)
        : ColorStyleValue(make_color_function_data(color_type, color_syntax, c1, c2, c3, alpha, name, origin_color))
    {
    }

    explicit ColorFunctionStyleValue(StyleValueFFI::StyleValueData const*);

    static StyleValueFFI::StyleValueData const* make_color_function_data(Optional<ColorType> color_type, ColorSyntax color_syntax, NonnullRefPtr<StyleValue const> const& c1, NonnullRefPtr<StyleValue const> const& c2, NonnullRefPtr<StyleValue const> const& c3, RefPtr<StyleValue const> const& alpha, Optional<Utf16FlyString> const& name, RefPtr<StyleValue const> const& origin_color)
    {
        auto const* alpha_data = alpha ? StyleValueFFI::rust_style_value_retain(alpha->rust_style_value_data()) : nullptr;
        auto const* origin_color_data = origin_color ? StyleValueFFI::rust_style_value_retain(origin_color->rust_style_value_data()) : nullptr;
        return StyleValueFFI::rust_style_value_create_color_function(
            color_type.has_value(), color_type_byte(color_type), to_underlying(color_syntax),
            StyleValueFFI::rust_style_value_retain(c1->rust_style_value_data()),
            StyleValueFFI::rust_style_value_retain(c2->rust_style_value_data()),
            StyleValueFFI::rust_style_value_retain(c3->rust_style_value_data()),
            alpha_data, name.has_value(), name.has_value() ? name->to_raw_leaked() : 0,
            origin_color_data);
    }
};

}
