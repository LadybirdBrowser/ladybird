/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "AbstractImageStyleValue.h"
#include <LibWeb/CSS/CSSImageValue.h>
#include <LibWeb/CSS/StyleValues/ImageSetStyleValue.h>
#include <LibWeb/CSS/StyleValues/ImageStyleValue.h>
#include <LibWeb/HTML/DecodedImageData.h>
#include <LibWeb/Layout/Node.h>

namespace Web::CSS {

StyleValueFFI::RetainedColorStop retain_color_stop_for_rust(ColorStopListElement const& stop)
{
    auto retain = [](StyleValue const* value) {
        return value ? StyleValueFFI::rust_style_value_retain(value->rust_style_value_data()) : nullptr;
    };
    return { { retain(stop.transition_hint.ptr()) }, { retain(stop.color_stop.color.ptr()) },
        { retain(stop.color_stop.position.ptr()) }, { retain(stop.color_stop.second_position.ptr()) } };
}

Vector<StyleValueFFI::RetainedColorStop> retain_color_stops_for_rust(ReadonlySpan<ColorStopListElement> color_stop_list)
{
    Vector<StyleValueFFI::RetainedColorStop> stops;
    stops.ensure_capacity(color_stop_list.size());
    for (auto const& stop : color_stop_list)
        stops.unchecked_append(retain_color_stop_for_rust(stop));
    return stops;
}

ColorStopListElement color_stop_from_rust_data(StyleValueFFI::RetainedColorStop const& stop)
{
    auto adopt = [](auto const& retained) -> ValueComparingRefPtr<StyleValue const> {
        auto const* data = static_cast<StyleValueFFI::StyleValueData const*>(retained.pointer);
        if (!data)
            return nullptr;
        return StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(data));
    };
    return {
        .transition_hint = adopt(stop.transition_hint),
        .color_stop = {
            .color = adopt(stop.color),
            .position = adopt(stop.position),
            .second_position = adopt(stop.second_position),
        },
    };
}

Vector<ColorStopListElement> color_stops_from_rust_data(StyleValueFFI::RetainedColorStop const* color_stop_list, size_t size)
{
    Vector<ColorStopListElement> stops;
    stops.ensure_capacity(size);
    for (size_t i = 0; i < size; ++i)
        stops.unchecked_append(color_stop_from_rust_data(color_stop_list[i]));
    return stops;
}

// https://drafts.css-houdini.org/css-typed-om-1/#reify-stylevalue
GC::Ref<CSSStyleValue> AbstractImageStyleValue::reify(Utf16FlyString const&) const
{
    // AD-HOC: There's no spec description of how to reify as a CSSImageValue.
    return CSSImageValue::create(*this);
}

void AbstractImageStyleValue::load_any_resources(Layout::NodeWithStyle const& layout_node)
{
    load_any_resources(const_cast<DOM::Document&>(layout_node.document()));
}

ImageStyleValue const* AbstractImageStyleValue::selected_image_style_value() const
{
    if (is_image())
        return &as_image();
    if (is_image_set()) {
        if (auto const* selected_image = as_image_set().selected_image(); selected_image && selected_image->is_image())
            return &selected_image->as_image();
    }
    return nullptr;
}

SizeWithAspectRatio AbstractImageStyleValue::natural_size(HTML::DecodedImageData const& decoded_image_data) const
{
    return {
        .width = decoded_image_data.intrinsic_width(),
        .height = decoded_image_data.intrinsic_height(),
        .aspect_ratio = decoded_image_data.intrinsic_aspect_ratio(),
    };
}

Optional<Painting::ImagePaint> AbstractImageStyleValue::image_paint(Painting::ImagePaintRequest const&) const
{
    return {};
}

ColorStopListElement ColorStopListElement::absolutized(ComputationContext const& context) const
{
    auto absolutize_if_nonnull = [&context](RefPtr<StyleValue const> const& input) -> RefPtr<StyleValue const> {
        if (!input)
            return {};
        return input->absolutized(context);
    };

    return {
        .transition_hint = absolutize_if_nonnull(transition_hint),
        .color_stop = {
            .color = absolutize_if_nonnull(color_stop.color),
            .position = absolutize_if_nonnull(color_stop.position),
            .second_position = absolutize_if_nonnull(color_stop.second_position),
        },
    };
}

}
