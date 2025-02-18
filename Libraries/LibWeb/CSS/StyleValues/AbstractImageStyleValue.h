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
