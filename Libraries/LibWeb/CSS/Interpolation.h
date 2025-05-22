/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

struct CalculationContext;

enum class AllowDiscrete {
    Yes,
    No,
};
ValueComparingRefPtr<CSSStyleValue const> interpolate_property(DOM::Element&, PropertyID, CSSStyleValue const& from, CSSStyleValue const& to, float delta, AllowDiscrete);

// https://drafts.csswg.org/css-transitions/#transitionable
bool property_values_are_transitionable(PropertyID, CSSStyleValue const& old_value, CSSStyleValue const& new_value, DOM::Element&, TransitionBehavior);

RefPtr<CSSStyleValue const> interpolate_value(DOM::Element&, CalculationContext const&, CSSStyleValue const& from, CSSStyleValue const& to, float delta, AllowDiscrete);
RefPtr<CSSStyleValue const> interpolate_repeatable_list(DOM::Element&, CalculationContext const&, CSSStyleValue const& from, CSSStyleValue const& to, float delta, AllowDiscrete);
RefPtr<CSSStyleValue const> interpolate_box_shadow(DOM::Element&, CalculationContext const&, CSSStyleValue const& from, CSSStyleValue const& to, float delta, AllowDiscrete);
RefPtr<CSSStyleValue const> interpolate_transform(DOM::Element&, CSSStyleValue const& from, CSSStyleValue const& to, float delta, AllowDiscrete);

Color interpolate_color(Color from, Color to, float delta);

}
