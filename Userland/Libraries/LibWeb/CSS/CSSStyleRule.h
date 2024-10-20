/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <LibWeb/CSS/CSSGroupingRule.h>
#include <LibWeb/CSS/CSSStyleDeclaration.h>
#include <LibWeb/CSS/Selector.h>

namespace Web::CSS {

class CSSStyleRule final : public CSSGroupingRule {
    WEB_PLATFORM_OBJECT(CSSStyleRule, CSSGroupingRule);
    JS_DECLARE_ALLOCATOR(CSSStyleRule);

public:
    [[nodiscard]] static JS::NonnullGCPtr<CSSStyleRule> create(JS::Realm&, SelectorList&&, PropertyOwningCSSStyleDeclaration&, CSSRuleList&);

    virtual ~CSSStyleRule() override = default;

    SelectorList const& selectors() const { return m_selectors; }
    SelectorList const& absolutized_selectors() const;
    PropertyOwningCSSStyleDeclaration const& declaration() const { return m_declaration; }

    virtual Type type() const override { return Type::Style; }

    String selector_text() const;
    void set_selector_text(StringView);

    CSSStyleDeclaration* style();

    [[nodiscard]] FlyString const& qualified_layer_name() const { return parent_layer_internal_qualified_name(); }

private:
    CSSStyleRule(JS::Realm&, SelectorList&&, PropertyOwningCSSStyleDeclaration&, CSSRuleList&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;
    virtual void clear_caches() override;
    virtual String serialized() const override;

    SelectorList m_selectors;
    mutable Optional<SelectorList> m_cached_absolutized_selectors;
    JS::NonnullGCPtr<PropertyOwningCSSStyleDeclaration> m_declaration;
};

template<>
inline bool CSSRule::fast_is<CSSStyleRule>() const { return type() == CSSRule::Type::Style; }

}
