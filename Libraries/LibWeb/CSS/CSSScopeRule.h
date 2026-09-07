/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Utf16String.h>
#include <LibWeb/CSS/CSSGroupingRule.h>
#include <LibWeb/CSS/RustScopeSelectors.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-cascade-6/#the-cssscoperule-interface
class CSSScopeRule final : public CSSGroupingRule {
    WEB_WRAPPABLE(CSSScopeRule, CSSGroupingRule);
    GC_DECLARE_ALLOCATOR(CSSScopeRule);

public:
    [[nodiscard]] static GC::Ref<CSSScopeRule> create(RustRule, CSSRuleList&);

    virtual ~CSSScopeRule() override;

    Optional<SelectorList> const& start_selectors() const { return m_selectors->start(); }
    Optional<SelectorList> const& end_selectors() const { return m_selectors->end(); }
    Optional<Utf16String> start() const;
    Optional<Utf16String> end() const;

private:
    CSSScopeRule(RustRule, CSSRuleList&);
    virtual void visit_edges(GC::Cell::Visitor&) override;
    virtual Utf16String serialized() const override;
    virtual void dump(StringBuilder&, int indent_levels) const override;

    NonnullRefPtr<RustScopeSelectors> m_selectors;
};

template<>
inline bool CSSRule::fast_is<CSSScopeRule>() const { return type() == CSSRule::Type::Scope; }

}
