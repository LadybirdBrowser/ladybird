/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/PercentageOr.h>
#include <LibWeb/CSS/StyleValues/ColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class AbstractImageStyleValue : public StyleValue {
public:
    using StyleValue::StyleValue;

    virtual Optional<CSSPixels> natural_width(DOM::Document const&) const { return {}; }
    virtual Optional<CSSPixels> natural_height(DOM::Document const&) const { return {}; }

    virtual Optional<CSSPixelFraction> natural_aspect_ratio(DOM::Document const& document) const
    {
        auto width = natural_width(document);
        auto height = natural_height(document);
        if (width.has_value() && height.has_value() && *height != 0)
            return *width / *height;
        return {};
    }

    virtual void load_any_resources(DOM::Document&) { }
    virtual void load_any_resources(Layout::NodeWithStyle const&);
    virtual void resolve_for_size(Layout::NodeWithStyle const&, CSSPixelSize) const { }

    virtual bool is_paintable(DOM::Document const&) const = 0;
    virtual void paint(DisplayListRecordingContext& context, DOM::Document const&, DevicePixelRect const& dest_rect, ImageRendering) const = 0;

    virtual Optional<Gfx::Color> color_if_single_pixel_bitmap(DOM::Document const&) const { return {}; }

    virtual GC::Ref<CSSStyleValue> reify(JS::Realm&, Utf16FlyString const& associated_property) const override;
};

// And now, some gradient related things. Maybe these should live somewhere else.

enum class GradientRepeating {
    Yes,
    No
};

struct ColorStopListElement {
    ValueComparingRefPtr<StyleValue const> transition_hint;
    struct ColorStop {
        ValueComparingRefPtr<StyleValue const> color;
        ValueComparingRefPtr<StyleValue const> position;
        ValueComparingRefPtr<StyleValue const> second_position {};
        bool operator==(ColorStop const&) const = default;
    } color_stop;

    bool operator==(ColorStopListElement const&) const = default;
    ColorStopListElement absolutized(ComputationContext const& context) const;
    bool is_computationally_independent() const
    {
        return (!transition_hint || transition_hint->is_computationally_independent())
            && (!color_stop.color || color_stop.color->is_computationally_independent())
            && (!color_stop.position || color_stop.position->is_computationally_independent())
            && (!color_stop.second_position || color_stop.second_position->is_computationally_independent());
    }
};
void serialize_color_stop_list(StringBuilder&, Vector<ColorStopListElement> const&, SerializationMode);

}
