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
    WEB_WRAPPABLE(CSSGroupingRule, CSSRule);

public:
    virtual ~CSSGroupingRule() = default;

    CSSRuleList const& css_rules() const { return ensure_rules(); }
    CSSRuleList& css_rules() { return ensure_rules(); }
    CSSRuleList* css_rules_for_bindings() { return &ensure_rules(); }
    WebIDL::ExceptionOr<u32> insert_rule(Utf16View rule, u32 index = 0);
    WebIDL::ExceptionOr<void> delete_rule(u32 index);

    virtual void set_parent_style_sheet(StyleSheetState*) override;

protected:
    // NB: A rule given no list, as a style rule is, makes its list when it is first read.
    CSSGroupingRule(GC::Ptr<CSSRuleList>, RustRule);
    virtual void visit_edges(GC::Cell::Visitor&) override;
    virtual void clear_caches() override;

private:
    CSSRuleList& ensure_rules() const;

    mutable GC::Ptr<CSSRuleList> m_rules;
};

}
