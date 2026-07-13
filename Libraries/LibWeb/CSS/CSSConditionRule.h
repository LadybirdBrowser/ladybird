/*
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Utf16String.h>
#include <LibWeb/CSS/CSSGroupingRule.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

class CSSConditionRule : public CSSGroupingRule {
    WEB_PLATFORM_OBJECT(CSSConditionRule, CSSGroupingRule);

public:
    virtual ~CSSConditionRule() = default;

    Utf16String condition_text() const { return serialized_condition_text(); }
    virtual Utf16String serialized_condition_text() const = 0;
    virtual bool condition_matches() const = 0;

    virtual void for_each_effective_rule(TraversalOrder, Function<void(CSSRule const&)> const& callback) const override;

protected:
    CSSConditionRule(JS::Realm&, CSSRuleList&, Type);

    virtual void initialize(JS::Realm&) override;
};

}
