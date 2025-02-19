/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/StyleValues/AbstractImageStyleValue.h>
#include <LibWeb/Painting/GradientPainting.h>

namespace Web::CSS {

class RadialGradientStyleValue final : public AbstractImageStyleValue {
public:
    enum class EndingShape {
        Circle,
        Ellipse
    };

    enum class Extent {
        ClosestCorner,
        ClosestSide,
        FarthestCorner,
        FarthestSide
    };

    struct CircleSize {
        Length radius;
        bool operator==(CircleSize const&) const = default;
    };

    struct EllipseSize {
        LengthPercentage radius_a;
        LengthPercentage radius_b;
        bool operator==(EllipseSize const&) const = default;
    };

    using Size = Variant<Extent, CircleSize, EllipseSize>;

    static ValueComparingNonnullRefPtr<RadialGradientStyleValue> create(EndingShape ending_shape, Size size, ValueComparingNonnullRefPtr<PositionStyleValue> position, Vector<LinearColorStopListElement> color_stop_list, GradientRepeating repeating, Optional<InterpolationMethod> interpolation_method)
    {
        VERIFY(!color_stop_list.is_empty());
        bool any_non_legacy = color_stop_list.find_first_index_if([](auto const& stop) { return !stop.color_stop.color->is_keyword() && stop.color_stop.color->as_color().color_syntax() == ColorSyntax::Modern; }).has_value();
        return adopt_ref(*new (nothrow) RadialGradientStyleValue(ending_shape, size, move(position), move(color_stop_list), repeating, interpolation_method, any_non_legacy ? ColorSyntax::Modern : ColorSyntax::Legacy));
    }

    virtual String to_string(SerializationMode) const override;

    void paint(PaintContext&, DevicePixelRect const& dest_rect, CSS::ImageRendering) const override;

    virtual bool equals(CSSStyleValue const& other) const override;

    Vector<LinearColorStopListElement> const& color_stop_list() const
    {
        return m_properties.color_stop_list;
    }

    InterpolationMethod interpolation_method() const
    {
        if (m_properties.interpolation_method.has_value())
            return m_properties.interpolation_method.value();

        return InterpolationMethod { .color_space = InterpolationMethod::default_color_space(m_properties.color_syntax) };
    }

    bool is_paintable() const override { return true; }

    void resolve_for_size(Layout::NodeWithStyle const&, CSSPixelSize) const override;

    CSSPixelSize resolve_size(Layout::Node const&, CSSPixelPoint, CSSPixelRect const&) const;

    bool is_repeating() const { return m_properties.repeating == GradientRepeating::Yes; }

    virtual ~RadialGradientStyleValue() override = default;

private:
    RadialGradientStyleValue(EndingShape ending_shape, Size size, ValueComparingNonnullRefPtr<PositionStyleValue> position, Vector<LinearColorStopListElement> color_stop_list, GradientRepeating repeating, Optional<InterpolationMethod> interpolation_method, ColorSyntax color_syntax)
        : AbstractImageStyleValue(Type::RadialGradient)
        , m_properties { .ending_shape = ending_shape, .size = size, .position = move(position), .color_stop_list = move(color_stop_list), .repeating = repeating, .interpolation_method = interpolation_method, .color_syntax = color_syntax }
    {
    }

    struct Properties {
        EndingShape ending_shape;
        Size size;
        ValueComparingNonnullRefPtr<PositionStyleValue> position;
        Vector<LinearColorStopListElement> color_stop_list;
        GradientRepeating repeating;
        Optional<InterpolationMethod> interpolation_method;
        ColorSyntax color_syntax;
        bool operator==(Properties const&) const = default;
    } m_properties;

    struct ResolvedDataCacheKey {
        Length::ResolutionContext length_resolution_context;
        CSSPixelSize size;
        bool operator==(ResolvedDataCacheKey const&) const = default;
    };
    mutable Optional<ResolvedDataCacheKey> m_resolved_data_cache_key;

    struct ResolvedData {
        Painting::RadialGradientData data;
        CSSPixelSize gradient_size;
        CSSPixelPoint center;
    };
    mutable Optional<ResolvedData> m_resolved;
};

}
