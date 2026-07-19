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

    StyleValue const& channel(size_t index) const
    {
        auto const& data = m_value->color_function;
        switch (index) {
        case 0:
            return *static_cast<StyleValue const*>(data.channel_0.pointer);
        case 1:
            return *static_cast<StyleValue const*>(data.channel_1.pointer);
        case 2:
            return *static_cast<StyleValue const*>(data.channel_2.pointer);
        }
        VERIFY_NOT_REACHED();
    }
    Array<ValueComparingNonnullRefPtr<StyleValue const>, 3> channels() const
    {
        auto const& data = m_value->color_function;
        return {
            ValueComparingNonnullRefPtr<StyleValue const> { *static_cast<StyleValue const*>(data.channel_0.pointer) },
            ValueComparingNonnullRefPtr<StyleValue const> { *static_cast<StyleValue const*>(data.channel_1.pointer) },
            ValueComparingNonnullRefPtr<StyleValue const> { *static_cast<StyleValue const*>(data.channel_2.pointer) },
        };
    }
    ValueComparingRefPtr<StyleValue const> alpha() const { return static_cast<StyleValue const*>(m_value->color_function.alpha.pointer); }
    Optional<Utf16FlyString> name() const
    {
        if (!m_value->color_function.has_name)
            return {};
        return Utf16FlyString::from_raw(m_value->color_function.name.raw);
    }
    ValueComparingRefPtr<StyleValue const> origin_color() const { return static_cast<StyleValue const*>(m_value->color_function.origin_color.pointer); }

    ColorFunctionDescriptor const& descriptor() const { return color_function_descriptor_for(*color_type()); }

    Optional<Color> to_color(ColorResolutionContext) const;
    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;
    void serialize(StringBuilder&, SerializationMode) const;
    bool equals(StyleValue const&) const;

    ValueComparingRefPtr<StyleValue const> resolve_relative_form(ColorResolutionContext const&) const;

    ValueComparingNonnullRefPtr<StyleValue const> computed_value_form() const;

    bool serializes_as_color_function() const
    {
        return descriptor().serialization_behavior == SerializationBehavior::ColorFunction;
    }

private:
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

    static StyleValueFFI::StyleValueData* make_color_function_data(Optional<ColorType> color_type, ColorSyntax color_syntax, NonnullRefPtr<StyleValue const> const& c1, NonnullRefPtr<StyleValue const> const& c2, NonnullRefPtr<StyleValue const> const& c3, RefPtr<StyleValue const> const& alpha, Optional<Utf16FlyString> const& name, RefPtr<StyleValue const> const& origin_color)
    {
        // The Rust allocation takes ownership of one strong reference to each non-null value
        // and one leaked reference to the name when present.
        c1->ref();
        c2->ref();
        c3->ref();
        if (alpha)
            alpha->ref();
        if (origin_color)
            origin_color->ref();
        return StyleValueFFI::rust_style_value_create_color_function(color_type.has_value(), color_type_byte(color_type), to_underlying(color_syntax), c1.ptr(), c2.ptr(), c3.ptr(), alpha.ptr(), name.has_value(), name.has_value() ? name->to_raw_leaked() : 0, origin_color.ptr());
    }
};

}
