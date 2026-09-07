/*
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/CSSConditionRule.h>

namespace Web::CSS {

CSSConditionRule::CSSConditionRule(CSSRuleList& rules, RustRule rule)
    : CSSGroupingRule(rules, move(rule))
{
}

}
