/*
 * Copyright (c) 2024-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSRule.h>
#include <LibWeb/CSS/CSSStyleProperties.h>

namespace Web::CSS {

class CSSNestedDeclarations final : public CSSRule {
    WEB_PLATFORM_OBJECT(CSSNestedDeclarations, CSSRule);
    GC_DECLARE_ALLOCATOR(CSSNestedDeclarations);

public:
    [[nodiscard]] static GC::Ref<CSSNestedDeclarations> create(JS::Realm&, CSSStyleProperties&);

    virtual ~CSSNestedDeclarations() override = default;

    CSSStyleProperties const& declaration() const { return m_declaration; }

    CSSStyleDeclaration* style();

    CSSStyleRule const& parent_style_rule() const;

private:
    CSSNestedDeclarations(JS::Realm&, CSSStyleProperties&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;
    virtual String serialized() const override;
    virtual void clear_caches() override;

    GC::Ref<CSSStyleProperties> m_declaration;
    GC::Ptr<CSSStyleRule const> mutable m_parent_style_rule;
};

template<>
inline bool CSSRule::fast_is<CSSNestedDeclarations>() const { return type() == CSSRule::Type::NestedDeclarations; }

}
