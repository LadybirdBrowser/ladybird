/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/CSS/Zoom.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Layout/Node.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-viewport/#zoom-om
// Computed style APIs (i.e., all values returned by getComputedStyle()) that are non-auto and non-percentage lengths
// must be unscaled.
NonnullRefPtr<StyleValue const> unscaled_by_zoom(PropertyID property_id, NonnullRefPtr<StyleValue const> value, double zoom)
{
    if (zoom == 1 || property_id == PropertyID::Zoom)
        return value;
    if (auto const* unscaled = StyleValueFFI::rust_style_value_unscaled_by_zoom(value->rust_style_value_data(), zoom))
        return StyleValue::adopt_rust_style_value_data(unscaled);
    return value;
}

// https://drafts.csswg.org/css-viewport/#unscaled
// The unscaled value of a CSS length relative to an element is the scaled value divided by the element's effective
// zoom.
double unscaled(double value, double effective_zoom)
{
    return effective_zoom == 1 ? value : value / effective_zoom;
}

CSSPixels unscaled(CSSPixels value, double effective_zoom)
{
    if (effective_zoom == 1)
        return value;
    return CSSPixels::nearest_value_for(unscaled(value.to_double(), effective_zoom));
}

// https://drafts.csswg.org/css-viewport/#effective-zoom
double effective_zoom(DOM::Element const& element)
{
    auto const* inherited_box_values = element.style_group<ComputedValues::InheritedBoxValues>();
    return inherited_box_values ? inherited_box_values->effective_zoom_value() : 1;
}

double effective_zoom(Layout::NodeWithStyle const& layout_node)
{
    return layout_node.style_group<ComputedValues::InheritedBoxValues>().effective_zoom_value();
}

}
