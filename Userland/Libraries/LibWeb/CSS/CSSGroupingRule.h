/*
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <LibWeb/CSS/CSSRule.h>
#include <LibWeb/CSS/CSSRuleList.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

class CSSGroupingRule : public CSSRule {
    WEB_PLATFORM_OBJECT(CSSGroupingRule, CSSRule);

public:
    virtual ~CSSGroupingRule() = default;

    CSSRuleList const& css_rules() const { return m_rules; }
    CSSRuleList& css_rules() { return m_rules; }
    CSSRuleList* css_rules_for_bindings() { return m_rules; }
    WebIDL::ExceptionOr<u32> insert_rule(StringView rule, u32 index = 0);
    WebIDL::ExceptionOr<void> delete_rule(u32 index);

    virtual void for_each_effective_rule(TraversalOrder, Function<void(CSSRule const&)> const& callback) const;

    virtual void set_parent_style_sheet(CSSStyleSheet*) override;

protected:
    CSSGroupingRule(JS::Realm&, CSSRuleList&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

private:
    JS::NonnullGCPtr<CSSRuleList> m_rules;
};

}
