/*
 * Copyright (c) 2025-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibWeb/CSS/CSSRule.h>

namespace Web::CSS::Parser {

enum class RuleContext : u8 {
    Unknown,
    Style,
    AtContainer,
    AtCounterStyle,
    AtMedia,
    AtFontFace,
    AtFontFeatureValues,
    FontFeatureValue,
    AtFunction,
    AtKeyframes,
    Keyframe,
    AtSupports,
    SupportsCondition,
    AtLayer,
    AtProperty,
    AtPage,
    Margin,
};
RuleContext rule_context_type_for_rule(CSSRule::Type);
RuleContext rule_context_type_for_at_rule(FlyString const&);

}
