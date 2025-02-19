/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/Angle.h>
#include <LibWeb/CSS/StyleValues/AbstractImageStyleValue.h>
#include <LibWeb/Painting/GradientPainting.h>

namespace Web::CSS {

class ConicGradientStyleValue final : public AbstractImageStyleValue {
public:
    static ValueComparingNonnullRefPtr<ConicGradientStyleValue> create(Angle from_angle, ValueComparingNonnullRefPtr<PositionStyleValue> position, Vector<AngularColorStopListElement> color_stop_list, GradientRepeating repeating, Optional<InterpolationMethod> interpolation_method)
    {
        VERIFY(!color_stop_list.is_empty());
        bool any_non_legacy = color_stop_list.find_first_index_if([](auto const& stop) { return !stop.color_stop.color->is_keyword() && stop.color_stop.color->as_color().color_syntax() == ColorSyntax::Modern; }).has_value();
        return adopt_ref(*new (nothrow) ConicGradientStyleValue(from_angle, move(position), move(color_stop_list), repeating, interpolation_method, any_non_legacy ? ColorSyntax::Modern : ColorSyntax::Legacy));
    }

    virtual String to_string(SerializationMode) const override;

    void paint(PaintContext&, DevicePixelRect const& dest_rect, CSS::ImageRendering) const override;

    virtual bool equals(CSSStyleValue const& other) const override;

    Vector<AngularColorStopListElement> const& color_stop_list() const
    {
        return m_properties.color_stop_list;
    }

    InterpolationMethod interpolation_method() const
    {
        if (m_properties.interpolation_method.has_value())
            return m_properties.interpolation_method.value();

        return InterpolationMethod { .color_space = InterpolationMethod::default_color_space(m_properties.color_syntax) };
    }

    float angle_degrees() const;

    bool is_paintable() const override { return true; }

    void resolve_for_size(Layout::NodeWithStyle const&, CSSPixelSize) const override;

    virtual ~ConicGradientStyleValue() override = default;

    bool is_repeating() const { return m_properties.repeating == GradientRepeating::Yes; }

private:
    ConicGradientStyleValue(Angle from_angle, ValueComparingNonnullRefPtr<PositionStyleValue> position, Vector<AngularColorStopListElement> color_stop_list, GradientRepeating repeating, Optional<InterpolationMethod> interpolation_method, ColorSyntax color_syntax)
        : AbstractImageStyleValue(Type::ConicGradient)
        , m_properties { .from_angle = from_angle, .position = move(position), .color_stop_list = move(color_stop_list), .repeating = repeating, .interpolation_method = interpolation_method, .color_syntax = color_syntax }
    {
    }

    struct Properties {
        Angle from_angle;
        ValueComparingNonnullRefPtr<PositionStyleValue> position;
        Vector<AngularColorStopListElement> color_stop_list;
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
        Painting::ConicGradientData data;
        CSSPixelPoint position;
    };
    mutable Optional<ResolvedData> m_resolved;
};

}
