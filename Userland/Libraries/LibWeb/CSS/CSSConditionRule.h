/*
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <LibWeb/CSS/CSSGroupingRule.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

class CSSConditionRule : public CSSGroupingRule {
    WEB_PLATFORM_OBJECT(CSSConditionRule, CSSGroupingRule);

public:
    virtual ~CSSConditionRule() = default;

    virtual String condition_text() const = 0;
    virtual bool condition_matches() const = 0;

    virtual void for_each_effective_rule(TraversalOrder, Function<void(CSSRule const&)> const& callback) const override;

protected:
    CSSConditionRule(JS::Realm&, CSSRuleList&);

    virtual void initialize(JS::Realm&) override;
};

}
