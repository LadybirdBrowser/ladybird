/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/CalculationResolutionContext.h>
#include <LibWeb/CSS/StyleValues/AnchorStyleValue.h>
#include <LibWeb/CSS/StyleValues/CalcNodeRef.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-anchor-position-1/#anchor-pos
Optional<CalcNodeRef> AnchorStyleValue::resolve_to_calculation_node(CalculationContext const& calculation_context, CalculationResolutionContext const& calculation_resolution_context) const
{
    if (!calculation_resolution_context.anchor_resolver)
        return {};

    // An anchor() function representing a resolvable anchor function resolves at computed value time (using style &
    // layout interleaving) to the <length> that would align the edge of the positioned boxes' inset-modified containing
    // block corresponding to the property the function appears in with the specified edge of the target anchor
    // element’s anchor box.
    if (auto side_px = calculation_resolution_context.anchor_resolver->resolve(*this); side_px.has_value())
        return CalcNodeRef::numeric(Length::make_px(side_px.release_value()));

    // If any of these conditions are false, the anchor() function computes to its specified fallback value. If no
    // fallback value is specified, it makes the declaration referencing it invalid at computed-value time.
    auto fallback_value = this->fallback_value();
    if (!fallback_value)
        return {};

    // NB: The fallback value can itself be an anchor(), which is resolved when the substituted tree is simplified.
    auto fallback_node = fallback_value->is_anchor()
        ? CalcNodeRef::non_math_function(fallback_value->as_anchor(), NumericType { NumericType::BaseType::Length, 1 })
        : CalcNodeRef::from_style_value(*fallback_value);
    return simplify_a_calculation_tree(fallback_node, calculation_context, calculation_resolution_context);
}

}
