/*
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/CSSConditionRulePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSConditionRule.h>

namespace Web::CSS {

CSSConditionRule::CSSConditionRule(JS::Realm& realm, CSSRuleList& rules)
    : CSSGroupingRule(realm, rules)
{
}

void CSSConditionRule::for_each_effective_rule(TraversalOrder order, Function<void(Web::CSS::CSSRule const&)> const& callback) const
{
    if (condition_matches())
        CSSGroupingRule::for_each_effective_rule(order, callback);
}

void CSSConditionRule::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSConditionRule);
}

}
