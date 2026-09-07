/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <LibWeb/CSS/CSSGroupingRule.h>
#include <LibWeb/CSS/CSSStyleProperties.h>
#include <LibWeb/CSS/Selector.h>

namespace Web::CSS {

class CSSStyleRule final : public CSSGroupingRule {
    WEB_WRAPPABLE(CSSStyleRule, CSSGroupingRule);
    GC_DECLARE_ALLOCATOR(CSSStyleRule);

public:
    [[nodiscard]] static GC::Ref<CSSStyleRule> create(RustRule, CSSRuleList&);

    virtual ~CSSStyleRule() override = default;

    SelectorList const& selectors() const;
    SelectorList const& absolutized_selectors() const;
    RustDeclarationBlock const& declaration() const { return m_declarations; }

    Utf16String selector_text() const;
    void set_selector_text(Utf16View);

    GC::Ref<CSSStyleProperties> style() const;
    GC::Ref<StylePropertyMap> style_map();

    [[nodiscard]] Utf16FlyString const& qualified_layer_name() const { return parent_layer_internal_qualified_name(); }

private:
    CSSStyleRule(RustRule, CSSRuleList&);
    GC::Ref<CSSStyleProperties> ensure_style_properties() const;
    virtual size_t external_memory_size() const override;
    virtual void visit_edges(GC::Cell::Visitor&) override;
    virtual void clear_caches() override;
    virtual Utf16String serialized() const override;
    virtual void dump(StringBuilder&, int indent_levels) const override;

    mutable Optional<SelectorList> m_selectors;
    mutable Optional<SelectorList> m_cached_absolutized_selectors;
    RustDeclarationBlock m_declarations;
    mutable GC::Ptr<CSSStyleProperties> m_declaration;
    GC::Ptr<StylePropertyMap> m_style_map;
};

template<>
inline bool CSSRule::fast_is<CSSStyleRule>() const { return type() == CSSRule::Type::Style; }

}
