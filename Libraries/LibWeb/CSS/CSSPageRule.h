/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSGroupingRule.h>
#include <LibWeb/CSS/CSSPageDescriptors.h>
#include <LibWeb/CSS/Selector.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-page-3/#at-ruledef-page
class CSSPageRule final : public CSSGroupingRule {
    WEB_PLATFORM_OBJECT(CSSPageRule, CSSGroupingRule);
    GC_DECLARE_ALLOCATOR(CSSPageRule);

public:
    [[nodiscard]] static GC::Ref<CSSPageRule> create(JS::Realm&, SelectorList&&, GC::Ref<CSSPageDescriptors>, CSSRuleList&);

    virtual ~CSSPageRule() override = default;

    String selector_text() const;
    void set_selector_text(StringView);

    GC::Ref<CSSPageDescriptors> style() { return m_style; }
    GC::Ref<CSSPageDescriptors const> descriptors() const { return m_style; }

private:
    CSSPageRule(JS::Realm&, SelectorList&&, GC::Ref<CSSPageDescriptors>, CSSRuleList&);

    virtual void initialize(JS::Realm&) override;
    virtual String serialized() const override;
    virtual void visit_edges(Visitor&) override;

    SelectorList m_selectors;
    GC::Ref<CSSPageDescriptors> m_style;
};

}
