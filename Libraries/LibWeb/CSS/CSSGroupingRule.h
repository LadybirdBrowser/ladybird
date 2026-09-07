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
    static constexpr size_t rules_offset() { return offsetof(CSSGroupingRule, m_rules); }
    virtual ~CSSGroupingRule() = default;

    CSSRuleList const& css_rules() const { return m_rules; }
    CSSRuleList& css_rules() { return m_rules; }
    CSSRuleList* css_rules_for_bindings() { return m_rules.ptr(); }
    WebIDL::ExceptionOr<u32> insert_rule(Utf16View rule, u32 index = 0);
    WebIDL::ExceptionOr<void> delete_rule(u32 index);

    virtual void set_parent_style_sheet(StyleSheetState*) override;

protected:
    CSSGroupingRule(CSSRuleList&, RustRule);
    virtual void visit_edges(GC::Cell::Visitor&) override;
    virtual void clear_caches() override;

private:
    GC::Ref<CSSRuleList> m_rules;
};

}
