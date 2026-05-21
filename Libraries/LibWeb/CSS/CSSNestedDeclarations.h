/*
 * Copyright (c) 2024-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSRule.h>
#include <LibWeb/CSS/CSSStyleProperties.h>
#include <LibWeb/CSS/Parser/Types.h>

namespace Web::CSS {

class CSSNestedDeclarations final : public CSSRule {
    WEB_PLATFORM_OBJECT(CSSNestedDeclarations, CSSRule);
    GC_DECLARE_ALLOCATOR(CSSNestedDeclarations);

public:
    [[nodiscard]] static GC::Ref<CSSNestedDeclarations> create(JS::Realm&, Parser::Parser&, Vector<Parser::Declaration> const&);
    [[nodiscard]] static GC::Ref<CSSNestedDeclarations> create(JS::Realm&, CSSStyleProperties&);

    virtual ~CSSNestedDeclarations() override = default;

    SelectorList const& absolutized_selectors() const;
    [[nodiscard]] FlyString const& qualified_layer_name() const { return parent_layer_internal_qualified_name(); }
    CSSStyleProperties const& declaration() const { return m_declaration; }

    GC::Ref<CSSStyleProperties> style();

    CSSStyleRule const& parent_style_rule() const;

private:
    CSSNestedDeclarations(JS::Realm&, CSSStyleProperties&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;
    virtual String serialized() const override;
    virtual void clear_caches() override;
    virtual void dump(StringBuilder&, int indent_levels) const override;

    GC::Ref<CSSStyleProperties> m_declaration;
    GC::Ptr<CSSStyleRule const> mutable m_parent_style_rule;
    mutable Optional<SelectorList> m_cached_absolutized_selectors;
};

template<>
inline bool CSSRule::fast_is<CSSNestedDeclarations>() const { return type() == CSSRule::Type::NestedDeclarations; }

}
