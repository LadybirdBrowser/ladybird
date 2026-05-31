/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "AbstractCanvasMixin.h"
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/ValueType.h>
#include <LibWeb/DOM/Document.h>

namespace Web::HTML {

// https://drafts.csswg.org/css-color-4/#parse-a-css-color-value
Optional<Color> AbstractCanvasMixin::parse_a_css_color_value(StringView const& value) const
{
    // To parse a CSS <color> value, given a string input, and an optional context element element:

    // 1. Parse input as a <color>. If the result is failure, return failure; otherwise, let color be the result.
    auto color = parse_css_type(CSS::Parser::ParsingParams { CSS::Parser::SpecialContext::CanvasContextGenericValue }, value, CSS::ValueType::Color);

    if (!color)
        return {};

    // 2. Let used color be the result of resolving color to a used color. If the value of other properties on the
    //    element a <color> is on is required to do the resolution (such as resolving a currentcolor or system color),
    //    use element if it was passed, or the initial values of the properties if not.

    // AD-HOC: The spec tells us to use the element's properties as context but doesn't tell us how to resolve other
    //         context dependent values (e.g. viewport relative lengths). This also differs from how font-relative
    //         lengths are absolutized (i.e. using the canvas' font rather than the element's) in other canvas values
    //         (e.g. letterSpacing) so we instead use a computation context based on the drawing state. See
    //         https://github.com/whatwg/html/issues/12505.
    auto computation_context = computation_context_for_drawing_state();

    auto color_resolution_context = canvas_element().visit(
        [&](GC::Ref<HTMLCanvasElement> const& canvas_element) {
            canvas_element->document().update_style_if_needed_for_element(*canvas_element);

            if (canvas_element->computed_properties())
                return CSS::ColorResolutionContext::for_element(*canvas_element);

            return CSS::ColorResolutionContext {};
        },
        [&](GC::Ref<OffscreenCanvas> const&) {
            return CSS::ColorResolutionContext {};
        });

    auto used_color = color->absolutized(computation_context)->to_color(color_resolution_context).value();

    // 3. Return used color.
    return used_color;
}

}
