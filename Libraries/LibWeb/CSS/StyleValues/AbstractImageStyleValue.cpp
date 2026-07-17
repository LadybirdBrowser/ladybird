/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "AbstractImageStyleValue.h"
#include <LibWeb/CSS/CSSImageValue.h>
#include <LibWeb/CSS/StyleValues/RustStyleValueHandle.h>
#include <LibWeb/Layout/Node.h>

namespace Web::CSS {

StyleValueFFI::RetainedColorStop retain_color_stop_for_rust(ColorStopListElement const& stop)
{
    return { { retain_style_value_for_rust(stop.transition_hint.ptr()) }, { retain_style_value_for_rust(stop.color_stop.color.ptr()) },
        { retain_style_value_for_rust(stop.color_stop.position.ptr()) }, { retain_style_value_for_rust(stop.color_stop.second_position.ptr()) } };
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
    return {
        .transition_hint = static_cast<StyleValue const*>(stop.transition_hint.pointer),
        .color_stop = {
            .color = static_cast<StyleValue const*>(stop.color.pointer),
            .position = static_cast<StyleValue const*>(stop.position.pointer),
            .second_position = static_cast<StyleValue const*>(stop.second_position.pointer),
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
GC::Ref<CSSStyleValue> AbstractImageStyleValue::reify(JS::Realm& realm, Utf16FlyString const&) const
{
    // AD-HOC: There's no spec description of how to reify as a CSSImageValue.
    return CSSImageValue::create(realm, *this);
}

void AbstractImageStyleValue::load_any_resources(Layout::NodeWithStyle const& layout_node)
{
    load_any_resources(const_cast<DOM::Document&>(layout_node.document()));
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

void serialize_color_stop_list(StringBuilder& builder, ReadonlySpan<ColorStopListElement> color_stop_list, SerializationMode mode)
{
    bool first = true;
    for (auto const& element : color_stop_list) {
        if (!first)
            builder.append(", "sv);

        if (element.transition_hint) {
            element.transition_hint->serialize(builder, mode);
            builder.append(", "sv);
        }

        element.color_stop.color->serialize(builder, mode);
        if (element.color_stop.position) {
            builder.append(' ');
            element.color_stop.position->serialize(builder, mode);
        }
        if (element.color_stop.second_position) {
            builder.append(' ');
            element.color_stop.second_position->serialize(builder, mode);
        }
        first = false;
    }
}

}
