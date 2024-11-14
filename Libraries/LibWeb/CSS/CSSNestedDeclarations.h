/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSRule.h>

namespace Web::CSS {

class CSSNestedDeclarations final : public CSSRule {
    WEB_PLATFORM_OBJECT(CSSNestedDeclarations, CSSRule);
    GC_DECLARE_ALLOCATOR(CSSNestedDeclarations);

public:
    [[nodiscard]] static GC::Ref<CSSNestedDeclarations> create(JS::Realm&, PropertyOwningCSSStyleDeclaration&);

    virtual ~CSSNestedDeclarations() override = default;

    PropertyOwningCSSStyleDeclaration const& declaration() const { return m_declaration; }

    CSSStyleDeclaration* style();

    CSSStyleRule const& parent_style_rule() const;

private:
    CSSNestedDeclarations(JS::Realm&, PropertyOwningCSSStyleDeclaration&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;
    virtual String serialized() const override;
    virtual void clear_caches() override;

    GC::Ref<PropertyOwningCSSStyleDeclaration> m_declaration;
    GC::Ptr<CSSStyleRule const> mutable m_parent_style_rule;
};

template<>
inline bool CSSRule::fast_is<CSSNestedDeclarations>() const { return type() == CSSRule::Type::NestedDeclarations; }

}
