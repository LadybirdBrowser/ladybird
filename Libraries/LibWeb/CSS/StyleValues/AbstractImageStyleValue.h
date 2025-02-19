/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/PercentageOr.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/CSSColorValue.h>

namespace Web::CSS {

class AbstractImageStyleValue : public CSSStyleValue {
public:
    using CSSStyleValue::CSSStyleValue;

    virtual Optional<CSSPixels> natural_width() const { return {}; }
    virtual Optional<CSSPixels> natural_height() const { return {}; }

    virtual Optional<CSSPixelFraction> natural_aspect_ratio() const
    {
        auto width = natural_width();
        auto height = natural_height();
        if (width.has_value() && height.has_value())
            return *width / *height;
        return {};
    }

    virtual void load_any_resources(DOM::Document&) { }
    virtual void resolve_for_size(Layout::NodeWithStyle const&, CSSPixelSize) const { }

    virtual bool is_paintable() const = 0;
    virtual void paint(PaintContext& context, DevicePixelRect const& dest_rect, ImageRendering) const = 0;

    virtual Optional<Gfx::Color> color_if_single_pixel_bitmap() const { return {}; }
};

// And now, some gradient related things. Maybe these should live somewhere else.

enum class GradientRepeating {
    Yes,
    No
};

enum class GradientSpace : u8 {
    sRGB,
    sRGBLinear,
    DisplayP3,
    A98RGB,
    ProPhotoRGB,
    Rec2020,
    Lab,
    OKLab,
    XYZD50,
    XYZD65,
    HSL,
    HWB,
    LCH,
    OKLCH,
};

enum class HueMethod : u8 {
    Shorter,
    Longer,
    Increasing,
    Decreasing,
};

struct InterpolationMethod {
    GradientSpace color_space;
    HueMethod hue_method = HueMethod::Shorter;

    String to_string() const
    {
        StringBuilder builder;

        switch (color_space) {
        case GradientSpace::OKLab:
            builder.append("in oklab"sv);
            break;
        case GradientSpace::sRGB:
            builder.append("in srgb"sv);
            break;
        case GradientSpace::sRGBLinear:
            builder.append("in srgb-linear"sv);
            break;
        case GradientSpace::DisplayP3:
            builder.append("in display-p3"sv);
            break;
        case GradientSpace::A98RGB:
            builder.append("in a98-rgb"sv);
            break;
        case GradientSpace::ProPhotoRGB:
            builder.append("in prophoto-rgb"sv);
            break;
        case GradientSpace::Rec2020:
            builder.append("in rec2020"sv);
            break;
        case GradientSpace::Lab:
            builder.append("in lab"sv);
            break;
        case GradientSpace::XYZD50:
            builder.append("in xyz-d50"sv);
            break;
        case GradientSpace::XYZD65:
            builder.append("in xyz-d65"sv);
            break;
        case GradientSpace::HSL:
            builder.append("in hsl"sv);
            break;
        case GradientSpace::HWB:
            builder.append("in hwb"sv);
            break;
        case GradientSpace::LCH:
            builder.append("in lch"sv);
            break;
        case GradientSpace::OKLCH:
            builder.append("in oklch"sv);
            break;
        }

        switch (hue_method) {
        case HueMethod::Shorter:
            // "shorter" is the default value and isn't serialized
            break;
        case HueMethod::Longer:
            builder.append(" longer hue"sv);
            break;
        case HueMethod::Increasing:
            builder.append(" increasing hue"sv);
            break;
        case HueMethod::Decreasing:
            builder.append(" decreasing hue"sv);
            break;
        }

        return MUST(builder.to_string());
    }

    static GradientSpace default_color_space(ColorSyntax color_syntax)
    {
        if (color_syntax == ColorSyntax::Legacy)
            return GradientSpace::sRGB;

        return GradientSpace::OKLab;
    }

    bool operator==(InterpolationMethod const&) const = default;
};

template<typename TPosition>
struct ColorStopListElement {
    using PositionType = TPosition;
    struct ColorHint {
        TPosition value;
        inline bool operator==(ColorHint const&) const = default;
    };

    Optional<ColorHint> transition_hint;
    struct ColorStop {
        RefPtr<CSSStyleValue> color;
        Optional<TPosition> position;
        Optional<TPosition> second_position = {};
        inline bool operator==(ColorStop const&) const = default;
    } color_stop;

    inline bool operator==(ColorStopListElement const&) const = default;
};

using LinearColorStopListElement = ColorStopListElement<LengthPercentage>;
using AngularColorStopListElement = ColorStopListElement<AnglePercentage>;

static void serialize_color_stop_list(StringBuilder& builder, auto const& color_stop_list, CSSStyleValue::SerializationMode mode)
{
    bool first = true;
    for (auto const& element : color_stop_list) {
        if (!first)
            builder.append(", "sv);

        if (element.transition_hint.has_value())
            builder.appendff("{}, "sv, element.transition_hint->value.to_string());

        builder.append(element.color_stop.color->to_string(mode));
        for (auto position : Array { &element.color_stop.position, &element.color_stop.second_position }) {
            if (position->has_value())
                builder.appendff(" {}"sv, (*position)->to_string());
        }
        first = false;
    }
}

}
