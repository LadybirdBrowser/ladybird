/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <LibWeb/CSS/CSSGroupingRule.h>
#include <LibWeb/CSS/CSSStyleProperties.h>
#include <LibWeb/CSS/Selector.h>

namespace Web::CSS {

class CSSStyleRule final : public CSSGroupingRule {
    WEB_PLATFORM_OBJECT(CSSStyleRule, CSSGroupingRule);
    GC_DECLARE_ALLOCATOR(CSSStyleRule);

public:
    [[nodiscard]] static GC::Ref<CSSStyleRule> create(JS::Realm&, SelectorList&&, CSSStyleProperties&, CSSRuleList&);

    virtual ~CSSStyleRule() override = default;

    SelectorList const& selectors() const { return m_selectors; }
    SelectorList const& absolutized_selectors() const;
    CSSStyleProperties const& declaration() const { return m_declaration; }

    String selector_text() const;
    void set_selector_text(StringView);

    GC::Ref<CSSStyleProperties> style();

    [[nodiscard]] FlyString const& qualified_layer_name() const { return parent_layer_internal_qualified_name(); }

private:
    CSSStyleRule(JS::Realm&, SelectorList&&, CSSStyleProperties&, CSSRuleList&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;
    virtual void clear_caches() override;
    virtual String serialized() const override;

    CSSStyleRule const* parent_style_rule() const;

    SelectorList m_selectors;
    mutable Optional<SelectorList> m_cached_absolutized_selectors;
    GC::Ref<CSSStyleProperties> m_declaration;
};

template<>
inline bool CSSRule::fast_is<CSSStyleRule>() const { return type() == CSSRule::Type::Style; }

}
