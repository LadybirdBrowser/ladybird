/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "AbstractCanvasMixin.h"
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Parser/RustSyntaxParsing.h>
#include <LibWeb/CSS/ValueType.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/ValueParserRustFFI.h>

namespace Web::HTML {

// https://drafts.csswg.org/css-color-4/#parse-a-css-color-value
Optional<Color> AbstractCanvasMixin::parse_a_css_color_value(Utf16View value) const
{
    // To parse a CSS <color> value, given a string input, and an optional context element element:

    // 1. Parse input as a <color>. If the result is failure, return failure; otherwise, let color be the result.
    RefPtr<CSS::StyleValue const> color;
    Optional<Color> resolved_color;
    bool found_cached_value = false;
    bool const input_is_cacheable = value.length_in_code_units() <= max_color_cache_input_length;
    if (input_is_cacheable) {
        for (size_t index = 0; index < m_color_cache.size(); ++index) {
            if (m_color_cache[index].input != value)
                continue;

            color = m_color_cache[index].parsed_value;
            resolved_color = m_color_cache[index].resolved_color;
            found_cached_value = true;

            if (index != m_color_cache.size() - 1) {
                auto entry = move(m_color_cache[index]);
                m_color_cache.remove(index);
                m_color_cache.append(move(entry));
            }
            break;
        }
    }

    if (!found_cached_value) {
        auto parsed_color = CSS::Parser::ValueParserFFI::rust_parse_simple_color(CSS::Parser::ffi_utf16_view(value));
        if (parsed_color.success)
            resolved_color = Color(parsed_color.red, parsed_color.green, parsed_color.blue, parsed_color.alpha);
        if (!resolved_color.has_value())
            color = parse_css_type(CSS::Parser::ParsingParams { CSS::Parser::SpecialContext::CanvasContextGenericValue }, value, CSS::ValueType::Color);

        if (input_is_cacheable) {
            if (m_color_cache.size() == max_color_cache_size)
                m_color_cache.remove(0);
            m_color_cache.append({ Utf16String::from_utf16(value), color, resolved_color });
        }
    }

    if (resolved_color.has_value())
        return resolved_color;

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
            canvas_element->document().update_style_for_element(*canvas_element, DOM::Document::StyleUpdateMode::OnlyIfNeeded);

            if (canvas_element->has_style())
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
