/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/KeyframeEffectPrototype.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/StyleValues/ColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

enum class AllowDiscrete {
    Yes,
    No,
};
ValueComparingRefPtr<StyleValue const> interpolate_property(DOM::Element&, PropertyID, StyleValue const& from, StyleValue const& to, float delta, AllowDiscrete);

// https://drafts.csswg.org/css-transitions/#transitionable
bool property_values_are_transitionable(PropertyID, StyleValue const& old_value, StyleValue const& new_value, DOM::Element&, TransitionBehavior);

Optional<LengthPercentage> interpolate_length_percentage(CalculationContext const& calculation_context, LengthPercentage const& from, LengthPercentage const& to, float delta);
Optional<LengthPercentageOrAuto> interpolate_length_percentage_or_auto(CalculationContext const& calculation_context, LengthPercentageOrAuto const& from, LengthPercentageOrAuto const& to, float delta);

RefPtr<StyleValue const> interpolate_value(DOM::Element&, CalculationContext const&, StyleValue const& from, StyleValue const& to, float delta, AllowDiscrete);
RefPtr<StyleValue const> interpolate_repeatable_list(DOM::Element&, CalculationContext const&, StyleValue const& from, StyleValue const& to, float delta, AllowDiscrete);
RefPtr<StyleValue const> interpolate_box_shadow(DOM::Element&, CalculationContext const&, StyleValue const& from, StyleValue const& to, float delta, AllowDiscrete);
RefPtr<StyleValue const> interpolate_transform(DOM::Element&, CalculationContext const&, StyleValue const& from, StyleValue const& to, float delta, AllowDiscrete);

Color interpolate_color(Color from, Color to, float delta, ColorSyntax syntax);

RefPtr<StyleValue const> composite_value(StyleValue const& a_underlying_value, StyleValue const& a_animated_value, Bindings::CompositeOperation);

}
