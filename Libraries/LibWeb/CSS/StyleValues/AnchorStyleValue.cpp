/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/CalculationResolutionContext.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/AnchorStyleValue.h>
#include <LibWeb/CSS/StyleValues/CalcNodeRef.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>

namespace Web::CSS {

static StyleValueFFI::StyleValueData* make_anchor_data(Optional<Utf16FlyString> const& anchor_name, ValueComparingNonnullRefPtr<StyleValue const> const& anchor_side, ValueComparingRefPtr<StyleValue const> const& fallback_value)
{
    // The Rust allocation takes ownership of one strong reference to the side and, when present,
    // the fallback value.
    anchor_side->ref();
    if (fallback_value)
        fallback_value->ref();
    return StyleValueFFI::rust_style_value_create_anchor(
        anchor_name.has_value(),
        anchor_name.has_value() ? anchor_name->to_raw_leaked() : 0,
        anchor_side.ptr(),
        fallback_value.ptr());
}

ValueComparingNonnullRefPtr<AnchorStyleValue const> AnchorStyleValue::create(
    Optional<Utf16FlyString> const& anchor_name,
    ValueComparingNonnullRefPtr<StyleValue const> const& anchor_side,
    ValueComparingRefPtr<StyleValue const> const& fallback_value)
{
    return adopt_ref(*new (nothrow) AnchorStyleValue(anchor_name, anchor_side, fallback_value));
}

AnchorStyleValue::AnchorStyleValue(Optional<Utf16FlyString> const& anchor_name,
    ValueComparingNonnullRefPtr<StyleValue const> const& anchor_side,
    ValueComparingRefPtr<StyleValue const> const& fallback_value)
    : AbstractNonMathCalcFunctionStyleValue(Type::Anchor, make_anchor_data(anchor_name, anchor_side, fallback_value))
{
}

void AnchorStyleValue::serialize(StringBuilder& builder, SerializationMode serialization_mode) const
{
    builder.append("anchor("sv);

    if (anchor_name().has_value())
        builder.append(serialize_an_identifier(anchor_name().value()));

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

bool AnchorStyleValue::equals(StyleValue const& other) const
{
    if (type() != other.type())
        return false;

    auto const& other_anchor = other.as_anchor();
    return anchor_name() == other_anchor.anchor_name() && anchor_side() == other_anchor.anchor_side() && fallback_value() == other_anchor.fallback_value();
}

}
