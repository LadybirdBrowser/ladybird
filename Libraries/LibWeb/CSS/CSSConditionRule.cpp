/*
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/CSSConditionRule.h>

namespace Web::CSS {

CSSConditionRule::CSSConditionRule(CSSRuleList& rules, Type type)
    : CSSGroupingRule(rules, type)
{
}

void CSSConditionRule::for_each_effective_rule(TraversalOrder order, Function<void(Web::CSS::CSSRule const&)> const& callback) const
{
    if (condition_matches())
        CSSGroupingRule::for_each_effective_rule(order, callback);
}

}
