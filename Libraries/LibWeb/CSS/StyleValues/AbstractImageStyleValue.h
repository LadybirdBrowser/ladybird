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
#include <LibWeb/CSS/Sizing.h>
#include <LibWeb/CSS/StyleValues/ColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/Export.h>
#include <LibWeb/Painting/ImagePaint.h>

namespace Web::CSS {

class WEB_API AbstractImageStyleValue : public StyleValue {
public:
    using StyleValue::StyleValue;

    virtual void load_any_resources(DOM::Document&) { }
    virtual void load_any_resources(Layout::NodeWithStyle const&);

    virtual bool is_paintable(GC::Ptr<HTML::DecodedImageData>) const = 0;
    virtual SizeWithAspectRatio natural_size(HTML::DecodedImageData const&) const;
    virtual Optional<Painting::ImagePaint> image_paint(Painting::ImagePaintRequest const&) const;

    ImageStyleValue const* selected_image_style_value() const;

    GC::Ref<CSSStyleValue> reify(Utf16FlyString const& associated_property) const;
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
};

namespace StyleValueFFI {

struct RetainedColorStop;

}

// Marshals a color stop for a Rust-owned gradient allocation, retaining one strong reference
// to each non-null sub-value.
StyleValueFFI::RetainedColorStop retain_color_stop_for_rust(ColorStopListElement const&);
Vector<StyleValueFFI::RetainedColorStop> retain_color_stops_for_rust(ReadonlySpan<ColorStopListElement>);
ColorStopListElement color_stop_from_rust_data(StyleValueFFI::RetainedColorStop const&);
Vector<ColorStopListElement> color_stops_from_rust_data(StyleValueFFI::RetainedColorStop const*, size_t);

}
