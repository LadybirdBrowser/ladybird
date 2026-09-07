/*
 * Copyright (c) 2024-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSRule.h>
#include <LibWeb/CSS/CSSStyleProperties.h>
#include <LibWeb/CSS/Selector.h>

namespace Web::CSS {

class CSSNestedDeclarations final : public CSSRule {
    WEB_WRAPPABLE(CSSNestedDeclarations, CSSRule);
    GC_DECLARE_ALLOCATOR(CSSNestedDeclarations);

public:
    [[nodiscard]] static GC::Ref<CSSNestedDeclarations> create(RustRule);

    virtual ~CSSNestedDeclarations() override = default;

    SelectorList const& absolutized_selectors() const;
    [[nodiscard]] Utf16FlyString const& qualified_layer_name() const { return parent_layer_internal_qualified_name(); }
    RustDeclarationBlock const& declaration() const { return m_declarations; }

    GC::Ref<CSSStyleProperties> style() const;

    CSSStyleRule const& parent_style_rule() const;

private:
    CSSNestedDeclarations(RustRule);
    GC::Ref<CSSStyleProperties> ensure_style_properties() const;
    virtual size_t external_memory_size() const override;

    virtual void visit_edges(Cell::Visitor&) override;
    virtual Utf16String serialized() const override;
    virtual void clear_caches() override;
    virtual void dump(StringBuilder&, int indent_levels) const override;

    RustDeclarationBlock m_declarations;
    mutable GC::Ptr<CSSStyleProperties> m_declaration;
    GC::Ptr<CSSStyleRule const> mutable m_parent_style_rule;
    mutable Optional<SelectorList> m_cached_absolutized_selectors;
};

template<>
inline bool CSSRule::fast_is<CSSNestedDeclarations>() const { return type() == CSSRule::Type::NestedDeclarations; }

}
