/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibWeb/CSS/CSSGroupingRule.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-cascade-6/#the-cssscoperule-interface
class CSSScopeRule final : public CSSGroupingRule {
    WEB_PLATFORM_OBJECT(CSSScopeRule, CSSGroupingRule);
    GC_DECLARE_ALLOCATOR(CSSScopeRule);

public:
    [[nodiscard]] static GC::Ref<CSSScopeRule> create(JS::Realm&, Optional<SelectorList>&& start_selectors, Optional<SelectorList>&& end_selectors, CSSRuleList&);

    virtual ~CSSScopeRule() override;

    Optional<SelectorList> const& start_selectors() const { return m_start_selectors; }
    Optional<SelectorList> const& end_selectors() const { return m_end_selectors; }
    Optional<SelectorList> const& start_selectors_for_matching() const;
    Optional<SelectorList> const& end_selectors_for_matching() const;
    GC::Ptr<CSSScopeRule const> nearest_ancestor_scope_rule() const;

    Optional<String> start() const;
    Optional<String> end() const;

private:
    CSSScopeRule(JS::Realm&, Optional<SelectorList>&& start_selectors, Optional<SelectorList>&& end_selectors, CSSRuleList&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;
    virtual void clear_caches() override;
    virtual String serialized() const override;
    virtual void dump(StringBuilder&, int indent_levels) const override;

    Optional<SelectorList> m_start_selectors;
    Optional<SelectorList> m_end_selectors;
    mutable Optional<SelectorList> m_cached_start_selectors_for_matching;
    mutable Optional<SelectorList> m_cached_end_selectors_for_matching;
    mutable Optional<GC::Ptr<CSSScopeRule const>> m_cached_nearest_ancestor_scope_rule;
};

template<>
inline bool CSSRule::fast_is<CSSScopeRule>() const { return type() == CSSRule::Type::Scope; }

SelectorList adapt_scope_end_selectors_for_matching(SelectorList const&);
Optional<SelectorList> const& scope_start_selectors_for_matching(CSSRule const&);
Optional<SelectorList> const& scope_end_selectors_for_matching(CSSRule const&);
GC::Ptr<CSSImportRule const> nearest_scoped_owner_import(CSSStyleSheet const*);
GC::Ptr<CSSRule const> nearest_ancestor_scope_rule_for_matching(CSSRule const&);

}
