/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/CalculationResolutionContext.h>
#include <LibWeb/CSS/StyleValues/AnchorStyleValue.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>

namespace Web::CSS {

ValueComparingNonnullRefPtr<AnchorStyleValue const> AnchorStyleValue::create(
    Optional<FlyString> const& anchor_name,
    ValueComparingNonnullRefPtr<StyleValue const> const& anchor_side,
    ValueComparingRefPtr<StyleValue const> const& fallback_value)
{
    return adopt_ref(*new (nothrow) AnchorStyleValue(anchor_name, anchor_side, fallback_value));
}

AnchorStyleValue::AnchorStyleValue(Optional<FlyString> const& anchor_name,
    ValueComparingNonnullRefPtr<StyleValue const> const& anchor_side,
    ValueComparingRefPtr<StyleValue const> const& fallback_value)
    : AbstractNonMathCalcFunctionStyleValue(Type::Anchor)
    , m_properties { .anchor_name = anchor_name, .anchor_side = anchor_side, .fallback_value = fallback_value }
{
}

void AnchorStyleValue::serialize(StringBuilder& builder, SerializationMode serialization_mode) const
{
    builder.append("anchor("sv);

    if (anchor_name().has_value())
        builder.append(anchor_name().value());

    if (anchor_name().has_value())
        builder.append(' ');
    anchor_side()->serialize(builder, serialization_mode);

    if (fallback_value()) {
        builder.append(", "sv);
        fallback_value()->serialize(builder, serialization_mode);
    }

    builder.append(')');
}

// https://drafts.csswg.org/css-anchor-position-1/#anchor-pos
RefPtr<CalculationNode const> AnchorStyleValue::resolve_to_calculation_node(CalculationContext const& calculation_context, CalculationResolutionContext const& calculation_resolution_context) const
{
    if (!calculation_resolution_context.anchor_resolver)
        return nullptr;

    // An anchor() function representing a resolvable anchor function resolves at computed value time (using style &
    // layout interleaving) to the <length> that would align the edge of the positioned boxes' inset-modified containing
    // block corresponding to the property the function appears in with the specified edge of the target anchor
    // element’s anchor box.
    if (auto side_px = calculation_resolution_context.anchor_resolver->resolve(*this); side_px.has_value())
        return NumericCalculationNode::create(Length::make_px(side_px.release_value()), calculation_context);

    // If any of these conditions are false, the anchor() function computes to its specified fallback value. If no
    // fallback value is specified, it makes the declaration referencing it invalid at computed-value time.
    auto const& fallback_value = m_properties.fallback_value;
    if (!fallback_value)
        return nullptr;

    // NB: The fallback value can itself be an anchor(), which is resolved when the substituted tree is simplified.
    NonnullRefPtr<CalculationNode const> fallback_node = fallback_value->is_anchor()
        ? static_cast<NonnullRefPtr<CalculationNode const>>(NonMathFunctionCalculationNode::create(fallback_value->as_anchor(), NumericType { NumericType::BaseType::Length, 1 }))
        : CalculationNode::from_style_value(*fallback_value, calculation_context);
    return simplify_a_calculation_tree(fallback_node, calculation_context, calculation_resolution_context);
}

bool AnchorStyleValue::equals(StyleValue const& other) const
{
    if (type() != other.type())
        return false;

    return m_properties == other.as_anchor().m_properties;
}

}
