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
    JS_DECLARE_ALLOCATOR(CSSNestedDeclarations);

public:
    [[nodiscard]] static JS::NonnullGCPtr<CSSNestedDeclarations> create(JS::Realm&, PropertyOwningCSSStyleDeclaration&);

    virtual ~CSSNestedDeclarations() override = default;

    virtual Type type() const override { return Type::NestedDeclarations; }
    PropertyOwningCSSStyleDeclaration const& declaration() const { return m_declaration; }

    CSSStyleDeclaration* style();

private:
    CSSNestedDeclarations(JS::Realm&, PropertyOwningCSSStyleDeclaration&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;
    virtual String serialized() const override;

    JS::NonnullGCPtr<PropertyOwningCSSStyleDeclaration> m_declaration;
};

template<>
inline bool CSSRule::fast_is<CSSNestedDeclarations>() const { return type() == CSSRule::Type::NestedDeclarations; }

}
