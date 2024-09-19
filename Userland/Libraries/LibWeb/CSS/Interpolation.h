/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

ValueComparingRefPtr<CSSStyleValue const> interpolate_property(DOM::Element&, PropertyID, CSSStyleValue const& from, CSSStyleValue const& to, float delta);

// https://drafts.csswg.org/css-transitions/#transitionable
bool property_values_are_transitionable(PropertyID, CSSStyleValue const& old_value, CSSStyleValue const& new_value);

NonnullRefPtr<CSSStyleValue const> interpolate_value(DOM::Element&, CSSStyleValue const& from, CSSStyleValue const& to, float delta);
NonnullRefPtr<CSSStyleValue const> interpolate_box_shadow(DOM::Element&, CSSStyleValue const& from, CSSStyleValue const& to, float delta);
RefPtr<CSSStyleValue const> interpolate_transform(DOM::Element&, CSSStyleValue const& from, CSSStyleValue const& to, float delta);

Color interpolate_color(Color from, Color to, float delta);

}
